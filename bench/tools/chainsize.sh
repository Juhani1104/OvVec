#!/bin/bash
# Overflow-chain length histogram of one or more SQLite files.
#
# Reports only counts per size bucket -- no row content, keys or table data
# leaves the file. Opens each file read-only with immutable=1, so it takes no
# locks and never writes (safe on a database another program has open, though
# the numbers may then be slightly stale).
#
# usage: chainsize.sh file.db [file.db ...]
# Any sqlite3 shell built with SQLITE_ENABLE_DBSTAT_VTAB works; set SQLITE3 to
# point at one, otherwise the shell produced by bench/build.sh is used.
SHELL_BIN="${SQLITE3:-$(dirname "$0")/../build/sqlite3-shell}"

for f in "$@"; do
  head -c 16 "$f" 2>/dev/null | grep -q "SQLite format 3" || {
    echo "skip (not SQLite): $f"; continue; }
  "$SHELL_BIN" -readonly "file:$f?immutable=1" <<'EOF'
.mode list
.separator " "
.headers off
WITH ps(sz) AS (SELECT page_size FROM pragma_page_size),
chains AS (
  SELECT substr(path, 1, instr(path, '+')-1) AS cell, count(*) AS n
  FROM dbstat WHERE pagetype='overflow' GROUP BY cell
)
SELECT
  printf('page=%d  chains=%d', (SELECT sz FROM ps), count(*)),
  printf('| <12K:%d', sum(n<=2)),
  printf(' 12-16K:%d', sum(n=3)),
  printf(' 16-100K:%d', sum(n BETWEEN 4 AND 24)),
  printf(' 100K-1M:%d', sum(n BETWEEN 25 AND 255)),
  printf(' >1M:%d', sum(n>=256)),
  printf('| pages in >=16K chains: %.1f%%',
         100.0*sum(CASE WHEN n>=4 THEN n ELSE 0 END)/max(sum(n),1))
FROM chains;
EOF
  echo "   ^ $f"
done
