#!/bin/bash

echo "Updating packages..."
sudo apt update
sudo -E env DEBIAN_FRONTEND=noninteractive apt -y -o Dpkg::Options::="--force-confdef" -o Dpkg::Options::="--force-confold" upgrade

REPO="AndreRH/hangover"
API_URL="https://api.github.com/repos/$REPO/releases/latest"

# Get the latest release version number and store it in a variable
latest_version=$(curl -s $API_URL | grep -oP '"tag_name": "\Khangover-\K([^"]+)' )

# Check if version number was successfully retrieved
if [ -z "$latest_version" ]; then
  echo "Unable to fetch the latest version number."
  exit 1
fi

echo "Latest version: $latest_version"

hangover_url="https://github.com/AndreRH/hangover/releases/download/hangover-${latest_version}/hangover_${latest_version}_debian13_trixie_arm64.tar"

mirror_sites=(
    ""
)

mkdir -p /tmp/hangover
cd /tmp/hangover

for mirror in "${mirror_sites[@]}"; do
    url="${mirror}${hangover_url}"
    echo "Attempting to download Hangover from $url..."
    curl -L -o hangover.tar "${url}" --fail --silent --show-error
    if [ $? -eq 0 ]; then
        echo "Successfully downloaded Hangover"
        break
    fi
    if [ -z "$mirror" ]; then
        cd /tmp
        rm -rf /tmp/hangover
        echo "Download failed...exiting installation..."
        exit
    fi
done

echo "Installing Hangover..."
tar xvf hangover.tar
sudo apt install -y ./hangover*.deb
if [ $? -ne 0 ]; then
    cd /tmp
    rm -rf /tmp/hangover
    echo "Installation failed...exiting installation..."
    exit 1
fi

echo "Initializing Wine..."
wineboot --init

echo "Installing DXVK..."
tar xvf dxvk-v*.tar.gz
mv dxvk-v*/x32/* /home/tiny/.wine/drive_c/windows/syswow64
mv dxvk-v*/arm64ec/* /home/tiny/.wine/drive_c/windows/system32

echo "Fixing fonts..."
regedit "Z:\\opt\\tiny\\extra\\chn_fonts.reg" && wine reg delete "HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes" /va /f

cd /tmp
rm -rf /tmp/hangover
echo "Installation complete"