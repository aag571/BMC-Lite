#!/usr/bin/env bash
set -Eeuo pipefail
mode=${1:-normal}
case "$mode" in
  normal|stress|valgrind|tsan) build=build-validation; sanitize=OFF ;;
  sanitize) build=build-sanitize; sanitize=ON ;;
  *) echo "Usage: $0 normal|sanitize|valgrind|stress|tsan" >&2; exit 2 ;;
esac
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DBMC_SANITIZE="$sanitize" -DBMC_TSAN=OFF
cmake --build "$build" --parallel 2
case "$mode" in
  normal) ctest --test-dir "$build" --output-on-failure ;;
  sanitize) ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir "$build" --output-on-failure ;;
  stress) ctest --test-dir "$build" -L stress --repeat until-fail:20 --output-on-failure ;;
  tsan)
    cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DBMC_TSAN=ON -DBMC_SANITIZE=OFF
    if [[ ${BMC_TSAN_NO_ASLR:-0} == 1 ]]; then
      # 某些 Linux 地址随机化布局与 GCC TSan 冲突；仅影响本次测试及其子进程。
      setarch "$(uname -m)" -R cmake --build build-tsan --parallel 2
      setarch "$(uname -m)" -R env TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-tsan --output-on-failure
    else
      cmake --build build-tsan --parallel 2
      TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-tsan --output-on-failure
    fi
    ;;
  valgrind)
    command -v valgrind >/dev/null || { echo "valgrind is not installed" >&2; exit 3; }
    valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=definite,indirect --error-exitcode=99 "$build/bmc_tests"
    valgrind --leak-check=full --errors-for-leak-kinds=definite,indirect --error-exitcode=99 "$build/bmc_stress"
    ;;
esac
