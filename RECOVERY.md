# Disaster recovery

How to reconstruct the working macOS agent from **just this repository plus your
certificate passphrase**, if you lose your build machine.

## What you need
1. **This repository.**
2. The **passphrase** you used to encrypt `signing/meshsign.p12.enc` — only you have it
   (it is stored nowhere).
3. A **Mac with Xcode command-line tools** — *only if you want to rebuild from source.*
   Otherwise the binaries in `prebuilt/` are ready to use.
4. Your **MeshCentral server's group values** (`MESH_ID`, `SERVER_ID`, host). These are
   deliberately **not** in this repo (they identify your server). Read them from your
   MeshCentral **console** (device group → *Add Agent* → the `.msh` file) or your server.
   Only needed to fill in the installer.

> None of the above lives on the build machine, so losing the build machine loses none of it.

## 1. Recover the signing identity
```bash
openssl enc -d -aes-256-cbc -pbkdf2 -iter 300000 \
  -in signing/meshsign.p12.enc -out meshsign.p12          # enter YOUR passphrase
security import meshsign.p12 -k ~/Library/Keychains/login.keychain-db -P mesh -T /usr/bin/codesign
```
`mesh` is the inner p12 password (published on purpose — useless without your outer
passphrase). Details in `signing/README.md`. If the passphrase is lost, generate a new
self-signed cert instead and re-grant Screen Recording once per machine.

## 2. Get the agent binaries
**Option A — use the prebuilt binaries (no build).** `prebuilt/meshagent_osx-arm-64` and
`-x86-64` are the current build. Re-sign them with the recovered cert so TCC grants persist:
```bash
codesign -f -s "MeshAgent KVM Signing" --identifier meshagent_osx-arm-64 prebuilt/meshagent_osx-arm-64
codesign -f -s "MeshAgent KVM Signing" --identifier meshagent_osx-x86-64 prebuilt/meshagent_osx-x86-64
```
(Or leave them ad-hoc — grants then reset on each agent update; see `signing/README.md`.)

**Option B — rebuild from source.** The patch targets a specific upstream commit:
```bash
git clone https://github.com/Ylianst/MeshAgent
cd MeshAgent
git checkout cb62daa82b6f23dd317eac77a16a398db03f43ea    # upstream base this patch was made against
git apply /path/to/this-repo/agent/login-kvm.patch       # or drop in agent/mac_events.c + agent/mac_kvm.c
```
Then build + sign per `agent/README.md`.

## 3. Rebuild the deployment
1. Fill the installer templates (`installer/meshinstall.sh`, `installer/meshinstall.command`,
   `installer/uninstall.sh`) with your server's values — see the README section
   *"Configuring for your own server and group."*
2. Put the served binaries + hosted installer on the MeshCentral server — see `server/README.md`
   (re-apply after any MeshCentral `npm` update).
3. Per machine, install with the three steps in the README *"TL;DR"* (enable Screen Sharing →
   run installer → grant Screen Recording).

## Recovery inventory
| Piece | Where it is |
|---|---|
| Agent source + patch + build steps | this repo (`agent/`) |
| Prebuilt binaries | this repo (`prebuilt/`) |
| Signing key (encrypted) | this repo (`signing/meshsign.p12.enc`) |
| Cert passphrase | **you** (memorized / password manager) |
| Server group IDs (`MESH_ID`/`SERVER_ID`/host) | your MeshCentral server + console |
| Filled-in installer (real values) | your MeshCentral server's `public/meshsetup/` |
