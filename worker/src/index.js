// ListenLink link service: maps short stream IDs to the producer's current
// (ephemeral) tunnel URL, so shared links survive tunnel restarts.
//
//   POST /l/register        {id, token, url}  -> 204 | 403 (token mismatch)
//   POST /l/unregister      {id, token}       -> 204 | 403
//   GET  /l/<id>/resolve    -> {"url": "..."} | 404   (CORS: any origin)
//   GET  /l/<id>            -> 302 to tunnel URL | offline page
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
<script>setTimeout(function(){ location.reload(); }, 15000);</script>
</body></html>`;

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

      await env.STREAMS.put(key, JSON.stringify({ token: body.token, url }),
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

      // Probe the tunnel before redirecting: a crashed DAW never unregisters,
      // and a dead trycloudflare host would show listeners a raw Cloudflare
      // error instead of our offline page.
      let alive = false;
      if (entry) {
        try {
          const probe = await fetch(entry.url + '/', {
            redirect: 'manual', signal: AbortSignal.timeout(4000) });
          alive = probe.status >= 200 && probe.status < 400;
        } catch (_) { alive = false; }
      }
      if (!alive)
        return new Response(offlinePage(parts[1]),
          { status: 200, headers: { 'Content-Type': 'text/html; charset=utf-8',
                                    'Cache-Control': 'no-store' } });
      return Response.redirect(entry.url, 302);
    }

    return new Response('Not found', { status: 404, headers: CORS });
  },
};
