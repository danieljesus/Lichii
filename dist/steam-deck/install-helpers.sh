#!/bin/bash
# install-helpers.sh: installs set-epp and set-gpuclk to /etc/lichii and lets the deck user run
# them without a password, so lichii-steam.sh can raise the CPU energy preference and fix the GPU
# clock while Lichii runs (and restore both when it exits). Run it once from a terminal:
#   sudo ./install-helpers.sh          (or ./install-helpers.sh and type the sudo password)
# Remove with: sudo rm -r /etc/lichii /etc/sudoers.d/zz-lichii-epp /etc/sudoers.d/zz-lichii-gpuclk
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
user=${SUDO_USER:-$USER}
sudo install -d -m 755 /etc/lichii
for helper in set-epp set-gpuclk; do
    sudo install -m 755 "$here/$helper" "/etc/lichii/$helper"
    echo "$user ALL=(root) NOPASSWD: /etc/lichii/$helper" | sudo tee "/etc/sudoers.d/zz-lichii-${helper#set-}" >/dev/null
    sudo chmod 440 "/etc/sudoers.d/zz-lichii-${helper#set-}"
    sudo visudo -cf "/etc/sudoers.d/zz-lichii-${helper#set-}" >/dev/null
done
sudo -n /etc/lichii/set-epp status | head -1
sudo -n /etc/lichii/set-gpuclk status | head -1
echo "Installed."
