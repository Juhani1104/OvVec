"""Corrupt-chain test.

The vectored path speculates on pages whose next-page pointers have not been
validated yet, so a corrupt pointer is the case where it could read out of
bounds or diverge from stock. For each trial one overflow page's 4-byte
next-page pointer is overwritten with a hostile value and both builds are asked
to read the BLOB; they must agree on the outcome -- same bytes, or both
failing. The patched build is the sanitizer one.
"""
import subprocess, os, sys, random, shutil

BIN=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build"); W="/tmp/ovvec-corrupt"
ENV=dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=1",
                     UBSAN_OPTIONS="halt_on_error=1")

def sh(binary,args):
    r=subprocess.run([f"{BIN}/{binary}"]+args,capture_output=True,text=True,env=ENV)
    h=r.stdout.split("hash=")[1].strip() if "hash=" in r.stdout else None
    return r.returncode, h, r.stderr.strip()[:200]

def pages(db):
    # the shell is built next to the drivers by build.sh, not in a temp dir:
    # a scratch path goes stale the moment the session that made it rotates
    out=subprocess.run([f"{BIN}/sqlite3-shell",db,
        "SELECT pageno FROM dbstat('main') WHERE name='target' AND path LIKE '%+%' "
        "ORDER BY substr(path, instr(path,char(43))+1);"],capture_output=True,text=True).stdout
    return [int(x) for x in out.split()]

def main(n):
    os.makedirs(W,exist_ok=True)
    rng=random.Random(99); PS=4096; bad=0; asan=0
    base=f"{W}/base.db"
    subprocess.run([f"{BIN}/blobbench-stock","gen",base,"--layout","purge",
                    "--size","512K","--pagesize",str(PS)],capture_output=True,env=ENV)
    pg=pages(base)
    for i in range(n):
        db=f"{W}/c{i}.db"; shutil.copy(base,db)
        victim=rng.choice(pg[:-1])
        evil=rng.choice([0, 1, 2, 0xFFFFFFFF, 0x7FFFFFFF, rng.randrange(1,1<<31), victim])
        with open(db,"r+b") as f:
            f.seek((victim-1)*PS); f.write(evil.to_bytes(4,"big"))
        mode=rng.choice([["--first"],["--cold"],["--blob"],[]])
        ra,ha,ea = sh("blobbench-stock",["read",db,"--reps","1"]+mode)
        rb,hb,eb = sh("blobbench-ovvec-asan",["read",db,"--reps","1"]+mode)
        if "Sanitizer" in eb or "runtime error" in eb:
            print(f"  [{i}] SANITIZER pg={victim} evil={evil:#x} mode={mode}\n     {eb}"); asan+=1
        elif (ra==0) != (rb==0) or (ra==0 and ha!=hb):
            print(f"  [{i}] DIVERGENCE pg={victim} evil={evil:#x} mode={mode} "
                  f"stock=(rc{ra},{ha}) ovvec=(rc{rb},{hb})"); bad+=1
        os.remove(db)
    print(f"{n} corrupted-chain trials: {bad} divergences, {asan} sanitizer reports")
    return 1 if (bad or asan) else 0

if __name__=="__main__":
    sys.exit(main(int(sys.argv[1]) if len(sys.argv)>1 else 60))
