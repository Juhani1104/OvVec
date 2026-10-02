/*
** Time one SQL statement against a fresh copy of a template database.
**
**   sqltime TEMPLATE WORKDIR COLD [SYNC [SQL]]
**
** TEMPLATE is copied to WORKDIR/work.db first, so every run starts from the
** same file. COLD=1 evicts the copy from the OS page cache before the
** statement runs (posix_fadvise, no root needed); COLD=0 reads it in so the
** cache is warm. SYNC is the synchronous pragma (default OFF). SQL defaults
** to deleting half of the rows of table t. Prints milliseconds.
*/
#include "sqlite3.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static double now(void){
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec*1e3 + t.tv_nsec/1e6;
}

int main(int argc, char **argv){
  char dst[512], cmd[4096], pre[256];
  sqlite3 *db;
  double t0, t1;
  int fd, rc;

  if( argc<4 ){
    fprintf(stderr, "usage: %s TEMPLATE WORKDIR COLD [SYNC [SQL]]\n", argv[0]);
    return 1;
  }
  snprintf(dst, sizeof dst, "%s/work.db", argv[2]);
  snprintf(cmd, sizeof cmd, "rm -f %s %s-journal %s-wal; cp %s %s",
           dst, dst, dst, argv[1], dst);
  if( system(cmd) ) return 1;

  fd = open(dst, O_RDONLY);
  fsync(fd);
  if( atoi(argv[3]) ){
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
  }else{
    static char buf[1<<20];
    while( read(fd, buf, sizeof buf)>0 ){}
  }
  close(fd);

  sqlite3_open(dst, &db);
  snprintf(pre, sizeof pre, "PRAGMA synchronous=%s; PRAGMA cache_size=-65536;"
           " SELECT count(*) FROM sqlite_schema", argc>4 ? argv[4] : "OFF");
  sqlite3_exec(db, pre, 0, 0, 0);

  t0 = now();
  rc = sqlite3_exec(db, argc>5 ? argv[5] : "DELETE FROM t WHERE id%2=0", 0,0,0);
  t1 = now();
  if( rc ){
    fprintf(stderr, "%s\n", sqlite3_errmsg(db));
    return 1;
  }
  sqlite3_close(db);
  printf("%.3f\n", t1-t0);
  return 0;
}
