#!/bin/bash
# Build two SQLite amalgamations -- stock and with ovvec.patch applied -- and
# link every benchmark driver against each.
#
# Needs the SQLite 3.53.4 source archive, sqlite-src-3530400.zip, in the
# repository root (or set SQLITE_ZIP to its path).
set -euo pipefail

BENCH="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$BENCH")"
ZIP="${SQLITE_ZIP:-$ROOT/sqlite-src-3530400.zip}"
OUT="$BENCH/build"
SRC="$OUT/src"

# SQLITE_DIRECT_OVERFLOW_READ is not a SQLite default, but OvVec's read path
# lives inside that ifdef, so the honest baseline is stock built with it too.
# DBSTAT_VTAB lets the tools measure where overflow pages actually sit.
CFLAGS="-O2 -DSQLITE_DIRECT_OVERFLOW_READ -DSQLITE_ENABLE_DBSTAT_VTAB -DSQLITE_THREADSAFE=0"

[ -f "$ZIP" ] || { echo "missing $ZIP (SQLite 3.53.4 source)"; exit 1; }
mkdir -p "$OUT"

amalgamate() {          # $1 = build name, $2 = patch to apply (or empty)
  local name="$1" patch="$2" dir="$SRC/$1"
  rm -rf "$dir" && mkdir -p "$dir"
  unzip -q "$ZIP" -d "$dir"
  dir="$dir/sqlite-src-3530400"
  [ -z "$patch" ] || ( cd "$dir" && patch -p1 -s < "$patch" )
  ( cd "$dir" && ./configure >/dev/null && make sqlite3.c shell.c >/dev/null )
  cp "$dir/sqlite3.c" "$OUT/sqlite3-$name.c"
  cp "$dir/sqlite3.h" "$OUT/sqlite3.h"
  cp "$dir/shell.c" "$OUT/shell.c"
  echo "  sqlite3-$name.c"
}

echo "building amalgamations:"
amalgamate stock ""
amalgamate ovvec "$ROOT/ovvec.patch"

echo "building drivers:"
for v in stock ovvec; do
  gcc $CFLAGS -I"$OUT" -o "$OUT/blobbench-$v" \
      "$BENCH/blobbench.c" "$OUT/sqlite3-$v.c" -lm -lpthread
  for d in sqltime rewrite crashtest; do
    gcc $CFLAGS -I"$OUT" -o "$OUT/$d-$v" "$BENCH/$d.c" "$OUT/sqlite3-$v.c" -lm
  done
  echo "  blobbench-$v sqltime-$v rewrite-$v crashtest-$v"
done
# A stock shell: the tools use it to generate data and to check results.
gcc $CFLAGS -I"$OUT" -o "$OUT/sqlite3-shell" "$OUT/shell.c" \
    "$OUT/sqlite3-stock.c" -lm
echo "  sqlite3-shell"
echo "done -> $OUT"
