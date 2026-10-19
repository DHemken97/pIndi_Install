#!/bin/bash
# Self-heal Windows (CRLF) line endings; this line works even with CRLF. Run as: bash install.sh
if grep -q $'\r' "$0"; then sed -i 's/\r$//' "$0"; exec bash "$0" "$@"; fi #
# PiNDI installer for Raspberry Pi OS Lite (Pi 3B, 32 or 64 bit).
#
#   cd /home/pindi && bash install.sh [--accept-ndi-license] [--skip-upgrade] [--no-reboot]
#
# Safe to run again (e.g. after changing Splash.jpg or pindi-player.c).
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "$(id -u)" -ne 0 ]; then
    exec sudo bash "$0" "$@"
fi

ACCEPT_NDI=0
UPGRADE=1
REBOOT=1
for arg in "$@"; do
    case "$arg" in
        --accept-ndi-license) ACCEPT_NDI=1 ;;
        --skip-upgrade) UPGRADE=0 ;;
        --no-reboot) REBOOT=0 ;;
        *) echo "Unknown option: $arg" >&2; exit 1 ;;
    esac
done

step() { echo; echo "==> $*"; }

# Files edited on Windows get CRLF line endings; normalise the ones we use.
sed -i 's/\r$//' "$SRC/pindi-player.c" "$SRC/pindi.service" "$SRC/pindi.conf"

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
step "Installing packages"
export DEBIAN_FRONTEND=noninteractive
APT_OPTS=(-y -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold)
apt-get update
if [ "$UPGRADE" -eq 1 ]; then
    apt-get "${APT_OPTS[@]}" full-upgrade
fi
apt-get "${APT_OPTS[@]}" install --no-install-recommends \
    gcc libc6-dev pkg-config \
    libdrm-dev libfreetype-dev libjpeg-dev \
    fonts-dejavu-core \
    avahi-daemon libavahi-client3 \
    systemd-resolved

# ---------------------------------------------------------------- NDI runtime
step "Installing NDI SDK runtime"
case "$(dpkg --print-architecture)" in
    arm64) NDI_ARCH=aarch64-rpi4-linux-gnueabi ;;
    armhf)
        case "$(uname -m)" in
            armv6*) NDI_ARCH=arm-rpi1-linux-gnueabihf ;;
            *) NDI_ARCH=arm-rpi3-linux-gnueabihf ;;
        esac ;;
    amd64) NDI_ARCH=x86_64-linux-gnu ;;
    *) echo "Unsupported architecture $(dpkg --print-architecture)" >&2; exit 1 ;;
esac

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
tar xzf "$SRC/Install_NDI_SDK_v6_Linux.tar.gz" -C "$TMP"
NDI_SH="$TMP/Install_NDI_SDK_v6_Linux.sh"
# The vendor script is an interactive self-extractor; unpack its payload directly.
LINE=$(awk '/^__NDI_ARCHIVE_BEGIN__/ { print NR + 1; exit 0; }' "$NDI_SH")
tail -n +"$LINE" "$NDI_SH" | tar xz -C "$TMP" \
    "NDI SDK for Linux/include" \
    "NDI SDK for Linux/lib/$NDI_ARCH" \
    "NDI SDK for Linux/NDI SDK License Agreement.txt" \
    "NDI SDK for Linux/licenses" \
    "NDI SDK for Linux/Version.txt"
SDK="$TMP/NDI SDK for Linux"
echo "$(cat "$SDK/Version.txt") ($NDI_ARCH)"

LIB=$(cd "$SDK/lib/$NDI_ARCH" && ls libndi.so.*.*.* | head -n 1)
rm -f /usr/local/lib/libndi.so*
install -m 644 "$SDK/lib/$NDI_ARCH/$LIB" /usr/local/lib/
ln -sf "$LIB" "/usr/local/lib/$(echo "$LIB" | cut -d . -f -3)"
ln -sf "$LIB" /usr/local/lib/libndi.so
ldconfig
install -d /usr/local/include/ndi /usr/local/share/doc/ndi
install -m 644 "$SDK"/include/*.h /usr/local/include/ndi/
install -m 644 "$SDK/NDI SDK License Agreement.txt" "$SDK"/licenses/* /usr/local/share/doc/ndi/

# ---------------------------------------------------------------- player
step "Building pindi-player"
# 32-bit Raspberry Pi OS targets ARMv6 by default; enable NEON on ARMv7/v8 CPUs.
CPU_FLAGS=""
if [ "$NDI_ARCH" = arm-rpi3-linux-gnueabihf ]; then
    CPU_FLAGS="-march=armv7-a -mfpu=neon-vfpv4"
fi
gcc -O2 -Wall $CPU_FLAGS -o "$TMP/pindi-player" "$SRC/pindi-player.c" \
    -I/usr/local/include/ndi $(pkg-config --cflags libdrm freetype2) \
    $(pkg-config --libs libdrm freetype2) -ljpeg -lm -ldl
install -m 755 "$TMP/pindi-player" /usr/local/bin/pindi-player

install -d /usr/share/pindi
install -m 644 "$SRC/Splash.jpg" /usr/share/pindi/splash.jpg
if [ ! -f /etc/default/pindi ]; then
    install -m 644 "$SRC/pindi.conf" /etc/default/pindi
fi

# Remove the old fbi based splash, if a previous version was installed.
if [ -f /etc/systemd/system/pindi-splash.service ]; then
    systemctl disable pindi-splash.service || true
    rm -f /etc/systemd/system/pindi-splash.service
fi
rm -f /usr/local/bin/pindi-splash-render /usr/share/pindi/splash-live.jpg

install -m 644 "$SRC/pindi.service" /etc/systemd/system/pindi.service
systemctl daemon-reload
systemctl enable pindi.service

# ---------------------------------------------------------------- boot speed
step "Optimising boot ($BOOT)"

# config.txt: no rainbow, no delays, no Wi-Fi/Bluetooth, no initramfs
# (ext4 and the SD card driver are built into the Raspberry Pi kernel).
CFG="$BOOT/config.txt"
[ -f "$CFG.pindi-orig" ] || cp "$CFG" "$CFG.pindi-orig"
sed -i '/^# --- PiNDI begin ---$/,/^# --- PiNDI end ---$/d' "$CFG"
sed -i -E 's/^(camera_auto_detect|display_auto_detect|auto_initramfs|disable_splash|boot_delay|initial_turbo)=/#&/' "$CFG"
cat >> "$CFG" <<'EOF'
# --- PiNDI begin ---
[all]
disable_splash=1
boot_delay=0
initial_turbo=30
camera_auto_detect=0
display_auto_detect=0
auto_initramfs=0
dtoverlay=disable-wifi
dtoverlay=disable-bt
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
echo "${NEW# } console=tty3 quiet loglevel=3 logo.nologo vt.global_cursor_default=0 consoleblank=0" > "$CMDLINE"

# Services not needed on a wired NDI display.
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
fi

# Camera / ISP / hardware codec drivers are never used; they cost boot time.
cat > /etc/modprobe.d/pindi-blacklist.conf <<'EOF'
# Installed by PiNDI: unused camera, ISP and V4L2 codec drivers.
blacklist bcm2835_v4l2
blacklist bcm2835_isp
blacklist bcm2835_codec
blacklist bcm2835_mmal_vchiq
blacklist vc_sm_cma
blacklist rpi_hevc_dec
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
for u in NetworkManager.service NetworkManager-dispatcher.service; do
    if unit_exists "$u"; then
        systemctl disable "$u" >/dev/null 2>&1 || true
    fi
done

step "Done"
echo "Serial: $(tr -d '\0' < /sys/firmware/devicetree/base/serial-number)"
echo "NDI stream to send: OUT_$(tr -d '\0' < /sys/firmware/devicetree/base/serial-number)"
echo "Settings: /etc/default/pindi   Logs: journalctl -u pindi -f"
echo "Original boot files saved as $CFG.pindi-orig and $CMDLINE.pindi-orig"
if [ "$REBOOT" -eq 1 ]; then
    echo "Rebooting in 5 seconds (Ctrl+C to cancel)..."
    sleep 5
    systemctl reboot
fi
