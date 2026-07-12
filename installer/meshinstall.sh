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
chmod 775 "$D/kvm"; chgrp staff "$D/kvm"

# ---- dual-session (Aqua+LoginWindow) -kvmagent LaunchAgent ----
cat > "/Library/LaunchAgents/$SV.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>$SV-launchagent</string>
<key>LimitLoadToSessionType</key><array><string>Aqua</string><string>LoginWindow</string></array>
<key>ProgramArguments</key><array><string>$D/kvm/$EXE</string><string>-kvmagent</string></array>
<key>WorkingDirectory</key><string>$D/kvm/</string>
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
/bin/launchctl enable system/com.apple.screensharing 2>/dev/null
/bin/launchctl bootstrap system /System/Library/LaunchDaemons/com.apple.screensharing.plist 2>/dev/null || \
/bin/launchctl load -w /System/Library/LaunchDaemons/com.apple.screensharing.plist 2>/dev/null
VNC_PW="$(head -c 24 /dev/urandom | base64 | LC_ALL=C tr -dc 'A-Za-z0-9' | cut -c1-8)"
[ -n "$VNC_PW" ] || VNC_PW="mkvnc$(date +%S)"
/System/Library/CoreServices/RemoteManagement/ARDAgent.app/Contents/Resources/kickstart \
    -configure -clientopts -setvnclegacy -vnclegacy yes -setvncpw -vncpw "$VNC_PW" >/dev/null 2>&1
umask 077
printf '%s\n' "$VNC_PW" > "$D/kvm/vnc.pw"; chmod 600 "$D/kvm/vnc.pw"; chown root:wheel "$D/kvm/vnc.pw"

# ---- restrict :5900 to loopback via pf ----
mkdir -p /etc/pf.anchors
cat > /etc/pf.anchors/meshagent-vnc.conf <<'PFCONF'
set skip on lo0
scrub-anchor "com.apple/*"
nat-anchor "com.apple/*"
rdr-anchor "com.apple/*"
dummynet-anchor "com.apple/*"
anchor "com.apple/*"
load anchor "com.apple" from "/etc/pf.anchors/com.apple"
block drop in quick proto tcp from any to any port = 5900
PFCONF
cat > /Library/LaunchDaemons/com.meshagent.pf.plist <<'PFPLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>com.meshagent.pf</string>
<key>ProgramArguments</key><array><string>/bin/sh</string><string>-c</string>
<string>/sbin/pfctl -f /etc/pf.anchors/meshagent-vnc.conf; /sbin/pfctl -e 2>/dev/null; exit 0</string></array>
<key>RunAtLoad</key><true/>
</dict></plist>
PFPLIST
chown root:wheel /etc/pf.anchors/meshagent-vnc.conf /Library/LaunchDaemons/com.meshagent.pf.plist
chmod 644 /Library/LaunchDaemons/com.meshagent.pf.plist
/sbin/pfctl -f /etc/pf.anchors/meshagent-vnc.conf >/dev/null 2>&1
/sbin/pfctl -e >/dev/null 2>&1
/bin/launchctl bootstrap system /Library/LaunchDaemons/com.meshagent.pf.plist 2>/dev/null || \
/bin/launchctl load /Library/LaunchDaemons/com.meshagent.pf.plist 2>/dev/null

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
