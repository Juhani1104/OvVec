"""Randomised-order A/B benchmark driver.

Alternating two binaries in a fixed order within each round is NOT sufficient:
on reads of a few hundred microseconds it produces a systematic bias large
enough to manufacture a 30% "regression" with non-overlapping ranges where the
two builds issue identical syscalls. Shuffle the order every round.
"""
import os
import subprocess, statistics, random, sys
BIN=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build")
def one(v, db, mode, reps):
    o=subprocess.run([f"{BIN}/blobbench-{v}","read",db,"--reps",str(reps)]+mode,
                     capture_output=True,text=True).stdout
    return float(o.split("median=")[1].split("ms")[0])
def compare(db, mode, reps, rounds, variants):
    res={v:[] for v in variants}
    order=list(variants)
    for _ in range(rounds):
        random.shuffle(order)                 # randomised order every round
        for v in order: res[v].append(one(v,db,mode,reps))
    base=statistics.median(res[variants[0]])
    out=[]
    for v in variants:
        a=sorted(res[v]); m=statistics.median(a)
        out.append(f"{v}={m:.3f}[{a[0]:.3f},{a[-1]:.3f}]{'' if v==variants[0] else f' {(m-base)/base*100:+.1f}%'}")
    return "  ".join(out)
random.seed(7)
W="/tmp/ovvec-sweep"
cells=[("fresh-64K",["--cold"],5,21),("churn-64K",["--cold"],5,21),
       ("churn-1M",["--cold"],5,21),("fresh-1M",["--cold"],5,15),
       ("fresh-8M",["--cold"],3,11),("purge-8M",["--cold"],3,11),
       ("fresh-8M",[],9,15),("frag8-8M",[],9,15)]
for db,mode,reps,rounds in cells:
    lbl=f"{db} {'cold' if mode else 'warm'}"
    print(f"{lbl:16s} {compare(f'{W}/{db}.db',mode,reps,rounds,['stock','nofloor','ovvec'])}")
