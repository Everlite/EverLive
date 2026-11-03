#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# System sleep/resume helper for OpenGC570D Userspace Driver.

set -eu

case "${1:-post}" in
    pre)
        exit 0
        ;;
    post|resume|*)
        /usr/local/libexec/gc570d-vfio-bind.sh || true
        sleep 1
        for rdir in /run/user/*; do
            [ -d "$rdir" ] || continue
            uid=$(basename "$rdir")
            user=$(id -nu "$uid" 2>/dev/null || true)
            [ -n "$user" ] || continue
            if [ -S "$rdir/gc570d/control.sock" ]; then
                su - "$user" -c "XDG_RUNTIME_DIR=$rdir /usr/local/bin/gc570d-ctl reinit" >/dev/null 2>&1 || \
                systemctl --machine="${user}@.host" --user restart opengc570d-userd.service >/dev/null 2>&1 || true
            else
                systemctl --machine="${user}@.host" --user try-restart opengc570d-userd.service >/dev/null 2>&1 || true
            fi
        done
        ;;
esac
