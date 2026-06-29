#!/bin/bash
set -euo pipefail

if [[ $# -ne 2 ]] || [[ "$1" == "-h" ]] || [[ "$1" == "--help" ]]; then
    echo "用法: $0 <软件名称> <版本号>"
    echo "示例: $0 pycharm 2026.1.2"
    exit 0
fi

NAME="$1"
VERSION="$2"

MIRRORS=(
    "https://gh-proxy.org/"
    "https://cdn.gh-proxy.org/"
    "https://edgeone.gh-proxy.org/"
    "https://gh.llkk.cc/"
    "https://github.moeyy.xyz/"
    "https://mirror.ghproxy.com/"
    ""
)

DEB_URL_BASE="https://github.com/JonasGroeger/jetbrains-ppa/raw/master/deb/${NAME}_${VERSION}_all.deb"
TMP_DEB="/tmp/jetbrains.deb"
TMP_EXTRACT="/tmp/jetbrains-extracted"
TMP_MODIFIED="/tmp/jetbrains-modified.deb"

cleanup() {
    rm -rf "$TMP_DEB" "$TMP_EXTRACT" "$TMP_MODIFIED" 2>/dev/null || true
}
trap cleanup EXIT

for mirror in "${MIRRORS[@]}"; do
    url="${mirror}${DEB_URL_BASE}"
    echo "尝试从 ${mirror:-直接} 下载..."
    if curl -s -L -f --connect-timeout 10 --max-time 10 "$url" -o "$TMP_DEB"; then
        echo "下载成功"
        break
    fi
done

if [[ ! -f "$TMP_DEB" ]]; then
    echo "错误：所有镜像下载失败" >&2
    exit 1
fi

sudo dpkg-deb -R "$TMP_DEB" "$TMP_EXTRACT"
sed -i 's/\.tar\.gz/-aarch64\.tar\.gz/g' "$TMP_EXTRACT/DEBIAN/preinst"
sed -i 's/\.tar\.gz/-aarch64\.tar\.gz/g' "$TMP_EXTRACT/DEBIAN/postinst"
sudo dpkg-deb -b "$TMP_EXTRACT" "$TMP_MODIFIED"
sudo apt install -y "$TMP_MODIFIED"

echo "安装完成"