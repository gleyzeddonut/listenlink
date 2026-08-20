#!/bin/bash
# Build a signed + notarized .pkg installer that puts the AU and VST3 into the
# system plugin folders (/Library/Audio/Plug-Ins). Run ./notarize.sh first so
# the installed bundles are signed and stapled; this script packages those.
set -euo pipefail
cd "$(dirname "$0")"

VER=$(sed -n 's/^project(ListenLink VERSION \(.*\))$/\1/p' CMakeLists.txt)
AU="$HOME/Library/Audio/Plug-Ins/Components/ListenLink.component"
VST3="$HOME/Library/Audio/Plug-Ins/VST3/ListenLink.vst3"
SIGN_ID="Developer ID Installer: Daniel Gleyzer (K7VM2MP885)"
PROFILE="listenlink-notary"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/au-root" "$WORK/vst3-root" dist

cp -R "$AU" "$WORK/au-root/"
cp -R "$VST3" "$WORK/vst3-root/"

echo "==> Building component packages (v$VER)..."
pkgbuild --root "$WORK/au-root" \
         --install-location "/Library/Audio/Plug-Ins/Components" \
         --identifier com.diy.listenlink.au --version "$VER" \
         "$WORK/au.pkg" > /dev/null
pkgbuild --root "$WORK/vst3-root" \
         --install-location "/Library/Audio/Plug-Ins/VST3" \
         --identifier com.diy.listenlink.vst3 --version "$VER" \
         "$WORK/vst3.pkg" > /dev/null

cat > "$WORK/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="1">
    <title>ListenLink $VER</title>
    <welcome mime-type="text/plain"><![CDATA[This installs the ListenLink audio plugin (AU + VST3).

Add it to your DAW's master bus, press play, and share the link it generates - anyone can listen live in their browser.]]></welcome>
    <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_localSystem="true"/>
    <choices-outline>
        <line choice="default"><line choice="au"/><line choice="vst3"/></line>
    </choices-outline>
    <choice id="default"/>
    <choice id="au" visible="false"><pkg-ref id="com.diy.listenlink.au"/></choice>
    <choice id="vst3" visible="false"><pkg-ref id="com.diy.listenlink.vst3"/></choice>
    <pkg-ref id="com.diy.listenlink.au" version="$VER">au.pkg</pkg-ref>
    <pkg-ref id="com.diy.listenlink.vst3" version="$VER">vst3.pkg</pkg-ref>
</installer-gui-script>
EOF

echo "==> Building signed product archive..."
productbuild --distribution "$WORK/distribution.xml" --package-path "$WORK" \
             --sign "$SIGN_ID" "dist/ListenLink-$VER.pkg"

echo "==> Notarizing (usually 1-5 minutes)..."
xcrun notarytool submit "dist/ListenLink-$VER.pkg" --keychain-profile "$PROFILE" --wait
xcrun stapler staple "dist/ListenLink-$VER.pkg"

echo "==> Verifying..."
spctl -a -t install -v "dist/ListenLink-$VER.pkg"
pkgutil --check-signature "dist/ListenLink-$VER.pkg" | head -3

# Stable-named copy: uploaded to each release so
# releases/latest/download/ListenLink.pkg always serves the newest installer
# (the site's download button and the in-plugin update fallback rely on it).
cp "dist/ListenLink-$VER.pkg" dist/ListenLink.pkg

echo
echo "Done. Installer: dist/ListenLink-$VER.pkg (+ stable copy dist/ListenLink.pkg)"
echo "Release: gh release create v$VER dist/ListenLink-$VER.pkg dist/ListenLink-$VER-mac.zip dist/ListenLink.pkg"
