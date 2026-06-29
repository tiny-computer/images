
# cleaning steps from tmoe
apt clean;apt autoclean;apt autoremove --purge -y || apt autoremove -y
cd /
rm -rvf tmp/.* tmp/* home/tiny/.cache home/tiny/.ICEauthority home/tiny/.Xauthority home/tiny/.bash_history home/tiny/.pki home/tiny/.chord home/tiny/.cocomusic.json home/tiny/.dbus home/tiny/.gnupg home/tiny/.gridea home/tiny/.mozilla home/tiny/.petal.db home/tiny/.config/Electron home/tiny/.config/Netron home/tiny/.config/chord home/tiny/.config/electron-netease-cloud-music home/tiny/.config/go-for-it home/tiny/.config/gridea home/tiny/.config/listen1 home/tiny/.config/lx-music-desktop home/tiny/.config/neofetch home/tiny/.config/netease-cloud-music-gtk home/tiny/.config/pulse home/tiny/.config/tenvideo_universal home/tiny/.xfce4-session.verbose-log home/tiny/.xfce4-session.verbose-log.last home/tiny/.zcompdump-localhost* home/tiny/.z home/tiny/.zsh_history
rm -rvf tmp/.* tmp/* root/.cache root/.ICEauthority root/.Xauthority root/.bash_history root/.pki root/.chord root/.cocomusic.json root/.dbus root/.gnupg root/.gridea root/.mozilla root/.petal.db root/.config/Electron root/.config/Netron root/.config/chord root/.config/electron-netease-cloud-music root/.config/go-for-it root/.config/gridea root/.config/listen1 root/.config/lx-music-desktop root/.config/neofetch root/.config/netease-cloud-music-gtk root/.config/pulse root/.config/tenvideo_universal root/.xfce4-session.verbose-log root/.xfce4-session.verbose-log.last root/.zcompdump-localhost* root/.z root/.zsh_history
rm -fv var/cache/apt/archives/* var/cache/apt/* var/cache/pacman/pkg/* var/mail/* var/log/* var/log/apt/* var/log/journal/* var/lib/apt/lists/*

tar --zstd -cpvf /Public/rootfs.tar.zst .tiny.yaml bin boot etc home lib media mnt opt root sbin srv usr var \
    --transform='s|^sys/fs/selinux$|sys/.empty|' \
    --transform='s|^proc/loadavg$|proc/.loadavg|' \
    --transform='s|^proc/stat$|proc/.stat|' \
    --transform='s|^proc/uptime$|proc/.uptime|' \
    --transform='s|^proc/version$|proc/.version|' \
    --transform='s|^proc/vmstat$|proc/.vmstat|' \
    --transform='s|^proc/sys/kernel/cap_last_cap$|proc/.sysctl_entry_cap_last_cap|' \
    --transform='s|^proc/sys/fs/inotify/max_user_watches$|proc/.sysctl_inotify_max_user_watches|' \
    sys/fs/selinux proc/loadavg proc/stat proc/uptime proc/version proc/vmstat \
    proc/sys/kernel/cap_last_cap proc/sys/fs/inotify/max_user_watches