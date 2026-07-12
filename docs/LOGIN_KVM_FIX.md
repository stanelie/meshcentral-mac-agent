# macOS login-window KVM input via screensharingd

## Summary

On macOS (verified on **Tahoe 26.5.1**, Apple Silicon), the MeshAgent KVM could
capture the login window but **could not inject keyboard/mouse there**:
`loginwindow` holds `kCGSSessionSecureInputPID` (SecureEventInput), and the
WindowServer drops every userspace-injected event (`CGEventPost`,
`IOHIDPostEvent`, …). Only a process holding Apple's private
`com.apple.private.hid.client.event-dispatch` entitlement can inject at the
login window — and Apple's own **`screensharingd`** has it.

**Fix:** at the login window, route KVM keyboard/mouse through the local
`screensharingd` VNC server (`localhost:5900`) instead of `CGEventPost`. The
kvmagent acts as a small VNC client and screensharingd performs the injection,
past SecureEventInput. In the logged-in session (where SecureEventInput is not
held) the normal `CGEventPost`/HID path is unchanged.

## Code changes (`meshcore/KVM/MacOS/mac_events.c`)

The VNC client (`vnc_connect`, `vnc_inject_key`, `vnc_inject_mouse`,
`adb_to_x11keysym`, `is_loginwindow`) already existed from an earlier attempt
but had been disabled when legacy VNC was wrongly presumed dead on Tahoe. This
fork **re-enables** it:

- `inject_key()` — when `is_loginwindow()`, call `vnc_inject_key(keycode, down)`
  instead of `CGEventPost`.
- `MouseAction()` — when `is_loginwindow()`, call
  `vnc_inject_mouse(absX, absY, button, wheel)` instead of `CGEventPost`.

`vnc_connect` speaks RFB 3.3 + **security type 2 (VncAuth, DES challenge)**,
reading the password from `<install>/kvm/vnc.pw` (with the VNC DES
key-bit-reversal quirk). Type-2 was confirmed to deliver working login-window
input on clean Tahoe — the "legacy VNC is dead" belief was a misdiagnosis of an
unrelated TCC/capture problem.

## Runtime prerequisites (handled by the packager postinstall)

1. **Legacy VNC enabled + password**, so the kvmagent can auth to
   `localhost:5900`:
   ```
   kickstart -configure -clientopts -setvnclegacy -vnclegacy yes -setvncpw -vncpw <pw>
   ```
   (This form sets only `VNCLegacyConnectionsEnabled`; it does NOT switch to
   full Remote Management, so native Screen Sharing is unaffected.)
2. **`<install>/kvm/vnc.pw`** containing the same password (mode 600, root).
3. Login-window **video** uses ScreenCaptureKit at uid=0 and needs Screen
   Recording TCC for the kvmagent (input via VNC needs no TCC).

## Build

```
cd meshagent-src
make macos ARCHID=29                 # Apple Silicon arm64, KVM=1
codesign -f -s - --identifier meshagent_osx-arm-64 meshagent_osx-arm-64
```
Output: `meshagent_osx-arm-64`. The same binary is used as the daemon and the
kvmagent (`-kvmagent`).

## MeshCentral onboarding note

MeshCentral withholds the agent **core** (no Desktop/Terminal/Files tabs) until
the connecting agent's binary hash matches the **served** binary
(`agents/meshagent_osx-arm-64`). So the fixed build must be deployed to the
server as the served binary too, otherwise fresh installs connect but show
`caps:24` (console+js only, no core). A node with the core shows `caps:31`.

## This version

- Based on upstream `Ylianst/MeshAgent`, agent version `v2.6.0`.
- Verified on macOS Tahoe 26.5.1: a fresh install onboards (`caps:31`) and drives
  **video + keyboard + mouse at the login window and in-session** through the
  MeshCentral Desktop tab.

## For the upstream PR

The upstream-relevant change is the `mac_events.c` login-window routing (the two
`is_loginwindow()` branches + the revived `vnc_*` client). The packaging/server
reconciliation pieces are deployment-specific and not part of the agent PR.
