// ListenLink link service: maps short stream IDs to the producer's current
// (ephemeral) tunnel URL, so shared links survive tunnel restarts.
//
//   POST /l/register        {id, token, url}  -> 204 | 403 (token mismatch)
//   POST /l/unregister      {id, token}       -> 204 | 403
//   GET  /l/<id>/resolve    -> {"url": "..."} | 404   (CORS: any origin)
//   GET  /l/<id>/ping       -> 204 if the stream is reachable right now | 404
//   GET  /l/<id>            -> listener page (fetched from the plugin through
//                              the tunnel and served from THIS host) | offline page
//   GET  /l/<id>/ws         -> WebSocket proxied to the tunnel's /ws
//
// Why serve + proxy instead of redirecting: every quick tunnel gets a brand-new
// trycloudflare.com hostname, and a listener whose resolver asks for it before
// Cloudflare has published it caches NXDOMAIN for up to 30 minutes (the zone's
// SOA minimum TTL). Measured 2026-09-10: Quad9 stayed NXDOMAIN for the life of
// a test tunnel while other resolvers had it within seconds. With the page and
// the socket both on gggaudio.store the browser never resolves the tunnel host;
// the hop to trycloudflare happens inside Cloudflare, which is authoritative for
// that zone. The redirect path is kept only for plugins older than 0.7.0, whose
// page does not know how to speak to /l/<id>/ws (detected by a meta marker).
//
// A mapping is claimed by whichever token first registers an id; later
// updates must present the same token. Entries expire 7 days after the
// last registration so abandoned ids age out of KV.

const TTL_SECONDS = 7 * 24 * 3600;
const ID_RE = /^[a-z0-9]{6,32}$/;
const CORS = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
  'Access-Control-Allow-Headers': 'Content-Type',
};

const offlinePage = (id) => `<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="robots" content="noindex"><title>ListenLink</title>
<style>
  :root { color-scheme: dark; }
  body { font-family: -apple-system, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
         background:#101014; color:#e8e8ec; min-height:100vh; margin:0; display:flex;
         align-items:center; justify-content:center; }
  .card { background:#1a1a21; border:1px solid #2a2a33; border-radius:16px;
          padding:40px 44px; width:min(420px,92vw); text-align:center; }
  h1 { font-size:20px; margin:0 0 6px; }
  p { color:#8a8a96; font-size:14px; margin:0; }
</style></head><body>
<div class="card"><h1>ListenLink</h1>
<p>This stream is offline right now.<br>Keep this link &mdash; it goes live again when the sender starts streaming.</p>
</div>
<script>
// The id is validated as [a-z0-9]{6,32} before this page is built.
// /ping only answers 204 once the tunnel is actually reachable, so a stale
// registration from a crashed DAW never turns this into a reload loop.
var tries = 0;
(function poll() {
  var wait = ++tries < 24 ? 2500 : 10000;   // 2.5s for the first minute, then 10s
  fetch('/l/${id}/ping', { cache: 'no-store' })
    .then(function(r){ if (r.status === 204) location.reload(); else setTimeout(poll, wait); })
    .catch(function(){ setTimeout(poll, wait); });
})();
</script>
</body></html>`;

// Fetch the listener page from the plugin through its tunnel. One quick retry
// covers a tunnel that registered seconds ago and is still coming up.
async function fetchPage(tunnelUrl) {
  for (let attempt = 0; attempt < 2; attempt++) {
    try {
      const r = await fetch(tunnelUrl + '/', {
        redirect: 'manual', signal: AbortSignal.timeout(5000),
        headers: { 'Accept': 'text/html' } });
      if (r.status >= 200 && r.status < 400) return r;
    } catch (_) {}
    if (attempt === 0) await new Promise(res => setTimeout(res, 1500));
  }
  return null;
}

const NO_STORE_HTML = { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' };

async function readBody(request) {
  try { return await request.json(); } catch (_) { return null; }
}

function valid(id, token) {
  return typeof id === 'string' && ID_RE.test(id)
      && typeof token === 'string' && token.length >= 16 && token.length <= 128;
}

export default {
  async fetch(request, env) {
    const { pathname } = new URL(request.url);
    const parts = pathname.split('/').filter(Boolean); // ["l", ...]

    if (request.method === 'OPTIONS')
      return new Response(null, { status: 204, headers: CORS });

    if (parts[0] !== 'l')
      return new Response('Not found', { status: 404 });

    // POST /l/register | /l/unregister
    if (request.method === 'POST' && (parts[1] === 'register' || parts[1] === 'unregister')) {
      const body = await readBody(request);
      if (!body || !valid(body.id, body.token))
        return new Response('Bad request', { status: 400, headers: CORS });

      const key = 'stream:' + body.id;
      const existing = await env.STREAMS.get(key, 'json');
      if (existing && existing.token !== body.token)
        return new Response('Forbidden', { status: 403, headers: CORS });

      if (parts[1] === 'unregister') {
        if (existing) await env.STREAMS.delete(key);
        return new Response(null, { status: 204, headers: CORS });
      }

      const url = String(body.url || '');
      if (!/^https:\/\/[a-z0-9-]+\.trycloudflare\.com$/.test(url))
        return new Response('Bad url', { status: 400, headers: CORS });

      await env.STREAMS.put(key, JSON.stringify({ token: body.token, url, t: Date.now() }),
                            { expirationTtl: TTL_SECONDS });
      return new Response(null, { status: 204, headers: CORS });
    }

    // GET /l/<id>[/resolve]
    if (request.method === 'GET' && parts[1] && ID_RE.test(parts[1])) {
      const entry = await env.STREAMS.get('stream:' + parts[1], 'json');

      if (parts[2] === 'resolve') {
        if (!entry)
          return new Response(JSON.stringify({ url: null }),
            { status: 404, headers: { ...CORS, 'Content-Type': 'application/json' } });
        return new Response(JSON.stringify({ url: entry.url }),
          { headers: { ...CORS, 'Content-Type': 'application/json',
                       'Cache-Control': 'no-store' } });
      }

      // GET /l/<id>/ping -> 204 only when the tunnel answers right now.
      if (parts[2] === 'ping') {
        const ok = entry && (await fetchPage(entry.url)) !== null;
        return new Response(null, { status: ok ? 204 : 404,
          headers: { ...CORS, 'Cache-Control': 'no-store' } });
      }

      // GET /l/<id>/ws -> proxy the WebSocket to the tunnel. Returning the
      // upstream 101 response as-is makes the edge pipe frames between the
      // two sockets; no Worker code runs per message, so audio costs nothing
      // here beyond the one request. The plugin's own 403 (sharing stopped)
      // passes straight through to the page.
      if (parts[2] === 'ws') {
        if (request.headers.get('Upgrade') !== 'websocket')
          return new Response('Expected WebSocket', { status: 426 });
        if (!entry)
          return new Response('Stream offline', { status: 404 });
        const url = new URL(request.url);
        return fetch(new Request(entry.url + '/ws' + url.search, request));
      }

      if (parts[2])
        return new Response('Not found', { status: 404, headers: CORS });

      // GET /l/<id> -> the listener page itself, served from this host.
      // Fetching it through the tunnel doubles as the liveness probe: a crashed
      // DAW never unregisters, and a dead trycloudflare host would otherwise
      // show listeners a raw Cloudflare error instead of our offline page.
      const page = entry ? await fetchPage(entry.url) : null;
      if (!page)
        return new Response(offlinePage(parts[1]), { status: 200, headers: NO_STORE_HTML });

      const html = await page.text();
      // Pages from plugins older than 0.7.0 only know how to reach /ws on the
      // host they were loaded from - keep redirecting those to the tunnel.
      if (!html.includes('name="ll-proxy"'))
        return Response.redirect(entry.url, 302);

      return new Response(html, { status: 200, headers: NO_STORE_HTML });
    }

    return new Response('Not found', { status: 404, headers: CORS });
  },
};
