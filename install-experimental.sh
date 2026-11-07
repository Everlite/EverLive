#!/usr/bin/env bash
# ==============================================================================
# install-experimental.sh — DEPRECATED
# ==============================================================================
# The former "Experimental" channel (lsfg-vk frame generation, side-by-side
# launcher/config/socket) has been merged into the regular EverLive release.
# There is no separate experimental install any more.
# ==============================================================================
set -euo pipefail

cat >&2 <<'EOF'
[!] install-experimental.sh is obsolete.

    The former "Experimental" channel is now part of the regular EverLive
    release: frame generation, the Console Hub and the userspace VFIO engine
    all ship in a single install with one set of paths:

        Launcher : ~/.local/share/applications/everlive.desktop
        Config   : ~/.config/everlive/
        Socket   : $XDG_RUNTIME_DIR/everlive.sock

    Use the regular installer instead:

        sudo ./install.sh

    It also removes leftover everlive-experimental.desktop entries from the
    earlier naming phase. To uninstall, run:  sudo ./uninstall.sh
EOF

exit 1
