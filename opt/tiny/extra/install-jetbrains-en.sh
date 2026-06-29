#!/bin/bash
set -euo pipefail

if [[ $# -ne 2 ]] || [[ "$1" == "-h" ]] || [[ "$1" == "--help" ]]; then
    echo "Usage: $0 <name> <version>"
    echo "Example: $0 pycharm 2026.1.2"
    exit 0
fi

NAME="$1"
VERSION="$2"

URL="https://github.com/JonasGroeger/jetbrains-ppa/raw/master/deb/${NAME}_${VERSION}_all.deb"
TMP_DEB="/tmp/jetbrains.deb"
TMP_EXTRACT="/tmp/jetbrains-extracted"
TMP_MODIFIED="/tmp/jetbrains-modified.deb"

cleanup() {
    rm -rf "$TMP_DEB" "$TMP_EXTRACT" "$TMP_MODIFIED" 2>/dev/null || true
}
trap cleanup EXIT

echo "Downloading from $URL ..."
if ! curl -s -L -f --connect-timeout 10 --max-time 10 "$URL" -o "$TMP_DEB"; then
    echo "Error: download failed" >&2
    exit 1
fi

sudo dpkg-deb -R "$TMP_DEB" "$TMP_EXTRACT"
sed -i 's/\.tar\.gz/-aarch64\.tar\.gz/g' "$TMP_EXTRACT/DEBIAN/preinst"
sed -i 's/\.tar\.gz/-aarch64\.tar\.gz/g' "$TMP_EXTRACT/DEBIAN/postinst"
sudo dpkg-deb -b "$TMP_EXTRACT" "$TMP_MODIFIED"
sudo apt install -y "$TMP_MODIFIED"

echo "Installation completed."