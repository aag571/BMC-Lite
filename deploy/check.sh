#!/usr/bin/env bash
# 发行包布局自检：只断言文件/目录是否存在，不构建、不运行、不改动任何文件。
#
#   bash deploy/check.sh [已解压的包根目录]
#
# 参数缺省为当前目录；它不被构建或测试调用，建议在暂存/解压好的包根目录手工执行一次。
# 每缺一项打印一行 missing: <相对路径>，缺少任意一项就以 1 退出，全部齐全时打印
# package layout: OK。
set -Eeuo pipefail
root=${1:-.}
fail=0
# 逐项累计失败而不立即退出，便于一次看全所有缺失项。
require() {
  if [[ ! -e "$root/$1" ]]; then
    echo "missing: $1" >&2
    fail=1
  fi
}
# 下面抽查仓库的关键文件而不是全部源文件，因此“通过”只说明布局没缺骨架。
require CMakeLists.txt
require include/bmc/core.hpp
require src/engine.cpp
require src/monitor.cpp
require src/readers.cpp
require src/chips.cpp
require src/calibration.cpp
require src/linux_io.cpp
require src/main.cpp
require config/mock.conf
require config/hardware.conf.example
require README.md
# build/ 只在源码构建树里出现；发行包内二进制位于 build-release/，所以这一项是条件检查。
if [[ -d "$root/build" ]]; then
  require build/bmc-lite
fi
if [[ "$fail" -ne 0 ]]; then
  exit 1
fi
echo "package layout: OK"
