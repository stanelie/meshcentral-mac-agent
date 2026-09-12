#!/bin/bash
# MeshCentral macOS agent installer with login-window KVM.
# Runs on any macOS (no pkg/pkgbuild). Usage: sudo bash meshinstall.sh
#
# ====================================================================
#  CONFIGURE THESE FIVE VALUES for YOUR server and device group.
#  See the repo README ("Configuring for your own server and group")
#  for how to obtain MESH_ID and SERVER_ID from your MeshCentral console.
# ====================================================================
MESH_NAME="__MESH_NAME__"       # your device group's name (any label)
MESH_ID="__MESH_ID__"           # 0x followed by 96 hex chars (from your .msh)
SERVER_ID="__SERVER_ID__"       # your server's ID (from your .msh; same for all groups)
MESH_SERVER="wss://__HOST__:443/agent.ashx"   # your MeshCentral host
BASE_URL="https://__HOST__"     # where you host the installer + binaries (see server/README.md)

# Screen Sharing (:5900) exposure policy. screensharingd serves BOTH Apple
# Screen Sharing (security type 30) and the legacy VNC (type 2) that the
# login-window KVM needs, on the SAME port -- they cannot be separated by
# port, only by source address.
#   lan    (default) reachable from private + link-local networks, so Screen
#          Sharing from other Macs on the LAN keeps working, while public
#          networks (hotel/cafe wifi) cannot reach it.
#   allow  no pf rules at all; :5900 reachable from anywhere the network allows.
#   block  loopback only. Native Screen Sharing from other Macs will NOT work.
# Override at install time:  SS_LAN_ACCESS=allow sudo -E bash meshinstall.sh
SS_LAN_ACCESS="${SS_LAN_ACCESS:-lan}"

if [ "$(id -u)" != "0" ]; then echo "Please run with sudo."; exit 1; fi

CO=meshagent; SV=meshagent; EXE=meshagent
D="/usr/local/mesh_services/$CO/$SV"
mkdir -p "$D"

# ---- fetch the architecture-specific fixed binary (thin: type 29 arm64 / 16 x86) ----
case "$(uname -m)" in
  arm64)  BIN_URL="$BASE_URL/meshsetup/meshagent_osx-arm-64" ;;
  x86_64) BIN_URL="$BASE_URL/meshsetup/meshagent_osx-x86-64" ;;
  *) echo "Unsupported architecture: $(uname -m)"; exit 1 ;;
esac
echo "Downloading agent for $(uname -m) ..."
# Clear the immutable flag first (see the kvm/ copy below for why anyone would
# have set it) -- otherwise curl fails with EPERM even as root.
chflags nouchg "$D/$EXE" 2>/dev/null || true
if ! curl -fsSL "$BIN_URL" -o "$D/$EXE"; then echo "ERROR: could not download $BIN_URL"; exit 1; fi
chmod 755 "$D/$EXE"; chown root:wheel "$D/$EXE"

# ---- build the .msh (group binding) ----
cat > "$D/$EXE.msh" <<MSH
MeshName=$MESH_NAME
MeshID=$MESH_ID
ServerID=$SERVER_ID
MeshServer=$MESH_SERVER
MSH
chown root:wheel "$D/$EXE.msh"; chmod 644 "$D/$EXE.msh"

# ---- LaunchDaemon (root, server connectivity) ----
cat > "/Library/LaunchDaemons/$SV.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>$SV</string>
<key>ProgramArguments</key><array><string>$D/$EXE</string></array>
<key>WorkingDirectory</key><string>$D/</string>
<key>RunAtLoad</key><true/>
<key>KeepAlive</key><dict><key>Crashed</key><true/></dict>
<key>ThrottleInterval</key><integer>5</integer>
</dict></plist>
PL
chown root:wheel "/Library/LaunchDaemons/$SV.plist"; chmod 644 "/Library/LaunchDaemons/$SV.plist"

# ---- kvm/ subdir: kvmagent runs here (own db + socket) ----
mkdir -p "$D/kvm"
# Replace the kvmagent binary via a temp file and an atomic rename, and VERIFY it.
#
# A plain `cp` over this path has two ways to fail, and the installer used to
# swallow both and report success anyway -- leaving the machine running the OLD
# agent while every post-install check passed. Observed 2026-09-12 on a Mac that
# did exactly that: the kvmagent was several builds behind and nothing said so.
#
#   1. chflags uchg. Someone may have pinned a binary here to stop MeshCentral's
#      own agent auto-update from replacing it. cp then returns EPERM even as
#      root, which is not obviously a permissions problem when you are already
#      root and the directory is writable.
#   2. The file is currently being executed. Writing into a running, signed
#      Mach-O is refused; renaming over it is not, because the running process
#      keeps its own inode and only the directory entry changes.
chflags nouchg "$D/kvm/$EXE" 2>/dev/null || true
if ! cp "$D/$EXE" "$D/kvm/$EXE.new"; then
    echo "ERROR: could not stage $D/kvm/$EXE.new -- aborting rather than leaving a stale agent."
    exit 1
fi
chmod 755 "$D/kvm/$EXE.new"; chown root:wheel "$D/kvm/$EXE.new"
# Checksum the staged file BEFORE the rename, and compare the landed file to
# that -- not to "$D/$EXE". The agent auto-updates itself, so "$D/$EXE" can be
# rewritten by the running daemon while this script is mid-flight; comparing
# against it produced a false "does not match" abort on a perfectly good install
# (measured 2026-09-12). What actually needs verifying is that the rename took
# effect, and this checks exactly that and nothing else.
KVMSUM="$(/usr/bin/shasum -a 256 "$D/kvm/$EXE.new" 2>/dev/null | awk '{print $1}')"
if ! mv -f "$D/kvm/$EXE.new" "$D/kvm/$EXE"; then
    rm -f "$D/kvm/$EXE.new"
    echo "ERROR: could not replace $D/kvm/$EXE -- aborting rather than leaving a stale agent."
    exit 1
fi
if [ -z "$KVMSUM" ] || [ "$KVMSUM" != "$(/usr/bin/shasum -a 256 "$D/kvm/$EXE" 2>/dev/null | awk '{print $1}')" ]; then
    echo "ERROR: $D/kvm/$EXE is not the binary we just staged -- aborting."
    exit 1
fi
cp "$D/$EXE.msh" "$D/kvm/$EXE.msh"
chmod 755 "$D/kvm/$EXE"; chown root:wheel "$D/kvm/$EXE"
chown root:wheel "$D/kvm/$EXE.msh"; chmod 644 "$D/kvm/$EXE.msh"
# SECURITY: this directory holds a binary that launchd executes as ROOT in the
# LoginWindow session, plus vnc.pw. It must NOT be writable by ordinary users --
# group-write here lets any member of 'staff' unlink the binary and substitute
# their own, i.e. local root. Keep it root:wheel 0755.
chown root:wheel "$D/kvm"; chmod 755 "$D/kvm"
# The kvmagent needs a writable working directory for its own db/log. Give it a
# separate sticky (1777, like /tmp) directory that contains no executables, so
# the Aqua (console-user) and LoginWindow (root) instances can both write and
# neither can delete the other's files.
mkdir -p "$D/kvmstate"
chown root:wheel "$D/kvmstate"; chmod 1777 "$D/kvmstate"

# ---- dual-session (Aqua+LoginWindow) -kvmagent LaunchAgent ----
cat > "/Library/LaunchAgents/$SV.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>$SV-launchagent</string>
<key>LimitLoadToSessionType</key><array><string>Aqua</string><string>LoginWindow</string></array>
<key>ProgramArguments</key><array><string>$D/kvm/$EXE</string><string>-kvmagent</string></array>
<key>WorkingDirectory</key><string>$D/kvmstate/</string>
<key>RunAtLoad</key><true/>
<key>KeepAlive</key><dict><key>SuccessfulExit</key><false/></dict>
<!-- The kvmagent exits every time a KVM session closes, so this is a NORMAL
     restart interval, not a crash backoff. At the launchd default (10) -- and
     far worse at the 30 this used to carry -- the agent is simply absent for
     that long after each session: reopening the Desktop tab inside the window
     silently gets nothing, and the installer's permission walkthrough had to
     force restarts with "launchctl kickstart -k" to make any progress
     (NOTE: keep backticks and dollar-paren out of this heredoc's TEXT. It is
     unquoted so that $D and $SV expand, so both forms get EXECUTED here, not
     printed. Both mistakes were made in turn: a backticked command in this very
     comment ran launchctl kickstart -k on every install and pasted its empty
     output into the plist, and the note warning about it then ran the ellipsis
     inside its own dollar-paren example.)
     (measured 2026-09-10: socket still dead 40s after a session closed).
     5 covers the exit-and-relaunch without allowing a hot crash loop. -->
<key>ThrottleInterval</key><integer>5</integer>
</dict></plist>
PL
chown root:wheel "/Library/LaunchAgents/$SV.plist"; chmod 644 "/Library/LaunchAgents/$SV.plist"

# ---- login-window input prereq: legacy VNC + per-machine random password ----
# Enable the Screen Sharing SERVICE itself (screensharingd on :5900). The
# kickstart below only sets the legacy-VNC *option*; without the running
# service nothing listens on :5900 and login-window input has no target.
# Record what the machine looked like BEFORE we touched it, so uninstall can
# put it back instead of leaving legacy VNC + a password enabled forever.
PRESTATE="$D/kvm/prestate.env"
if [ ! -f "$PRESTATE" ]; then
    # Whether Screen Sharing was ALREADY on. Getting this right matters: if we
    # record "on" when it was off, uninstall leaves Screen Sharing + legacy VNC
    # enabled on a machine that never had them -- the exact thing this prestate
    # file exists to prevent.
    #
    # Two tests that look right and are NOT (both measured 2026-09-09, clean Tahoe
    # 26.6.2 VM):
    #   - `launchctl print system/com.apple.screensharing` exit status: always 0,
    #     because that system LaunchDaemon is always *registered* even when off.
    #     This is the bug that recorded PRIOR_SS=on unconditionally.
    #   - grepping its output for `state = running`: the job is socket-activated,
    #     so it reads `state = not running` while Screen Sharing is fully enabled
    #     and serving (:5900 listening). Also emits several nested `state = active`
    #     lines, so a bare `state =` grep is ambiguous.
    #   - `launchctl print-disabled system` reports "enabled" even when off.
    #
    # What actually tracks it: is anything LISTENING on :5900 (0 before enabling,
    # non-zero after).
    if [ -x /usr/sbin/lsof ] && \
       [ "$(/usr/sbin/lsof -nP -iTCP:5900 -sTCP:LISTEN 2>/dev/null | /usr/bin/grep -c LISTEN)" -gt 0 ]; then
        PRIOR_SS=on; else PRIOR_SS=off; fi
    PRIOR_LEGACY="$(/usr/bin/defaults read /Library/Preferences/com.apple.RemoteManagement \
                    VNCLegacyConnectionsEnabled 2>/dev/null || echo 0)"
    umask 077
    printf 'PRIOR_SS=%s\nPRIOR_LEGACY=%s\n' "$PRIOR_SS" "$PRIOR_LEGACY" > "$PRESTATE"
    chown root:wheel "$PRESTATE"; chmod 600 "$PRESTATE"
fi

/bin/launchctl enable system/com.apple.screensharing 2>/dev/null
/bin/launchctl bootstrap system /System/Library/LaunchDaemons/com.apple.screensharing.plist 2>/dev/null || \
/bin/launchctl load -w /System/Library/LaunchDaemons/com.apple.screensharing.plist 2>/dev/null
# VNC DES keys are exactly 8 bytes, so 8 chars is the maximum useful length
# (~2.2e14 combinations). Never fall back to a guessable value -- abort instead.
VNC_PW="$(LC_ALL=C tr -dc 'A-Za-z0-9' < /dev/urandom | head -c 8)"
if [ "${#VNC_PW}" -ne 8 ]; then
    echo "ERROR: could not generate a VNC password from /dev/urandom; aborting."
    exit 1
fi
/System/Library/CoreServices/RemoteManagement/ARDAgent.app/Contents/Resources/kickstart \
    -configure -clientopts -setvnclegacy -vnclegacy yes -setvncpw -vncpw "$VNC_PW" >/dev/null 2>&1
umask 077
printf '%s\n' "$VNC_PW" > "$D/kvm/vnc.pw"; chmod 600 "$D/kvm/vnc.pw"; chown root:wheel "$D/kvm/vnc.pw"

# ---- :5900 exposure policy via pf ----------------------------------------
# NOTE: we build our ruleset by APPENDING to the system /etc/pf.conf rather
# than hardcoding Apple's anchor list, so a macOS update that changes pf.conf
# is not silently discarded. "set skip on lo0" must precede the anchors and
# is what keeps the agent's own loopback connection to :5900 working.
PF_ANCHOR=/etc/pf.anchors/meshagent-vnc.conf
PF_PLIST=/Library/LaunchDaemons/com.meshagent.pf.plist

pf_write_ruleset() {   # $1 = "lan" | "block"
    mkdir -p /etc/pf.anchors
    {
        echo "# Generated by meshinstall.sh -- do not edit; re-run the installer."
        echo "set skip on lo0"
        if [ -r /etc/pf.conf ]; then
            grep -v '^[[:space:]]*set[[:space:]]\+skip' /etc/pf.conf
        else
            echo 'scrub-anchor "com.apple/*"'
            echo 'nat-anchor "com.apple/*"'
            echo 'rdr-anchor "com.apple/*"'
            echo 'dummynet-anchor "com.apple/*"'
            echo 'anchor "com.apple/*"'
            echo 'load anchor "com.apple" from "/etc/pf.anchors/com.apple"'
        fi
        if [ "$1" = "lan" ]; then
            # First matching "quick" rule wins: LAN sources pass, the rest drop.
            echo 'pass in quick proto tcp from 10.0.0.0/8     to any port = 5900'
            echo 'pass in quick proto tcp from 172.16.0.0/12  to any port = 5900'
            echo 'pass in quick proto tcp from 192.168.0.0/16 to any port = 5900'
            echo 'pass in quick proto tcp from 169.254.0.0/16 to any port = 5900'
            echo 'pass in quick proto tcp from fc00::/7       to any port = 5900'
            echo 'pass in quick proto tcp from fe80::/10      to any port = 5900'
        fi
        echo 'block drop in quick proto tcp from any to any port = 5900'
    } > "$PF_ANCHOR"
    chown root:wheel "$PF_ANCHOR"; chmod 644 "$PF_ANCHOR"
}

pf_remove() {
    /bin/launchctl bootout system "$PF_PLIST" 2>/dev/null
    rm -f "$PF_PLIST" "$PF_ANCHOR"
    # Restore the stock ruleset. Never "pfctl -d": pf enable/disable is
    # reference counted and other software (VPNs, Internet Sharing) may rely
    # on it. Apple's default ruleset filters nothing, so leaving pf enabled
    # with it loaded is harmless.
    [ -r /etc/pf.conf ] && /sbin/pfctl -f /etc/pf.conf >/dev/null 2>&1
}

case "$SS_LAN_ACCESS" in
  allow)
    pf_remove
    echo "Screen Sharing (:5900): no pf restriction -- reachable from any network."
    ;;
  lan|block)
    pf_write_ruleset "$SS_LAN_ACCESS"
    # Validate before loading: a syntax error would otherwise leave the machine
    # with whatever ruleset happened to be active.
    if /sbin/pfctl -n -f "$PF_ANCHOR" >/dev/null 2>&1; then
        cat > "$PF_PLIST" <<'PFPLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>com.meshagent.pf</string>
<key>ProgramArguments</key><array><string>/bin/sh</string><string>-c</string>
<string>/sbin/pfctl -f /etc/pf.anchors/meshagent-vnc.conf; /sbin/pfctl -E 2>/dev/null; exit 0</string></array>
<key>RunAtLoad</key><true/>
</dict></plist>
PFPLIST
        chown root:wheel "$PF_PLIST"; chmod 644 "$PF_PLIST"
        /sbin/pfctl -f "$PF_ANCHOR" >/dev/null 2>&1
        /sbin/pfctl -E >/dev/null 2>&1
        /bin/launchctl bootstrap system "$PF_PLIST" 2>/dev/null || \
        /bin/launchctl load "$PF_PLIST" 2>/dev/null
        if [ "$SS_LAN_ACCESS" = "lan" ]; then
            echo "Screen Sharing (:5900): allowed from LAN/link-local, blocked from public networks."
        else
            echo "Screen Sharing (:5900): loopback only -- other Macs CANNOT screen share to this one."
        fi
    else
        echo "WARNING: generated pf ruleset failed to parse; leaving pf untouched."
        rm -f "$PF_ANCHOR"
    fi
    ;;
  *)
    echo "WARNING: unknown SS_LAN_ACCESS='$SS_LAN_ACCESS' (use lan|allow|block); leaving pf untouched."
    ;;
esac

# ---- start daemon + dual-session agent (+ TCC watcher) ----
/bin/launchctl load "/Library/LaunchDaemons/$SV.plist" 2>/dev/null
CUID=$(stat -f%u /dev/console 2>/dev/null || echo "")
CUSER=$(stat -f%Su /dev/console 2>/dev/null || echo "")
if [ -n "$CUID" ] && [ "$CUSER" != "root" ] && [ -n "$CUSER" ]; then
    /bin/launchctl bootstrap "gui/$CUID" "/Library/LaunchAgents/$SV.plist" 2>/dev/null || \
    /bin/launchctl load "/Library/LaunchAgents/$SV.plist" 2>/dev/null
    CHOME=$(dscl . -read "/Users/$CUSER" NFSHomeDirectory 2>/dev/null | awk '{print $2}')
    if [ -n "$CHOME" ]; then
        WD="$CHOME/Library/Application Support/MeshAgent"; WL="$CHOME/Library/LaunchAgents"
        mkdir -p "$WD" "$WL"
        cat > "$WD/tcc-watcher.sh" <<WSH
#!/bin/bash
KP="/Library/LaunchAgents/$SV.plist"; [ -f "\$KP" ] || exit 0
U=\$(id -u); sleep 1
/bin/launchctl bootout "gui/\$U" "\$KP" 2>/dev/null; sleep 1
/bin/launchctl bootstrap "gui/\$U" "\$KP" 2>/dev/null
WSH
        chmod 755 "$WD/tcc-watcher.sh"; chown "$CUSER" "$WD/tcc-watcher.sh" "$WD"
        cat > "$WL/com.meshagent.tcc-watcher.plist" <<WLP
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>com.meshagent.tcc-watcher</string>
<key>ProgramArguments</key><array><string>/bin/bash</string><string>$WD/tcc-watcher.sh</string></array>
<key>WatchPaths</key><array><string>/Library/Application Support/com.apple.TCC/TCC.db</string></array>
<key>RunAtLoad</key><false/>
</dict></plist>
WLP
        chown "$CUSER" "$WL/com.meshagent.tcc-watcher.plist"
        /bin/launchctl bootstrap "gui/$CUID" "$WL/com.meshagent.tcc-watcher.plist" 2>/dev/null
    fi
fi
echo "MeshAgent installed for group '$MESH_NAME' ($(uname -m))."

# ---- privacy permissions, asked NOW rather than on the first remote session ----
# Run the walkthrough as the CONSOLE USER: TCC attributes a grant to the running
# program, and the grants have to land on the kvm binary inside that user's Aqua
# session. Root cannot stand in for them.
#
# MESH_SKIP_PERMS=1 skips this (unattended/MDM installs, where a PPPC profile
# should be delivering these grants instead).
# MESH_NO_FDA=1 skips only the optional Full Disk Access step.
if [ "${MESH_SKIP_PERMS:-0}" != "1" ] && [ -n "$CUID" ] && [ -n "$CUSER" ] && [ "$CUSER" != "root" ]; then
    if [ -e /dev/tty ]; then
        # Apple's OWN Screen Sharing helper needs consent too, separately from
        # anything meshagent asks for. Login-window input is driven through
        # screensharingd, and the first time that happens macOS asks the operator
        # to let ScreensharingAgent "bypass the system private window picker and
        # directly access the screen and audio". Measured 2026-09-10: it fires
        # once per machine (it did not return after a reboot and a second
        # pre-login connect), so asking here means it is answered during the
        # install instead of ambushing whoever makes the first support call.
        #
        # This runs as ROOT on purpose -- it reads kvm/vnc.pw (root:wheel 0600).
        # Unlike the meshagent grants it needs no responsible-process care: the
        # process macOS judges is Apple's ScreensharingAgent, spawned by
        # screensharingd from launchd, so it is its own responsible process no
        # matter who opens the connection.
        if /usr/sbin/netstat -an 2>/dev/null | grep -q '\.5900 .*LISTEN'; then
            {
                echo
                echo "=== MeshAgent: Screen Sharing consent (Apple's own helper) ==="
                echo "  Opening a brief local Screen Sharing session, so macOS asks for this"
                echo "  now rather than on your first login-window connection."
                echo "  Your screen may flicker for a couple of seconds."
            } >/dev/tty
            "$D/kvm/$EXE" -vncprobe >/dev/tty 2>&1
            {
                echo "  If macOS asked to allow direct access to the screen and audio, allow it."
                printf "  Press Return to continue : "
            } >/dev/tty
            read -r _ </dev/tty || true
        fi

        FDAARG=""
        [ "${MESH_NO_FDA:-0}" = "1" ] && FDAARG="nofda"
        /bin/launchctl asuser "$CUID" /usr/bin/sudo -u "$CUSER" \
            "$D/kvm/$EXE" -requestperms $FDAARG </dev/tty >/dev/tty 2>&1 || \
            echo "  (permission walkthrough exited early -- grant them in System Settings)"
        # Grants only take effect on a fresh process, so restart the agent.
        /bin/launchctl bootout "gui/$CUID" "/Library/LaunchAgents/$SV.plist" 2>/dev/null
        sleep 1
        /bin/launchctl bootstrap "gui/$CUID" "/Library/LaunchAgents/$SV.plist" 2>/dev/null
    else
        echo "  (no terminal: skipping the permission walkthrough --"
        echo "   grant Screen Recording and Accessibility in System Settings)"
    fi
fi

# ---- post-install verification ------------------------------------------
# Everything below is checkable without Full Disk Access, so the installer never
# needs FDA itself.
#
# The TCC grants are NOT checked here. Note the precise reason: the system TCC.db
# is not "closed even to root" -- SIP makes it read-ONLY, but a root process whose
# *responsible* process holds Full Disk Access can read it (measured 2026-09-09:
# `sqlite3 .../TCC.db "select ..."` over ssh succeeds, because
# /usr/libexec/sshd-keygen-wrapper has kTCCServiceSystemPolicyAllFiles). An
# installer run from a plain Terminal generally does NOT have that, and writing is
# blocked outright by SIP regardless. So the grants are reported as manual steps
# rather than probed or pre-seeded.
echo
echo "--- checks ---"
if /bin/launchctl print system/com.apple.screensharing >/dev/null 2>&1; then
    echo "  [ok]   screensharingd is running"
else
    echo "  [FAIL] screensharingd is NOT running -- login-window input cannot work."
fi
if /usr/sbin/netstat -an 2>/dev/null | grep -q '\.5900 .*LISTEN'; then
    echo "  [ok]   something is listening on :5900"
else
    echo "  [FAIL] nothing is listening on :5900."
fi
if [ "$PRIOR_SS" = "off" ]; then
    echo "  [WARN] Screen Sharing was OFF before this install and was started by"
    echo "         launchctl. That starts the daemon but does NOT create the TCC"
    echo "         grants its helper needs, and may leave the service with no"
    echo "         allowed-users list -- so login-window input will be silently"
    echo "         ignored AND other Macs may be refused. Toggle it properly in"
    echo "         System Settings > General > Sharing > Screen Sharing."
else
    echo "  [ok]   Screen Sharing was already enabled before install"
fi
echo
echo "--- remaining MANUAL step (macOS will not let an installer do this one) ---"
echo "  System Settings > General > Sharing > Screen Sharing  ......  ON"
echo "     (this is what grants com.apple.screensharing.agent the ScreenCapture"
echo "      + PostEvent TCC rights that make login-window input work at all;"
echo "      enabling the service from the command line does NOT create them)"
echo
echo "  Screen Recording and Accessibility were requested above by the permission"
echo "  walkthrough. If you skipped a step there, grant it in"
echo "  System Settings > Privacy & Security, then run:"
echo "      sudo launchctl bootout gui/\$(id -u) /Library/LaunchAgents/$SV.plist"
echo "      sudo launchctl bootstrap gui/\$(id -u) /Library/LaunchAgents/$SV.plist"
echo "  (a grant only takes effect in a freshly started agent)"
echo
echo "  On macOS 15 and later, Screen Recording approval EXPIRES and re-prompts"
echo "  periodically. On a fleet, deploy an MDM PPPC configuration profile instead"
echo "  -- that is the only way to make these grants permanent and silent."

# ---- warn if FileVault is on: login-window KVM can't work across reboots ----
if /usr/bin/fdesetup status 2>/dev/null | grep -q "FileVault is On"; then
    echo
    echo "WARNING: FileVault is ENABLED. After a reboot this Mac stops at the pre-boot"
    echo "  disk-unlock screen (before macOS and this agent run), so remote login-window"
    echo "  video/input will NOT be available until someone unlocks it physically."
    echo "  For unattended remote access at the login window, disable FileVault:"
    echo "      sudo fdesetup disable"
fi
exit 0
