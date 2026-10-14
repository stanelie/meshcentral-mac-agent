#!/bin/bash
# MeshAgent FULL uninstall (macOS).
# Run:  curl -fsSL https://__HOST__/meshsetup/uninstall.sh | sudo bash
#   (or just: sudo bash uninstall.sh)
[ "$(id -u)" = 0 ] || { echo "Please run with sudo."; exit 1; }

PRESTATE="/usr/local/mesh_services/meshagent/meshagent/kvm/prestate.env"
PRIOR_SS=""; PRIOR_LEGACY=""
# shellcheck disable=SC1090
[ -r "$PRESTATE" ] && . "$PRESTATE"

# Who is the console user?
#
# NOT from /dev/console ownership. That is what this used to do, and it is
# unreliable: measured on a VM with user1 logged in and active,
#     stat -f %Su /dev/console  ->  root
#     scutil State:/Users/ConsoleUser Name  ->  user1
# With CUSER=root every block below is skipped in silence -- the permission
# walkthrough, the TCC watcher, and the per-user LaunchAgent bootstrap -- and the
# install still reports success. That is almost certainly behind more than one
# "the installer did nothing" report.
#
# SCDynamicStore is the authority (it is what the agent itself uses). Fall back
# to /dev/console only if that yields nothing, and treat root or loginwindow as
# "nobody is logged in".
CUSER=$(echo "show State:/Users/ConsoleUser" | /usr/sbin/scutil 2>/dev/null | /usr/bin/awk '/Name :/ { print $3 }')
case "$CUSER" in loginwindow|root|"") CUSER="" ;; esac
if [ -z "$CUSER" ]; then
    CUSER=$(stat -f%Su /dev/console 2>/dev/null || echo "")
    case "$CUSER" in root|"") CUSER="" ;; esac
fi
if [ -n "$CUSER" ]; then CUID=$(id -u "$CUSER" 2>/dev/null || echo ""); else CUID=""; fi
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

# --- per-user watcher: every home directory, not just the console user ---
for UH in /Users/*; do
    [ -d "$UH" ] || continue
    case "$(basename "$UH")" in Shared|Guest) continue ;; esac
    UA="$UH/Library/LaunchAgents/com.meshagent.tcc-watcher.plist"
    if [ -e "$UA" ]; then
        UUID=$(stat -f%u "$UH" 2>/dev/null)
        [ -n "$UUID" ] && launchctl bootout "gui/$UUID" "$UA" 2>/dev/null
        rm -f "$UA"; echo "  removed watcher for $(basename "$UH")"
    fi
    rm -rf "$UH/Library/Application Support/MeshAgent"
done

# --- kill stragglers ---
pkill -f "mesh_services/meshagent" 2>/dev/null
pkill -f "meshagent -kvmagent" 2>/dev/null
pkill -x meshagent 2>/dev/null

# --- remove program files ---
rm -rf /usr/local/mesh_services/meshagent
rmdir /usr/local/mesh_services 2>/dev/null
rm -f /tmp/meshagent-kvm* /tmp/meshagent-clip.sock /tmp/meshagent-tcc-watcher.log /tmp/kvm_debug.log

# --- remove our pf ruleset and restore the stock one ---
rm -f /etc/pf.anchors/meshagent-vnc.conf
# Do NOT "pfctl -d": pf enable/disable is reference counted and other software
# (VPN clients, Internet Sharing) may depend on it being up. Reloading Apple's
# default ruleset removes our :5900 rules and filters nothing else.
[ -r /etc/pf.conf ] && pfctl -f /etc/pf.conf >/dev/null 2>&1
echo "  restored stock pf ruleset (/etc/pf.conf)"

# --- restore the Screen Sharing / legacy-VNC state we found at install time ---
# Leaving legacy VNC on with our random password would keep an extra
# authentication surface open on :5900 after the agent is gone.
KICKSTART=/System/Library/CoreServices/RemoteManagement/ARDAgent.app/Contents/Resources/kickstart
if [ "$PRIOR_LEGACY" = "0" ] || [ -z "$PRIOR_LEGACY" ]; then
    [ -x "$KICKSTART" ] && "$KICKSTART" -configure -clientopts -setvnclegacy -vnclegacy no >/dev/null 2>&1
    echo "  disabled legacy VNC (it was off before install)"
fi
if [ "$PRIOR_SS" = "off" ]; then
    launchctl bootout system /System/Library/LaunchDaemons/com.apple.screensharing.plist 2>/dev/null
    launchctl disable system/com.apple.screensharing 2>/dev/null
    echo "  turned Screen Sharing back off (it was off before install)"
elif [ -n "$PRIOR_SS" ]; then
    echo "  left Screen Sharing enabled (it was already on before install)"
fi

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
