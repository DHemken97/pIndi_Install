#!/bin/bash
# Build a signed PiNDI software update package for the web interface (System -> Software update).
# The NDI SDK archive is left out: devices keep the SDK they have.
#   bash make-update.sh            -> pindi-update-<version>.tar.gz
#
# Devices only install packages signed with the private key whose public half (update-key.pub) they
# were installed with. The private key stays on this computer, outside the repository:
#   PINDI_SIGNING_KEY (default ~/.pindi/update-signing-key.pem), made on the first run.
# Back it up: without it, devices can only be updated by running install.sh on them.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
KEY=${PINDI_SIGNING_KEY:-$HOME/.pindi/update-signing-key.pem}
if [ ! -f "$KEY" ]; then
    if [ -f update-key.pub ]; then
        echo "The signing key $KEY is missing, but update-key.pub exists: restore the key from your backup" >&2
        echo "(or delete update-key.pub to start a new key pair; installed devices then need install.sh)." >&2
        exit 1
    fi
    mkdir -p "$(dirname "$KEY")"
    (umask 077 && openssl genpkey -algorithm ed25519 -out "$KEY")
    openssl pkey -in "$KEY" -pubout -out update-key.pub
    echo "New signing key: $KEY (back it up). Its public key is in update-key.pub: commit that."
fi
if [ "$(openssl pkey -in "$KEY" -pubout | tr -d '\r')" != "$(tr -d '\r' < update-key.pub)" ]; then
    echo "$KEY is not the key for update-key.pub: devices would refuse the package." >&2
    exit 1
fi

VERSION=$(tr -d '\r\n' < VERSION)
OUT="pindi-update-$VERSION.tar.gz"
FILES=(VERSION README.md install.sh uninstall.sh pindi.conf Splash.jpg update-key.pub
       pindi-player.c pindi-sender.c pindi-web.py
       pindi.service pindi-sender.service pindi-web.service)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
PKG="$TMP/pindi-$VERSION"
mkdir "$PKG"
cp "${FILES[@]}" "$PKG/"
sed -i 's/\r$//' "$PKG/"*.{sh,c,py,service,conf,pub} "$PKG/VERSION"
# Every file's hash, signed: the device checks both before it runs anything from the package.
(cd "$PKG" && sha256sum "${FILES[@]}" > SHA256SUMS)
openssl pkeyutl -sign -inkey "$KEY" -rawin -in "$PKG/SHA256SUMS" -out "$PKG/SHA256SUMS.sig"
tar czf "$OUT" -C "$TMP" "pindi-$VERSION"
echo "$OUT ($(du -k "$OUT" | cut -f1) KB, signed)"
