"""Report the traversal direction of every overflow chain in a database.

Overflow pages show up in dbstat with a path ending '+NNNNNN', where NNNNNN is
the position in that cell's chain in fixed-width HEX -- so it orders as text,
never as an integer.  What matters is whether consecutive pages of a chain step
forwards or backwards, because a backwards walk is invisible to readahead.
"""
import subprocess, sys, os
from collections import defaultdict
BIN = os.path.dirname(os.path.abspath(__file__)) + "/../build"
SQL = ("SELECT name, path, pageno FROM dbstat('main') WHERE path LIKE '%+%' "
       "ORDER BY name, path;")
def analyse(db):
    r = subprocess.run([f"{BIN}/sqlite3-shell", db], input=SQL,
                       capture_output=True, text=True)
    if r.returncode:
        return None                      # 真的讀不到
    if "|" not in r.stdout:
        return (0, 0, 0, 0, 0)           # 讀得到，但一個 overflow 頁都沒有
    chains = defaultdict(list)
    for line in r.stdout.splitlines():
        if line.count("|") < 2: continue
        name, path, pg = line.rsplit("|", 2)
        cell, seq = path.rsplit("+", 1)
        chains[(name, cell)].append((seq, int(pg)))
    fwd = back = other = 0
    nchain = npage = 0
    for v in chains.values():
        v.sort()
        pgs = [p for _, p in v]
        npage += len(pgs)
        if len(pgs) < 2: continue
        nchain += 1
        for a, b in zip(pgs, pgs[1:]):
            if b == a + 1: fwd += 1
            elif b < a:    back += 1
            else:          other += 1
    return nchain, npage, fwd, back, other
if __name__ == "__main__":
    print(f"{'database':44s} {'鏈數':>6s} {'頁數':>7s} {'+1':>7s} {'往回':>7s} {'倒序%':>7s}")
    for db in sys.argv[1:]:
        res = analyse(db)
        if res is None:
            print(f"{os.path.basename(db):44s}   (不是 SQLite 或讀不到)"); continue
        nc, np, f, b, o = res
        tot = f + b + o
        pct = f"{b/tot*100:6.1f}%" if tot else "     -"
        print(f"{os.path.basename(db):44s} {nc:6d} {np:7d} {f:7d} {b:7d} {pct:>7s}")
