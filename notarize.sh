#!/bin/bash
# Notarize and staple the installed ListenLink plugins, then produce a
# ready-to-share zip in dist/. One-time setup first (see README):
#   xcrun notarytool store-credentials listenlink-notary \
#     --apple-id <your-apple-id-email> --team-id K7VM2MP885 --password <app-specific-password>
set -euo pipefail
cd "$(dirname "$0")"

AU="$HOME/Library/Audio/Plug-Ins/Components/ListenLink.component"
VST3="$HOME/Library/Audio/Plug-Ins/VST3/ListenLink.vst3"
PROFILE="listenlink-notary"

mkdir -p dist
rm -f dist/ListenLink-notarize.zip dist/ListenLink-mac.zip

echo "==> Zipping for notarization..."
ditto -c -k --keepParent "$AU" dist/au.zip
ditto -c -k --keepParent "$VST3" dist/vst3.zip

echo "==> Submitting to Apple (usually 1-5 minutes each)..."
xcrun notarytool submit dist/au.zip   --keychain-profile "$PROFILE" --wait
xcrun notarytool submit dist/vst3.zip --keychain-profile "$PROFILE" --wait
rm -f dist/au.zip dist/vst3.zip

echo "==> Stapling tickets..."
xcrun stapler staple "$AU"
xcrun stapler staple "$VST3"

echo "==> Verifying..."
spctl -a -t open --context context:primary-signature -v "$AU"   || true
codesign --verify --strict "$AU"   && echo "AU OK"
codesign --verify --strict "$VST3" && echo "VST3 OK"

echo "==> Building distribution zip..."
ditto -c -k --keepParent "$AU" dist/_au.zip
TMP=$(mktemp -d)
cp -R "$AU" "$VST3" "$TMP/"
cat > "$TMP/INSTALL.txt" <<'EOF'
ListenLink — install
====================
1. Copy ListenLink.component to ~/Library/Audio/Plug-Ins/Components/
2. Copy ListenLink.vst3      to ~/Library/Audio/Plug-Ins/VST3/
3. Restart your DAW (or rescan plugins) and add ListenLink to the master bus.

Optional, for public links: install cloudflared (free):  brew install cloudflared
EOF
rm -f dist/_au.zip
ditto -c -k "$TMP" dist/ListenLink-mac.zip
rm -rf "$TMP"

echo
echo "Done. Shareable build: dist/ListenLink-mac.zip"
