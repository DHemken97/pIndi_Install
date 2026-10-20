#!/bin/bash
# Build a PiNDI software update package for the web interface (System -> Software update).
# The NDI SDK archive is left out: devices keep the SDK they have.
#   bash make-update.sh            -> pindi-update-<version>.tar.gz
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
VERSION=$(tr -d '\r\n' < VERSION)
OUT="pindi-update-$VERSION.tar.gz"
FILES=(VERSION README.md install.sh pindi.conf Splash.jpg
       pindi-player.c pindi-sender.c pindi-web.py
       pindi.service pindi-sender.service pindi-web.service)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir "$TMP/pindi-$VERSION"
cp "${FILES[@]}" "$TMP/pindi-$VERSION/"
sed -i 's/\r$//' "$TMP/pindi-$VERSION/"*.{sh,c,py,service,conf} "$TMP/pindi-$VERSION/VERSION"
tar czf "$OUT" -C "$TMP" "pindi-$VERSION"
echo "$OUT ($(du -k "$OUT" | cut -f1) KB)"
