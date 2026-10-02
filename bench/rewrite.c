/*
** Rewrite large values, a few per transaction.
**
**   rewrite TEMPLATE WORKDIR NTXN ROWS SIZE SYNC [SQL]
**
** Copies TEMPLATE to WORKDIR/m.db, then runs NTXN transactions, each of which
** executes SQL for ROWS randomly chosen rows (fixed seed, so every build sees
** the same sequence). SQL is bound with ?1 = SIZE plus up to 63 bytes and
** ?2 = a row id; the default stores a zero-filled value of that size. Prints
** the total milliseconds and the final page count of the file.
*/
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void){
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec*1e3 + t.tv_nsec/1e6;
}

int main(int argc, char **argv){
  char dst[512], cmd[4096], pre[200];
  const char *zSql;
  sqlite3 *db;
  sqlite3_stmt *st;
  sqlite3_int64 nRow;
  int nTxn, nRows, sz, i, r, nPage;
  double t0, t1;

  if( argc<7 ){
    fprintf(stderr,
        "usage: %s TEMPLATE WORKDIR NTXN ROWS SIZE SYNC [SQL]\n", argv[0]);
    return 1;
  }
  snprintf(dst, sizeof dst, "%s/m.db", argv[2]);
  snprintf(cmd, sizeof cmd, "rm -f %s %s-journal; cp %s %s; cat %s >/dev/null",
           dst, dst, argv[1], dst, dst);
  if( system(cmd) ) return 1;
  nTxn = atoi(argv[3]);
  nRows = atoi(argv[4]);
  sz = atoi(argv[5]);
  zSql = argc>7 ? argv[7] : "UPDATE t SET b=zeroblob(?1) WHERE id=?2";

  sqlite3_open(dst, &db);
  snprintf(pre, sizeof pre, "PRAGMA synchronous=%s; PRAGMA cache_size=-65536;",
           argv[6]);
  sqlite3_exec(db, pre, 0, 0, 0);
  sqlite3_prepare_v2(db, "SELECT max(id) FROM t", -1, &st, 0);
  sqlite3_step(st);
  nRow = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  if( sqlite3_prepare_v2(db, zSql, -1, &st, 0) ){
    fprintf(stderr, "%s\n", sqlite3_errmsg(db));
    return 1;
  }

  srand(42);
  t0 = now();
  for(i=0; i<nTxn; i++){
    sqlite3_exec(db, "BEGIN", 0, 0, 0);
    for(r=0; r<nRows; r++){
      sqlite3_bind_int(st, 1, sz + rand()%64);
      sqlite3_bind_int64(st, 2, 1 + rand()%nRow);
      if( sqlite3_step(st)!=SQLITE_DONE ){
        fprintf(stderr, "%s\n", sqlite3_errmsg(db));
        return 1;
      }
      sqlite3_reset(st);
    }
    if( sqlite3_exec(db, "COMMIT", 0, 0, 0) ){
      fprintf(stderr, "%s\n", sqlite3_errmsg(db));
      return 1;
    }
  }
  t1 = now();
  sqlite3_finalize(st);

  sqlite3_prepare_v2(db, "PRAGMA page_count", -1, &st, 0);
  sqlite3_step(st);
  nPage = sqlite3_column_int(st, 0);
  sqlite3_finalize(st);
  sqlite3_close(db);
  printf("%.1f %d\n", t1-t0, nPage);
  return 0;
}
