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

## Login-window VNC keyboard silently dropped Shift'ed characters
- Symptom: typing a password containing a symbol (e.g. `!`) or an uppercase letter at the
  login window produced the *unshifted* character instead (`!` → `1`), so any password with
  such characters failed to unlock — while the same password worked fine once logged in
  (in-session input uses a different path, `CGEventKeyboardSetUnicodeString`, unaffected).
- Root cause: the RFB `KeyEvent` sender (`vnc_inject_key` in `mac_events.c`) mapped each
  physical key to only its **unshifted** X11 keysym and sent a separate Shift-down `KeyEvent`
  alongside it, assuming `screensharingd` would combine the two like an X server would. It
  doesn't reliably do that — the base keysym was injected as typed, ignoring the held Shift.
- Fix: track Shift state client-side and resolve the **shifted** keysym ourselves (US layout)
  before sending a single fully-resolved `KeyEvent`, the same way Enter/Tab/arrows etc. were
  already sent as one resolved keysym rather than a raw scancode. No dependency on the
  receiver combining modifier + base key.
- Diagnostic tip: the login window's **username field** is plaintext (unlike the password
  field's dots), so typing test characters there while watching what actually appears is a
  reliable way to debug keysym-mapping issues without needing real credentials.

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

## Slow / OpenCore-Legacy-Patcher hardware: single-thread starvation (2026-07-23)
On a 2015 MacBookAir7,2 running macOS 15.7.7 via **OpenCore Legacy Patcher**, the Desktop
session was unusable (black screen, random disconnects, multi-second input lag). Root cause was
never one bug — it was several things each stalling the agent's **single JS thread**, which on
this slow, unsupported-GPU hardware was enough to starve the KVM video/input relay:

- **`kvm_check_permission()` from the root daemon**: requested TCC prompts (`AXIsProcessTrusted`
  with prompt, `CGRequestScreenCaptureAccess`) from a LaunchDaemon with no GUI session → hung the
  daemon. Guarded to the per-user kvmagent (`getuid()!=0`). (`agentcore.c`)
- **`ax_poll` `exit(2)` from the daemon**: the Accessibility-grant poll thread `exit(2)`s to force a
  launchd restart, but the daemon's `KeepAlive` is `{Crashed:true}` only — a clean exit killed it
  permanently. Guarded to the kvmagent. (`meshconsole/main.c`)
- **In-session capture via `CGWindowListCreateImage`**: obsoleted in macOS 15 and **~1s/call** on
  this hardware (vs 3–7ms for `CGDisplayCreateImage`). Switched in-session capture to
  `CGDisplayCreateImage` first, falling back to the window-list API only when it fails (headless
  VMs). Measured with a `dlsym` micro-bench since the SDK hard-errors on the obsoleted symbol.
  (`mac_kvm.c`)
- **Clipboard auto-poll was the biggest culprit.** The MeshCentral viewer polls the remote
  clipboard **every ~1s** when "Auto clipboard" is on (`getclip tag:3`). Each poll ran, on the root
  daemon's JS thread, a `zsh` + **`su -`** login shell to run `pbpaste`/`pbcopy` — because those
  tools must reach `pasteboardd` in the user's **Aqua GUI Mach-bootstrap namespace**, which the root
  daemon isn't in. On this hardware every spawn is **~0.8s** (even `launchctl asuser` and a bare
  `execFile({uid})` — the process *spawn itself* is the cost, not just session-crossing). At 1/s
  this consumed most of the thread. `clipboard.js`'s `dispatchRead/Write` also called
  `consoleUid()` (two more `/bin/sh` spawns) on darwin where the result is unused — removed.

### Native clipboard (the fix)
dwservice is smooth on macOS partly because its macOS clipboard is a **no-op stub**; its real design
elsewhere is on-demand + change-detected, run **inside the user session**. We took the same
principle: a **native `NSPasteboard`** clipboard served by the in-session **kvmagent** (already in the
Aqua session, so pasteboard access is a few ms with **no spawn**), exposed over a dedicated unix
socket `/tmp/meshagent-clip.sock` from an **isolated pthread** (so the KVM relay is untouched). The
root daemon's `message-box.js` `get/setClipboard` connect to that socket instead of spawning shells.
Wire protocol: `R`→`[u32 changeCount][u32 len][len UTF8]`, `C`→`[u32 changeCount]`,
`W[u32 len][data]`→`K`. (`mac_kvm_sck.m`, `meshconsole/main.c`, `makefile` adds `-framework AppKit`,
`modules/message-box.js`.)

**Status:** video + input + **local→remote** clipboard confirmed working and smooth with
auto-clipboard on. **remote→local** clipboard is still under investigation (the native read works
end-to-end when tested directly against the socket as root; the path from the viewer's 1s poll back
to the local browser clipboard is not yet delivering — likely the viewer-side `document.hasFocus()` /
`deskLastClipboardReceived` dedup interaction, still being debugged).
