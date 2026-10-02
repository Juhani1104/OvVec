"""Compare stock SQLite with OvVec on synthetic workloads.

    python3 bench/compare.py WORKDIR [ROUNDS]

Generates every test database under WORKDIR (about 1 GB), then times each
workload with both builds. The order of the builds is shuffled every round:
alternating them in a fixed order is biased enough to invent differences that
are not there. Reports the median of ROUNDS (default 7) with the range; '*'
marks a result whose ranges do not overlap.

Run bench/build.sh first.
"""
import os, random, statistics, subprocess, sys

BENCH = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(BENCH, "build")
SHELL = os.path.join(BIN, "sqlite3-shell")
W = sys.argv[1] if len(sys.argv) > 1 else "/tmp/ovvec-compare"
ROUNDS = int(sys.argv[2]) if len(sys.argv) > 2 else 7
BUILDS = ("stock", "ovvec")


def sh(db, *sql):
    subprocess.run([SHELL, db, *sql], check=True, capture_output=True)


def table(db, n, size, journal="delete", refill=False):
    """n rows of `size` random bytes in table t(id, b)."""
    if os.path.exists(db):
        return
    fill = ("WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c"
            f" WHERE i<{n}) INSERT INTO t SELECT i, randomblob({size}) FROM c;")
    sh(db, f"PRAGMA journal_mode={journal};",
       "CREATE TABLE t(id INTEGER PRIMARY KEY, b BLOB);", fill)
    if refill:
        # Delete everything in one transaction and fill again: SQLite hands
        # the freed pages back highest first, so every value is now stored
        # back to front. Kernel readahead cannot follow that order.
        sh(db, "DELETE FROM t;", fill)


def generate():
    os.makedirs(W, exist_ok=True)
    gen = os.path.join(BIN, "blobbench-stock")
    for layout in ("fresh", "purge", "frag8"):
        db = f"{W}/read-{layout}.db"
        if not os.path.exists(db):
            subprocess.run([gen, "gen", db, "--layout", layout, "--size", "8M"],
                           check=True, capture_output=True)
    for size in ("64K", "256K", "1M"):
        db = f"{W}/read-{size}.db"
        if not os.path.exists(db):
            subprocess.run([gen, "gen", db, "--layout", "fresh", "--size", size],
                           check=True, capture_output=True)
    for size in (65536, 262144, 1048576):
        table(f"{W}/del-{size}.db", 104857600 // size, size)
    table(f"{W}/del-8M-reversed.db", 12, 8388608, refill=True)
    table(f"{W}/wal.db", 800, 65536, journal="wal")
    table(f"{W}/rewrite.db", 200, 262144)
    for b in BUILDS:
        os.makedirs(f"{W}/run-{b}", exist_ok=True)


def workloads():
    """name -> function(build) returning the command line to time."""
    def blob(db, *extra):
        return lambda b: [f"{BIN}/blobbench-{b}", "read", f"{W}/{db}",
                          "--reps", "15", *extra]

    def sql(db, cold, sync, stmt=None):
        return lambda b: [f"{BIN}/sqltime-{b}", f"{W}/{db}", f"{W}/run-{b}",
                          str(cold), sync] + ([stmt] if stmt else [])

    def rewrite(ntxn, rows, sync, stmt):
        return lambda b: [f"{BIN}/rewrite-{b}", f"{W}/rewrite.db",
                          f"{W}/run-{b}", str(ntxn), str(rows), "262144",
                          sync, stmt]

    insert = ("WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c"
              " WHERE i<2000) INSERT INTO t(b) SELECT randomblob(3000) FROM c")
    resize = "UPDATE t SET b=substr(b,2)||x'00'||substr(b,1,1+0*?1) WHERE id=?2"
    replace = "UPDATE t SET b=substr(b,2)||substr(b,1,1+0*?1) WHERE id=?2"
    return {
        "read 8 MB, stored in order, cached":       blob("read-fresh.db"),
        "read 8 MB, stored back to front, cached":  blob("read-purge.db"),
        "read 8 MB, stored back to front, disk":    blob("read-purge.db", "--cold"),
        "read 8 MB, scattered in 32 KB pieces, disk": blob("read-frag8.db", "--cold"),
        "read 64 KB, cached":                       blob("read-64K.db"),
        "read 256 KB, cached":                      blob("read-256K.db"),
        "read 1 MB, cached":                        blob("read-1M.db"),
        "delete half of 64 KB values":              sql("del-65536.db", 0, "FULL"),
        "delete half of 256 KB values":             sql("del-262144.db", 0, "FULL"),
        "delete half of 1 MB values":               sql("del-1048576.db", 0, "FULL"),
        "delete 8 MB values, back to front, disk":  sql("del-8M-reversed.db", 1, "FULL"),
        "WAL: insert 2000 rows, synchronous=NORMAL": sql("wal.db", 0, "NORMAL", insert),
        "WAL: insert 2000 rows, synchronous=OFF":   sql("wal.db", 0, "OFF", insert),
        "rewrite 20 x 256 KB per txn, new size":    rewrite(6, 20, "FULL", resize),
        "rewrite 20 x 256 KB per txn, same size":   rewrite(6, 20, "FULL", replace),
        "rewrite 20 x 256 KB per txn, new size, sync off":  rewrite(6, 20, "OFF", resize),
        "rewrite 20 x 256 KB per txn, same size, sync off": rewrite(6, 20, "OFF", replace),
        "rewrite 1 x 256 KB per txn, same size, sync off":  rewrite(30, 1, "OFF", replace),
    }


def run(cmd):
    out = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
    if "median=" in out:
        return float(out.split("median=")[1].split("ms")[0])
    return float(out.split()[0])


def main():
    generate()
    jobs = workloads()
    res = {(name, b): [] for name in jobs for b in BUILDS}
    for r in range(ROUNDS):
        order = list(res)
        random.shuffle(order)
        for name, b in order:
            res[(name, b)].append(run(jobs[name](b)))
        print(f"round {r + 1}/{ROUNDS}", file=sys.stderr, flush=True)
    print(f"{'workload':50} {'stock ms':>10} {'ovvec ms':>10} {'change':>8}")
    for name in jobs:
        a, o = sorted(res[(name, "stock")]), sorted(res[(name, "ovvec")])
        ma, mo = statistics.median(a), statistics.median(o)
        # A range needs a few samples before it means anything
        sep = ("*" if ROUNDS >= 3 and (o[-1] < a[0] or o[0] > a[-1])
               else " ")
        print(f"{name:50} {ma:10.3f} {mo:10.3f} {(mo - ma) / ma * 100:+7.1f}%{sep}")


if __name__ == "__main__":
    main()
