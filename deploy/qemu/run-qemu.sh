#!/usr/bin/env bash
set -Eeuo pipefail
disk=${DISK_IMAGE:-bmc-lite.qcow2}
memory=${MEMORY:-1024}
if [[ ! -f "$disk" ]]; then
  echo "missing $disk; create an Ubuntu cloud image first" >&2
  exit 2
fi
exec qemu-system-x86_64 \
  -enable-kvm -m "$memory" -smp 2 -nographic \
  -drive "file=$disk,if=virtio,format=qcow2" \
  -nic user,hostfwd=tcp::8000-:8000,hostfwd=tcp::2222-:22
