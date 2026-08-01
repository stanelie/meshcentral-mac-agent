# MeshCentral server deployment

The server side is two sets of **files** placed into the MeshCentral install, plus **one patch
to MeshCentral's own viewer code** (see §3). All three must be **re-applied after a MeshCentral
`npm` update** (an update overwrites them).

Set `MC` to your MeshCentral install root (the directory that contains
`node_modules/meshcentral`), e.g.:

```bash
MC=/opt/meshcentral/node_modules/meshcentral   # adjust to your install
```

## 1. Served agent binaries (required for the Desktop tab)

MeshCentral withholds the agent **core** until the connecting agent's binary hash matches the
**served** binary. Replace the three macOS served binaries with the fixed builds so fresh
installs onboard with full capabilities (`caps:31`, not `caps:24`):

```bash
cd "$MC/agents"
# back up first
for f in meshagent_osx-arm-64 meshagent_osx-x86-64 meshagent_osx-universal-64; do
  cp -a "$f" "$f.bak-$(date +%Y%m%d-%H%M%S)"
done
# copy in the fixed builds (from this repo's prebuilt/, or your own build)
cp /path/to/prebuilt/meshagent_osx-arm-64      meshagent_osx-arm-64
cp /path/to/prebuilt/meshagent_osx-x86-64      meshagent_osx-x86-64
lipo -create meshagent_osx-arm-64 meshagent_osx-x86-64 -output meshagent_osx-universal-64  # if you don't have one
# re-hash the served agents
systemctl restart meshcentral      # or: sudo -u ... node meshcentral --restart, per your setup
```

Side effect: other Macs already reporting the old binary will self-update to the fixed build
on next connect (same lineage, only the login-input routing differs).

## 2. Hosted installer + binaries (the `curl | bash` endpoint)

Place the installer scripts and thin binaries in MeshCentral's built-in **`public/`** folder,
which is served **unauthenticated** at the web root by MeshCentral's existing static handler —
**no config, code, or routing change**:

```bash
DEST="$MC/public/meshsetup"
mkdir -p "$DEST"
cp installer/meshinstall.sh        "$DEST/"
cp installer/meshinstall.command   "$DEST/"
cp installer/uninstall.sh          "$DEST/"
# the double-click launcher, zipped so the +x bit survives download:
( cd installer && zip -X "$DEST/meshinstall.command.zip" meshinstall.command )
# the thin binaries the installer downloads by architecture:
cp prebuilt/meshagent_osx-arm-64   "$DEST/"
cp prebuilt/meshagent_osx-x86-64   "$DEST/"
chmod 644 "$DEST"/*
```

Resulting public URLs (`https://<host>/meshsetup/…`): `meshinstall.sh`,
`meshinstall.command`, `meshinstall.command.zip`, `uninstall.sh`, `meshagent_osx-arm-64`,
`meshagent_osx-x86-64`.

> ⚠️ **Do NOT use `domain.share` in `config.json`** to serve these. It mounts a static
> directory at the site root and **shadows MeshCentral's own routing**, taking the web UI
> down. The `public/` folder is the safe, side-effect-free location. After any server change,
> verify the web UI (root + login return HTTP 200) before walking away.

## 3. Viewer patch — Cmd+C / Cmd+V on macOS remotes (`viewer-cmdkey.patch`)

Without this, **no Cmd shortcut works** on a macOS remote: Cmd+C types a bare `c`, or nothing
at all. Two independent upstream MeshCentral bugs, both in
`public/scripts/agent-desktop-0.0.2.js`:

1. **`metaKey` missing from the modifier check** (broke *every* browser). `xxKeyUp`,
   `xxKeyDown` and `xxKeyPress` tested `(e.ctrlKey != true) && (e.altKey != true)` but not
   `metaKey`. With Cmd held, the letter was treated as ordinary text and routed to the unicode
   path — which depends on the browser's `keypress` event, and **macOS browsers do not fire
   `keypress` while Cmd is held**. The letter vanished; only the modifier arrived.

2. **Firefox reports `keyCode 224` for Command; Chrome reports `91`** (broke Firefox only).
   `obj.localKeyMap = true` is the default, so the `(obj.localKeyMap == false)` guard is always
   false and the `e.code` lookup table — which correctly maps `"MetaLeft": 91` — is **dead code
   for everyone**. Every key falls through to the legacy `event.keyCode` branch, where Firefox's
   224 was unmapped and silently dropped. Fixed by one more entry in the file's existing
   Firefox-fixup chain.

Because that `e.code` table is dead, **any other Firefox keyCode divergence will fail the same
way** — that fixup chain is where to add it.

```bash
cd "$MC/public/scripts"
cp -p agent-desktop-0.0.2.js "agent-desktop-0.0.2.js.bak-$(date +%Y%m%d-%H%M%S)"
patch -p0 < /path/to/server/viewer-cmdkey.patch
node --check agent-desktop-0.0.2.js && echo SYNTAX OK
```

Then hard-refresh the browser (Cmd+Shift+R) — it's a static script, so no restart is needed.
Verify in **both** Chrome and Firefox; they failed for different reasons and one working does
not imply the other does.

> Only applies when `minify` is disabled in `config.json` (`"_minify"` with the underscore
> prefix = disabled, which is the default). If minify is ever enabled, the server serves
> `agent-desktop-0.0.2-min.js` instead and this patch becomes inert — the min file would need
> the same treatment.

Both bugs are upstream MeshCentral (confirmed on 1.2.0) and worth reporting so they stop
needing re-application.

## Why not the console's "Add Agent" download?

MeshCentral's built-in macOS download is a **legacy `.mpkg`** (PackageMaker bundle). Modern
macOS rejects it outright: *"Legacy installer package … incompatible with this version of
macOS."* That is stock MeshCentral behaviour on a Linux server (it can't run
`pkgbuild`/`productbuild`), not something this project changed — which is the whole reason for
the hosted shell installer.

## Re-apply checklist after a MeshCentral npm update

- [ ] Re-copy the 3 fixed served binaries into `agents/` and restart MeshCentral.
- [ ] Re-copy the `meshsetup/` files into `public/`.
- [ ] Re-apply `viewer-cmdkey.patch` to `public/scripts/agent-desktop-0.0.2.js`, then test
      Cmd+C on a macOS remote in **both** Chrome and Firefox.
- [ ] `curl -fsSI https://<host>/ ` → 200, and a login → 200 (web UI healthy).
