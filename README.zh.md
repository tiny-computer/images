# images
System files for Tiny Container!

[🇺🇸 English](README.md) | 🇨🇳 中文

这个仓库存放用于小小容器（Tiny Container）软件的容器文件，以及它们的制作指导。

> **Note**
> 这不是我制作已发布的容器的过程，只是制作一个能被小小容器识别的容器文件的指导。

嗯，releases页面有一些我手动制作好的容器。和以前一样，我还是不会自动

## 简介

通常容器文件就是一个.tar.zst压缩包，但是为了能让软件识别，还必须在根目录加入一个配置文件/.tiny.yaml。

.tiny.yaml的格式可以参考仓库的proot-distro.tiny.yaml。这是一个我尝试将安装在proot-distro的容器打包导入小小容器所使用的配置文件。虽然看着很长，但大多数都是不必要的快捷命令（quick_commands）和选项配置（options），所以不必担心。

/.tiny.yaml应该在压缩包的最前面以便软件快速识别。打包方式可以参考/opt/tiny/export.sh。

## 容器制作

之前，小小电脑（Tiny Computer）的容器是使用tmoe制作的，那么这次就用proot-distro试试！

先在proot-distro新安装debian trixie容器并进入，然后依次执行下面的指令。

下面的指令不是自动化脚本。

```bash

# 这些操作可能对你来说不全是必要的，所以按需使用！

# 改 DNS 服务器
cat >/etc/resolv.conf <<EOF
nameserver 114.114.114.114
nameserver 114.114.115.115
nameserver 8.8.8.8
nameserver 8.8.4.4
EOF

# 改镜像源
cat >/etc/apt/sources.list.d/debian.sources <<EOF
Types: deb
URIs: http://ftp.cn.debian.org/debian/
Suites: trixie trixie-updates trixie-backports
Components: main contrib non-free non-free-firmware
Signed-By: /usr/share/keyrings/debian-archive-keyring.pgp

Types: deb
URIs: http://security.debian.org/debian-security/
Suites: trixie-security
Components: main contrib non-free non-free-firmware
Signed-By: /usr/share/keyrings/debian-archive-keyring.pgp
EOF

# 更新
apt update
apt upgrade -y
apt install dialog -y # 以便可以配置一些选项而不是默认
apt install locales -y # 为了切换语言和地区
apt install sudo zstd -y # sudo 是为了给 tiny 用户赋予权限，zstd 打包容器的格式

# 切换中文
sed -i 's/^# *zh_CN.UTF-8/zh_CN.UTF-8/' /etc/locale.gen
grep -q '^zh_CN.UTF-8' /etc/locale.gen || echo 'zh_CN.UTF-8 UTF-8' >>/etc/locale.gen
locale-gen zh_CN.UTF-8

cat >/etc/default/locale <<EOF
LANG=zh_CN.UTF-8
LANGUAGE=zh_CN:zh
LC_ALL=zh_CN.UTF-8
EOF

echo 'Asia/Shanghai' >/etc/timezone
ln -sf /usr/share/zoneinfo/Asia/Shanghai /etc/localtime

# 新建 tiny 用户，密码 tiny；root 用户密码也改为 tiny。
# 这是为了和示例配置文件配合，因为配置文件就是以 tiny 用户登录的。
# 我不考虑容器的安全性，一切为了易于使用。tiny 用户使用 sudo 不需要输入密码。
useradd -m -s /bin/bash -u 1000 tiny
echo 'tiny:tiny' | chpasswd
echo 'root:tiny' | chpasswd
echo 'tiny ALL=(ALL:ALL) NOPASSWD:ALL' >>/etc/sudoers
chmod 0440 /etc/sudoers

# 在图形界面中 tiny 执行特权操作时不再需要输入密码。
mkdir -p /etc/polkit-1/rules.d/
cat >/etc/polkit-1/rules.d/99-allow-all.rules <<EOF
polkit.addRule(function(action, subject) {
    if (subject.user == "tiny") {
        return polkit.Result.YES;
    }
});
EOF

# 不需要 termux 对容器的初始化设置
rm /etc/profile.d/termux-profile.sh
```

然后准备好你的 .tiny.yaml 配置文件即可。如果你使用示例配置文件，它得配合一些 /opt/tiny 的文件，也需要准备好，可以从我发布的容器中复制出来。这基本上就是这个项目的前身小小电脑中的 patch.tar.gz，文件来源可以去那边找到，新添加的文件的源码在这里给出。

现在容器就准备好了。打包可以参考 /opt/tiny/export.sh 的命令！

## 图形界面

现在，你的容器应该可以在小小容器软件打开了。下面的工作可以在小小容器完成。

```bash
# 安装xfce4，vnc服务器，dbus-launch命令，pipewire，wget和curl（许多以前写的快捷命令需要它们）
sudo apt update
sudo apt install dbus-x11 xfce4 tigervnc-standalone-server pipewire wget curl
# 由于使用pipewire，不再需要pulseaudio
sudo apt autoremove --purge pulseaudio

# 安装一些基本软件。如浏览器，图像查看，压缩文件管理，deb包安装。
# 这里的火狐浏览器同时安装了中文语言包，你可能不需要，请注意
sudo apt install firefox-esr firefox-esr-l10n-zh-cn xarchiver unrar ristretto gdebi mousepad xfce4-terminal

# 安装字体
sudo apt install fonts-wqy-zenhei fonts-wqy-microhei ttf-mscorefonts-installer

# xfce的经典菜单插件
sudo apt install xfce4-whiskermenu-plugin
```

然后是一些美化工作。我打算安装[vinceliuice的Fluent主题](https://github.com/vinceliuice/Fluent-gtk-theme)。

```bash
cat > install-fluent-theme.sh << 'EOF'
#!/bin/bash
set -euo pipefail

TMPDIR=$(mktemp -d /tmp/fluent-theme-XXXXXX)
trap 'rm -rf "$TMPDIR"' EXIT

echo "=== [1/4] 安装依赖 ==="
sudo apt-get update
sudo apt-get install -y git sassc gtk2-engines-murrine gnome-themes-extra

echo "=== [2/4] 克隆仓库 ==="
git clone --depth 1 https://github.com/vinceliuice/Fluent-gtk-theme.git   "$TMPDIR/gtk-theme"
git clone --depth 1 https://github.com/vinceliuice/Fluent-icon-theme.git  "$TMPDIR/icon-theme"
git clone --depth 1 -b Wallpaper https://github.com/vinceliuice/Fluent-gtk-theme.git "$TMPDIR/wallpaper"

echo "=== [3/4] 安装 GTK 主题 ==="
cd "$TMPDIR/gtk-theme"
./install.sh

echo "=== [4/4] 安装图标主题 ==="
cd "$TMPDIR/icon-theme"
./install.sh 
# -n Fluent

echo "=== [5/5] 安装壁纸 ==="
cd "$TMPDIR/wallpaper"
./install-wallpapers.sh

echo ""
echo "=== 安装完成，临时文件已自动清理 ==="
echo "在 外观设置 和 窗口管理器 中手动选择 Fluent 主题即可。"
EOF

chmod +x install-fluent-theme.sh
```
执行install-fluent-theme.sh，然后就安装好主题了。

现在可以去"功能"页面AVNC或Termux:X11的编辑中取消命令的注释了。启用任一前端然后重启容器，你应该可以成功进入图形界面了！
