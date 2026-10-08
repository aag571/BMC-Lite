#!/usr/bin/env bash
# 在 KVM 上启动 x86_64 Ubuntu guest，用于集成验证。
#
#   DISK_IMAGE=bmc-lite.qcow2 MEMORY=1024 bash deploy/qemu/run-qemu.sh
#
# 前置条件：已按 deploy/qemu/README.md 准备好 qcow2 云镜像（默认 ./bmc-lite.qcow2），
# 且当前用户能访问 /dev/kvm；需要 qemu-system-x86_64。
# 端口转发：guest 8000 -> 宿主 8000（只读管理面），guest 22 -> 宿主 2222（ssh）。
# 镜像文件不存在时打印提示并以 2 退出。
set -Eeuo pipefail
disk=${DISK_IMAGE:-bmc-lite.qcow2}
memory=${MEMORY:-1024}
if [[ ! -f "$disk" ]]; then
  echo "missing $disk; create an Ubuntu cloud image first" >&2
  exit 2
fi
# exec 让 qemu 顶替当前 shell，终端与 qemu 的退出码直接透传。
exec qemu-system-x86_64 \
  -enable-kvm -m "$memory" -smp 2 -nographic \
  -drive "file=$disk,if=virtio,format=qcow2" \
  -nic user,hostfwd=tcp::8000-:8000,hostfwd=tcp::2222-:22
