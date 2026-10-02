/*
** Kill a process in the middle of a transaction, for recovery testing.
**
**   crashtest TEMPLATE WORKDIR NPRE ROWS SIZE SEED CACHE
**
** Copies TEMPLATE to WORKDIR/c.db and commits NPRE transactions, each of
** which rewrites ROWS random rows -- alternately rotating a value by one byte
** (same size) and replacing it with SIZE/2..3*SIZE/2 bytes (new size) -- then
** deletes two rows and inserts two large ones. After the last commit the
** file (and its WAL, if any) is copied to WORKDIR/snap.db. One more
** transaction then starts and the process calls _exit() before COMMIT, with
** CACHE pages of page cache so that dirty pages have already been spilled.
**
** Opening c.db afterwards with any SQLite build must recover it to exactly
** the content of snap.db; compare the two with the shell's sha3_query().
*/
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv){
  char dst[512], cmd[4096], pre[128];
  sqlite3 *db;
  sqlite3_stmt *st, *mx;
  int nPre, nRows, sz, n, t, r;

  if( argc<8 ){
    fprintf(stderr,
        "usage: %s TEMPLATE WORKDIR NPRE ROWS SIZE SEED CACHE\n", argv[0]);
    return 1;
  }
  snprintf(dst, sizeof dst, "%s/c.db", argv[2]);
  snprintf(cmd, sizeof cmd, "rm -f %s %s-journal %s-wal %s-shm; cp %s %s",
           dst, dst, dst, dst, argv[1], dst);
  if( system(cmd) ) return 1;
  nPre = atoi(argv[3]);
  nRows = atoi(argv[4]);
  sz = atoi(argv[5]);
  srand(atoi(argv[6]));

  sqlite3_open(dst, &db);
  snprintf(pre, sizeof pre, "PRAGMA cache_size=%s", argv[7]);
  sqlite3_exec(db, pre, 0, 0, 0);
  sqlite3_prepare_v2(db,
      "UPDATE t SET b=CASE WHEN ?1%2 THEN substr(b,2)||substr(b,1,1)"
      " ELSE zeroblob(?1) END WHERE id=?2", -1, &st, 0);
  sqlite3_prepare_v2(db, "SELECT max(id) FROM t", -1, &mx, 0);
  sqlite3_step(mx);
  n = sqlite3_column_int(mx, 0);
  sqlite3_finalize(mx);

  for(t=0; t<=nPre; t++){
    sqlite3_exec(db, "BEGIN", 0, 0, 0);
    for(r=0; r<nRows; r++){
      sqlite3_bind_int(st, 1, sz/2 + rand()%sz);
      sqlite3_bind_int(st, 2, 1 + rand()%n);
      if( sqlite3_step(st)!=SQLITE_DONE ) return 2;
      sqlite3_reset(st);
      if( t==nPre && r==nRows-1 ) _exit(0);
    }
    sqlite3_exec(db,
        "DELETE FROM t WHERE id IN (SELECT id FROM t ORDER BY random() LIMIT 2)",
        0, 0, 0);
    sqlite3_exec(db,
        "INSERT INTO t(b) VALUES(randomblob(70000)),(randomblob(300000))",
        0, 0, 0);
    if( sqlite3_exec(db, "COMMIT", 0, 0, 0) ) return 3;
    if( t==nPre-1 ){
      snprintf(cmd, sizeof cmd, "cp %s %s/snap.db; rm -f %s/snap.db-wal;"
               " cp %s-wal %s/snap.db-wal 2>/dev/null; true",
               dst, argv[2], argv[2], dst, argv[2]);
      if( system(cmd) ) return 4;
    }
  }
  return 0;
}
