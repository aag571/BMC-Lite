#!/usr/bin/env bash
set -Eeuo pipefail

build_dir=${BUILD_DIR:-build-release}
prefix=/opt/bmc-lite
service_user=bmc-lite

if [[ ! -x "$build_dir/bmc-lite" ]]; then
  echo "Build the Release binary before installation." >&2
  exit 2
fi
if ldd "$build_dir/bmc-lite" 2>&1 | grep -q 'not found'; then
  echo "Missing runtime libraries. TLS releases require OpenSSL 3 (Ubuntu: libssl3t64)." >&2
  exit 2
fi
if [[ ${EUID} -ne 0 ]]; then
  echo "Run from the project root with sudo bash deploy/install.sh." >&2
  exit 1
fi
if systemctl is-active --quiet bmc-lite.service; then
  echo "Stop the existing service before replacing its binary." >&2
  exit 3
fi
id "$service_user" >/dev/null 2>&1 || useradd --system --home "$prefix" --shell /usr/sbin/nologin "$service_user"
install -d -m 0755 "$prefix" "$prefix/config"
install -d -o "$service_user" -g "$service_user" -m 0750 "$prefix/control"
install -d -m 0755 "$prefix/tools"
install -d -m 0755 "$prefix/docs"
install -m 0644 docs/*.md "$prefix/docs/"
install -m 0644 README.md README.en.md "$prefix/"
install -m 0644 tools/bmc_manage.py "$prefix/tools/bmc_manage.py"
install -d -o "$service_user" -g "$service_user" -m 0750 /var/log/bmc-lite
install -m 0755 "$build_dir/bmc-lite" "$prefix/bmc-lite.new"
mv -f "$prefix/bmc-lite.new" "$prefix/bmc-lite"
install -m 0644 config/hardware.conf.example "$prefix/config/hardware.conf.example"
if [[ ! -f "$prefix/config/rules.conf" ]]; then
  install -m 0644 config/rules.conf "$prefix/config/rules.conf"
fi
if [[ ! -f "$prefix/config/hardware.conf" ]]; then
  install -m 0640 -o root -g "$service_user" config/mock.conf "$prefix/config/hardware.conf"
fi
install -m 0644 deploy/bmc-lite.service /etc/systemd/system/bmc-lite.service
systemctl daemon-reload
echo "Installed to $prefix with simulation as the initial configuration."
echo "Start with: sudo systemctl enable --now bmc-lite.service"
echo "Review the hardware template and device permissions before using hardware."
echo "Control is disabled by default. See docs/control.md or docs/control.en.md for HTTPS setup."
