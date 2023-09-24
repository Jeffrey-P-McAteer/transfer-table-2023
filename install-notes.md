
# Write image to SD card

```bash
mkdir -p build
wget -O build/ArchLinuxARM-rpi-armv7-latest.tar.gz http://os.archlinuxarm.org/os/ArchLinuxARM-rpi-armv7-latest.tar.gz

# See https://archlinuxarm.org/platforms/armv8/broadcom/raspberry-pi-3
fdisk /dev/sdX
mkfs.vfat /dev/sdX1
mkfs.ext4 /dev/sdX2
mkdir -p build/sd_root
mount /dev/sdX2 build/sd_root
mkdir -p build/sd_root/boot
mount /dev/sdX1 build/sd_root/boot

sudo bsdtar -xpf build/ArchLinuxARM-rpi-armv7-latest.tar.gz -C build/sd_root
sync
umount build/sd_root/boot build/sd_root

```

# Boot Pi & Install Packages

```bash
# We're real boring, the software is controlled by the user "user"
useradd -m -G wheel user
passwd user # user

# Overwrite DNS settings from host
echo "nameserver 192.168.5.1" > /run/systemd/resolve/resolv.conf

pacman -S vim
vim /etc/pacman.d/mirrorlist

pacman -Syu

pacman -S openssh
systemctl enable sshd

vim /health-check.sh <<EOF
#!/bin/bash

network_to_connect_to='MacHome 2.4ghz'
wlan_devs=(
  wlan0
  wlan1
  wlan2
)
for wlan in "${wlan_devs[@]}" ; do
  iwctl station "$wlan" scan
  sleep 1
  iwctl station "$wlan" connect "$network_to_connect_to"
done

EOF

vim /etc/systemd/system/health-check.service <<EOF
[Unit]
Description=Health Check

[Service]
Type=oneshot
ExecStart=/bin/bash /health-check.sh
EOF
vim /etc/systemd/system/health-check.timer <<EOF
[Unit]
Description=Health Check

[Timer]
OnBootSec=45s
# every 3 minutes after activation
OnUnitActiveSec=3m


[Install]
WantedBy=timers.target
EOF

pacman -S iwd
systemctl enable iwd.service

pacman -S dhcpcd
systemctl enable dhcpcd.service

systemctl enable health-check.timer


pacman -S sudo
vim /etc/sudoers # Allow wheel w/o pw


# 2023-08-07, looking into controlling the board w/o connected motor.
sudo pacman -S usbutils
lsusb # Found "RobotAndRobot.com RNR ECO MOTION 2.0"

sudo pacman -S ntp
sudo systemctl enable --now ntpd.service

sudo pacman -Sy git base-devel wget python python-pip
sudo pacman -Sy xmlto kmod inetutils bc libelf git cpio perl tar xz

## Yay
cd /opt/
git clone https://aur.archlinux.org/yay.git
cd yay
makepkg -si


###
## C Code build below
###

yay -Sy pigpio
# Possibly; sudo systemctl enable --now pigpiod.service

gcc -g -o gpio-motor-control gpio-motor-control.c -lpigpio -lrt -lpthread


```
