#!/usr/bin/env bash
# 生成 Release 发行包：构建 -> 暂存到临时目录 -> 打 tar.gz 并写 SHA256 文件。
#
#   bash tools/package-release.sh [额外的 cmake 参数...]
#
# 需要 cmake 与 tar；BMC_TLS=ON 时构建带 TLS 的发行版，默认为 OFF。
# 产物落在当前目录：bmc-lite-release.tar.gz 与 bmc-lite-release.tar.gz.sha256。
set -Eeuo pipefail
# 发行包固定为 Release，且不启用测试与 sanitizer/TSan 插桩。
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DBMC_SANITIZE=OFF -DBMC_TSAN=OFF -DBMC_TLS="${BMC_TLS:-OFF}" "$@"
cmake --build build-release --parallel 2
stage=$(mktemp -d)
# 打包完成后由 trap 删除暂存目录，避免在工作区留下中间文件。
trap 'rm -rf "$stage"' EXIT
package="$stage/bmc-lite-release"
# 包内目录结构与仓库一致：二进制进 build-release/，解压后可直接跑 deploy/install.sh。
mkdir -p "$package/build-release"
install -m 0755 build-release/bmc-lite "$package/build-release/bmc-lite"
# 配置模板、部署脚本（含 qemu 子目录）、工具与文档按原权限复制。
cp -a config deploy tools docs README.md README.en.md "$package/"
# -C 切到暂存目录，使归档顶层目录名为 bmc-lite-release 而不是随机临时目录名。
tar -C "$stage" -czf bmc-lite-release.tar.gz bmc-lite-release
# 校验文件与压缩包同目录，可用 sha256sum -c bmc-lite-release.tar.gz.sha256 复核。
sha256sum bmc-lite-release.tar.gz > bmc-lite-release.tar.gz.sha256
echo "Created bmc-lite-release.tar.gz and its checksum."
