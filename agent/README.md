# Building the agent binaries

The changes touch eight files. Everything else is stock
[Ylianst/MeshAgent](https://github.com/Ylianst/MeshAgent).

| file | what it carries |
|---|---|
| `meshcore/KVM/MacOS/mac_events.c` | login-window input routing, private-event-source modifier state, macOS-11-safe HID init |
| `meshcore/KVM/MacOS/mac_events.h` | declaration for the above |
| `meshcore/KVM/MacOS/mac_kvm.c` | login-window video capture (CoreGraphics fallback for macOS < 14), stale-session cleanup, the `-requestperms` walkthrough |
| `meshcore/KVM/MacOS/mac_kvm_sck.m` | ScreenCaptureKit capture path (macOS 14+; compiled out on older SDKs) |
| `meshconsole/main.c` | `-kvmagent` and `-requestperms` entry points |
| `meshcore/agentcore.c` | KVM session plumbing |
| `microstack/ILibProcessPipe.c` | `ILibProcessPipe_JoinWindowServerAuditSession_OSX` |
| `makefile` | builds `mac_kvm_sck.m`, links ScreenCaptureKit |

## What's here
- `mac_events.c`, `mac_kvm.c`, `mac_kvm_sck.m` — full modified files (drop-in replacements).
- `login-kvm.patch` — the exact diff vs the upstream base, covering **all eight** files.
  This is the authoritative, complete form of the change; the three drop-ins above are a
  subset and are **not** sufficient on their own. Building from drop-ins alone fails with
  `hid_inject_init` undeclared and `ILibProcessPipe_JoinWindowServerAuditSession_OSX`
  undefined, because `mac_events.h`, `main.c`, `agentcore.c`, `ILibProcessPipe.c` and the
  makefile only exist in the patch.
- `kvm_input_harness.c` — a standalone validation tool (see bottom).

## Build (on a Mac with the Xcode command-line tools)

```bash
git clone https://github.com/Ylianst/MeshAgent
cd MeshAgent
git checkout cb62daa82b6f23dd317eac77a16a398db03f43ea   # upstream base this patch targets

# apply our changes — the patch is required (it carries all eight files):
git apply /path/to/agent/login-kvm.patch

# The drop-ins are byte-identical to what the patch produces; copying them over
# afterwards is a no-op, and is only useful when editing them as the source of
# truth and regenerating the patch:
#   cp /path/to/agent/mac_{events.c,kvm.c,kvm_sck.m} meshcore/KVM/MacOS/

# Apple Silicon (arm64), agent id 29:
make macos ARCHID=29
codesign -f -s - --identifier meshagent_osx-arm-64 meshagent_osx-arm-64   # local test only — see below

# Intel (x86_64), agent id 16 — clear stale .o first, force the arch:
find microstack microscript meshcore meshconsole -name '*.o' -delete
make macos ARCHID=16 MACOSARCH="-target x86_64-apple-macos11"
codesign -f -s - --identifier meshagent_osx-x86-64 meshagent_osx-x86-64
```

> The `--identifier` **must** be `meshagent_osx-arm-64` / `meshagent_osx-x86-64`. macOS TCC
> looks permissions up by the code-signing identifier; re-signing without it breaks the
> Screen Recording grant. Ad-hoc signing (`-s -`) is fine for a local test build.

> **For anything you ship or serve, sign with the stable cert instead of ad-hoc** —
> `../signing/sign-agent-binaries.sh` (see [../signing/README.md](../signing/README.md)).
> Ad-hoc gives a **cdhash-based** designated requirement, so the TCC grant is tied to that
> exact build and every rebuild forces users to re-approve Screen Recording. The cert gives
> `identifier "…" and certificate root = H"3d2edf19…"`, which is **hash-independent**, so
> grants survive rebuilds. The binaries in [`../prebuilt/`](../prebuilt/) are cert-signed;
> replacing them with ad-hoc builds silently breaks grant persistence across the fleet.

> **Build on a macOS 14+ SDK for any target that runs macOS 14+ (e.g. Apple Silicon on
> Sonoma/Sequoia/Tahoe).** `mac_kvm_sck.m` uses ScreenCaptureKit (`SCScreenshotManager`),
> which only exists in the macOS 14+ SDK; it is guarded out when built against an older SDK
> (e.g. an Intel Mac on the macOS 12 CLT), and the agent falls back to the CoreGraphics
> capture path. That fallback is fine for macOS 11–13 Intel targets, but an arm64 binary
> built on a pre-14 SDK will lack ScreenCaptureKit and degrade login-window capture on
> macOS 14+. Build each arch on an SDK ≥ the oldest OS that arch must fully support.

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
