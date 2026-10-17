#!/bin/bash
# Sign and notarize an already-built MeshAgent-Installer.pkg.
#
# Deliberately separate from build-pkg.sh so it can run on a DIFFERENT machine:
# the Developer ID private key should live on the machine that owns it and not be
# copied around. Building needs no identity; signing needs nothing but the pkg.
#
#   ./sign-pkg.sh MeshAgent-Installer.pkg "Developer ID Installer: Name (TEAMID)" meshnotary
#
# Leave the notary profile off to sign without notarizing (useful to check the
# certificate works before involving Apple):
#   ./sign-pkg.sh MeshAgent-Installer.pkg "Developer ID Installer: Name (TEAMID)"
set -euo pipefail

PKG="${1:-}"
SIGN_ID="${2:-}"
NOTARY_PROFILE="${3:-}"

if [ -z "$PKG" ] || [ -z "$SIGN_ID" ]; then
    echo "usage: $0 <unsigned.pkg> \"Developer ID Installer: Name (TEAMID)\" [notary-profile]"
    echo
    echo "Available installer identities on this Mac:"
    security find-identity -v 2>/dev/null | grep "Developer ID Installer" || echo "  (none found)"
    exit 1
fi
[ -f "$PKG" ] || { echo "ERROR: no such file: $PKG"; exit 1; }

# Fail early with a useful message rather than a codesign error nobody can read.
if ! security find-identity -v 2>/dev/null | grep -qF "$SIGN_ID"; then
    echo "ERROR: identity not found or not valid on this Mac:"
    echo "    $SIGN_ID"
    echo "Installer identities available:"
    security find-identity -v 2>/dev/null | grep "Developer ID Installer" || echo "  (none)"
    echo
    echo "An identity that exists but is not listed as valid usually means Apple's"
    echo "intermediate certificate is missing. Install the 'Developer ID' intermediate"
    echo "from https://www.apple.com/certificateauthority/ and try again."
    exit 1
fi

OUT="${PKG%.pkg}-signed.pkg"
echo "=== signing ==="
productsign --sign "$SIGN_ID" "$PKG" "$OUT"
pkgutil --check-signature "$OUT" | sed 's/^/  /'

if [ -z "$NOTARY_PROFILE" ]; then
    echo
    echo "Signed but NOT notarized: $OUT"
    echo "Gatekeeper still blocks a downloaded copy until it is notarized."
    exit 0
fi

echo "=== notarizing (uploads to Apple and waits; usually a minute or two) ==="
xcrun notarytool submit "$OUT" --keychain-profile "$NOTARY_PROFILE" --wait

echo "=== stapling the ticket into the package ==="
xcrun stapler staple "$OUT"

echo "=== Gatekeeper verdict (this is what a downloading Mac will decide) ==="
spctl --assess -vv --type install "$OUT" 2>&1 | sed 's/^/  /'
echo
echo "Done: $OUT"
echo "This one is safe to host for download."
