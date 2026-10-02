#!/bin/bash
# Crash-recovery check: kill the OvVec build in the middle of a transaction,
# let the *stock* shell recover the file, and require exactly the content of
# the last commit. Covers the rollback journal and WAL, with the page cache
# small enough that dirty pages reach the file before the crash.
#
#   bench/recovery.sh [WORKDIR]
set -uo pipefail
BENCH="$(cd "$(dirname "$0")" && pwd)"
BIN="$BENCH/build"
W="${1:-/tmp/ovvec-recovery}"
mkdir -p "$W/run"

digest() {
  "$BIN/sqlite3-shell" "$1" "PRAGMA integrity_check;
    SELECT hex(sha3_query('SELECT id,b FROM t ORDER BY id'))" | tr '\n' ' '
}

ok=0; bad=0
for journal in delete wal; do
  for size in 65536 262144; do
    t="$W/$journal-$size.db"
    if [ ! -f "$t" ]; then
      "$BIN/sqlite3-shell" "$t" "PRAGMA journal_mode=$journal;" \
        "CREATE TABLE t(id INTEGER PRIMARY KEY, b BLOB);
         WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c
           WHERE i<$((52428800 / size))) INSERT INTO t SELECT i, randomblob($size) FROM c;" >/dev/null
    fi
    for seed in 1 2 3 4 5 6 7 8; do
      for cache in 20 200; do
        for npre in 1 3; do
          rm -f "$W/run/snap.db"*
          "$BIN/crashtest-ovvec" "$t" "$W/run" $npre 30 200000 $seed $cache
          if [ "$(digest "$W/run/c.db")" = "$(digest "$W/run/snap.db")" ]; then
            ok=$((ok + 1))
          else
            bad=$((bad + 1))
            echo "MISMATCH: $journal size=$size seed=$seed cache=$cache npre=$npre"
          fi
        done
      done
    done
  done
done
echo "recovered correctly: $ok   mismatches: $bad"
[ $bad -eq 0 ]
