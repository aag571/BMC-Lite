#!/usr/bin/env bash
# 在目标机安装 BMC-Lite：前置校验 -> 建用户与目录 -> 装二进制、配置与 systemd 单元。
#
#   sudo bash deploy/install.sh
#
# 在仓库根或解压后的包根目录执行；二进制取自 build-release/，可用 BUILD_DIR 覆盖。
# 需要 root、systemctl、useradd 与 install(1)。
# 退出码：2 = 二进制缺失或动态库不全，1 = 非 root，3 = 服务正在运行。
set -Eeuo pipefail

build_dir=${BUILD_DIR:-build-release}
prefix=/opt/bmc-lite
service_user=bmc-lite

# 校验 1/4：二进制必须已经构建且可执行。
if [[ ! -x "$build_dir/bmc-lite" ]]; then
  echo "Build the Release binary before installation." >&2
  exit 2
fi
# 校验 2/4：ldd 输出里出现 not found 说明运行期缺库；TLS 版本另外需要 OpenSSL 3。
if ldd "$build_dir/bmc-lite" 2>&1 | grep -q 'not found'; then
  echo "Missing runtime libraries. TLS releases require OpenSSL 3 (Ubuntu: libssl3t64)." >&2
  exit 2
fi
# 校验 3/4：后面的 useradd/install/systemctl 都需要 root，提前失败以免只装一半。
if [[ ${EUID} -ne 0 ]]; then
  echo "Run from the project root with sudo bash deploy/install.sh." >&2
  exit 1
fi
# 校验 4/4：正在运行的服务持有旧二进制的文件描述符，必须先停服再替换。
if systemctl is-active --quiet bmc-lite.service; then
  echo "Stop the existing service before replacing its binary." >&2
  exit 3
fi
# 服务用户已存在就复用；不存在才新建（系统账户、无登录 shell、home 指向前缀）。
id "$service_user" >/dev/null 2>&1 || useradd --system --home "$prefix" --shell /usr/sbin/nologin "$service_user"
# 安装前缀与配置目录为 0755：可读可遍历，不放敏感内容。
install -d -m 0755 "$prefix" "$prefix/config"
# control/ 放控制面令牌与证书，只给服务用户（0750）；这里只建目录，不生成凭证。
install -d -o "$service_user" -g "$service_user" -m 0750 "$prefix/control"
# tools/ 与 docs/ 分别放管理脚本和文档，保持默认权限。
install -d -m 0755 "$prefix/tools"
install -d -m 0755 "$prefix/docs"
# 文档、README 与管理脚本每次安装都刷新，便于对照新版本。
install -m 0644 docs/*.md "$prefix/docs/"
install -m 0644 README.md README.en.md "$prefix/"
install -m 0644 tools/bmc_manage.py "$prefix/tools/bmc_manage.py"
# 运行期日志与 SEL 落在 /var/log/bmc-lite，属主为服务用户。
install -d -o "$service_user" -g "$service_user" -m 0750 /var/log/bmc-lite
# 先写 .new 再 mv：同一文件系统内的重命名是原子的，不会留下半个二进制。
install -m 0755 "$build_dir/bmc-lite" "$prefix/bmc-lite.new"
mv -f "$prefix/bmc-lite.new" "$prefix/bmc-lite"
# 示例模板每次覆盖；真正的配置在下面按“缺失才补”的规则处理。
install -m 0644 config/hardware.conf.example "$prefix/config/hardware.conf.example"
# 已有配置一律保留，这里只补缺失的规则文件。
if [[ ! -f "$prefix/config/rules.conf" ]]; then
  install -m 0644 config/rules.conf "$prefix/config/rules.conf"
fi
# 首次安装（hardware.conf 不存在）才播种：直接进模拟模式，0640 且属主 root:bmc-lite。
if [[ ! -f "$prefix/config/hardware.conf" ]]; then
  install -m 0640 -o root -g "$service_user" config/mock.conf "$prefix/config/hardware.conf"
fi
# 覆盖单元文件后必须 daemon-reload，systemctl 才会读取新定义。
install -m 0644 deploy/bmc-lite.service /etc/systemd/system/bmc-lite.service
systemctl daemon-reload
echo "Installed to $prefix with simulation as the initial configuration."
echo "Start with: sudo systemctl enable --now bmc-lite.service"
echo "Review the hardware template and device permissions before using hardware."
echo "Control is disabled by default. See docs/control.md or docs/control.en.md for HTTPS setup."
