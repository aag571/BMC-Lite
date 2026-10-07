#!/usr/bin/env bash
set -Eeuo pipefail
root=${1:-.}
fail=0
require() {
  if [[ ! -e "$root/$1" ]]; then
    echo "missing: $1" >&2
    fail=1
  fi
}
require CMakeLists.txt
require include/bmc/core.hpp
require src/engine.cpp
require src/monitor.cpp
require src/readers.cpp
require src/main.cpp
require config/mock.conf
require config/hardware.conf.example
require README.md
if [[ -d "$root/build" ]]; then
  require build/bmc-lite
fi
if [[ "$fail" -ne 0 ]]; then
  exit 1
fi
echo "package layout: OK"
