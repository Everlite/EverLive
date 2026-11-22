#!/usr/bin/env bash
# ==============================================================================
# EverLive - Deinstaller (v2.0.1)
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "$EUID" -ne 0 ]; then
    echo "Bitte mit sudo ausführen: sudo ./uninstall.sh"
    exit 1
fi

TARGET_USER="${SUDO_USER:-$USER}"
TARGET_UID="$(id -u "${TARGET_USER}" 2>/dev/null || echo 0)"
# Resolve the home directory without eval-ing user-controlled text.
TARGET_HOME="$(getent passwd "${TARGET_USER}" | cut -d: -f6)"
[ -n "${TARGET_HOME}" ] || TARGET_HOME="/home/${TARGET_USER}"

# Remove distribution-package files only when no package owns them, so
# uninstall.sh never leaves a package with missing files.
is_package_owned() {
    local path="$1"

    if command -v pacman >/dev/null 2>&1 && pacman -Qqo "${path}" >/dev/null 2>&1; then
        return 0
    fi
    if command -v dpkg >/dev/null 2>&1 && dpkg -S "${path}" >/dev/null 2>&1; then
        return 0
    fi
    if command -v rpm >/dev/null 2>&1 && rpm -qf "${path}" >/dev/null 2>&1; then
        return 0
    fi
    return 1
}

remove_unowned() {
    local path

    for path in "$@"; do
        if [ ! -e "${path}" ] && [ ! -L "${path}" ]; then
            continue
        fi
        if is_package_owned "${path}"; then
            echo "[*] Behalte paketverwaltete Datei: ${path}"
            continue
        fi
        rm -rf "${path}"
    done
}

echo "[*] Deinstalliere EverLive..."

# 1. Laufende Daemons beenden
killall -9 everlive_userd gc573_user_streamer opengc570d-userd gc570d-ctl 2>/dev/null || true
EVERLIVE_RUNTIME="${XDG_RUNTIME_DIR:-/run/user/${TARGET_UID}}"
rm -f "${EVERLIVE_RUNTIME}/gc570d/control.sock" 2>/dev/null || true
rm -f "${EVERLIVE_RUNTIME}/everlive.sock" "${EVERLIVE_RUNTIME}/everlive.sock.lock" 2>/dev/null || true
rm -f /dev/shm/gc570d_v4l2_shm 2>/dev/null || true
rm -f /dev/shm/gc573_v4l2_shm 2>/dev/null || true

# 2. Systemweite App-Installation entfernen (Hub, Launcher, Desktop, Icons)
if [ -f "${SCRIPT_DIR}/Makefile" ]; then
    make -C "${SCRIPT_DIR}" uninstall-app >/dev/null 2>&1 || true
fi
rm -rf /usr/local/lib/everlive
rm -f /usr/local/bin/everlive /usr/local/bin/everlive_camd
rm -f /usr/local/lib/libeverlive-cam-v4l2.so
rm -rf /usr/local/share/doc/everlive
rm -f /usr/local/share/applications/everlive.desktop
rm -f /usr/local/share/icons/hicolor/*/apps/everlive.png
rm -f /usr/local/share/pixmaps/everlive.bmp

# 3. VFIO & Memlock Regeln entfernen
rm -f /etc/udev/rules.d/99-gc573-vfio.rules
rm -f /etc/security/limits.d/99-everlive-memlock.conf
# GC573 engine binaries:
if [ -d "${SCRIPT_DIR}/tools/opengc573-userd" ]; then
    make -C "${SCRIPT_DIR}/tools/opengc573-userd" uninstall >/dev/null 2>&1 || true
fi
rm -f /usr/local/bin/gc573_user_streamer /usr/local/bin/everlive_userd /usr/local/bin/gc573_user_probe
# GC570D (DUO) packaging installed through tools/opengc570d-userd/Makefile:
# binaries, udev rule, memlock limits, systemd user unit and sleep hook.
if [ -d "${SCRIPT_DIR}/tools/opengc570d-userd" ]; then
    make -C "${SCRIPT_DIR}/tools/opengc570d-userd" uninstall >/dev/null 2>&1 || true
fi
rm -f /etc/security/limits.d/99-gc570d-memlock.conf
rm -f /usr/lib/systemd/system-sleep/gc570d-sleep.sh
rm -f /etc/gc570d.conf.example
rm -f /usr/local/bin/opengc570d-userd /usr/local/bin/gc570d-ctl
rm -f /usr/local/lib/libgc570d-v4l2.so
rm -f /usr/local/lib/libgc573-v4l2.so
rm -f /usr/local/lib/libeverlive-vk-pacer.so
rm -f /usr/local/libexec/gc570d-vfio-bind.sh /usr/local/libexec/gc570d-sleep.sh
rm -f /usr/local/lib/systemd/user/opengc570d-userd.service
rm -f /etc/udev/rules.d/99-gc570d-vfio.rules

# libeverlive-core (everlive-ctl, library, headers, service unit, service script)
if [ -d "${SCRIPT_DIR}/tools/libeverlive-core" ]; then
    make -C "${SCRIPT_DIR}/tools/libeverlive-core" uninstall >/dev/null 2>&1 || true
fi
rm -f /usr/local/bin/everlive-ctl /usr/local/bin/everlive-service
rm -f /usr/local/lib/libeverlive_core.so* /usr/local/lib/libeverlive_core.a
rm -f /usr/local/include/everlive_card.h /usr/local/include/everlive_frame.h \
      /usr/local/include/everlive_framestore.h \
      /usr/local/include/everlive_ctl.h /usr/local/include/everlive_json.h \
      /usr/local/include/everlive_ipc.h /usr/local/include/everlive_handoff.h
rm -f /usr/local/lib/systemd/user/everlive.service
rm -f /usr/local/bin/vfio_pci_bind.sh

# 4. Legacy Kernel-Treiber & DKMS Reste entfernen (falls vorhanden)
rmmod open_gc573 2>/dev/null || true
rm -rf "/lib/modules/$(uname -r)/updates/open_gc573" 2>/dev/null || true
depmod -a "$(uname -r)" 2>/dev/null || true
if command -v dkms >/dev/null 2>&1; then
    dkms remove -m everlive --all 2>/dev/null || true
    rm -rf /usr/src/everlive-* 2>/dev/null || true
fi
rm -f /etc/modules-load.d/everlive.conf 2>/dev/null || true
rm -f /etc/modules-load.d/everlive-vfio.conf 2>/dev/null || true
rm -f /etc/modprobe.d/everlive-vfio.conf 2>/dev/null || true
rm -f /etc/modules-load.d/everlive-v4l2loopback.conf 2>/dev/null || true
rm -f /etc/modprobe.d/everlive-v4l2loopback.conf 2>/dev/null || true
rm -f /etc/modprobe.d/blacklist-everlive-legacy.conf 2>/dev/null || true
rm -f /etc/pipewire/pipewire.conf.d/50-everlive-capture.conf 2>/dev/null || true
rm -f /etc/udev/rules.d/70-everlive.rules 2>/dev/null || true
rm -f /usr/local/bin/everlive-audio 2>/dev/null || true

# Virtual Camera entladen (das Paket v4l2loopback bleibt installiert).
modprobe -r v4l2loopback 2>/dev/null || true

udevadm control --reload-rules 2>/dev/null || true
udevadm trigger 2>/dev/null || true

# 5. Desktop-Einträge & Icons entfernen
#
# Remove both the current launcher and the leftover entry from the earlier
# "experimental" naming phase of this project.
APPS_DIR="${TARGET_HOME}/.local/share/applications"
rm -f "${APPS_DIR}/everlive.desktop"
rm -f "${APPS_DIR}/everlive-experimental.desktop"
rm -f "${APPS_DIR}/everlive-intern.desktop"

# Remove reversible "(EverLive Camera)" LD_PRELOAD shortcuts and strip the
# injection from same-name launcher overrides created by older installers.
if [ -d "${APPS_DIR}" ]; then
    for desktop in "${APPS_DIR}"/*.desktop; do
        [ -f "${desktop}" ] || continue
        if grep -q '^X-EverLive-Preload=true' "${desktop}" 2>/dev/null; then
            rm -f "${desktop}"
            continue
        fi
        if grep -qE '^Exec=env LD_PRELOAD=.*libgc(573|570d)-v4l2\.so' "${desktop}" 2>/dev/null; then
            owner="$(stat -c '%u:%g' "${desktop}" 2>/dev/null || true)"
            mode="$(stat -c '%a' "${desktop}" 2>/dev/null || true)"
            sed -i -E 's|^Exec=env LD_PRELOAD=[^ ]+libgc(573|570d)-v4l2\.so |Exec=|' "${desktop}" || true
            if [ -n "${owner}" ]; then chown "${owner}" "${desktop}" 2>/dev/null || true; fi
            if [ -n "${mode}" ]; then chmod "${mode}" "${desktop}" 2>/dev/null || true; fi
        fi
    done
fi
rm -f "${TARGET_HOME}/.config/autostart/everlive.desktop"
rmdir "${TARGET_HOME}/.config/everlive-experimental" 2>/dev/null || true
rm -f "${TARGET_HOME}/.local/share/icons/hicolor/"*/apps/everlive.png
# install.sh also copies the launcher/icons system-wide, while `make install`
# stores its own copy under /usr/local.  Keep files owned by a distribution
# package, and remove everything else.
remove_unowned /usr/local/share/applications/everlive.desktop
remove_unowned /usr/local/share/icons/hicolor/*/apps/everlive.png
remove_unowned /usr/local/share/pixmaps/everlive.bmp
remove_unowned /usr/share/applications/everlive.desktop
remove_unowned /usr/share/icons/hicolor/*/apps/everlive.png
remove_unowned /usr/share/pixmaps/everlive.bmp
for cache in "${TARGET_HOME}/.local/share/applications/mimeinfo.cache" \
             "${TARGET_HOME}/.local/share/applications/intent.cache" \
             "${TARGET_HOME}/.local/share/icons/hicolor/icon-theme.cache"; do
    if [ -e "${cache}" ]; then
        chown "${TARGET_USER}:${TARGET_USER}" "${cache}" 2>/dev/null || true
    fi
done
if command -v update-desktop-database >/dev/null 2>&1; then
    if [ "${TARGET_USER}" != "root" ]; then
        sudo -u "${TARGET_USER}" env "HOME=${TARGET_HOME}" \
            update-desktop-database "${TARGET_HOME}/.local/share/applications" 2>/dev/null || true
    else
        update-desktop-database "${TARGET_HOME}/.local/share/applications" 2>/dev/null || true
    fi
    for dir in /usr/local/share/applications /usr/share/applications; do
        if [ -d "${dir}" ]; then
            update-desktop-database "${dir}" 2>/dev/null || true
        fi
    done
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    if [ "${TARGET_USER}" != "root" ]; then
        sudo -u "${TARGET_USER}" env "HOME=${TARGET_HOME}" \
            gtk-update-icon-cache -f -t "${TARGET_HOME}/.local/share/icons/hicolor" 2>/dev/null || true
    else
        gtk-update-icon-cache -f -t "${TARGET_HOME}/.local/share/icons/hicolor" 2>/dev/null || true
    fi
    for dir in /usr/local/share/icons/hicolor /usr/share/icons/hicolor; do
        if [ -d "${dir}" ]; then
            gtk-update-icon-cache -f -t "${dir}" 2>/dev/null || true
        fi
    done
fi
if command -v kbuildsycoca6 >/dev/null 2>&1; then
    if [ "${TARGET_USER}" != "root" ]; then
        sudo -u "${TARGET_USER}" env "HOME=${TARGET_HOME}" \
            kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
    else
        kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
    fi
elif command -v kbuildsycoca5 >/dev/null 2>&1; then
    if [ "${TARGET_USER}" != "root" ]; then
        sudo -u "${TARGET_USER}" env "HOME=${TARGET_HOME}" \
            kbuildsycoca5 --noincremental >/dev/null 2>&1 || true
    else
        kbuildsycoca5 --noincremental >/dev/null 2>&1 || true
    fi
fi

echo "[+] EverLive wurde sauber deinstalliert."
