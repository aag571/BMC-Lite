#!/usr/bin/env bash
# 写入路径基准：用 strace 精确统计 write/fdatasync 次数，同时报告吞吐。
#
#   bash tools/bench-writes.sh [记录数]
#
# 需要 strace；缺失时会退化为只测吞吐并明确提示。
set -Eeuo pipefail

records=${1:-100000}
build=${BUILD_DIR:-build-bench}
binary="$build/bench-writes"
workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT

cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF >/dev/null
cmake --build "$build" --parallel 2 --target bench-writes >/dev/null

run_case() {
  local label=$1; shift
  local log="$workdir/$label.strace"
  local out="$workdir/$label.out"
  if command -v strace >/dev/null 2>&1; then
    strace -f -c -o "$log" "$binary" --dir "$workdir/$label" --records "$records" "$@" >"$out" 2>"$workdir/$label.err" || {
      echo "FAILED: $label" >&2; cat "$workdir/$label.err" >&2; return 1; }
    local writes fdatasyncs
    writes=$(awk '/^[[:space:]]*[0-9]+[[:space:]]+write$/ {print $1}' "$log")
    fdatasyncs=$(awk '/^[[:space:]]*[0-9]+[[:space:]]+fdatasync$/ {print $1}' "$log")
    printf '%-26s writes=%-8s fdatasync=%-8s %s\n' \
      "$label" "${writes:-0}" "${fdatasyncs:-0}" "$(tr '\n' ' ' <"$out")"
  else
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
