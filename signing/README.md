# Code-signing key backup (encrypted)

`meshsign.p12.enc` is the **encrypted backup** of the private code-signing key used to sign the
macOS agent binaries. Signing with this key gives the binaries a TCC designated requirement
that is **independent of the code hash**:

```
identifier "meshagent_osx-arm-64" and certificate root = H"3d2edf1912f8cf1e3dd1cdd97e933ed353d4cb92"
```

so Screen Recording / Accessibility grants **survive agent rebuilds** (no re-approving on every
update). See the build/signing notes for how it's used.

## Is it safe to have this in a public repo?
Yes. `meshsign.p12.enc` is encrypted with **AES-256 (openssl, PBKDF2)** using a passphrase that
only you hold — it is never stored anywhere. Without that passphrase the file is useless. **The
passphrase is the only secret; guard it** (password manager). If you lose the passphrase you
lose this backup — but that's not catastrophic: you'd just generate a new cert and re-grant
Screen Recording once per machine on the next update.

## Recover the key on a new Mac
```bash
# 1) decrypt (enter your passphrase)
openssl enc -d -aes-256-cbc -pbkdf2 -iter 300000 \
  -in meshsign.p12.enc -out meshsign.p12

# 2) import into the login keychain (the .p12's own password is: mesh)
security import meshsign.p12 -k ~/Library/Keychains/login.keychain-db -P mesh -T /usr/bin/codesign

# 3) confirm it's usable
codesign -f -s "MeshAgent KVM Signing" --identifier meshagent_osx-arm-64 /path/to/binary
codesign -d -r- /path/to/binary   # designated => ...certificate root = H"3d2edf19..."
```
(The inner `.p12` password `mesh` is deliberately published here — it is useless without first
decrypting the outer AES layer with your passphrase, so it does not weaken anything.)

## Rotate later
For production hardening, replace this self-signed cert with an **Apple Developer ID** identity
(persistent grants + Gatekeeper-approved + notarizable). Re-export/replace `meshsign.p12` and
re-sign; existing grants would need a one-time re-approval on the switch.
