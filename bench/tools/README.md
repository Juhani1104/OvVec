# Replay tools

Both tools replay a *known* overflow chain against the raw database file, with
no SQLite involved. They exist to separate "what the I/O pattern costs" from
"what the patch manages to recover".

Get a chain in traversal order with:

```sh
blobbench layout <db>            # summary
# full page list, in chain order (note: dbstat's overflow sequence number is
# fixed-width HEX, so it must be ordered as text, never CAST to INTEGER):
sqlite3 <db> "SELECT pageno FROM dbstat('main') WHERE name='target' \
   AND path LIKE '%+%' ORDER BY substr(path, instr(path,'+')+1);" > chain.txt
```

- `ceiling.c` — upper bound: one preadv per maximal contiguous run, either
  direction, chain fully known in advance.
- `spec.c` — what a real implementation can reach on a *first* read, where page
  i+1 is only knowable after page i has been read. Compares per-page, the
  forward-only policy, bidirectional, bidirectional + run-length learning, and
  the oracle.

Build: `gcc -O2 -o ceiling ceiling.c` (same for spec).
Run:   `./spec <db> chain.txt`
