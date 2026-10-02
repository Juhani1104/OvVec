# OvVec benchmark notes

Detailed notes on the **read path**: how the layouts are generated, how reads
are measured, every regression that shaped the design, and the negative
results. The [top-level README](../README.md) is the place to start; this file
is for anyone checking the work.

```sh
./build.sh                    # stock + patched amalgamations, every driver
python3 compare.py WORKDIR    # the comparison quoted in the top-level README
./recovery.sh WORKDIR         # crash-recovery check
./run.sh [WORKDIR]            # the read-path matrix described below
```

## What gets built

`build.sh` unpacks the SQLite 3.53.4 source archive twice, applies
`../ovvec.patch` to one copy, regenerates the amalgamation for each, and links
every driver against both:

| driver | what it does |
|---|---|
| `blobbench` | generates the read layouts below and times reading one large value |
| `sqltime` | times one SQL statement on a fresh copy of a template database |
| `rewrite` | runs N transactions that each rewrite a few large values |
| `crashtest` | kills the process mid-transaction, for `recovery.sh` |

`tools/difftest.py` and `tools/corrupttest.py` expect an extra ASan build of
`blobbench` (`blobbench-ovvec-asan`), which `build.sh` does not produce.

Both builds use identical flags:

```
-O2 -DSQLITE_DIRECT_OVERFLOW_READ -DSQLITE_ENABLE_DBSTAT_VTAB -DSQLITE_THREADSAFE=0
```

`SQLITE_DIRECT_OVERFLOW_READ` is **not** a SQLite default, but OvVec lives inside
that ifdef, so the honest baseline is stock built with it too. `DBSTAT_VTAB` is
only used by the `layout` subcommand.

## Layout definitions

The original definitions of `frag2/4/8` were not recorded, so these are a
reconstruction. `blobbench layout` reports the run-length distribution of the
target BLOB's overflow chain via `dbstat`, which confirms each generator does
what it claims (`mean_run` comes out at exactly 8.0 / 4.0 / 2.0).

| layout | how it is built | measured mean run |
|---|---|---|
| `fresh` | target inserted into an empty database, so pages are allocated by extending the file | 2050 (one run) |
| `fragK` | fill with rows occupying exactly K overflow pages, delete every other row (leaving K-page holes on the free-list), then insert the target | K |
| `churn` | 10k seeded pseudo-random insert/update/delete ops before inserting the target | varies (~293 in the current build) |

`auto_vacuum=NONE` and `journal_mode=DELETE` are set during generation: the
former keeps freed pages on the free-list instead of truncating them, the latter
matters because direct overflow read is disabled for any page that lives in a WAL.

## Read modes

| mode | connection | page cache | what it isolates |
|---|---|---|---|
| default (`warm`) | reused across reps | hot | steady-state reads with `BtCursor.aOverflow[]` already populated |
| `--first` | fresh per rep | hot | first traversal of the chain — the speculative-batching path |
| `--cold` | fresh per rep | evicted with `posix_fadvise(DONTNEED)` (no root needed) | real device I/O |

Correctness is checked on every single read: the BLOB is hashed (FNV-1a) and any
mismatch aborts the run, so stock and patched must return identical bytes across
all layouts. The hash is computed **outside** the timed region — over 8 MiB it
costs several ms, enough to swamp the effect being measured.

## What the read path now does

OvVec detects a physically contiguous run of overflow pages in **either**
direction and reads it with one `preadv()`, sizing the speculative window from
the length of the previous run in the same chain.

Direction matters because SQLite's free-list returns pages in descending order
after a bulk `DELETE` -- log rotation, cache purge, `DELETE ... WHERE ts < x`
followed by more inserts. In such a database the overflow chains run backwards.
A forward-only test sees no contiguity there at all, and neither does kernel
readahead, so those reads collapse to one 4 KiB device I/O per page: 127 ms for
an 8 MiB BLOB against 3.6 ms for a contiguous one, a 36x cliff.

Window sizing matters because a window wider than the run reads pages that are
then discarded. Three rules came out of measurement, each fixing a regression
that the previous version caused:

1. **Size the window from the previous run's length**, not a fixed 8 pages. A
   fixed window cost **+92.6%** on warm reads of 4-page runs, because it read
   2.5x the data it used.
2. **Bound the window in bytes, not just pages** (`SQLITE_OVVEC_MAX_BYTES`,
   512 KiB = 128 pages at the default page size). The cost of a wrong guess is
   `(window - run) * pageSize`, so a page-counted window over-reads sixteen
   times as much at a 64 KiB page size. Unbounded, a 64 KiB-page database read
   4,778 KiB where stock read 3,434 KiB -- 39% more data to save six syscalls,
   and slower on every device tested.
3. **Do not speculate at all when the previous run was a single page**, unless
   the current run has already proven itself (four pages or more, at which
   point the doubling schedule takes over). Chains of isolated hops gain
   nothing from speculation and pay for every wrong guess. The "unless" matters:
   without it, a long run that happens to follow one isolated hop is never
   batched, which silently removed the entire gain on one layout.

Reading a run's length out of the chain cache has to be bounded, or the
measurement that sizes the window costs more than the window saves. Four pages
settle whether the current run is established, and the previous run's length is
never consulted once it is, so neither backwards scan needs to be unbounded.
Without those bounds the scan is repeated for every speculative decision and
the cost of reading one BLOB grows quadratically:

| BLOB | backwards steps, unbounded | bounded |
|---|---|---|
| 16 MiB | 67,452 | 106 |
| 64 MiB | 1,055,964 | 394 |
| 256 MiB | 16,806,492 | 1,546 |
| 512 MiB | 67,298,517 | 3,085 |

Doubling the BLOB size quadrupled the step count. That growth only shows up on
layouts built from **long** runs; where runs are a handful of pages the walk was
already short and the bound changes almost nothing (64 MiB, 4 KiB pages:
`fresh` 1,055,964 -> 394, but `frag8` 20,504 -> 20,492).

Bounding it must not change any decision, and checking that `preadv` counts
match is not enough: a first attempt capped the previous run's length at
`SQLITE_OVVEC_MAX_PAGES+1`, which silently shrank some windows from 128 pages
to 127, because the window is `nPrev-nInRun` and the cap has to leave room for
the subtraction. The counts were identical; the requests were not. `OVVEC_TRACE=1`
dumps every vectored request (offset, page count, direction) so the two builds
can be diffed line by line -- across 63 combinations of layout, size and page
size the sequences are byte-identical, and so are the BLOB hashes.

## Results

Stock vs OvVec, both built with `SQLITE_DIRECT_OVERFLOW_READ`. Medians over
randomised-order rounds; `+` means OvVec is slower; `*` marks non-overlapping
ranges.

8 MiB BLOB, 4 KiB pages, NVMe/XFS:

| layout | chain | cold | warm |
|---|---|---|---|
| `purge` | descending, long runs | **-89.7%*** | -37.7% |
| `frag8` | descending, 8-page runs | **-52.1%*** | -27.5% |
| `rnddel` | descending, 8-page runs | **-52.0%*** | -17.9% |
| `frag4` | descending, 4-page runs | **-20.7%*** | +6.8% |
| `frag2` | descending, 2-page runs | +0.1% | -8.2% |
| `churn` | ascending, long runs | +0.5% | **-58.2%*** |
| `fresh` | one ascending run | +0.3% | **-57.8%*** |

A 158-cell sweep over 3 devices (NVMe/XFS, HDD/ext4, tmpfs) x 4 page sizes
(512 to 65536) x 6 layouts x {warm, cold} gives 44 cells with a clear gain, 32
with an overlapping gain, 44 ties, 34 overlapping slowdowns and 4 apparent
regressions. **Re-measured at 35 rounds, none of those 4 reproduce** -- every
one falls back into overlapping ranges. With 158 cells and a
non-overlapping-ranges criterion at 7-9 rounds, a handful of false positives is
expected; they must be re-measured before being believed.

What does survive is a tendency to be a few percent slower on **warm reads at
large page sizes** (e.g. HDD, 16 KiB pages, 8-page runs: 0.029 ms to 0.036 ms).
It is microseconds, and it is the residual over-read that the byte bound
limits but does not eliminate.

Smaller pages go the other way: at 512-byte pages the gains are the largest
measured anywhere, -60% warm and -66% cold on descending chains, on both NVMe
and spinning disk.

### Correctness

| check | result |
|---|---|
| `testrunner.tcl all` -- every tcl test plus all permutations | **4,524,828 tests, 0 errors** |
| `make sdevtest` -- the suite under ASan and UBSan | **980,149 tests, 0 errors** |
| `make quicktest` (patched tree) | **978,706 tests, 0 errors** |
| differential fuzz, 600 cases, patched build under ASan+UBSan | 0 failures |
| differential fuzz, 300 cases, patched build under `SQLITE_DEBUG` | 0 failures |
| chain-length boundary cases (1/2/3/8/127/128/129/255/256/257 pages, +-1) | 180 cases, 0 failures |
| corrupted next-page pointers | 100 trials, 0 divergences, 0 sanitizer reports |
| layout x size x read-mode hash comparison | 105 combinations, all identical to stock |
| multi-row scan: one cursor, rows of differing length | 162 cases (ASan and `SQLITE_DEBUG`), 0 failures |
| pages resident in an un-checkpointed WAL | `preadv` count 0 -- every path declines -- hashes match |
| `mmap_size` enabled | 6 layouts x 3 modes, hashes match, no assertion |
| concurrent readers against a writer and checkpointer | 4 builds x 2 journal modes x 32 readers, 0 failures |

Run the suite with `--jobs 8`, not the default 20. This is a shared machine
(21 logged-in users, other people's jobs running for weeks); at 20-way
parallelism the timing- and memory-sensitive permutations -- `memsubsys1`,
`memsubsys2`, `no_mutex_try`, `prepare`, `mmap` -- produced 645 spurious
failures in one run, and the runner itself was killed twice. The same binaries
gave 0 errors at `--jobs 8`. Check `uptime` before trusting a full-suite run.
Also raise nothing and lower `ulimit -n` to something normal (1024): with this
machine's default of 524,288, `misc7.test`'s out-of-file-descriptors test opens
half a million descriptors and is killed after 462 s, where it passes in 2.6 s
at a sane limit.

Two earlier full-suite runs each reported one failure -- `misc7.test` (the
descriptor limit above) and `config=mmap thread1.test` ("unfreed memory" from a
threaded test, which passed 5 times out of 5 standalone). Neither can involve
this patch: both operate on tables whose largest value is 64 bytes, so no
overflow page exists, and `preadv` is called zero times during them.

Note that `testrunner.tcl` invoked directly does **not** rebuild. Swapping the
sources under it changes nothing about what is tested; a stock baseline needs
the binaries rebuilt first.

Build configurations that compile cleanly: without
`SQLITE_DIRECT_OVERFLOW_READ` (the whole patch compiles out),
`SQLITE_OMIT_INCRBLOB`, `SQLITE_OMIT_AUTOVACUUM`, `SQLITE_OMIT_WAL`,
`SQLITE_DEBUG`, `SQLITE_MAX_MMAP_SIZE`, `SQLITE_THREADSAFE=1`. A 32-bit build
was **not** tested: no multilib headers on this machine. Valgrind and
MemorySanitizer were also unavailable, so uninitialised-read coverage rests on
ASan plus `SQLITE_DEBUG` rather than a dedicated checker.

The multi-row scan is worth calling out: `BtCursor.aOverflow[]` is reallocated
and reused between rows of a scan, so a stale entry from a longer row could
steer a batch onto the wrong pages. Nothing else in this harness exercises that.

The concurrency result cost one false alarm. The first version of the scan loop
ignored `sqlite3_step`'s return code, so a scan cut short by `SQLITE_BUSY`
hashed only part of the table and looked exactly like a corrupted read -- it
failed once in 24 runs under the patch and zero times under stock, which is the
shape of a real bug. It was the harness. The loop now treats anything but
`SQLITE_DONE` as fatal.

The fuzz cases randomise layout, BLOB size, page size (512..65536), reserved
bytes, journal mode and the offset/length of partial `sqlite3_blob_read` calls.
Path coverage was checked rather than assumed: every layout except `frag2`
(whose 2-page runs cannot satisfy the two-link confirmation) issues `preadv`,
as does every page size from 512 to 65536; `reserved_bytes > 0` correctly
issues none, since `usableSize != pageSize` disables the path.

## Measurement methodology

Every comparison here is the median of many rounds in which the **order of the
binaries is randomised within each round**. Alternating them in a fixed
A-then-B order is not enough: on reads of a few hundred microseconds it
produces a systematic bias large enough to invent regressions that do not
exist. A fixed-order run of a cold 64 KiB read reported OvVec 30.6% slower than
stock with non-overlapping ranges; randomising the order showed the two
indistinguishable -- the only possible answer, since for that workload the
patched build issues exactly the same syscalls as stock (`pread64=102`, no
`preadv`). `tools/rand.py` does it correctly.

Randomising is necessary but not sufficient. Non-overlapping ranges at 7-9
rounds still produce false positives across a large sweep: 4 of 158 cells
looked like regressions and none of them reproduced at 35 rounds. Any cell that
matters gets re-measured before it is believed or acted on.

## Where the gain is, and where it is not

The wins are on **descending** chains and on **warm** reads. Cold reads of a
readahead-friendly chain are a wash or slightly worse, and that is not a defect
to be tuned away -- it is kernel readahead doing the same job better.

For `churn-1M` (one 246-page ascending run) the patched build issues 107
syscalls against stock's 1,302 and is still 6.2% slower: stock's 256 sequential
`pread`s let readahead prefetch asynchronously in 128 KiB windows, overlapping
the next fetch with the current copy, while two synchronous 512 KiB `preadv`s
overlap with nothing. Fewer syscalls is not less time.

The three confirmed regressions -- `64K churn cold` +7.4%, `1M churn cold`
+6.2%, `64K frag4 cold` +6.5% -- are all cold reads of small payloads on
layouts the OS can already see, and cost 0.03-0.08 ms each. The same layouts
gain 50% or more when warm. Resolving the trade-off would require knowing at
the b-tree layer whether the pages are already cached, which it cannot.

A guard was tried: suppress vectored reads for ascending runs narrower than one
readahead window (32 pages). It changed nothing measurable in any cell, having
been motivated by the fixed-order artifact above, and was removed rather than
shipped as unjustified complexity.

## Remaining limit: the two-link confirmation

Speculation only starts after two consecutive contiguous links have been
observed, so every run costs two page-at-a-time reads at its head. For 8-page
runs that is 2 `pread64` per `preadv` (measured: 5,165 and 2,600 over 9 reads),
which is why short runs reach -52% against an oracle ceiling of -75%. For
2-page runs the rule can never be satisfied at all, so `frag2` gets nothing.

Removing it means speculating at a run boundary with no confirmation, which
`tools/spec.c` shows costs far more in over-read than it returns.

## Negative result: skipping the confirmation at run boundaries

`tools/spec.c` simulates a policy that, on a run boundary, opens the next
window immediately using the learned run length and no reconfirmation -- the
obvious way to remove the cost described above. In the cold regime it buys
about 2 points on 8-page runs (-52.2% vs -50.3%) while reading 4,595 pages
instead of 2,570, over twice the data, and it *regresses* on 2-page runs
(+8.6%) because the probe can only ever fire on the second page of a run and so
always misses. Since over-read is what hurts warm reads, that trade is worse
still there. It is not implemented.

Note this is the opposite conclusion to sizing the window from the previous
run's length, which *is* implemented: that changes how wide a speculative read
is, not whether one is issued without evidence, and it reduced over-read rather
than increasing it.

## Known gaps vs. the originally recorded results

- **Cold reads on an ascending, contiguous chain show no speedup** -- see above;
  this is readahead doing the batching for free, not the patch failing. The
  previously recorded "first read, cold -27.1%" has not been reproduced under
  any condition tested here and should not be quoted until it is.
- **The `LargeCopy` baseline is not restored.** It was a third build variant (one
  big pread plus a memcpy of each payload out of a temp buffer); that code is not
  in the repository and would have to be rewritten as a separate patch.
- **The fragmented rows are noisy.** `frag4` has come out anywhere between +1.5%
  and −29.2% across runs at 25 reps. Run counts need raising, and stock/patched
  should be interleaved within a run rather than measured back to back, before
  any fragmented-layout number is quoted.
