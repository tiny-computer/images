# images
System files for Tiny Container!

🇺🇸 English | [🇨🇳 中文](README.zh.md)

This repository stores container files for the Tiny Container (小小容器) app, along with guides for creating them.

> **Note**
> This is not the process I use to build and publish containers. It is only a guide for creating a container file that can be recognized by Tiny Container.

You can find some manually built containers on the releases page.

## Introduction

A container file is typically a `.tar.zst` archive. For the software to recognize it, you must also include a configuration file at the root: `/.tiny.yaml`.

You can refer to `proot-distro.tiny.yaml` in this repo for the `.tiny.yaml` format. It's an example configuration I used to package a container installed via proot-distro for import into Tiny Container. Don't worry about its length — most of it consists of optional quick commands and options.

The `/.tiny.yaml` file should be placed at the very beginning of the archive so the software can detect it quickly. See `/opt/tiny/export.sh` for packaging instructions.

## Container Creation

Previously, containers for Tiny Computer (小小电脑) were built using tmoe. This time, let's try proot-distro!

First, install a fresh Debian Trixie container in proot-distro, enter it, then run the following commands in order.

These commands are not an automation script.

```bash

# Not all of these steps may be necessary for you — use only what you need!

# Change DNS servers
# cat >/etc/resolv.conf <<EOF
# nameserver 114.114.114.114
# nameserver 114.114.115.115
# nameserver 8.8.8.8
# nameserver 8.8.4.4
# EOF

# Change APT mirror
# cat >/etc/apt/sources.list.d/debian.sources <<EOF
# Types: deb
# URIs: http://ftp.cn.debian.org/debian/
Suites: trixie trixie-updates trixie-backports
Components: main contrib non-free non-free-firmware
# Signed-By: /usr/share/keyrings/debian-archive-keyring.pgp

# Types: deb
# URIs: http://security.debian.org/debian-security/
# Suites: trixie-security
Components: main contrib non-free non-free-firmware
# Signed-By: /usr/share/keyrings/debian-archive-keyring.pgp
# EOF

# Update
apt update
apt upgrade -y
apt install dialog -y     # So you can configure some options rather than using defaults
apt install locales -y    # To switch language and region
apt install sudo zstd -y  # sudo: grant privileges to tiny user; zstd: format for packaging containers

# Switch locale
sed -i 's/^# *en_US.UTF-8/en_US.UTF-8/' /etc/locale.gen
grep -q '^en_US.UTF-8' /etc/locale.gen || echo 'en_US.UTF-8 UTF-8' >>/etc/locale.gen
locale-gen en_US.UTF-8

cat >/etc/default/locale <<EOF
LANG=en_US.UTF-8
LANGUAGE=en_US:en
LC_ALL=en_US.UTF-8
EOF

echo 'America/New_York' >/etc/timezone
ln -sf /usr/share/zoneinfo/America/New_York /etc/localtime

# Create tiny user with password "tiny"; also set root password to "tiny".
# This matches the sample config, which logs in as the tiny user.
# I don't care about container security — everything is optimized for ease of use.
# The tiny user can sudo without entering a password.
useradd -m -s /bin/bash -u 1000 tiny
echo 'tiny:tiny' | chpasswd
echo 'root:tiny' | chpasswd
echo 'tiny ALL=(ALL:ALL) NOPASSWD:ALL' >>/etc/sudoers
chmod 0440 /etc/sudoers

# Allow tiny to perform privileged GUI operations without password.
mkdir -p /etc/polkit-1/rules.d/
cat >/etc/polkit-1/rules.d/99-allow-all.rules <<EOF
polkit.addRule(function(action, subject) {
    if (subject.user == "tiny") {
        return polkit.Result.YES;
    }
});
EOF

# Remove termux initialization for the container
rm /etc/profile.d/termux-profile.sh
```

Then prepare your `.tiny.yaml` configuration file. If you use the sample config, you'll also need to prepare the files under `/opt/tiny` — you can copy them from a container I've released. This is essentially the `patch.tar.gz` from the predecessor project Tiny Computer. The source code for newly added files is provided here.

Your container is now ready! Refer to the commands in `/opt/tiny/export.sh` for packaging.

## Graphical Interface

At this point, your container should be openable in the Tiny Container app. The remaining work can be done inside Tiny Container.

```bash
# Install xfce4, VNC server, dbus-launch, pipewire, wget and curl (needed by many quick commands)
sudo apt update
sudo apt install dbus-x11 xfce4 tigervnc-standalone-server pipewire wget curl -y
# Since we're using pipewire, we no longer need pulseaudio
sudo apt autoremove --purge pulseaudio -y

# Install some basic applications: browser, image viewer, archive manager, .deb installer.
# Note: Firefox ESR with Chinese language pack is installed — you may not need the language pack.
sudo apt install firefox-esr xarchiver unrar ristretto gdebi mousepad xfce4-terminal -y # firefox-esr-l10n-zh-cn

# Install Chinese fonts
# sudo apt install fonts-wqy-zenhei fonts-wqy-microhei ttf-mscorefonts-installer

# Classic XFCE menu plugin
sudo apt install xfce4-whiskermenu-plugin -y
```

Now for some theming. I plan to install [vinceliuice's Fluent theme](https://github.com/vinceliuice/Fluent-gtk-theme).

```bash
cat > install-fluent-theme.sh << 'EOF'
#!/bin/bash
set -euo pipefail

TMPDIR=$(mktemp -d /tmp/fluent-theme-XXXXXX)
trap 'rm -rf "$TMPDIR"' EXIT

echo "=== [1/4] Installing dependencies ==="
sudo apt-get update
sudo apt-get install -y git sassc gtk2-engines-murrine gnome-themes-extra

echo "=== [2/4] Cloning repositories ==="
git clone --depth 1 https://github.com/vinceliuice/Fluent-gtk-theme.git   "$TMPDIR/gtk-theme"
git clone --depth 1 https://github.com/vinceliuice/Fluent-icon-theme.git  "$TMPDIR/icon-theme"
git clone --depth 1 -b Wallpaper https://github.com/vinceliuice/Fluent-gtk-theme.git "$TMPDIR/wallpaper"

echo "=== [3/4] Installing GTK theme ==="
cd "$TMPDIR/gtk-theme"
./install.sh

echo "=== [4/4] Installing icon theme ==="
cd "$TMPDIR/icon-theme"
./install.sh
# -n Fluent

echo "=== [5/5] Installing wallpaper ==="
cd "$TMPDIR/wallpaper"
./install-wallpapers.sh

echo ""
echo "=== Installation complete. Temporary files have been cleaned up. ==="
echo "Manually select the Fluent theme in Appearance Settings and Window Manager."
EOF

chmod +x install-fluent-theme.sh
```
Run `install-fluent-theme.sh` and the theme will be installed.

Now go to the "Features" page and uncomment the commands under AVNC or Termux:X11. Enable either frontend, restart the container, and you should be able to enter the graphical interface!
