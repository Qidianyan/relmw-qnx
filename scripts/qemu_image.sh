#!/usr/bin/env bash
# Create (once) and boot a QNX 8.0 x86_64 QEMU target with mkqnximage (QNX SDP 8.0 required).
#   QNX_SDP=~/qnx800 scripts/qemu_image.sh        # builds ./qnx-vm and runs it (serial console)
set -euo pipefail
QNX_SDP="${QNX_SDP:-$HOME/qnx800}"
VM_DIR="${VM_DIR:-$PWD/qnx-vm}"
# shellcheck disable=SC1091
source "$QNX_SDP/qnxsdp-env.sh"
mkdir -p "$VM_DIR" && cd "$VM_DIR"
[ -f "$HOME/.ssh/id_ed25519.pub" ] || ssh-keygen -t ed25519 -N "" -f "$HOME/.ssh/id_ed25519"
if [ ! -d output ]; then
  mkqnximage --type=qemu --arch=x86_64 --hostname=relmw-qnx --ssh-ident="$HOME/.ssh/id_ed25519.pub"
fi
echo "Target IP: $(mkqnximage --getip 2>/dev/null || echo '[run inside VM: ifconfig]')"
exec mkqnximage --run
