#!/bin/bash
# Self-heal Windows (CRLF) line endings; this line works even with CRLF. Run as: bash displaylink-setup.sh
if grep -q $'\r' "$0"; then sed -i 's/\r$//' "$0"; exec bash "$0" "$@"; fi #
# Optional: DisplayLink USB docks and adapters as further PiNDI outputs ("external displays").
#
#   sudo bash displaylink-setup.sh --accept-displaylink-license      install (or update)
#   sudo bash displaylink-setup.sh --remove                          remove again
#
# Installs Synaptics' DisplayLink driver, which is closed source and comes under its own licence (shown
# at https://www.synaptics.com/products/displaylink-graphics/downloads/ubuntu; --accept-displaylink-license
# accepts it): its kernel module (evdi) is built with DKMS, so it is rebuilt for every new kernel, and
# DisplayLink Manager runs as pindi-displaylink.service. The player finds the dock's displays by itself.
# Needs internet access. Officially DisplayLink supports Ubuntu only; tested on Raspberry Pi OS (trixie,
# kernel 6.18) with a StarTech USB3DOCKH2DP.
set -Eeuo pipefail

VERSION=6.4.0-22
URL="https://www.synaptics.com/sites/default/files/exe_files/2026-10/DisplayLink%20USB%20Graphics%20Software%20for%20Ubuntu6.4-EXE.zip"
SHA256=7e1a81d2194a40b20de704cb3846d66cbfb1e7837e30de23455729ba14f0296d
DIR=/opt/pindi-displaylink
UNIT=pindi-displaylink.service

if [ "$(id -u)" -ne 0 ]; then
    exec sudo bash "$0" "$@"
fi
ACCEPT=0
REMOVE=0
for arg in "$@"; do
    case "$arg" in
        --accept-displaylink-license) ACCEPT=1 ;;
        --remove) REMOVE=1 ;;
        *) echo "Unknown option: $arg" >&2; exit 1 ;;
    esac
done
step() { echo; echo "==> $*"; }

if [ "$REMOVE" -eq 1 ]; then
    step "Removing DisplayLink"
    systemctl disable --now "$UNIT" >/dev/null 2>&1 || true
    rm -f "/etc/systemd/system/$UNIT" /etc/modules-load.d/pindi-evdi.conf /etc/modprobe.d/pindi-evdi.conf
    systemctl daemon-reload
    for v in $(dkms status evdi 2>/dev/null | sed -n 's|^evdi/\([^,: ]*\).*|\1|p' | sort -u); do
        dkms remove "evdi/$v" --all >/dev/null 2>&1 || true
        rm -rf "/usr/src/evdi-$v"
    done
    modprobe -r evdi 2>/dev/null || true
    rm -rf "$DIR"
    echo "DisplayLink removed. Restart the player (or reboot): sudo systemctl restart pindi"
    exit 0
fi

if [ "$ACCEPT" -ne 1 ]; then
    echo "This downloads and installs Synaptics' DisplayLink driver, which is closed source and subject to"
    echo "its own licence (see https://www.synaptics.com/products/displaylink-graphics/downloads/ubuntu)."
    read -r -p "Type y to accept the DisplayLink licence and continue: " REPLY
    [ "$REPLY" = "y" ] || [ "$REPLY" = "Y" ] || { echo "Aborted."; exit 1; }
fi

KVER=$(uname -r)
FLAVOUR=${KVER##*-}   # v8, 2712, v7, v7l: the headers meta package keeps headers for future kernels
step "Build tools and kernel headers ($KVER)"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends dkms gcc make libc6-dev curl python3 \
    "linux-headers-$KVER" "linux-headers-rpi-$FLAVOUR"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
step "Downloading DisplayLink $VERSION"
curl -sSfL -o "$TMP/dl.zip" "$URL"
echo "$SHA256  $TMP/dl.zip" | sha256sum -c --quiet || { echo "The download does not match the expected file." >&2; exit 1; }
python3 -c "import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$TMP/dl.zip" "$TMP"
sh "$TMP"/run/displaylink-driver-*.run --noexec --target "$TMP/pkg" >/dev/null
case "$(uname -m)" in
    aarch64) ARCH=aarch64-linux-gnu ;;
    armv7l|armv6l) ARCH=arm-linux-gnueabihf ;;
    x86_64) ARCH=x64-ubuntu-1604 ;;
    *) echo "No DisplayLink build for $(uname -m)" >&2; exit 1 ;;
esac

step "Kernel module (evdi, DKMS)"
mkdir -p "$TMP/evdi"
tar xzf "$TMP/pkg/evdi.tar.gz" -C "$TMP/evdi"
EVDI_VER=$(sed -n 's/^PACKAGE_VERSION="\{0,1\}\([^"]*\)"\{0,1\}$/\1/p' "$TMP/evdi/module/dkms.conf")
[ -n "$EVDI_VER" ] || { echo "Cannot read the evdi version" >&2; exit 1; }
for v in $(dkms status evdi 2>/dev/null | sed -n 's|^evdi/\([^,: ]*\).*|\1|p' | sort -u); do
    dkms remove "evdi/$v" --all >/dev/null 2>&1 || true   # an older one
    rm -rf "/usr/src/evdi-$v"
done
rm -rf "/usr/src/evdi-$EVDI_VER"
cp -r "$TMP/evdi/module" "/usr/src/evdi-$EVDI_VER"
dkms add "evdi/$EVDI_VER"
dkms build "evdi/$EVDI_VER"
dkms install "evdi/$EVDI_VER" --force
echo evdi > /etc/modules-load.d/pindi-evdi.conf
echo "options evdi initial_device_count=0" > /etc/modprobe.d/pindi-evdi.conf

step "DisplayLink Manager ($DIR)"
make -C "$TMP/evdi/library" -j"$(nproc)" >/dev/null
systemctl stop "$UNIT" >/dev/null 2>&1 || true
install -d "$DIR"
install -m 755 "$TMP/pkg/$ARCH/DisplayLinkManager" "$DIR/"
install -m 644 "$TMP/pkg/$ARCH"/libusb-1.0.so.* "$DIR/"
ln -sf "$(cd "$DIR" && ls libusb-1.0.so.*.* | head -n 1)" "$DIR/libusb-1.0.so.0"
cp -P "$TMP/evdi/library"/libevdi.so* "$DIR/"
install -m 644 "$TMP/pkg"/*.spkg "$TMP/pkg/LICENSE" "$TMP/pkg/3rd_party_licences.txt" "$DIR/"
cat > "/etc/systemd/system/$UNIT" <<EOF
[Unit]
Description=DisplayLink Manager (USB docks as PiNDI external displays)
After=systemd-modules-load.service

[Service]
Type=simple
ExecStartPre=/sbin/modprobe evdi
ExecStart=$DIR/DisplayLinkManager
WorkingDirectory=$DIR
Environment=LD_LIBRARY_PATH=$DIR
Restart=always
RestartSec=3
NoNewPrivileges=yes
ProtectHome=yes
PrivateTmp=yes

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
modprobe evdi
systemctl enable --now "$UNIT"

step "Done"
echo "DisplayLink $VERSION is running. Its displays appear as further outputs in the web interface"
echo "within a few seconds of a monitor being connected to the dock."
