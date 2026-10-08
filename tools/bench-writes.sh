#!/usr/bin/env bash
# 写入路径基准：用 strace 精确统计 write/fdatasync 次数，同时报告吞吐。
#
#   bash tools/bench-writes.sh [记录数]
#
# 默认记录数 100000；构建目录可用 BUILD_DIR 覆盖（默认 build-bench）。
# 需要 strace；缺失时会退化为只测吞吐并明确提示。
# 六个场景：logger 的逐条 / 4 KiB 批量 / 64 KiB 批量 / 每条同步，
# 以及 SEL 的批量 / 每条重要记录。
set -Eeuo pipefail

records=${1:-100000}
build=${BUILD_DIR:-build-bench}
binary="$build/bench-writes"
workdir=$(mktemp -d)
# 每个场景在 workdir 下用独立子目录存数据，退出时整体删除。
trap 'rm -rf "$workdir"' EXIT

# 只构建基准目标；构建日志丢弃，保证脚本 stdout 上只有基准结果。
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF >/dev/null
cmake --build "$build" --parallel 2 --target bench-writes >/dev/null

# run_case <标签> [bench-writes 参数...]：跑一个场景，打印 syscall 计数与吞吐。
run_case() {
  local label=$1; shift
  local log="$workdir/$label.strace"
  local out="$workdir/$label.out"
  if command -v strace >/dev/null 2>&1; then
    # -f 跟踪子进程，-e 只跟踪 write/fdatasync，-c 输出汇总统计而不是逐条。
    strace -f -e trace=write,fdatasync -c -o "$log" "$binary" --dir "$workdir/$label" --records "$records" "$@" >"$out" 2>"$workdir/$label.err" || {
      echo "FAILED: $label" >&2; cat "$workdir/$label.err" >&2; return 1; }
    local writes fdatasyncs
    # strace -c 的第四列为调用次数，末列为 syscall 名。
    # 所以先按末列匹配 write/fdatasync，再取第 4 列；匹配不到时为空，下面用 :-0 兜底。
    writes=$(awk '$NF == "write" {print $4}' "$log")
    fdatasyncs=$(awk '$NF == "fdatasync" {print $4}' "$log")
    printf '%-26s writes=%-8s fdatasync=%-8s %s\n' \
      "$label" "${writes:-0}" "${fdatasyncs:-0}" "$(tr '\n' ' ' <"$out")"
  else
    # 没有 strace：只跑吞吐，并在同一列位置标注 (no strace)，避免与 syscall 计数混淆。
    "$binary" --dir "$workdir/$label" --records "$records" "$@" >"$out"
    printf '%-26s (no strace)                        %s\n' "$label" "$(tr '\n' ' ' <"$out")"
  fi
}

echo "records=$records build=$build"
echo "--- Logger ---"
run_case "logger-per-record" --mode logger --batch 1
run_case "logger-batch-4k"    --mode logger --batch 4096
run_case "logger-batch-64k"   --mode logger --batch 65536
run_case "logger-sync-per-rec" --mode logger --batch 4096 --sync
echo "--- SEL ---"
run_case "sel-batched"        --mode sel
run_case "sel-important"      --mode sel --important
echo
echo "说明：writes 为 write(2) 调用次数，fdatasync 为同步次数。"
echo "logger-per-record 与 logger-batch-* 的对比即逐条写入与批量合并的差异。"
