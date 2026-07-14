# macOS remote-desktop findings

Field notes from making MeshAgent KVM work on macOS 11–26 (Big Sur → Tahoe). The
mechanism is in [architecture.md](architecture.md); this collects the gotchas, dead ends,
and one unresolved issue so they aren't rediscovered the hard way.

## Login-window input only works through `screensharingd` — and it needs a TCC grant
- At the login window, SecureEventInput makes the WindowServer drop every userspace-injected
  event (`CGEventPost`, `IOHIDPostEvent`, `IOHIDUserDevice`…). Only Apple's `screensharingd`
  (holds `com.apple.private.hid.client.event-dispatch`) can inject there. The agent forwards
  login-window input to it over the loopback legacy-VNC port.
- **The non-obvious part:** `screensharingd` only injects if its helper
  `com.apple.screensharing.agent` has the **ScreenCapture + PostEvent** TCC grants. Those are
  created by enabling **Screen Sharing in System Settings** — *not* by `kickstart`/`launchctl`.
  Without them, screensharingd authenticates the VNC connection but logs `ignore key event`
  (`postEvents flag 0`). This is why the install requires the one-time Screen Sharing toggle.
- Dead ends: `kickstart -activate … -privs -all` (ARD managed mode) does **not** grant input;
  a "fuller" RFB handshake makes no difference; the block is purely the helper's TCC grant.

## macOS version compatibility (Big Sur–Ventura vs Sonoma+)
- **ScreenCaptureKit is macOS 14+.** On 11–13 the agent falls back to `CGWindowListCreateImage`
  (in-session and at the login window) / `CGDisplayCreateImage`; without that, login video is
  black on older macOS. (`mac_kvm.c`)
- **`hid_inject_init` crashed on macOS 11.** Probing `IOHIDUserDevice` returns an invalid
  (non-NULL) ref when the kernel denies it, and activating it crashes the process right after
  init — the agent crash-loops and the screen is permanently black. The AppleVirtualPlatform
  HID probes crash similarly. Both are unused (login = screensharingd VNC, in-session =
  `CGEventPost`), so they're skipped. No effect on macOS 14+. (`mac_events.c`)

## TCC grant persistence across agent updates
- Ad-hoc signing (`codesign -s -`) gives a TCC requirement pinned to the **code hash**, so every
  rebuild/update resets the Screen Recording / Accessibility grants (re-approve each time).
- Signing with a **stable cert** gives an **identity-based** requirement (`certificate root =
  H"…"`) that is hash-independent — grants survive rebuilds. tccd *does* accept a self-signed
  cert grant. See [../signing/README.md](../signing/README.md).

## Apple Silicon self-update codesigning transient
- When the **served** binary changes, arm64 daemons self-update; launchd can exec the
  partially-written binary mid-swap → `OS_REASON_CODESIGNING` kill → the daemon is briefly
  offline (may need a reboot / `launchctl kickstart -k system/<service>` to recover). The final
  on-disk binary is valid. **x86_64 is immune** (no strict codesign-at-exec), and **fresh
  `curl|bash` installs are immune** (no self-update). Pre-existing MeshAgent behaviour, not
  caused by signing.

## FileVault must be off for login-window access across reboots
- With FileVault on, a reboot stops at the pre-boot disk-unlock screen (EFI/recoveryOS) before
  macOS, screensharingd, or the agent exist — nothing to capture or inject, and the normal login
  window is usually skipped entirely. See [architecture.md](architecture.md#limitations).

## Viewer does not auto-follow the login ↔ session handoff (UNRESOLVED — upstream issue)
Symptom: after a user logs in (or out), the MeshCentral Desktop viewer freezes on the last
frame and the operator must disconnect/reconnect.

Root cause (traced on macOS Tahoe):
- On login the **login-window agent socket does not close** (the dual-session LaunchAgent keeps
  the uid-0 instance running; both `/tmp/meshagent-kvm-0.sock` and `-<uid>.sock` exist while
  logged in). So the agent's IPC-close handler **never fires** on login — it is the wrong hook.
- Instead a fresh desktop setup runs and `require('user-sessions').consoleUid()` **throws**
  `nobody logged into console` during the transition — the darwin `consoleUid()` runs `who`,
  which is momentarily empty while the console session switches (and its child-process spawn
  flakes under the burst of calls). So the KVM connects to the login-window session (uid 0) and
  shows a frozen frame. The same `consoleUid()` unreliability breaks several paths at once
  (desktop setup, the server core's `handleServerCommand`, clipboard).

Dead ends tried:
1. **Blocking retry inside `consoleUid()`** (retry `who` before throwing) → **blacks the login
   window** — it stalls the agent's single JS event loop and starves the login-window video relay.
2. **Timer-based non-blocking retry in the IPC-close handler** → **inert**, because that handler
   never fires on login (the socket doesn't close).

Conclusion: the real fix is a **reliable, non-blocking console-user detection** on macOS during
session transitions, spanning both the agent and the server-pushed `meshcore.js` — best done
**upstream** (Ylianst/MeshAgent + MeshCentral), which owns that code. A local, non-fleet-disrupting
palliative would be to wrap the remaining unguarded `consoleUid()` calls in `meshcore.js` in
`try/catch`, but that only silences the exceptions; it does not make the viewer follow the session
(the uid-0-vs-user selection during the transient window is the actual problem).

## Misc TCC gotchas
- The system `TCC.db` is **read-only even to root** without Full Disk Access (disabling SIP is
  not enough) — you cannot insert grants directly; use the System Settings prompt or an MDM PPPC
  profile.
- tccd looks up ScreenCapture by the code-signing **identifier** — always re-sign with the same
  `--identifier`, or the grant stops matching.
- A stale Screen Recording entry for the agent's path **suppresses the re-prompt** after a binary
  change: remove it in System Settings (and reboot) so a fresh install prompts again.
