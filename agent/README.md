# Building the agent binaries

The changes live in `meshcore/KVM/MacOS/mac_events.c` (login-window input routing + the HID
init that's macOS-11-safe) and `meshcore/KVM/MacOS/mac_kvm.c` (login-window video capture with
a CoreGraphics fallback for macOS < 14), plus a one-line makefile tweak. Everything else is
stock [Ylianst/MeshAgent](https://github.com/Ylianst/MeshAgent).

## What's here
- `mac_events.c`, `mac_kvm.c` — the full modified files (drop-in replacements).
- `login-kvm.patch` — the exact diff vs the upstream base (both files), for review / upstream PR.
- `kvm_input_harness.c` — a standalone validation tool (see bottom).

## Build (on a Mac with the Xcode command-line tools)

```bash
git clone https://github.com/Ylianst/MeshAgent
cd MeshAgent
git checkout cb62daa82b6f23dd317eac77a16a398db03f43ea   # upstream base this patch targets

# apply our changes — either drop in the files:
cp /path/to/agent/mac_events.c meshcore/KVM/MacOS/mac_events.c
cp /path/to/agent/mac_kvm.c    meshcore/KVM/MacOS/mac_kvm.c
# ...or apply the patch (covers both files):
#   git apply /path/to/agent/login-kvm.patch

# Apple Silicon (arm64), agent id 29:
make macos ARCHID=29
codesign -f -s - --identifier meshagent_osx-arm-64 meshagent_osx-arm-64

# Intel (x86_64), agent id 16 — clear stale .o first, force the arch:
find microstack microscript meshcore meshconsole -name '*.o' -delete
make macos ARCHID=16 MACOSARCH="-target x86_64-apple-macos11"
codesign -f -s - --identifier meshagent_osx-x86-64 meshagent_osx-x86-64
```

> The `--identifier` **must** be `meshagent_osx-arm-64` / `meshagent_osx-x86-64`. macOS TCC
> looks permissions up by the code-signing identifier; re-signing without it breaks the
> Screen Recording grant. Ad-hoc signing (`-s -`) is fine.

Optional universal binary (agent id 10005):
```bash
lipo -create meshagent_osx-arm-64 meshagent_osx-x86-64 -output meshagent_osx-universal-64
```

The same binary serves as both the daemon and the kvmagent (invoked with `-kvmagent`).
Prebuilt, signed copies are in [`../prebuilt/`](../prebuilt/).

## Runtime prerequisites (the installer handles these)
1. Legacy VNC + a password so the kvmagent can auth to `localhost:5900`:
   `kickstart -configure -clientopts -setvnclegacy -vnclegacy yes -setvncpw -vncpw <pw>`
2. `<install>/kvm/vnc.pw` with the same password (mode 600, root).
3. **Screen Sharing enabled in System Settings** so Apple's `com.apple.screensharing.agent`
   gets its ScreenCapture + PostEvent grants — see [../docs/architecture.md](../docs/architecture.md).
4. Screen Recording granted to the kvmagent (for login-window video via ScreenCaptureKit).

## Validating injection without MeshCentral

`kvm_input_harness.c` connects to `/tmp/meshagent-kvm-<uid>.sock` and writes raw
`MNG_KVM_KEY`(1) / `MNG_KVM_MOUSE`(2) frames — the same wire format the daemon relays — so you
can prove login-window injection independently of onboarding:

```bash
clang -arch arm64 -o kvmharness kvm_input_harness.c && codesign -f -s - kvmharness
sudo ./kvmharness /tmp/meshagent-kvm-0.sock   # at the login window (console = root)
```
A character should appear in the password field and Return should trigger the wrong-password
shake. Watch `log stream --predicate 'process == "screensharingd"'`: you want
`postEvents flag 1` and **no** `ignore key event`.
