#!/bin/bash
# MeshAgent FULL uninstall (macOS).
# Run:  curl -fsSL https://__HOST__/meshsetup/uninstall.sh | sudo bash
#   (or just: sudo bash uninstall.sh)
[ "$(id -u)" = 0 ] || { echo "Please run with sudo."; exit 1; }

CUSER=$(stat -f%Su /dev/console 2>/dev/null)
CUID=$(stat -f%u /dev/console 2>/dev/null)
echo "Uninstalling MeshAgent (console user: ${CUSER:-?}/${CUID:-?})"

# --- boot out + remove any mesh LaunchDaemons ---
for f in /Library/LaunchDaemons/meshagent*.plist /Library/LaunchDaemons/com.meshagent*.plist; do
    [ -e "$f" ] || continue
    launchctl bootout system "$f" 2>/dev/null
    rm -f "$f"; echo "  removed daemon $(basename "$f")"
done

# --- boot out + remove any mesh LaunchAgents (system domain) ---
for f in /Library/LaunchAgents/meshagent*.plist /Library/LaunchAgents/com.meshagent*.plist; do
    [ -e "$f" ] || continue
    [ -n "$CUID" ] && launchctl bootout "gui/$CUID" "$f" 2>/dev/null
    launchctl bootout system "$f" 2>/dev/null
    rm -f "$f"; echo "  removed agent $(basename "$f")"
done

# --- per-user watcher for the console user ---
if [ -n "$CUSER" ] && [ "$CUSER" != root ]; then
    UA="/Users/$CUSER/Library/LaunchAgents/com.meshagent.tcc-watcher.plist"
    [ -n "$CUID" ] && launchctl bootout "gui/$CUID" "$UA" 2>/dev/null
    rm -f "$UA"
    rm -rf "/Users/$CUSER/Library/Application Support/MeshAgent"
fi

# --- kill stragglers ---
pkill -f "mesh_services/meshagent" 2>/dev/null
pkill -f "meshagent -kvmagent" 2>/dev/null
pkill -x meshagent 2>/dev/null

# --- remove program files ---
rm -rf /usr/local/mesh_services/meshagent
rmdir /usr/local/mesh_services 2>/dev/null
rm -f /tmp/meshagent-kvm* /tmp/meshagent-tcc-watcher.log /tmp/kvm_debug.log

# --- remove pf loopback bits (our :5900 block) ---
rm -f /etc/pf.anchors/meshagent-vnc.conf
pfctl -d >/dev/null 2>&1   # our anchor is gone; drop the block rule

echo
echo "============================================================"
echo " Program files + services removed."
echo
echo " NEXT — clear the stale permission so a reinstall prompts you:"
echo "   System Settings > Privacy & Security > Screen Recording"
echo "        select any 'meshagent' entry, click the (–) button"
echo "   (Do the same under Accessibility if 'meshagent' is listed.)"
echo
echo " Then REBOOT before reinstalling."
echo "============================================================"
