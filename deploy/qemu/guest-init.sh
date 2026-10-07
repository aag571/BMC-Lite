#!/usr/bin/env bash
set -Eeuo pipefail
prefix=${PREFIX:-/opt/bmc-lite}
sudo apt-get update
sudo apt-get install -y ca-certificates python3
sudo install -d -m 0755 "$prefix"/config /var/log/bmc-lite
sudo useradd --system --home "$prefix" --shell /usr/sbin/nologin bmc-lite 2>/dev/null || true
sudo chown -R bmc-lite:bmc-lite /var/log/bmc-lite
echo "Copy the release archive here, extract it, then run: sudo bash deploy/install.sh"
echo "Management port is forwarded to host port 8000 by run-qemu.sh"
