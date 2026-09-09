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
cp "$D/$EXE" "$D/kvm/$EXE"
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
<key>ThrottleInterval</key><integer>30</integer>
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
    if /bin/launchctl print system/com.apple.screensharing >/dev/null 2>&1; then
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

# ---- post-install verification ------------------------------------------
# Everything below is checkable without Full Disk Access. The TCC grants
# themselves are NOT readable (the system TCC.db is closed even to root), so
# those are reported as manual steps rather than guessed at.
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
echo "--- remaining MANUAL steps (macOS will not let an installer do these) ---"
echo "  1. System Settings > General > Sharing > Screen Sharing  ......  ON"
echo "     (this is what grants com.apple.screensharing.agent the ScreenCapture"
echo "      + PostEvent TCC rights that make login-window input work at all)"
echo "  2. System Settings > Privacy & Security > Screen Recording  ...  enable 'meshagent'"
echo "  3. System Settings > Privacy & Security > Accessibility  ......  enable 'meshagent'"
echo "     (needed for in-session keyboard/mouse; the login window does not use it)"
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
