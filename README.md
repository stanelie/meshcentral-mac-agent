# MeshCentral macOS agent — login-window KVM

A macOS [MeshCentral](https://meshcentral.com/) agent build and deployment kit that adds
**full remote desktop — video *and* keyboard *and* mouse — at the macOS login window**,
on Apple Silicon and Intel. Verified on **macOS 11 Big Sur through macOS 26 Tahoe**
(video + keyboard + mouse, in-session and at the login screen).

Stock MeshCentral can show the login screen but **cannot type or click on it**: the
login window holds *SecureEventInput*, so the WindowServer drops every event injected by
`CGEventPost` / `IOHIDPostEvent`. This fork routes login-window input through Apple's own
`screensharingd`, which is the only process allowed to inject events there — giving real
control of the machine before anyone logs in.

It also ships a friction-free installer (a `curl | bash` one-liner and a double-click
`.command`), because MeshCentral's built-in macOS download is a **legacy `.mpkg` that
modern macOS refuses to install**.

> ### ⚠️ Requirement: FileVault must be **disabled**
> "Login window" here means the normal macOS login window, which appears **after** macOS has
> booted and the agent is running. With **FileVault** enabled, a reboot instead stops at the
> **pre-boot disk-unlock screen** — that runs in the EFI/recoveryOS environment *before*
> macOS, launchd, `screensharingd`, or the MeshAgent exist, so there is nothing to capture or
> inject and no way to unlock the disk remotely. (FileVault also passes that unlock straight
> through to the user session, so the normal login window is usually skipped entirely.)
> **For remote access at/through the login window to work after a reboot, disable FileVault**
> (`sudo fdesetup disable`). This is a hard macOS limitation, not something the agent can work
> around. See [docs/architecture.md](docs/architecture.md#limitations).

---

## TL;DR — install on a Mac

> This assumes an admin has already configured and hosted the installer for their own
> MeshCentral server and device group — see
> [Configuring for your own server and group](#configuring-for-your-own-server-and-group).
> Replace `<your-host>` below with that server.

1. **System Settings → General → Sharing → turn on _Screen Sharing_.**
   (One-time per machine. This is what makes login-window keyboard/mouse work — see
   [Architecture](docs/architecture.md).)
2. Run the installer:
   ```bash
   curl -fsSL https://<your-host>/install.sh | sudo bash
   ```
   …or download and double-click `meshinstall.command`, which walks through all three steps.
3. **System Settings → Privacy & Security → Screen Recording → enable `meshagent`**
   (for the remote video). Enable it under **Accessibility** too if you want in-session
   mouse/keyboard as well as login-window control.

The Mac appears in your MeshCentral console under the configured landing group; move it to
its real group from there.

To remove everything cleanly:
```bash
curl -fsSL https://<your-host>/uninstall.sh | sudo bash
```

---

## How it works (short version)

| Concern | Logged-in session | **Login window (uid 0)** |
|---|---|---|
| **Video** | ScreenCaptureKit in the kvmagent (Aqua) | ScreenCaptureKit in the kvmagent (uid 0) |
| **Input** | `CGEventPost` (HID tap) | **VNC → `screensharingd` on `localhost:5900`** |

The agent runs a **dual-session LaunchAgent** (`LimitLoadToSessionType = [Aqua,
LoginWindow]`) so a `kvmagent` exists both in the user's session (uid 501) and at the
login window (uid 0). At the login window the agent detects `is_loginwindow()` and, instead
of `CGEventPost`, connects to the local `screensharingd` as a small RFB (legacy VNC, type-2
DES) client and lets **screensharingd** perform the injection — it holds
`com.apple.private.hid.client.event-dispatch`, the entitlement that bypasses
SecureEventInput.

**The non-obvious requirement:** screensharingd only injects if Apple's helper
`com.apple.screensharing.agent` has the **ScreenCapture + PostEvent** TCC grants. Those are
created by enabling **Screen Sharing in System Settings** — *not* by `kickstart`/`launchctl`.
Without them, screensharingd authenticates the connection but logs `ignore key event` and
drops all input. This single fact is why step 1 above is mandatory. Full detail in
[docs/architecture.md](docs/architecture.md).

---

## Repository layout

```
agent/            The agent source change that enables login-window input
  mac_events.c        Full modified file (KVM keyboard/mouse for macOS)
  login-kvm.patch     Exact diff vs upstream Ylianst/MeshAgent (for review / upstream PR)
  kvm_input_harness.c Standalone tool to validate injection into kvm-<uid>.sock
installer/        Client-side install/uninstall
  meshinstall.sh      Hosted installer (arch-detect, agent + login-KVM setup)
  meshinstall.command Double-click launcher that guides the 3 manual steps
  uninstall.sh        Full clean removal
  mac_kvm.c           Full modified file (KVM capture / relay for macOS)
  README.md           How to build the binaries (pins the upstream base commit)
prebuilt/         Thin binaries (arm64 + x86_64), ad-hoc signed
signing/          Encrypted signing-key backup + how persistent TCC grants work
docs/
  architecture.md     The full technical story: SecureEventInput, screensharingd, TCC
  macos-findings.md   Hard-won gotchas, dead ends, and the unresolved handoff issue
  LOGIN_KVM_FIX.md    Original concise fix note (agent code change)
server/
  README.md           MeshCentral server-side deployment + re-apply-after-update
RECOVERY.md       Rebuild everything from this repo + your cert passphrase alone
```

If you ever lose your build machine, **[RECOVERY.md](RECOVERY.md)** is the self-contained
recipe to reconstruct the working agent from this repository plus your certificate passphrase.

---

## Configuring for your own server and group

The installer scripts in this repo are **templates** — they contain `__PLACEHOLDER__`
values and will not run until you fill them in for your own MeshCentral server and device
group. There is nothing tying this to any particular organization; you point it at your
server and your group.

### Step 1 — get your server + group values

Every MeshCentral agent is bound to a group by four values that live in a small **`.msh`**
(mesh settings) text file. The easiest way to read yours:

1. In the MeshCentral web console, open (or create) the **device group** you want Macs to
   land in.
2. Click **"Add Agent"**. In the dialog, download the agent/installer for that group (any
   macOS option is fine — you only need the settings, not the package itself).
3. Open the downloaded package/zip and find the **`.msh`** file inside (or, if the console
   offers a direct "mesh settings"/`.msh` link, use that). It looks like:
   ```
   MeshName=Servers
   MeshID=0x1A2B3C…(96 hex chars)…F0
   ServerID=9D8C7B…(96 hex chars)…04
   MeshServer=wss://mc.example.com:443/agent.ashx
   ```
   - `MeshName` / `MeshID` are **per group** (different for each group).
   - `ServerID` / `MeshServer` are **per server** (the same for every group on that server).

> On the server itself you can also read a group's `MeshID` from the MeshCentral database,
> but the console `.msh` above is the version-independent way that needs no server access.

### Step 2 — fill in the installer

Edit the five variables at the top of
[`installer/meshinstall.sh`](installer/meshinstall.sh) with those values, and replace every
`__HOST__` in [`installer/meshinstall.command`](installer/meshinstall.command) and
[`installer/uninstall.sh`](installer/uninstall.sh) with your MeshCentral host. `BASE_URL` is
wherever you host the installer + binaries (see below) — usually the same host.

### Step 3 — put files on the server

Two things live on the MeshCentral server (see [server/README.md](server/README.md) for exact
commands):

1. **Served agent binaries** in `node_modules/meshcentral/agents/` must be the fixed builds
   (`meshagent_osx-arm-64`, `-x86-64`, `-universal-64`). MeshCentral withholds the agent
   *core* (no Desktop tab) until the connecting agent's hash matches the served binary.
2. **Your configured installer + the binaries** hosted under
   `node_modules/meshcentral/public/` (served unauthenticated at the web root). The two
   scripts sit at the top so the typed URL stays short — `install.sh` and `uninstall.sh` —
   with the binaries and the double-click launcher under `meshsetup/`.

Then your install command is `curl -fsSL https://<your-host>/install.sh | sudo bash`, and the
double-click `meshinstall.command.zip` is downloadable from `/meshsetup/`.

## Building the agent binaries

See [agent/README.md](agent/README.md). In short: apply `agent/login-kvm.patch` to an
`Ylianst/MeshAgent` checkout (or drop in `agent/mac_events.c`), then
`make macos ARCHID=29` (arm64) / `ARCHID=16` (x86_64) and ad-hoc codesign with the
matching `--identifier`. Prebuilt signed binaries are in [`prebuilt/`](prebuilt/).

---

## Coexisting with normal macOS Screen Sharing

`screensharingd` serves **both** Apple Screen Sharing (RFB security type 30, Apple DH) and
the legacy VNC (type 2) this agent uses for login-window input — on the **same port 5900,
from the same daemon**. Verified against a stock Mac:

| Client negotiates | screensharingd offers |
|---|---|
| RFB 3.3 (what this agent uses) | `2` — legacy VNC (DES) |
| RFB 3.8 (Screen Sharing.app) | `30, 33, 36, 2, 35` — `30` = Apple DH |

So the two **cannot be separated by port**, only by source address. The installer's
`SS_LAN_ACCESS` setting controls that:

| `SS_LAN_ACCESS` | `:5900` reachable from | Screen Sharing from another Mac |
|---|---|---|
| `lan` *(default)* | loopback + private/link-local ranges | **works on the LAN**, refused from public networks |
| `allow` | anywhere the network allows (no `pf` rules) | **works everywhere** |
| `block` | loopback only | **does not work** |

```bash
SS_LAN_ACCESS=allow sudo -E bash meshinstall.sh
```

> Earlier versions of this installer hardcoded the `block` behaviour, which is why a Mac
> with the agent installed could no longer be reached by normal Screen Sharing.

## Security notes

- Each machine gets a **unique random VNC password** (`kvm/vnc.pw`, root-only, 8 chars —
  the maximum a VNC DES key can carry), not a shared secret.
- With `SS_LAN_ACCESS=lan` or `allow`, that legacy-VNC password becomes an **additional
  authentication surface on the LAN** alongside Apple's own auth. It is an online-only
  brute force against ~2.2e14 combinations (the agent authenticates over loopback, so no
  challenge/response is ever observable on the wire), but it is a real trade for the
  convenience of native Screen Sharing. Use `block` if you do not need it.
- The agent's own directory `kvm/` is `root:wheel 0755`: it holds a binary launchd
  executes **as root** at the login window, so it must not be group-writable.
- Login-window input flows over loopback to Apple's screensharingd; the operator↔agent
  link is MeshCentral's normal end-to-end tunnel.
- `uninstall.sh` restores the stock `pf` ruleset and turns legacy VNC back off if it was
  off before install.

## Credits & license

The agent is a fork of [Ylianst/MeshAgent](https://github.com/Ylianst/MeshAgent)
(Apache-2.0). The macOS login-window input change (`agent/mac_events.c`,
`agent/login-kvm.patch`) is offered back upstream; see `docs/LOGIN_KVM_FIX.md`. All agent
code retains its upstream Apache-2.0 license (see [LICENSE](LICENSE)). The installer,
server, and documentation in this repository are provided under the same terms.
