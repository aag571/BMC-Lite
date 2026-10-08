#!/usr/bin/env bash
# 本地验证入口：按模式配置、构建并运行 C++/CTest 用例。
#
#   bash tools/validate.sh [normal|sanitize|valgrind|stress|tsan]
#
# 省略模式等同于 normal；未知模式打印用法并以 2 退出。
# 需要 cmake 与 C++ 编译器；valgrind 模式额外需要 valgrind（缺了以 3 退出）。
# 构建目录：build-validation（normal/stress/valgrind/tsan 共用）、build-sanitize、
# build-tsan；ASan/UBSan 与 TSan 的插桩互斥，必须各自独立成目录。
# BMC_TSAN_NO_ASLR=1 时，tsan 模式用 setarch -R 关闭本次构建与测试的地址随机化。
set -Eeuo pipefail
mode=${1:-normal}
case "$mode" in
  # 四种非 sanitizer 模式共用一个构建目录；只有 sanitize 换目录并打开开关。
  normal|stress|valgrind|tsan) build=build-validation; sanitize=OFF ;;
  sanitize) build=build-sanitize; sanitize=ON ;;
  *) echo "Usage: $0 normal|sanitize|valgrind|stress|tsan" >&2; exit 2 ;;
esac
# 这里完成四种模式的配置与构建；tsan 模式在后面单独配置 build-tsan。
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DBMC_SANITIZE="$sanitize" -DBMC_TSAN=OFF
cmake --build "$build" --parallel 2
# 各模式复用同一份构建产物，区别只在 ctest 的选项：sanitize 加大检测开关，
# stress 只重复 stress 标签的用例。
case "$mode" in
  normal) ctest --test-dir "$build" --output-on-failure ;;
  sanitize) ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir "$build" --output-on-failure ;;
  stress) ctest --test-dir "$build" -L stress --repeat until-fail:20 --output-on-failure ;;
  tsan)
    # TSan 用自己的目录，并在重新配置时显式关掉 sanitizer。
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
    # 提前退出，避免把“没跑 valgrind”当成“valgrind 通过”。
    command -v valgrind >/dev/null || { echo "valgrind is not installed" >&2; exit 3; }
    # 两个目标都要求 definite/indirect 泄漏为 0，否则以 99 退出。
    valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=definite,indirect --error-exitcode=99 "$build/bmc_tests"
    valgrind --leak-check=full --errors-for-leak-kinds=definite,indirect --error-exitcode=99 "$build/bmc_stress"
    ;;
esac
