# MeshCentral macOS agent — login-window KVM

A macOS [MeshCentral](https://meshcentral.com/) agent build and deployment kit that adds
**full remote desktop — video *and* keyboard *and* mouse — at the macOS login window**,
including on Apple Silicon and Intel, on modern macOS (verified on **Tahoe 26.5.1**).

Stock MeshCentral can show the login screen but **cannot type or click on it**: the
login window holds *SecureEventInput*, so the WindowServer drops every event injected by
`CGEventPost` / `IOHIDPostEvent`. This fork routes login-window input through Apple's own
`screensharingd`, which is the only process allowed to inject events there — giving real
control of the machine before anyone logs in.

It also ships a friction-free installer (a `curl | bash` one-liner and a double-click
`.command`), because MeshCentral's built-in macOS download is a **legacy `.mpkg` that
modern macOS refuses to install**.

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
   curl -fsSL https://<your-host>/meshsetup/meshinstall.sh | sudo bash
   ```
   …or download and double-click `meshinstall.command`, which walks through all three steps.
3. **System Settings → Privacy & Security → Screen Recording → enable `meshagent`**
   (for the remote video). Enable it under **Accessibility** too if you want in-session
   mouse/keyboard as well as login-window control.

The Mac appears in your MeshCentral console under the configured landing group; move it to
its real group from there.

To remove everything cleanly:
```bash
curl -fsSL https://<your-host>/meshsetup/uninstall.sh | sudo bash
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
prebuilt/         Signed thin binaries (arm64 + x86_64), same as served by the server
docs/
  architecture.md     The full technical story: SecureEventInput, screensharingd, TCC
  LOGIN_KVM_FIX.md    Original concise fix note (agent code change)
server/
  README.md           MeshCentral server-side deployment + re-apply-after-update
```

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
   `node_modules/meshcentral/public/meshsetup/` (served unauthenticated at
   `https://<your-host>/meshsetup/…`).

Then your install command is `curl -fsSL https://<your-host>/meshsetup/meshinstall.sh | sudo
bash`, and the double-click `meshinstall.command.zip` is downloadable from the same folder.

## Building the agent binaries

See [agent/README.md](agent/README.md). In short: apply `agent/login-kvm.patch` to an
`Ylianst/MeshAgent` checkout (or drop in `agent/mac_events.c`), then
`make macos ARCHID=29` (arm64) / `ARCHID=16` (x86_64) and ad-hoc codesign with the
matching `--identifier`. Prebuilt signed binaries are in [`prebuilt/`](prebuilt/).

---

## Security notes

- `screensharingd`'s `:5900` is restricted to **loopback only** via a `pf` anchor the
  installer ships — the VNC password is never reachable from the network; only the local
  kvmagent uses it.
- Each machine gets a **unique random VNC password** (`kvm/vnc.pw`, root-only), not a
  shared secret.
- Login-window input flows over loopback to Apple's screensharingd; the operator↔agent
  link is MeshCentral's normal end-to-end tunnel.

## Credits & license

The agent is a fork of [Ylianst/MeshAgent](https://github.com/Ylianst/MeshAgent)
(Apache-2.0). The macOS login-window input change (`agent/mac_events.c`,
`agent/login-kvm.patch`) is offered back upstream; see `docs/LOGIN_KVM_FIX.md`. All agent
code retains its upstream Apache-2.0 license (see [LICENSE](LICENSE)). The installer,
server, and documentation in this repository are provided under the same terms.
