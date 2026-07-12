#!/bin/bash
# MeshAgent macOS installer (double-click launcher).
# Guides the 3 manual steps and runs the hosted install script.
# CONFIGURE: replace __HOST__ below with your MeshCentral host before hosting this file.
clear
cat <<'BANNER'
============================================================
              MeshAgent macOS installer
============================================================
BANNER
echo
echo "STEP 1 of 3 — Turn ON Screen Sharing (needed for login-screen control):"
echo "    System Settings  >  General  >  Sharing  >  Screen Sharing   (ON)"
echo
printf "  Press Return once Screen Sharing is ON... "
read _
echo
echo "STEP 2 of 3 — Installing the agent (you will be asked for your password)..."
echo
if ! curl -fsSL https://__HOST__/meshsetup/meshinstall.sh | sudo bash; then
    echo
    echo "  !! Install failed — check the network and run this again."
    printf "  Press Return to close... "; read _
    exit 1
fi
echo
echo "STEP 3 of 3 — Grant Screen Recording (needed for the remote video):"
echo "    System Settings  >  Privacy & Security  >  Screen Recording"
echo "        enable  meshagent"
echo "    (Optional: also enable meshagent under Accessibility for in-session"
echo "     mouse/keyboard, not just the login screen.)"
echo
echo "------------------------------------------------------------"
echo "  Done. The Mac will appear in your MeshCentral console"
echo "  under the configured landing group. Move it to its real group there."
echo "------------------------------------------------------------"
echo
printf "  Press Return to close this window... "; read _
