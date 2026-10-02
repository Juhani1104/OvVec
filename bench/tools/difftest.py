"""Differential test: stock vs patched SQLite must return byte-identical BLOBs.

Randomises everything that steers the vectored-read paths -- layout (and so
chain direction and run length), BLOB size, page size, reserved bytes, journal
mode, read mode and the offset/length of partial reads -- then compares the
hash both builds report. The patched build is the AddressSanitizer/UBSan one,
so a wrong iovec slot shows up as a memory error rather than silently correct
output on this particular machine.
"""
import subprocess, random, sys, os, shutil

BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build")
W   = "/tmp/ovvec-diff"
ENV = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=1",
                       UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")

LAYOUTS   = ["fresh","frag2","frag4","frag8","churn","purge","rnddel"]
PAGESIZES = [512,1024,2048,4096,8192,16384,32768,65536]
RESERVES  = [0,0,0,4,32]
SIZES     = ["4K","60K","64K","100K","512K","1M","3M"]

def run(binary, args):
    r = subprocess.run([f"{BIN}/{binary}"]+args, capture_output=True, text=True, env=ENV)
    if r.returncode != 0:
        return None, f"exit={r.returncode} {r.stderr.strip()[:400]}"
    return r.stdout, None

def main(n):
    os.makedirs(W, exist_ok=True)
    rng = random.Random(int(sys.argv[2]) if len(sys.argv)>2 else 1)
    patched = sys.argv[3] if len(sys.argv)>3 else "blobbench-ovvec-asan"
    print(f"  comparing blobbench-stock vs {patched}")
    bad = 0
    for i in range(n):
        L    = rng.choice(LAYOUTS)
        ps   = rng.choice(PAGESIZES)
        res  = rng.choice(RESERVES)
        size = rng.choice(SIZES)
        wal  = rng.random() < 0.25
        db   = f"{W}/d{i}.db"
        gen  = ["gen",db,"--layout",L,"--size",size,"--pagesize",str(ps),"--reserve",str(res)]
        if wal: gen.append("--wal")
        out,err = run("blobbench-stock", gen)
        if err: print(f"  [{i}] GEN FAILED {L} ps={ps} res={res} {size} wal={wal}: {err}"); bad+=1; continue

        mode = rng.choice([[],["--cold"],["--blob"],["--first"],["--blob","--cold"]])
        if rng.random() < 0.4:                     # partial read window
            mode = [m for m in mode if m!="--blob"] + ["--blob"]
            off = rng.randrange(0, 1<<20)
            ln  = rng.randrange(1, 1<<20)
            mode += ["--offset",str(off),"--len",str(ln)]
        args = ["read",db,"--reps","2"]+mode

        a,ea = run("blobbench-stock", args)
        b,eb = run(patched, args)
        desc = f"{L} ps={ps} res={res} size={size} wal={wal} mode={' '.join(mode) or 'warm'}"
        if ea or eb:
            print(f"  [{i}] CRASH {desc}\n        stock: {ea}\n        ovvec: {eb}"); bad+=1
        else:
            ha = a.split("hash=")[1].strip(); hb = b.split("hash=")[1].strip()
            if ha != hb:
                print(f"  [{i}] HASH MISMATCH {desc}  stock={ha} ovvec={hb}"); bad+=1
        os.remove(db)
        for ext in ("-wal","-shm","-journal"):
            if os.path.exists(db+ext): os.remove(db+ext)
        if (i+1) % 25 == 0: print(f"  ...{i+1}/{n} done, {bad} failures", flush=True)
    print(f"{n} cases, {bad} failures", flush=True)
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main(int(sys.argv[1]) if len(sys.argv)>1 else 100))
