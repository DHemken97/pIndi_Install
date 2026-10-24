#!/bin/bash
# Self-heal Windows (CRLF) line endings; this line works even with CRLF. Run as: bash install.sh
if grep -q $'\r' "$0"; then sed -i 's/\r$//' "$0"; exec bash "$0" "$@"; fi #
# PiNDI installer for Raspberry Pi OS Lite (Pi 3, 4 and 5; 32 or 64 bit). See README.md.
#
#   cd /home/pindi && bash install.sh [--accept-ndi-license] [--skip-upgrade] [--offline] [--no-reboot]
#
# Safe to run again (e.g. after changing Splash.jpg or pindi-player.c).
# --offline: no apt at all (the packages must already be installed); used by the web
# interface's software update. Without Install_NDI_SDK_v6_Linux.tar.gz next to this
# script, the NDI SDK that is already installed is kept.
set -Eeuo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "$(id -u)" -ne 0 ]; then
    exec sudo bash "$0" "$@"
fi

ACCEPT_NDI=0
UPGRADE=1
REBOOT=1
OFFLINE=0
for arg in "$@"; do
    case "$arg" in
        --accept-ndi-license) ACCEPT_NDI=1 ;;
        --skip-upgrade) UPGRADE=0 ;;
        --no-reboot) REBOOT=0 ;;
        --offline) OFFLINE=1 ;;
        *) echo "Unknown option: $arg" >&2; exit 1 ;;
    esac
done

step() { echo; echo "==> $*"; }

# Every program, script and unit file is replaced through put: atomically (a power cut leaves the old or
# the new file, never half of one), keeping the old one in $PREV. If the installation fails, the previous
# version's files come back; after a successful one they stay in /var/lib/pindi/previous.
PREV=/var/lib/pindi/previous.new
rm -rf "$PREV"
install -d -m 700 "$PREV"
put() { # put SRC DEST MODE
    local dest=$2
    if [ -e "$dest" ] && [ ! -e "$PREV$dest" ]; then
        install -d "$PREV$(dirname "$dest")"
        cp -a "$dest" "$PREV$dest"
    elif [ ! -e "$dest" ]; then
        echo "$dest" >> "$PREV/.added"
    fi
    install -m "$3" "$1" "$dest.pindi-new"
    mv -f "$dest.pindi-new" "$dest"
}
rollback() {
    trap - ERR
    echo "Installation failed: putting the previous version's files back." >&2
    (cd "$PREV" && find . -type f ! -name .added) | while IFS= read -r f; do
        f=${f#.}
        cp -a "$PREV$f" "$f.pindi-new" && mv -f "$f.pindi-new" "$f"
    done
    if [ -f "$PREV/.added" ]; then
        while IFS= read -r f; do rm -f "$f"; done < "$PREV/.added"
    fi
    systemctl daemon-reload || true
}
trap rollback ERR

# Files edited on Windows get CRLF line endings; normalise the ones we use.
sed -i 's/\r$//' "$SRC/pindi-player.c" "$SRC/pindi.service" "$SRC/pindi.conf" \
    "$SRC/pindi-web.py" "$SRC/pindi-web.service" "$SRC/pindi-webd.service" "$SRC/pindi-sender.c" \
    "$SRC/pindi-sender.service"

MODEL=$(tr -d '\0' < /proc/device-tree/model 2>/dev/null || true)
echo "Model: $MODEL"

BOOT=/boot/firmware
[ -f "$BOOT/config.txt" ] || BOOT=/boot

# ---------------------------------------------------------------- NDI license
if [ "$ACCEPT_NDI" -ne 1 ]; then
    echo "This installs the NDI SDK runtime, which is subject to the NDI SDK License"
    echo "Agreement (https://ndi.video/sdk/). The full text is copied to"
    echo "/usr/local/share/doc/ndi/ during installation."
    read -r -p "Type y to accept the NDI SDK license and continue: " REPLY
    [ "$REPLY" = "y" ] || [ "$REPLY" = "Y" ] || { echo "Aborted."; exit 1; }
fi

# ---------------------------------------------------------------- packages
PACKAGES=(gcc libc6-dev pkg-config
    libdrm-dev libfreetype-dev libjpeg-dev libturbojpeg0-dev libasound2-dev
    v4l-utils
    fonts-dejavu-core
    avahi-daemon libavahi-client3
    systemd-resolved
    python3 libpam-runtime
    openssl)  # web interface: HTTPS certificate, update signatures
# The NDI Advanced SDK decodes NDI|HX (H.264/HEVC) with FFmpeg 7's libavcodec, loaded at run time.
if [ -f "$SRC/Install_NDI_Advanced_SDK_v6_Linux.tar.gz" ]; then
    PACKAGES+=(libavcodec61)
fi
if [ "$OFFLINE" -eq 1 ]; then
    step "Checking packages (offline)"
    MISSING=()
    for p in "${PACKAGES[@]}"; do
        dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q "install ok installed" || MISSING+=("$p")
    done
    if [ "${#MISSING[@]}" -gt 0 ]; then
        echo "Missing packages: ${MISSING[*]}" >&2
        echo "This version needs them: run install.sh on the device with internet access." >&2
        exit 1
    fi
else
    step "Installing packages"
    export DEBIAN_FRONTEND=noninteractive
    APT_OPTS=(-y -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold)
    apt-get update
    if [ "$UPGRADE" -eq 1 ]; then
        apt-get "${APT_OPTS[@]}" full-upgrade
    fi
    apt-get "${APT_OPTS[@]}" install --no-install-recommends "${PACKAGES[@]}"
fi

# ---------------------------------------------------------------- NDI runtime
step "Installing NDI SDK runtime"
case "$(dpkg --print-architecture)" in
    arm64) NDI_ARCH=aarch64-rpi4-linux-gnueabi ;;
    armhf)
        case "$(uname -m)-$MODEL" in
            armv6*) NDI_ARCH=arm-rpi1-linux-gnueabihf ;;
            *"Pi 4"*|*"Pi 400"*|*"Pi 5"*|*"Pi 500"*|*"Compute Module 4"*|*"Compute Module 5"*)
                NDI_ARCH=arm-rpi4-linux-gnueabihf ;;
            *) NDI_ARCH=arm-rpi3-linux-gnueabihf ;;
        esac ;;
    amd64) NDI_ARCH=x86_64-linux-gnu ;;
    *) echo "Unsupported architecture $(dpkg --print-architecture)" >&2; exit 1 ;;
esac

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
# The NDI Advanced SDK (Install_NDI_Advanced_SDK_v6_Linux.tar.gz) is used when present.
NDI_KIND=""
for kind in Advanced_SDK SDK; do
    if [ -f "$SRC/Install_NDI_${kind}_v6_Linux.tar.gz" ]; then NDI_KIND=$kind; break; fi
done
if [ -z "$NDI_KIND" ] && [ -e /usr/local/lib/libndi.so ] &&
   [ -f /usr/local/include/ndi/Processing.NDI.Lib.h ]; then
    echo "No NDI SDK archive here: keeping the installed one ($(readlink /usr/local/lib/libndi.so))"
else
[ -n "$NDI_KIND" ] || { echo "Install_NDI_SDK_v6_Linux.tar.gz is missing" >&2; exit 1; }
NDI_TGZ="$SRC/Install_NDI_${NDI_KIND}_v6_Linux.tar.gz"
if [ "$NDI_KIND" = Advanced_SDK ]; then
    NDI_DIR="NDI Advanced SDK for Linux"; NDI_LICENSE="NDI Advanced License Agreement.txt"
else
    NDI_DIR="NDI SDK for Linux"; NDI_LICENSE="NDI SDK License Agreement.txt"
fi
# NDI publishes no checksums. Its SHA-256 is shown so it can be compared with a known-good copy; with
# <archive>.sha256 next to it (from "sha256sum <archive>"), a different file is refused.
echo "SDK archive SHA-256: $(sha256sum "$NDI_TGZ" | cut -d' ' -f1)"
if [ -f "$NDI_TGZ.sha256" ]; then
    (cd "$SRC" && sha256sum -c --status "$NDI_TGZ.sha256") ||
        { echo "$NDI_TGZ does not match $NDI_TGZ.sha256" >&2; exit 1; }
fi
# The vendor script is an interactive self-extractor; unpack its payload directly. It is
# streamed from the archive, not written out: /tmp is in RAM and the Advanced one is 350 MB.
LINE=$( (tar xzOf "$NDI_TGZ" || true) | awk '/^__NDI_ARCHIVE_BEGIN__/ { print NR + 1; exit 0; }')
tar xzOf "$NDI_TGZ" | tail -n +"$LINE" | tar xz -C "$TMP" \
    "$NDI_DIR/include" \
    "$NDI_DIR/lib/$NDI_ARCH" \
    "$NDI_DIR/$NDI_LICENSE" \
    "$NDI_DIR/licenses" \
    "$NDI_DIR/Version.txt"
SDK="$TMP/$NDI_DIR"
echo "$(cat "$SDK/Version.txt") ($NDI_ARCH, ${NDI_KIND//_/ })"

# libndi.so.6.x.y (standard) or libndi_advanced.so.6.x.y; both names point at it.
LIB=$(cd "$SDK/lib/$NDI_ARCH" && ls libndi*.so.*.*.* | head -n 1)
MAJOR=$(echo "$LIB" | sed 's/.*\.so\.\([0-9]*\)\..*/\1/')
rm -f /usr/local/lib/libndi.so* /usr/local/lib/libndi_advanced.so*
install -m 644 "$SDK/lib/$NDI_ARCH/$LIB" /usr/local/lib/
for name in libndi libndi_advanced; do
    ln -sf "$LIB" "/usr/local/lib/$name.so.$MAJOR"
    ln -sf "$LIB" "/usr/local/lib/$name.so"
done
ldconfig
install -d /usr/local/include/ndi /usr/local/share/doc/ndi
rm -rf /usr/local/include/ndi/*.h /usr/local/share/doc/ndi/*
install -m 644 "$SDK"/include/*.h /usr/local/include/ndi/
install -m 644 "$SDK/$NDI_LICENSE" /usr/local/share/doc/ndi/
cp -r "$SDK"/licenses/. /usr/local/share/doc/ndi/
fi

# ---------------------------------------------------------------- player
step "Building pindi-player and pindi-sender"
# 32-bit Raspberry Pi OS targets ARMv6 by default; enable NEON on ARMv7/v8 CPUs.
CPU_FLAGS=""
case "$NDI_ARCH" in
    arm-rpi3-linux-gnueabihf|arm-rpi4-linux-gnueabihf) CPU_FLAGS="-march=armv7-a -mfpu=neon-vfpv4" ;;
esac
# Both handle data from the network and from USB devices: buffer overflows abort instead of running on.
HARDEN="-D_FORTIFY_SOURCE=2 -fstack-protector-strong -Wformat -Werror=format-security -Wl,-z,relro,-z,now"
gcc -O2 -Wall $HARDEN $CPU_FLAGS -pthread -o "$TMP/pindi-player" "$SRC/pindi-player.c" \
    -I/usr/local/include/ndi $(pkg-config --cflags libdrm freetype2) \
    $(pkg-config --libs libdrm freetype2) -ljpeg -lasound -lm -ldl
gcc -O2 -Wall $HARDEN $CPU_FLAGS -pthread -o "$TMP/pindi-sender" "$SRC/pindi-sender.c" \
    -I/usr/local/include/ndi -lturbojpeg -lasound -lm -ldl
put "$TMP/pindi-player" /usr/local/bin/pindi-player 755
put "$TMP/pindi-sender" /usr/local/bin/pindi-sender 755

install -d /usr/share/pindi /etc/pindi
put "$SRC/Splash.jpg" /usr/share/pindi/splash.jpg 644
# Defaults for "Reset settings" in the web UI, and the version it shows.
put "$SRC/pindi.conf" /usr/share/pindi/pindi.conf 644
echo "$(tr -d '\r\n' < "$SRC/VERSION" 2>/dev/null || echo unknown) (installed $(date +%Y-%m-%d))" \
    > "$TMP/version"
put "$TMP/version" /usr/share/pindi/version 644
# Software updates from the web interface must be signed with this key's pair (make-update.sh).
if [ -f "$SRC/update-key.pub" ]; then
    put "$SRC/update-key.pub" /usr/share/pindi/update-key.pub 644
else
    echo "Warning: update-key.pub is missing: the web interface will refuse every software update." >&2
fi
if [ ! -f /etc/default/pindi ]; then
    install -m 644 "$SRC/pindi.conf" /etc/default/pindi
fi

# Remove the old fbi based splash, if a previous version was installed.
if [ -f /etc/systemd/system/pindi-splash.service ]; then
    systemctl disable pindi-splash.service || true
    rm -f /etc/systemd/system/pindi-splash.service
fi
rm -f /usr/local/bin/pindi-splash-render /usr/share/pindi/splash-live.jpg

put "$SRC/pindi.service" /etc/systemd/system/pindi.service 644
# The player runs unprivileged: this loads the display drivers (and the I2C drivers HDMI needs for EDID:
# Pi 3 i2c_bcm2835, Pi 4/5 i2c_brcmstb) as root at the same moment, instead of waiting for udev.
cat > "$TMP/pindi-drivers.service" <<'EOF'
[Unit]
Description=PiNDI: load the display drivers early
DefaultDependencies=no

[Service]
Type=oneshot
ExecStart=-/sbin/modprobe -q -a i2c_bcm2835 i2c_brcmstb vc4
EOF
put "$TMP/pindi-drivers.service" /etc/systemd/system/pindi-drivers.service 644

# The player and sender run as this unprivileged user; the display, decoder, capture, CEC and sound
# devices are open to these groups.
if ! id pindi-media >/dev/null 2>&1; then
    useradd --system --user-group --home-dir /var/lib/pindi-media --no-create-home \
        --shell /usr/sbin/nologin pindi-media
fi
for g in video audio render; do
    if getent group "$g" >/dev/null; then usermod -aG "$g" pindi-media; fi
done

# Web administration (HTTPS on port 443; port 80 redirects). Logins are the Pi's own users in the sudo group.
# Two services: the network-facing front end runs as the unprivileged pindi-web user; a root broker without
# network access (pindi-webd) signs users in and makes every change, asked over a local socket.
if ! id pindi-web >/dev/null 2>&1; then
    useradd --system --user-group --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin pindi-web
fi
# NDI config (pindi-media), capture modes and throttling (video), the logs page (systemd-journal).
for g in pindi-media video systemd-journal; do
    if getent group "$g" >/dev/null; then usermod -aG "$g" pindi-web; fi
done
install -d /usr/local/lib/pindi
put "$SRC/pindi-web.py" /usr/local/lib/pindi/pindi-web.py 755
# Optional DisplayLink docks as external displays: sudo bash /usr/local/lib/pindi/displaylink-setup.sh
if [ -f "$SRC/displaylink-setup.sh" ]; then
    put "$SRC/displaylink-setup.sh" /usr/local/lib/pindi/displaylink-setup.sh 755
fi
cat > /etc/pam.d/pindi-web <<'EOF'
# PiNDI web interface: same password check as a normal login / SSH.
@include common-auth
@include common-account
EOF
put "$SRC/pindi-webd.service" /etc/systemd/system/pindi-webd.service 644
put "$SRC/pindi-web.service" /etc/systemd/system/pindi-web.service 644
# The front end serves TLS: its key is readable by pindi-web (the broker keeps it that way).
if [ -d /etc/pindi/tls ]; then
    # A certificate from before certificates could be uploaded is the device's own: the web interface makes
    # it again for the current names and addresses (an empty list never matches).
    if [ -f /etc/pindi/tls/cert.pem ] && [ ! -f /etc/pindi/tls/self-signed ] && [ ! -f /etc/pindi/tls/uploaded ]; then
        : > /etc/pindi/tls/self-signed
    fi
    chown -R root:pindi-web /etc/pindi/tls
    chmod 750 /etc/pindi/tls
    chmod 640 /etc/pindi/tls/key.pem 2>/dev/null || true
fi

# Outgoing NDI streams from capture devices: does nothing until enabled in the web UI.
put "$SRC/pindi-sender.service" /etc/systemd/system/pindi-sender.service 644

# NDI sends each video frame as a burst of UDP packets, one burst per receiver. The 208 KB default
# socket buffer overflows with two receivers at 1080p: the kernel drops packets and every receiver's
# audio stalls for a second or two while they are resent. Device settings can turn this off
# (PINDI_NET_BUFFERS=0), which an update keeps.
if ! grep -q '^PINDI_NET_BUFFERS="\?0' /etc/default/pindi 2>/dev/null; then
    cat > /etc/sysctl.d/90-pindi-ndi.conf <<'EOF'
# Written by the PiNDI installer (Device settings: Large network buffers).
net.core.wmem_default = 4194304
net.core.rmem_default = 4194304
net.core.wmem_max = 8388608
net.core.rmem_max = 8388608
EOF
    sysctl -q -p /etc/sysctl.d/90-pindi-ndi.conf || true
fi

# Receive NDI over TCP: reliable UDP stalls the audio for a second or more when packets are lost.
# Only on devices that never chose a transport in Device settings (which writes both choices out).
python3 - <<'EOF'
import json, os
p = "/etc/pindi/ndi-config.v1.json"
try:
    cfg = json.load(open(p))
except (OSError, ValueError):
    cfg = {}
ndi = cfg.setdefault("ndi", {})
if "rudp" not in ndi:
    ndi["rudp"] = {"recv": {"enable": False}}
    ndi["unicast"] = {"recv": {"enable": False}}
    ndi["tcp"] = {"recv": {"enable": True}}
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w") as f:
        json.dump(cfg, f, indent=2)
        f.write("\n")
EOF
# It holds the NDI licence (vendor ID): readable by the media services, not by every user.
chown root:pindi-media /etc/pindi/ndi-config.v1.json
chmod 640 /etc/pindi/ndi-config.v1.json
chmod 755 /etc/pindi
# Left over from running as root (systemd hands it to pindi-media at the next start anyway).
if [ -d /run/pindi ]; then chown -R pindi-media:pindi-media /run/pindi; fi

# Operating-system updates: off unless a daily time is set (or "Install OS updates now") in the web
# interface. apt's own daily timers stay off (below), so nothing downloads or upgrades during a show.
cat > "$TMP/pindi-os-update.service" <<'EOF'
[Unit]
Description=PiNDI operating-system updates (web interface: Device settings / System)
After=network-online.target

[Service]
Type=oneshot
Nice=19
IOSchedulingClass=idle
CPUWeight=10
TimeoutStartSec=2h
StandardOutput=truncate:/var/log/pindi-os-update.log
StandardError=inherit
Environment=DEBIAN_FRONTEND=noninteractive
ExecStart=/bin/sh -c 'echo "Operating-system update, $$(date)"; if apt-get update && apt-get -y \
    -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold upgrade; then \
    echo PINDI-OS-UPDATE-OK; else echo PINDI-OS-UPDATE-FAILED; exit 1; fi'
EOF
put "$TMP/pindi-os-update.service" /etc/systemd/system/pindi-os-update.service 644

# Name lookups by LLMNR (systemd-resolved, port 5355 on every interface) are not needed: NDI and
# <name>.local use mDNS (avahi).
install -d /etc/systemd/resolved.conf.d
cat > /etc/systemd/resolved.conf.d/pindi.conf <<'EOF'
# Written by the PiNDI installer.
[Resolve]
LLMNR=no
EOF
systemctl try-restart systemd-resolved.service || true

systemctl daemon-reload
systemctl enable pindi.service pindi-webd.service pindi-web.service pindi-sender.service

# ---------------------------------------------------------------- boot speed
step "Optimising boot ($BOOT)"

# config.txt: no rainbow, no delays, no Wi-Fi (unless on in the web UI)/Bluetooth (the overlays are named
# differently on the Pi 5). Pi 3/4: 128 MB for the video unit (default 76), which
# holds every hardware H.264 decoder and encoder: with 76 a fourth one (e.g. two
# HX displays and two HX inputs) fails with "Not enough GPU mem". The initramfs is skipped only when booting from the SD
# card, whose driver and ext4 are built into the kernel; USB/NVMe boot keeps it.
CFG="$BOOT/config.txt"
[ -f "$CFG.pindi-orig" ] || cp "$CFG" "$CFG.pindi-orig"
INITRAMFS=1
case "$(findmnt -no SOURCE /)" in
    /dev/mmcblk*) INITRAMFS=0 ;;
esac
# Wi-Fi stays switched on when it was turned on in the web UI (Wi-Fi card).
WIFI="dtoverlay=disable-wifi"
if [ -L /etc/systemd/system/sys-subsystem-net-devices-wlan0.device.wants/wpa_supplicant@wlan0.service ]; then
    WIFI="#dtoverlay=disable-wifi"
fi
sed -i '/^# --- PiNDI begin ---$/,/^# --- PiNDI end ---$/d' "$CFG"
sed -i -E 's/^(camera_auto_detect|display_auto_detect|auto_initramfs|disable_splash|boot_delay|initial_turbo)=/#&/' "$CFG"
cat >> "$CFG" <<EOF
# --- PiNDI begin ---
[all]
disable_splash=1
boot_delay=0
initial_turbo=30
camera_auto_detect=0
display_auto_detect=0
auto_initramfs=$INITRAMFS
[pi3]
gpu_mem=128
$WIFI
dtoverlay=disable-bt
[pi4]
gpu_mem=128
$WIFI
dtoverlay=disable-bt
[pi5]
$WIFI-pi5
dtoverlay=disable-bt-pi5
[all]
# --- PiNDI end ---
EOF

# cmdline.txt: quiet boot, kernel messages off the screen, no serial console.
CMDLINE="$BOOT/cmdline.txt"
[ -f "$CMDLINE.pindi-orig" ] || cp "$CMDLINE" "$CMDLINE.pindi-orig"
set -f
NEW=""
for tok in $(tr -d '\r\n' < "$CMDLINE"); do
    case "$tok" in
        console=*|quiet|splash|loglevel=*|logo.nologo|vt.global_cursor_default=*|consoleblank=*|plymouth.*) ;;
        *) NEW="$NEW $tok" ;;
    esac
done
set +f
# Written next to it and renamed: a power cut must never leave an empty kernel command line (no boot).
echo "${NEW# } console=tty3 quiet loglevel=3 logo.nologo vt.global_cursor_default=0 consoleblank=0" \
    > "$CMDLINE.pindi-new"
sync "$CMDLINE.pindi-new"
mv -f "$CMDLINE.pindi-new" "$CMDLINE"

# Services not needed on a wired NDI display. apt's daily timers would download and upgrade at random
# times, during a show; OS updates run at a time set in the web interface instead (pindi-os-update).
unit_exists() { [ -n "$(systemctl list-unit-files --no-legend "$1" 2>/dev/null)" ]; }
for u in \
    getty@tty1.service \
    bluetooth.service hciuart.service wpa_supplicant.service ModemManager.service \
    NetworkManager-wait-online.service systemd-networkd-wait-online.service \
    console-setup.service keyboard-setup.service \
    e2scrub_reap.service udisks2.service triggerhappy.service \
    dphys-swapfile.service \
    apt-daily.timer apt-daily-upgrade.timer man-db.timer dpkg-db-backup.timer e2scrub_all.timer
do
    if unit_exists "$u"; then
        systemctl disable "$u" >/dev/null 2>&1 || true
    fi
done
# These get pulled in by other units / D-Bus even when disabled.
for u in bluetooth.service hciuart.service wpa_supplicant.service ModemManager.service \
         NetworkManager-wait-online.service apt-daily.service apt-daily-upgrade.service; do
    if unit_exists "$u"; then
        systemctl mask "$u" >/dev/null 2>&1 || true
    fi
done
# The Pi 3 has no bootloader EEPROM.
if ! grep -qE "Raspberry Pi (4|5|400|500|Compute Module (4|5))" /proc/device-tree/model 2>/dev/null; then
    unit_exists rpi-eeprom-update.service && systemctl disable rpi-eeprom-update.service >/dev/null 2>&1 || true
elif command -v rpi-eeprom-config >/dev/null; then
    # Pi 4/5: the bootloader draws its own screen on HDMI - the network-install prompt after every cold boot
    # (NET_INSTALL_AT_POWER_ON=1 in Raspberry Pi's images), flashing up before the splash. DISABLE_HDMI=1
    # keeps it off HDMI and skips network install (up to a second faster); boot errors still show on the
    # green LED. Written only when different; the EEPROM is updated at the next reboot.
    EEPROM=$(rpi-eeprom-config 2>/dev/null || true)
    if [ -n "$EEPROM" ] && ! { grep -qx "DISABLE_HDMI=1" <<<"$EEPROM" && grep -qx "NET_INSTALL_AT_POWER_ON=0" <<<"$EEPROM"; }; then
        { grep -v -E "^(DISABLE_HDMI|NET_INSTALL_AT_POWER_ON)=" <<<"$EEPROM"; printf '[all]\nDISABLE_HDMI=1\nNET_INSTALL_AT_POWER_ON=0\n'; } \
            > "$TMP/bootloader.conf"
        if rpi-eeprom-config --apply "$TMP/bootloader.conf" >/dev/null 2>&1; then
            echo "Bootloader: its HDMI screen switched off (applied at the next reboot)"
        else
            echo "Bootloader: could not update its configuration (it keeps its HDMI screen)" >&2
        fi
    fi
fi

# Camera / ISP drivers are never used; they cost boot time. The Pi 3/4 hardware H.264
# decoder (bcm2835_codec and what it needs) is kept when the NDI Advanced SDK is
# installed: the player uses it for NDI|HX.
{
    echo "# Installed by PiNDI: unused camera, ISP and V4L2 codec drivers."
    echo "blacklist bcm2835_v4l2"
    echo "blacklist bcm2835_isp"
    if [ ! -e /usr/local/lib/libndi_advanced.so ]; then
        echo "blacklist bcm2835_codec"
        echo "blacklist bcm2835_mmal_vchiq"
        echo "blacklist vc_sm_cma"
    fi
    echo "blacklist rpi_hevc_dec"
} > /etc/modprobe.d/pindi-blacklist.conf

# Unattended operation: the hardware watchdog reboots the device if the system hangs,
# and the journal stays small so it never fills the SD card.
install -d /etc/systemd/system.conf.d /etc/systemd/journald.conf.d
cat > /etc/systemd/system.conf.d/pindi-watchdog.conf <<'EOF'
[Manager]
RuntimeWatchdogSec=15s
RebootWatchdogSec=2min
EOF
cat > /etc/systemd/journald.conf.d/pindi.conf <<'EOF'
[Journal]
SystemMaxUse=64M
RuntimeMaxUse=32M
EOF

# cloud-init (Raspberry Pi Imager settings) has already done its job on first boot.
if [ -d /etc/cloud ]; then
    touch /etc/cloud/cloud-init.disabled
fi

# No swap: the player needs well under the Pi 3's 1 GB and swap setup costs seconds at boot.
if [ -f /etc/rpi/swap.conf ]; then
    sed -i '/^Mechanism=/d' /etc/rpi/swap.conf
    sed -i 's/^\[Main\]$/[Main]\nMechanism=none/' /etc/rpi/swap.conf
fi

systemctl enable avahi-daemon.service >/dev/null 2>&1 || true

# Wired DHCP via systemd-networkd: it starts several seconds before NetworkManager
# on a Pi 3, so the IP and NDI sources appear much sooner. Same DHCP client ID
# (MAC) as NetworkManager, so the device keeps its address/reservation.
# For a static IP edit /etc/systemd/network/20-pindi-wired.network
# (Address=, Gateway=, DNS= under [Network], and remove DHCP=yes).
if [ ! -f /etc/systemd/network/20-pindi-wired.network ]; then
    cat > /etc/systemd/network/20-pindi-wired.network <<'EOF'
[Match]
Type=ether

[Network]
DHCP=yes

[DHCPv4]
ClientIdentifier=mac
EOF
fi
systemctl enable systemd-networkd.service systemd-resolved.service
# Enabling networkd also enables its wait-online service (disabled above): nothing here waits for it.
systemctl disable systemd-networkd-wait-online.service >/dev/null 2>&1 || true

# Clock: the Pi has no RTC. Public NTP servers in addition to any from DHCP or the
# web UI: timesyncd rotates through all of them, so a DHCP-advertised server that
# doesn't answer no longer leaves the clock unsynced (latency figures need it).
install -d /etc/systemd/timesyncd.conf.d
cat > /etc/systemd/timesyncd.conf.d/pindi.conf <<'EOF'
[Time]
NTP=0.debian.pool.ntp.org 1.debian.pool.ntp.org 2.debian.pool.ntp.org 3.debian.pool.ntp.org
EOF
systemctl restart systemd-timesyncd.service || true
for u in NetworkManager.service NetworkManager-dispatcher.service; do
    if unit_exists "$u"; then
        systemctl disable "$u" >/dev/null 2>&1 || true
    fi
done

# Installed: the files this run replaced become the "previous version" (kept for a manual rollback).
sync
trap - ERR
rm -rf /var/lib/pindi/previous
mv "$PREV" /var/lib/pindi/previous

step "Done"
echo "Serial: $(tr -d '\0' < /sys/firmware/devicetree/base/serial-number)"
echo "NDI stream to send: OUT_$(tr -d '\0' < /sys/firmware/devicetree/base/serial-number)"
echo "Settings: /etc/default/pindi   Logs: journalctl -u pindi -f"
echo "Web admin: https://<this device's IP>/ (log in as a sudo user, e.g. the SSH user; the browser warns"
echo "  about the device's own certificate the first time)"
echo "Original boot files saved as $CFG.pindi-orig and $CMDLINE.pindi-orig; undo everything: uninstall.sh"
if [ "$REBOOT" -eq 1 ]; then
    echo "Rebooting in 5 seconds (Ctrl+C to cancel)..."
    sleep 5
    systemctl reboot
fi
