#!/bin/bash
# Self-heal Windows (CRLF) line endings; this line works even with CRLF. Run as: bash uninstall.sh
if grep -q $'\r' "$0"; then sed -i 's/\r$//' "$0"; exec bash "$0" "$@"; fi #
# Undo install.sh: remove PiNDI and give the system back its own boot settings, services and networking
# (NetworkManager). Settings are deleted too: download a settings backup in the web interface first.
#
#   sudo bash uninstall.sh [--keep-ndi-sdk] [--no-reboot]
#
# Packages install.sh added (compilers, libraries) stay installed; remove them with apt if wanted.
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    exec sudo bash "$0" "$@"
fi
KEEP_SDK=0
REBOOT=1
for arg in "$@"; do
    case "$arg" in
        --keep-ndi-sdk) KEEP_SDK=1 ;;
        --no-reboot) REBOOT=0 ;;
        *) echo "Unknown option: $arg" >&2; exit 1 ;;
    esac
done
read -r -p "Remove PiNDI and all its settings from this device? Type y to continue: " REPLY
[ "$REPLY" = "y" ] || [ "$REPLY" = "Y" ] || { echo "Aborted."; exit 1; }

step() { echo; echo "==> $*"; }
unit_exists() { [ -n "$(systemctl list-unit-files --no-legend "$1" 2>/dev/null)" ]; }

step "Stopping and removing the PiNDI services"
for u in pindi.service pindi-sender.service pindi-web.service pindi-webd.service pindi-reboot.timer pindi-os-update.timer \
         pindi-os-update.service pindi-reboot.service; do
    systemctl disable --now "$u" >/dev/null 2>&1 || true
done
rm -f /etc/systemd/system/pindi.service /etc/systemd/system/pindi-sender.service \
      /etc/systemd/system/pindi-web.service /etc/systemd/system/pindi-webd.service /etc/systemd/system/pindi-reboot.timer \
      /etc/systemd/system/pindi-reboot.service /etc/systemd/system/pindi-os-update.timer \
      /etc/systemd/system/pindi-os-update.service /etc/systemd/system/pindi-drivers.service
rm -f /usr/local/bin/pindi-player /usr/local/bin/pindi-sender /etc/pam.d/pindi-web
rm -rf /usr/local/lib/pindi /usr/share/pindi /etc/pindi /var/lib/pindi /var/lib/pindi-media /run/pindi \
       /run/pindi-ctl /run/pindi-web /run/pindi-webd
rm -f /etc/default/pindi /var/log/pindi-update.log /var/log/pindi-os-update.log
for u in pindi-media pindi-web; do
    if id "$u" >/dev/null 2>&1; then userdel "$u" || true; fi
done
if [ "$KEEP_SDK" -eq 0 ]; then
    rm -f /usr/local/lib/libndi.so* /usr/local/lib/libndi_advanced.so*
    rm -rf /usr/local/include/ndi /usr/local/share/doc/ndi
    ldconfig
fi

step "System settings"
rm -f /etc/sysctl.d/90-pindi-ndi.conf /etc/modprobe.d/pindi-blacklist.conf \
      /etc/systemd/system.conf.d/pindi-watchdog.conf /etc/systemd/journald.conf.d/pindi.conf \
      /etc/systemd/timesyncd.conf.d/pindi.conf /etc/systemd/resolved.conf.d/pindi.conf
if [ -f /etc/rpi/swap.conf ]; then sed -i '/^Mechanism=none$/d' /etc/rpi/swap.conf; fi

step "Networking back to NetworkManager"
# Wi-Fi as set up by the web interface.
rm -f /etc/systemd/system/sys-subsystem-net-devices-wlan0.device.wants/wpa_supplicant@wlan0.service
rm -rf /etc/systemd/system/wpa_supplicant@wlan0.service.d
rm -f /etc/wpa_supplicant/wpa_supplicant-wlan0.conf
# A static address set in the web interface is lost: set it again with nmtui / nmcli.
rm -f /etc/systemd/network/20-pindi-wired.network /etc/systemd/network/25-pindi-wifi.network
systemctl disable systemd-networkd.service systemd-networkd.socket >/dev/null 2>&1 || true

step "Services PiNDI switched off: back to the system's defaults"
for u in getty@tty1.service bluetooth.service hciuart.service wpa_supplicant.service ModemManager.service \
         NetworkManager.service NetworkManager-dispatcher.service NetworkManager-wait-online.service \
         systemd-networkd-wait-online.service console-setup.service keyboard-setup.service \
         e2scrub_reap.service udisks2.service triggerhappy.service dphys-swapfile.service \
         apt-daily.service apt-daily-upgrade.service apt-daily.timer apt-daily-upgrade.timer man-db.timer \
         dpkg-db-backup.timer e2scrub_all.timer rpi-eeprom-update.service; do
    if unit_exists "$u"; then
        systemctl unmask "$u" >/dev/null 2>&1 || true
        systemctl preset "$u" >/dev/null 2>&1 || true
    fi
done
rm -f /etc/cloud/cloud-init.disabled

step "Boot configuration"
BOOT=/boot/firmware
[ -f "$BOOT/config.txt" ] || BOOT=/boot
CFG="$BOOT/config.txt"
CMDLINE="$BOOT/cmdline.txt"
# config.txt: drop PiNDI's block and give back the lines it commented out (other edits are kept).
sed -i '/^# --- PiNDI begin ---$/,/^# --- PiNDI end ---$/d' "$CFG"
sed -i -E 's/^#((camera_auto_detect|display_auto_detect|auto_initramfs|disable_splash|boot_delay|initial_turbo)=)/\1/' "$CFG"
# cmdline.txt was rewritten as a whole: the original comes back (written next to it, then renamed).
if [ -f "$CMDLINE.pindi-orig" ]; then
    cp "$CMDLINE.pindi-orig" "$CMDLINE.pindi-new"
    sync "$CMDLINE.pindi-new"
    mv -f "$CMDLINE.pindi-new" "$CMDLINE"
    rm -f "$CMDLINE.pindi-orig" "$CFG.pindi-orig"
fi
systemctl daemon-reload
sync

step "Done"
echo "PiNDI is removed. The device starts with the normal console and NetworkManager after a reboot."
if [ "$REBOOT" -eq 1 ]; then
    echo "Rebooting in 5 seconds (Ctrl+C to cancel)..."
    sleep 5
    systemctl reboot
fi
