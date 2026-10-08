#!/usr/bin/env bash
# guest 内的一次性初始化：装最小运行依赖，并准备安装前缀与日志目录。
#
#   sudo bash deploy/qemu/guest-init.sh
#
# 需要 guest 内可用的 sudo 与 apt；PREFIX 可覆盖安装前缀（默认 /opt/bmc-lite）。
# 之后把发行包复制进 guest、解压，再运行 deploy/install.sh。
set -Eeuo pipefail
prefix=${PREFIX:-/opt/bmc-lite}
sudo apt-get update
# 最小的两个运行依赖：ca-certificates（TLS 证书库）与 python3（供 bmc_manage.py）。
sudo apt-get install -y ca-certificates python3
# 预建安装前缀下的 config 与日志目录，让后面放入发行包有落脚点；
# install.sh 会再按自己的属主与权限重新校正这两个目录。
sudo install -d -m 0755 "$prefix"/config /var/log/bmc-lite
# 服务用户可能已存在（脚本被重复执行），存在时忽略 useradd 的失败。
sudo useradd --system --home "$prefix" --shell /usr/sbin/nologin bmc-lite 2>/dev/null || true
# 日志目录交给服务用户，daemon 运行期在其中追加日志与 SEL。
sudo chown -R bmc-lite:bmc-lite /var/log/bmc-lite
echo "Copy the release archive here, extract it, then run: sudo bash deploy/install.sh"
echo "Management port is forwarded to host port 8000 by run-qemu.sh"
