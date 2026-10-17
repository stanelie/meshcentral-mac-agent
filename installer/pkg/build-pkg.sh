#!/bin/bash
# Build (and optionally sign + notarize) the MeshAgent installer package.
#
#   ./build-pkg.sh mc.example.com                 -> unsigned
#   ./build-pkg.sh mc.example.com "Developer ID Installer: Name (TEAMID)" meshnotary
#
# Signing is usually NOT done here. Use sign-pkg.sh on the machine that holds the
# Developer ID private key -- it takes the finished package and needs neither this
# repo nor a rebuild, so the key never has to be copied to a build machine. The
# signing arguments below exist for the case where both happen on one Mac.
#
# The package has NO payload: its postinstall fetches https://<host>/install.sh
# and runs it. That keeps one source of truth for the install, and it keeps the
# AGENT binaries out of the package -- they stay on the self-signed cert that the
# TCC grants are pinned to. Signing them with a Developer ID instead would
# invalidate Screen Recording and Accessibility on every Mac in the fleet.
set -euo pipefail

HOST="${1:-}"
SIGN_ID="${2:-}"
NOTARY_PROFILE="${3:-}"
[ -n "$HOST" ] || { echo "usage: $0 <mesh-host> [\"Developer ID Installer: ...\"] [notary-profile]"; exit 1; }

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/MeshAgent-Installer.pkg"
VERSION="$(date +%Y.%m.%d)"
WORK="$(mktemp -d /tmp/meshpkg.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/scripts"
sed "s|__HOST__|$HOST|g" "$HERE/scripts/postinstall" > "$WORK/scripts/postinstall"
chmod 755 "$WORK/scripts/postinstall"
grep -q '__HOST__' "$WORK/scripts/postinstall" && { echo "ERROR: __HOST__ not substituted"; exit 1; }

# --nopayload: scripts only. installs nothing itself.
pkgbuild --identifier ca.exmachina.meshagent.installer \
         --version "$VERSION" \
         --nopayload \
         --scripts "$WORK/scripts" \
         "$WORK/component.pkg"

# A distribution package so the installer window can carry a title and refuse
# unsupported systems.
cat > "$WORK/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>MeshAgent</title>
    <options customize="never" require-scripts="true" hostArchitectures="arm64,x86_64"/>
    <volume-check>
        <allowed-os-versions><os-version min="11.0"/></allowed-os-versions>
    </volume-check>
    <choices-outline><line choice="default"/></choices-outline>
    <choice id="default" title="MeshAgent"><pkg-ref id="ca.exmachina.meshagent.installer"/></choice>
    <pkg-ref id="ca.exmachina.meshagent.installer">component.pkg</pkg-ref>
</installer-gui-script>
XML

productbuild --distribution "$WORK/distribution.xml" --package-path "$WORK" "$WORK/unsigned.pkg"

if [ -z "$SIGN_ID" ]; then
    cp "$WORK/unsigned.pkg" "$OUT"
    echo "built UNSIGNED: $OUT"
    echo "  (unsigned packages are blocked by Gatekeeper on download -- for local testing only)"
    exit 0
fi

productsign --sign "$SIGN_ID" "$WORK/unsigned.pkg" "$OUT"
echo "signed: $OUT"
pkgutil --check-signature "$OUT" | sed 's/^/  /'

if [ -n "$NOTARY_PROFILE" ]; then
    echo "submitting for notarization (this waits for Apple)..."
    xcrun notarytool submit "$OUT" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$OUT"
    echo "--- stapled; Gatekeeper assessment ---"
    spctl --assess -vv --type install "$OUT" 2>&1 | sed 's/^/  /'
fi
