#!/bin/zsh
# build-pkg.sh — the signed macOS installer for the Transmit device.
#
#   dist/QCBridgeAE-<version>-<arch>.pkg
#     └─ QCBridgeAE-core.pkg (component, non-relocatable, preinstall)
#          └─ /Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/QCBridgeAE/
#               └── QCBridgeAE-Transmit.bundle
#
# Input: build/QCBridgeAE-Transmit.bundle (cmake --build build). The bundle
# is re-signed here with the Developer ID Application identity, hardened
# runtime and a timestamp (notarization needs all three); the pkg with the
# Developer ID Installer identity. Same shape as the agent's and UFB's.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$REPO/packaging/macos"
BUNDLE="${QCBAE_BUNDLE:-$REPO/build/QCBridgeAE-Transmit.bundle}"
PKG_ID="ski.bialkow.qcbridgeae"
MEDIACORE="Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/QCBridgeAE"
[ -d "$BUNDLE" ] || { echo "ERROR: missing $BUNDLE — cmake --build build first" >&2; exit 1; }
VERSION="$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" "$BUNDLE/Contents/Info.plist")"
ARCH="$(uname -m)"
mkdir -p "$REPO/dist"
PKG="$REPO/dist/QCBridgeAE-$VERSION-$ARCH.pkg"

STAGE="$(mktemp -d -t qcbae-pkg)"
trap 'rm -rf "$STAGE"' EXIT
ROOT="$STAGE/root/$MEDIACORE"
mkdir -p "$ROOT" "$STAGE/pkgs" "$STAGE/scripts" "$STAGE/resources"
echo "[pkg] staging payload (v$VERSION, $ARCH)"
ditto "$BUNDLE" "$ROOT/QCBridgeAE-Transmit.bundle"

identity="$(security find-identity -v -p codesigning 2>/dev/null | grep -o '"Developer ID Application: [^"]*"' | head -1 | tr -d '"')"
[ -n "$identity" ] || { echo "ERROR: no Developer ID Application identity on this Mac" >&2; exit 1; }
codesign --force --sign "$identity" --options runtime --timestamp "$ROOT/QCBridgeAE-Transmit.bundle"
codesign --verify --strict "$ROOT/QCBridgeAE-Transmit.bundle"
echo "[pkg] bundle signed with: $identity"

cp "$SRC/scripts/preinstall" "$STAGE/scripts/preinstall"
chmod 755 "$STAGE/scripts/preinstall"
cp "$SRC/welcome.txt" "$STAGE/resources/welcome.txt"
pkgbuild --analyze --root "$STAGE/root" "$STAGE/components.plist" >/dev/null
i=0
while /usr/libexec/PlistBuddy -c "Print :$i" "$STAGE/components.plist" >/dev/null 2>&1; do
    /usr/libexec/PlistBuddy -c "Set :$i:BundleIsRelocatable false" "$STAGE/components.plist" 2>/dev/null \
        || /usr/libexec/PlistBuddy -c "Add :$i:BundleIsRelocatable bool false" "$STAGE/components.plist"
    i=$((i + 1))
done
echo "[pkg] $i bundle entries pinned non-relocatable"
pkgbuild --root "$STAGE/root" \
         --component-plist "$STAGE/components.plist" \
         --identifier "$PKG_ID" \
         --version "$VERSION" \
         --install-location / \
         --scripts "$STAGE/scripts" \
         "$STAGE/pkgs/QCBridgeAE-core.pkg" >/dev/null
sed "s/@VERSION@/$VERSION/g" "$SRC/distribution.xml" > "$STAGE/distribution.xml"
rm -f "$PKG"
installer_id="$(security find-identity -v 2>/dev/null | grep -o '"Developer ID Installer: [^"]*"' | head -1 | tr -d '"')"
if [ -n "$installer_id" ]; then
    productbuild --distribution "$STAGE/distribution.xml" --package-path "$STAGE/pkgs" \
                 --resources "$STAGE/resources" --sign "$installer_id" --timestamp "$PKG" >/dev/null
    pkgutil --check-signature "$PKG" | sed 's/^/  /' | head -3
else
    echo "WARN: no Developer ID Installer identity — UNSIGNED pkg (local testing only)" >&2
    productbuild --distribution "$STAGE/distribution.xml" --package-path "$STAGE/pkgs" \
                 --resources "$STAGE/resources" "$PKG" >/dev/null
fi
echo "[pkg] done: $PKG ($(du -sh "$PKG" | cut -f1))"
