# ListenLink

A free DIY stand-in for Audiomovers ListenTo: an audio plugin you drop on
Ableton's master bus that streams whatever passes through it to a web link
anyone can open in a browser.

## How it works

- The plugin passes audio through untouched and copies it into a lock-free FIFO.
- A tiny web server embedded in the plugin (ports 17654–17663) serves a
  listener page at `/` and streams 16-bit stereo PCM over a WebSocket at `/ws`.
- The listener page plays the stream with an AudioWorklet (~150 ms jitter
  buffer, so total latency is typically well under half a second plus network).
- "Create public link" shells out to `cloudflared` (a free Cloudflare quick
  tunnel) and scrapes the `https://….trycloudflare.com` URL — that's the link
  you send to your client. No account needed.

A **Stream Quality** selector in the plugin picks the format (saved with your
Ableton project):

| Setting | Bandwidth per listener | Notes |
|---|---|---|
| Lossless PCM (default) | ~1.6 Mbit/s | Uncompressed 16-bit; needs a solid upload connection |
| Opus 256 kbps | ~0.26 Mbit/s | Perceptually transparent; the sweet spot for remote listeners |
| Opus 128 kbps | ~0.13 Mbit/s | For many listeners or weak connections |

Opus is encoded in the plugin (libopus, statically linked) in 20 ms frames at
48 kHz — projects at other rates are resampled internally. Browsers decode via
the native WebCodecs `AudioDecoder`; a browser without it (pre-16.4 Safari)
automatically reconnects with `?fmt=pcm` and receives raw PCM instead. You can
switch quality mid-stream; listeners re-sync in under a second.

## Using it

1. In Ableton: add **ListenLink** (Audio Units or VST3 → DIY) to the master bus.
2. Open the plugin UI:
   - Stereo level meters show the signal passing through, and the status line
     shows how many people are connected.
   - **Share on your network** — a `http://<your-lan-ip>:17654` link for
     anyone on the same Wi-Fi.
   - **Create public link** — after a few seconds a
     `https://xxx.trycloudflare.com` link appears. Copy and send it.
3. The listener opens the link and presses **Listen**. The page shows its own
   level meters plus a "N listening" count when more than one person is on.

Notes:

- Quick-tunnel URLs are temporary: a new random URL is generated each time you
  press "Create public link" (and it dies when you stop it / close the project).
- macOS may ask to allow the plugin (i.e. Ableton) to accept incoming network
  connections — allow it.
- The first time you play back, listeners hear ~0.2 s behind you. If a
  listener's connection hiccups, their buffer rebuilds automatically.

## Building

Requires Xcode command-line tools, CMake ≥ 3.22, and internet (JUCE is
fetched automatically). `cloudflared` is optional but needed for public links
(`brew install cloudflared`).

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```

The AU (`~/Library/Audio/Plug-Ins/Components/ListenLink.component`) and VST3
(`~/Library/Audio/Plug-Ins/VST3/ListenLink.vst3`) are installed automatically
after the build. Restart Ableton (or rescan plugins) to pick them up.

A Standalone app is also built (`build/ListenLink_artefacts/Release/Standalone/`)
which is handy for testing the server without a DAW.

## Distributing to others

Every build is automatically signed with the Developer ID certificate in the
Keychain (set `-DLL_CODESIGN_ID=""` at configure time to build unsigned).
To make a zip that installs cleanly on other Macs, notarize it:

1. One-time setup — create an app-specific password at
   [account.apple.com](https://account.apple.com) (Sign-In & Security →
   App-Specific Passwords), then store it:

   ```sh
   xcrun notarytool store-credentials listenlink-notary \
     --apple-id <your-apple-id-email> --team-id K7VM2MP885 --password <app-specific-password>
   ```

2. After that, any time you want a shareable build:

   ```sh
   ./notarize.sh
   ```

   This notarizes and staples both plugins and produces
   `dist/ListenLink-mac.zip` with install instructions inside. Recipients just
   copy the two bundles into their plugin folders — no Gatekeeper warnings.

Recipients need `cloudflared` (free) for public links; LAN streaming works
without it. Note: JUCE's free license is AGPLv3, so share the source alongside
binaries (this folder, or a public repo).
