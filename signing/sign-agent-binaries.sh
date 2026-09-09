#!/bin/bash
# Sign the macOS agent binaries with the stable "MeshAgent KVM Signing" self-signed
# cert, so TCC grants (Screen Recording / Accessibility) survive agent rebuilds.
#
# The signed binary's designated requirement becomes:
#   identifier "<id>" and certificate root = H"3d2edf19...ed353d4cb92"
# which is INDEPENDENT of the code hash — so a grant given for one build keeps
# working for later builds signed with this same cert.
#
# Prereq: the signing identity must be in the login keychain. If missing:
#   security import signing/meshsign.p12 -k ~/Library/Keychains/login.keychain-db -P mesh -T /usr/bin/codesign
#
# Run from the meshagent-src dir (where the freshly-built binaries are).
set -e
CERT="MeshAgent KVM Signing"
security find-identity -p codesigning 2>/dev/null | grep -q "$CERT" || {
  echo "ERROR: signing identity '$CERT' not in keychain — import signing/meshsign.p12 first"; exit 1; }

for id in meshagent_osx-arm-64 meshagent_osx-x86-64 meshagent_osx-universal-64; do
  [ -f "$id" ] || { echo "skip $id (not built)"; continue; }
  codesign -f -s "$CERT" --identifier "$id" "$id"
  echo "signed $id"
  codesign -d -r- "$id" 2>&1 | grep 'designated =>' | sed 's/^/   /'
done
echo "Done. (ad-hoc equivalent was: codesign -f -s - --identifier <id> <binary>)"
