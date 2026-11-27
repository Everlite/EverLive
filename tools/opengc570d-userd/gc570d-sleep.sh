#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# System sleep/resume helper for OpenGC570D Userspace Driver.

set -eu

case "${1:-post}" in
    pre)
        exit 0
        ;;
    post|resume|*)
        /usr/local/libexec/gc570d-vfio-bind.sh --bind || true
        sleep 1
        for rdir in /run/user/*; do
            [ -d "$rdir" ] || continue
            uid=$(basename "$rdir")
            user=$(id -nu "$uid" 2>/dev/null || true)
            [ -n "$user" ] || continue
            # Reinit over the control socket is debug-gated; restart the
            # user unit instead so resume recovery never depends on it.
            systemctl --machine="${user}@.host" --user try-restart opengc570d-userd.service >/dev/null 2>&1 || true
        done
        ;;
esac
