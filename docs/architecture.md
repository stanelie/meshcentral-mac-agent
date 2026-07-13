# Architecture — login-window KVM on macOS

This document explains how remote keyboard/mouse works **at the macOS login window**, why
the obvious approaches fail, and the exact runtime state required. It reflects behaviour
verified on macOS **Tahoe 26.5.1** (Apple Silicon + Intel).

## The problem: SecureEventInput

At the login window, `loginwindow` holds `kCGSSessionSecureInputPID` (SecureEventInput).
The WindowServer then delivers keyboard events **only** from physical hardware or
kernel-level virtual HID devices. Every userspace injection API is dropped:

| Method | Why it fails at the login window |
|---|---|
| `CGEventPost(kCGHIDEventTap, …)` | SecureEventInput — WindowServer discards it |
| `IOHIDPostEvent` | deprecated, same blocking |
| `IOHIDUserDevice` (virtual HID) | requires `com.apple.hid.manager.*` entitlements, all **restricted** on macOS 26 — an ad-hoc binary claiming them is killed at exec |
| DYLD-inject into `hidd` | `IOHIDDevice::start()` → `0xe00002c9` (hidd *is* the HID server: circular) |

The one process that can inject at the login window is Apple's **`screensharingd`**, because
it holds the private entitlement `com.apple.private.hid.client.event-dispatch`, which lets it
call `IOHIDEventSystemClientDispatchEvent` past SecureEventInput. This is exactly how Apple
Screen Sharing types on the login window. So the strategy is: **don't fight
SecureEventInput — delegate login-window injection to screensharingd.**

## The design

The kvmagent is loaded by a **dual-session LaunchAgent**:

```xml
<key>LimitLoadToSessionType</key><array><string>Aqua</string><string>LoginWindow</string></array>
```

So it runs:
- in the user's **Aqua** session as uid 501 (normal logged-in KVM), and
- at the **LoginWindow** as **uid 0**.

The MeshCentral daemon relays the operator's KVM stream to whichever kvmagent owns the
current console, over `‌/tmp/meshagent-kvm-<console_uid>.sock` (uid 0 at the login window,
501 in-session). Two paths then diverge by context (`is_loginwindow()` = `getuid()==0` or
the console user is `NULL`/`"loginwindow"`):

### Video — always ScreenCaptureKit
The kvmagent captures via ScreenCaptureKit in both contexts, including at uid 0. This needs
the **kvmagent** to hold the **Screen Recording** (`kTCCServiceScreenCapture`) TCC grant,
obtained from the install-time prompt. Video does not involve screensharingd.

### Input — split by context
- **Logged-in session:** `CGEventPost(kCGHIDEventTap, …)` (needs Accessibility TCC). Unchanged
  from upstream except for a fix so events reach the focused app rather than `loginwindow`.
- **Login window:** the agent connects to `screensharingd` at `localhost:5900` as a small
  RFB client and forwards input as VNC `KeyEvent`/`PointerEvent` messages. screensharingd
  performs the injection.

The RFB client (`vnc_connect`, `vnc_inject_key`, `vnc_inject_mouse`, `adb_to_x11keysym` in
`agent/mac_events.c`) speaks **RFB 3.3 with security type 2 (VncAuth, DES challenge)** and
reads the password from `<install>/kvm/vnc.pw` (applying the VNC DES key-bit-reversal quirk).
It reconnects automatically with a short backoff.

## The critical requirement: screensharingd's own TCC grants

This is the subtle part that makes or breaks login-window input, and it is **not** about the
VNC auth, the RFB handshake, or ARD privileges.

When a VNC client connects, screensharingd decides whether that session may inject. In its
unified log this shows as:

```
screensharingd: screenCapture flag <0|1>  postEvents flag <0|1>
```

`postEvents flag 1` → input is injected. `postEvents flag 0` → every key is dropped with
`ignore key event`. That flag is **0** unless Apple's helper **`com.apple.screensharing.agent`**
can itself capture the screen — and that requires the helper to hold two TCC grants:

```
kTCCServiceScreenCapture  | com.apple.screensharing.agent | 2
kTCCServicePostEvent      | com.apple.screensharing.agent | 2
```

On a machine where these are **absent**, tccd logs:

```
ScreensharingAgent: Cannot get shareable content
tccd: For com.apple.screensharing.agent … Auth Right: Unknown (None)
tccd: Service kTCCServiceScreenCapture does not allow prompting; returning denied
screensharingd: screenCapture flag 0  postEvents flag 0
screensharingd: ignore key event
```

Note the code requirement **matches** (`anchor apple`) — the grant is simply **missing**, not
corrupted. And at the login window tccd cannot prompt, so it denies.

**What creates the grants:** enabling **Screen Sharing in System Settings → General →
Sharing**. That GUI toggle (a privileged path) inserts both rows. Enabling screensharingd
via `kickstart`/`launchctl` does **not** — it starts the service and even authenticates VNC
clients, but leaves the helper ungranted, so input is silently ignored. This is why the
installer cannot do it headlessly and the one-time System Settings toggle is required.

The system `TCC.db` is read-only even to root (it needs Full Disk Access; disabling SIP is
not sufficient), so inserting the grant directly is not a viable install step. On
MDM-managed fleets the equivalent is a **PPPC configuration profile** pre-approving
`com.apple.screensharing.agent` for ScreenCapture + PostEvent.

### Dead ends (so you don't repeat them)
- `kickstart -activate -allowAccessFor -allUsers -privs -all` — puts screensharingd into
  ARD-managed mode and does **not** grant input; if you ran it, revert with
  `kickstart -deactivate -configure -access -off` and
  `defaults delete com.apple.RemoteManagement ARD_AllLocalUsers ARD_AllLocalUsersPrivs`.
- Sending a "fuller" RFB handshake (SetPixelFormat/SetEncodings/FramebufferUpdateRequest) —
  makes no difference; a full TigerVNC-style client is ignored identically when the helper
  is ungranted.

## macOS version support (Big Sur → Tahoe)

The agent targets macOS 11+ on both architectures. Two areas are version-sensitive and are
handled in the code:

- **Video capture.** ScreenCaptureKit only exists on macOS 14+. On older systems the agent
  falls back to CoreGraphics: `CGWindowListCreateImage` in-session, and the same at the
  **login window** (with `CGDisplayCreateImage` as a further fallback) — these capture the
  login UI on a physical Mac when Screen Recording is granted. Without the fallback, login
  video is black on macOS 11-13 (`mac_kvm.c`).
- **HID init crash on macOS 11.** `hid_inject_init()` used to probe `IOHIDUserDevice` and the
  AppleVirtualPlatform HID bridge. On Big Sur the denied `IOHIDUserDevice` create returns an
  invalid (non-NULL) ref and activating it **crashes the process** right after init — before
  the capture loop — so the agent crash-loops and the screen is permanently black. Those
  probes are never the working injection path (login = screensharingd VNC, in-session =
  `CGEventPost`), so they are skipped; the code goes straight to `CGEventPost` (`mac_events.c`).

Neither change affects macOS 14+ behaviour (SCK is used there, and the skipped HID probes
returned NULL there anyway).

## Limitations

### FileVault must be disabled
The "login window" this project controls is the **normal macOS login window** — the one drawn
by `loginwindow` after macOS has booted, launchd has started the LaunchAgents, and the kvmagent
+ screensharingd are running.

With **FileVault** enabled, a cold boot or reboot does not reach that state. It first stops at
the **pre-boot authentication (disk-unlock) screen**, which runs in the EFI / recoveryOS
environment **before** the macOS kernel, launchd, `screensharingd`, or the MeshAgent exist.
Nothing is running there, so:

- there is no framebuffer source and no event-dispatch process to drive — **no remote video or
  input at the unlock screen**, and no way to unlock the disk remotely; and
- once someone unlocks the disk **physically**, FileVault forwards that authentication straight
  into the user's session (pre-boot auth → auto-login), so the normal login window is typically
  **skipped entirely** — the moment this project targets never appears.

This is a fundamental property of FileVault, not something the agent can bypass (bypassing
pre-boot disk encryption is exactly what FileVault exists to prevent). **For unattended
remote access at/through the login window across reboots, disable FileVault:**

```bash
sudo fdesetup disable
```

If FileVault is required by policy, remote login-window control is not achievable on that
machine after a reboot; you would only regain remote access once a user has physically unlocked
and logged in.

## Onboarding: served-binary hash match

MeshCentral withholds the agent **core** (Desktop/Terminal/Files tabs) until the connecting
agent's binary hash matches the **served** binary in `agents/`. A mismatch yields a node with
`caps:24` (console + js only, no core); a healthy node shows `caps:31` plus a `core` field.
So the fixed build must be deployed as the served binary too. See `server/README.md`.

## Security posture

- **`:5900` is loopback-only** via a `pf` anchor the installer ships (`set skip on lo0` +
  Apple's default anchors + `block drop in quick proto tcp … port 5900`), re-applied at boot
  by a LaunchDaemon. Only the local kvmagent reaches screensharingd.
- **Per-machine random VNC password** in `kvm/vnc.pw` (mode 600, root) — no shared secret.
- Operator ↔ agent uses MeshCentral's normal end-to-end tunnel; the VNC hop is loopback only.

## Files that implement this

- `agent/mac_events.c` — `is_loginwindow()` routing in `inject_key()` / `MouseAction()`, plus
  the `vnc_*` RFB client. `agent/login-kvm.patch` is the diff vs upstream.
- `installer/meshinstall.sh` — dual-session LaunchAgent, `kvm/` subdir, legacy-VNC + random
  `vnc.pw`, pf loopback anchor.
- `agent/kvm_input_harness.c` — feeds raw `MNG_KVM_KEY`/`MNG_KVM_MOUSE` frames straight into
  `/tmp/meshagent-kvm-<uid>.sock` to validate injection independently of MeshCentral.
