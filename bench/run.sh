#!/bin/bash
# Reproduce the OvVec read-path experiments.  Usage:  ./run.sh [workdir]
# Default workdir is /tmp/ovvec-bench; point it at the device you want to measure.
set -euo pipefail
BENCH="$(cd "$(dirname "$0")" && pwd)"
BIN="$BENCH/build"
W="${1:-/tmp/ovvec-bench}"
REPS=25
SIZE=8M

[ -x "$BIN/blobbench-ovvec" ] || "$BENCH/build.sh"
mkdir -p "$W"

med(){ "$1" read "$2" --reps "$REPS" ${3:-} | sed 's/.*median=[ ]*\([0-9.]*\).*/\1/'; }
pct(){ python3 -c "print(f'{($2-$1)/$1*100:+.1f}%')" ; }

echo "=== OvVec read-path benchmark ==="
echo "workdir=$W  reps=$REPS  $(uname -sr)  $(date -Is)"
echo

echo "--- 1. physical layout produced by each generator (target BLOB = $SIZE) ---"
for L in fresh frag8 frag4 frag2 churn purge rnddel; do
  "$BIN/blobbench-stock" gen "$W/$L.db" --layout "$L" --size "$SIZE" >/dev/null
  "$BIN/blobbench-stock" layout "$W/$L.db"
done
echo

echo "--- 2. latency by layout (warm page cache, chain cache populated) ---"
printf "%-8s %10s %10s %9s\n" layout stock_ms ovvec_ms delta
for L in fresh frag8 frag4 frag2 churn purge rnddel; do
  s=$(med "$BIN/blobbench-stock" "$W/$L.db"); o=$(med "$BIN/blobbench-ovvec" "$W/$L.db")
  printf "%-8s %10s %10s %9s\n" "$L" "$s" "$o" "$(pct "$s" "$o")"
done
echo

echo "--- 3. latency by read mode (fresh layout) ---"
printf "%-8s %10s %10s %9s\n" mode stock_ms ovvec_ms delta
for M in "" "--first" "--cold"; do
  s=$(med "$BIN/blobbench-stock" "$W/fresh.db" "$M"); o=$(med "$BIN/blobbench-ovvec" "$W/fresh.db" "$M")
  printf "%-8s %10s %10s %9s\n" "${M:---warm}" "$s" "$o" "$(pct "$s" "$o")"
done
echo

echo "--- 4. size sweep (fresh layout, warm) ---"
printf "%-8s %10s %10s %9s\n" size stock_ms ovvec_ms delta
for S in 64K 256K 1M 4M 8M 16M; do
  "$BIN/blobbench-stock" gen "$W/sz.db" --layout fresh --size "$S" >/dev/null
  s=$(med "$BIN/blobbench-stock" "$W/sz.db"); o=$(med "$BIN/blobbench-ovvec" "$W/sz.db")
  printf "%-8s %10s %10s %9s\n" "$S" "$s" "$o" "$(pct "$s" "$o")"
done
echo

echo "--- 5. read syscalls for $REPS reads (fresh layout, warm) ---"
for v in stock ovvec; do
  printf "%-6s " "$v"
  strace -c -f -e trace=pread64,preadv "$BIN/blobbench-$v" read "$W/fresh.db" --reps "$REPS" 2>&1 \
    | awk '/pread64|preadv/{printf "%s=%s ", $NF, $(NF-1)} END{print ""}'
done
echo
echo "(correctness: every read above verifies the BLOB hash; a mismatch aborts the run)"
