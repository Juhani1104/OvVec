/*
** blobbench -- OvVec benchmark driver.
**
** Generates SQLite databases whose overflow-page chains have a controlled
** degree of physical contiguity, then times reading a large BLOB back out.
** Compiled twice: once against stock SQLite, once against the OvVec patch.
**
**   blobbench gen    <db> --layout L --size S [--pagesize N]
**   blobbench read   <db> --reps N [--first] [--cold]
**   blobbench layout <db>
**
** Layouts:
**   fresh   target BLOB inserted into an empty database; its overflow pages
**           are allocated by extending the file, so the chain is contiguous.
**   fragK   the file is first filled with filler rows whose payload occupies
**           exactly K overflow pages, then every other filler row is deleted.
**           That leaves the free-list holding K-page holes, so the target
**           BLOB inserted afterwards gets a chain of K-page runs.
**   churn   10k pseudo-random insert/update/delete operations are applied
**           before the target BLOB is inserted, so it is allocated out of an
**           organically fragmented free-list.
**
** Reads:
**   default  one connection, reused across reps -- after the first read the
**            cursor's aOverflow[] chain cache is populated.
**   --first  a fresh connection per rep, so every read is a first traversal
**            with no chain cache.
**   --cold   as --first, and the database file is evicted from the page cache
**            before each rep (posix_fadvise(DONTNEED); needs no root).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include "sqlite3.h"

#define TARGET_ID 1

static double now_us(void){
  struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec*1e6 + t.tv_nsec/1e3;
}
static int cmp_double(const void *a, const void *b){
  double x=*(const double*)a, y=*(const double*)b; return x<y?-1:(x>y);
}
/* FNV-1a, used only to prove stock and patched builds return identical bytes */
static uint64_t hash64(const unsigned char *p, sqlite3_int64 n){
  uint64_t h=1469598103934665603ULL;
  for(sqlite3_int64 i=0;i<n;i++){ h^=p[i]; h*=1099511628211ULL; }
  return h;
}
/* deterministic LCG so churn layouts are reproducible */
static uint64_t rng_s = 88172645463325252ULL;
static uint64_t rnd(void){ rng_s^=rng_s<<13; rng_s^=rng_s>>7; rng_s^=rng_s<<17; return rng_s; }

static void die(sqlite3 *db, const char *what){
  fprintf(stderr,"%s: %s\n", what, db?sqlite3_errmsg(db):"(no db)");
  exit(1);
}
static void xexec(sqlite3 *db, const char *sql){
  char *err=0;
  if( sqlite3_exec(db,sql,0,0,&err)!=SQLITE_OK ){
    fprintf(stderr,"sql failed: %s\n  %s\n", err, sql); exit(1);
  }
}
static sqlite3_int64 parse_size(const char *s){
  char *end; double v=strtod(s,&end);
  if( *end=='k'||*end=='K' ) v*=1024;
  else if( *end=='m'||*end=='M' ) v*=1024*1024;
  else if( *end=='g'||*end=='G' ) v*=1024*1024*1024;
  return (sqlite3_int64)v;
}
static void insert_blob(sqlite3 *db, const char *tbl, sqlite3_int64 id, sqlite3_int64 n){
  char sql[128]; sqlite3_stmt *st;
  unsigned char *buf = malloc(n);
  if( !buf ){ fprintf(stderr,"oom\n"); exit(1); }
  /* content is a function of id so the payload is not trivially compressible
  ** and so the verification hash is layout-independent */
  for(sqlite3_int64 i=0;i<n;i++) buf[i] = (unsigned char)((i*31 + id*17) & 0xff);
  snprintf(sql,sizeof sql,"INSERT INTO %s(id,v) VALUES(?,?)",tbl);
  if( sqlite3_prepare_v2(db,sql,-1,&st,0)!=SQLITE_OK ) die(db,"prepare insert");
  sqlite3_bind_int64(st,1,id);
  sqlite3_bind_blob64(st,2,buf,n,SQLITE_STATIC);
  if( sqlite3_step(st)!=SQLITE_DONE ) die(db,"step insert");
  sqlite3_finalize(st); free(buf);
}

/* ------------------------------------------------------------------ gen */
static int cmd_gen(int argc, char **argv){
  const char *path=argv[0], *layout="fresh";
  sqlite3_int64 size=8*1024*1024; int pagesize=4096, i, reserve=0, bWal=0;
  int nRows=1, bNoCkpt=0;
  for(i=1;i<argc;i++){
    if( !strcmp(argv[i],"--layout") && i+1<argc ) layout=argv[++i];
    else if( !strcmp(argv[i],"--size") && i+1<argc ) size=parse_size(argv[++i]);
    else if( !strcmp(argv[i],"--pagesize") && i+1<argc ) pagesize=atoi(argv[++i]);
    else if( !strcmp(argv[i],"--reserve") && i+1<argc ) reserve=atoi(argv[++i]);
    else if( !strcmp(argv[i],"--wal") ) bWal=1;
    else if( !strcmp(argv[i],"--nockpt") ) bNoCkpt=1;
    else if( !strcmp(argv[i],"--rows") && i+1<argc ) nRows=atoi(argv[++i]);
    else { fprintf(stderr,"gen: unknown arg %s\n",argv[i]); return 1; }
  }
  unlink(path);
  sqlite3 *db;
  if( sqlite3_open(path,&db)!=SQLITE_OK ) die(db,"open");
  char pragma[64];
  /* Reserved bytes make usableSize != pageSize, which every OvVec path
  ** explicitly refuses; generating such a database checks that the fallback
  ** still produces correct results. */
  if( reserve>0 ){
    int r = reserve;
    if( sqlite3_file_control(db,"main",SQLITE_FCNTL_RESERVE_BYTES,&r)!=SQLITE_OK ){
      fprintf(stderr,"reserve_bytes failed\n"); exit(1);
    }
  }
  snprintf(pragma,sizeof pragma,"PRAGMA page_size=%d;",pagesize);
  xexec(db,pragma);
  /* Direct overflow read is disabled for any page that lives in a WAL, so a
  ** WAL database exercises the fallback until it is checkpointed. */
  xexec(db, bWal ? "PRAGMA journal_mode=WAL;" : "PRAGMA journal_mode=DELETE;");
  /* Without this the WAL is checkpointed when the last connection closes, so
  ** --nockpt would not actually leave any page living in the WAL. */
  if( bWal && bNoCkpt ) xexec(db,"PRAGMA wal_autocheckpoint=0;");
  xexec(db,"PRAGMA auto_vacuum=NONE;");      /* keep freed pages on the free-list */
  xexec(db,"PRAGMA synchronous=OFF;");
  xexec(db,"CREATE TABLE filler(id INTEGER PRIMARY KEY, v BLOB);");
  xexec(db,"CREATE TABLE target(id INTEGER PRIMARY KEY, v BLOB);");

  int fragK = 0;
  if( !strncmp(layout,"frag",4) ) fragK = atoi(layout+4);

  if( fragK>0 ){
    /* Filler rows of exactly fragK overflow pages each.  Enough of them to
    ** cover the target twice over, since half will be deleted. */
    sqlite3_int64 fillsz = (sqlite3_int64)fragK*(pagesize-4);
    sqlite3_int64 nfill  = (size/fillsz + 1)*3;
    xexec(db,"BEGIN;");
    for(sqlite3_int64 k=1;k<=nfill;k++) insert_blob(db,"filler",k,fillsz);
    xexec(db,"COMMIT;");
    xexec(db,"DELETE FROM filler WHERE id%2=0;");
  }else if( !strcmp(layout,"purge") || !strcmp(layout,"rnddel") ){
    /* Realistic reuse patterns: fill the file, free half of it in one
    ** transaction, then insert the target out of the resulting free-list.
    **   purge  -- delete the oldest half (log rotation / cache purge)
    **   rnddel -- delete a random half (scattered row deletion)
    ** Filler rows are large enough to own several overflow pages each. */
    sqlite3_int64 fillsz = (sqlite3_int64)8*(pagesize-4);
    sqlite3_int64 nfill  = (size/fillsz + 1)*3;
    xexec(db,"BEGIN;");
    for(sqlite3_int64 k=1;k<=nfill;k++) insert_blob(db,"filler",k,fillsz);
    xexec(db,"COMMIT;");
    if( !strcmp(layout,"purge") ){
      char sql[128];
      snprintf(sql,sizeof sql,"DELETE FROM filler WHERE id<=%lld",(long long)(nfill/2));
      xexec(db,sql);
    }else{
      xexec(db,"DELETE FROM filler WHERE (id*2654435761)%2=0;");
    }
  }else if( !strcmp(layout,"churn") ){
    sqlite3_int64 maxid=0;
    xexec(db,"BEGIN;");
    for(int k=0;k<10000;k++){
      uint64_t r=rnd(); int op=r%3;
      sqlite3_int64 sz = (sqlite3_int64)(1+(r>>8)%12)*(pagesize-4);
      if( op==0 || maxid==0 ){
        insert_blob(db,"filler",++maxid,sz);
      }else if( op==1 ){
        char sql[128];
        snprintf(sql,sizeof sql,"DELETE FROM filler WHERE id=%lld",
                 (long long)(1+(r>>16)%maxid));
        xexec(db,sql);
      }else{
        char sql[160];
        snprintf(sql,sizeof sql,
          "UPDATE filler SET v=zeroblob(%lld) WHERE id=%lld",
          (long long)sz, (long long)(1+(r>>16)%maxid));
        xexec(db,sql);
      }
      if( k%1000==999 ){ xexec(db,"COMMIT;"); xexec(db,"BEGIN;"); }
    }
    xexec(db,"COMMIT;");
  }else if( strcmp(layout,"fresh")!=0 ){
    fprintf(stderr,"gen: unknown layout %s\n",layout); return 1;
  }

  /* Several rows of differing size share one cursor during a scan, so
  ** aOverflow[] is reallocated and reused between them; a stale entry left
  ** from a longer row would steer a batch onto the wrong pages. */
  {
    sqlite3_int64 k;
    for(k=0;k<nRows;k++){
      sqlite3_int64 sz = nRows==1 ? size
                       : (size/nRows) * (1 + (k%5)) / 3 + (k%7)*(pagesize-4) + 1;
      if( sz<1 ) sz = 1;
      insert_blob(db,"target",TARGET_ID+k,sz);
    }
  }
  if( bWal && !bNoCkpt ) xexec(db,"PRAGMA wal_checkpoint(TRUNCATE);");
  if( bWal && bNoCkpt ){
    /* Closing the last connection checkpoints and removes the WAL, so leave
    ** the process without closing: the WAL survives on disk and the reader
    ** finds the target's pages living in it, where direct overflow read --
    ** and therefore every OvVec path -- must decline. */
    printf("gen  layout=%-6s size=%lld pagesize=%d reserve=%d wal=1 rows=%d (WAL left dirty) -> %s\n",
           layout,(long long)size,pagesize,reserve,nRows,path);
    fflush(stdout);
    _exit(0);
  }
  sqlite3_close(db);

  printf("gen  layout=%-6s size=%lld pagesize=%d reserve=%d wal=%d rows=%d -> %s\n",
         layout,(long long)size,pagesize,reserve,bWal,nRows,path);
  return 0;
}

/* --------------------------------------------------------------- layout */
/* Report the physical run-length distribution of the target BLOB's chain. */
static int cmd_layout(int argc, char **argv){
  const char *path=argv[0];
  sqlite3 *db; sqlite3_stmt *st;
  if( sqlite3_open(path,&db)!=SQLITE_OK ) die(db,"open");
  /* Order by the overflow sequence number encoded in dbstat's path
  ** ('/1c2/000+000007' is the 8th page of that cell's chain; the suffix is
  ** fixed-width HEX, so it must be ordered as text, not CAST to INTEGER),
  ** NOT by pageno:
  ** what matters is whether the chain is contiguous *in traversal order*,
  ** which is the only thing either OvVec or kernel readahead can exploit. */
  if( sqlite3_prepare_v2(db,
        "SELECT pageno FROM dbstat('main') "
        "WHERE name='target' AND path LIKE '%+%' "
        "ORDER BY substr(path, instr(path,char(43))+1)",-1,&st,0)
      !=SQLITE_OK ) die(db,"dbstat (build without SQLITE_ENABLE_DBSTAT_VTAB?)");
  sqlite3_int64 prev=-1, run=0, npage=0, nrun=0, maxrun=0, nback=0;
  while( sqlite3_step(st)==SQLITE_ROW ){
    sqlite3_int64 pg = sqlite3_column_int64(st,0);
    npage++;
    if( pg==prev+1 ){ run++; }
    else {
      if( prev>=0 && pg<prev ) nback++;     /* chain steps backwards here */
      if(run>maxrun) maxrun=run; if(run) nrun++; run=1;
    }
    prev=pg;
  }
  if(run){ nrun++; if(run>maxrun) maxrun=run; }
  sqlite3_finalize(st); sqlite3_close(db);
  printf("layout %-34s pages=%lld runs=%lld mean_run=%.1f max_run=%lld backward_links=%lld\n",
         path,(long long)npage,(long long)nrun,
         nrun?(double)npage/nrun:0.0,(long long)maxrun,(long long)nback);
  return 0;
}

/* ----------------------------------------------------------------- read */
static void evict(const char *path){
  int fd=open(path,O_RDONLY);
  if( fd<0 ) return;
  fsync(fd);
  posix_fadvise(fd,0,0,POSIX_FADV_DONTNEED);
  close(fd);
}
static int gScan = 0;
static double read_once(sqlite3 *db, uint64_t *phash, sqlite3_int64 *pn){
  sqlite3_stmt *st;
  if( gScan ){
    /* One cursor, every row, ascending: aOverflow[] is resized and reused
    ** between rows of different lengths. */
    uint64_t h=1469598103934665603ULL; sqlite3_int64 tot=0; double t0;
    if( sqlite3_prepare_v2(db,"SELECT v FROM target ORDER BY id",-1,&st,0)!=SQLITE_OK )
      die(db,"prepare scan");
    t0=now_us();
    int rc;
    while( (rc=sqlite3_step(st))==SQLITE_ROW ){
      const unsigned char *p=sqlite3_column_blob(st,0);
      sqlite3_int64 n=sqlite3_column_bytes(st,0);
      for(sqlite3_int64 i=0;i<n;i++){ h^=p[i]; h*=1099511628211ULL; }
      tot+=n;
    }
    /* A scan cut short by SQLITE_BUSY hashes only part of the table, which
    ** looks exactly like a corrupted read; treat anything but DONE as fatal
    ** rather than letting it masquerade as a data mismatch. */
    if( rc!=SQLITE_DONE ){
      fprintf(stderr,"scan ended with rc=%d: %s\n", rc, sqlite3_errmsg(db));
      exit(3);
    }
    { double dt=now_us()-t0; sqlite3_finalize(st); *phash=h; *pn=tot; return dt; }
  }
  if( sqlite3_prepare_v2(db,"SELECT v FROM target WHERE id=1",-1,&st,0)!=SQLITE_OK )
    die(db,"prepare read");
  double t0=now_us();
  if( sqlite3_step(st)!=SQLITE_ROW ) die(db,"step read");
  const unsigned char *p = sqlite3_column_blob(st,0);
  sqlite3_int64 n = sqlite3_column_bytes(st,0);
  double dt = now_us()-t0;
  /* Verification hashing is deliberately OUTSIDE the timed region: it is pure
  ** CPU work identical for both builds, and over an 8 MiB BLOB it costs several
  ** milliseconds -- enough to swamp the I/O difference being measured. */
  uint64_t h = hash64(p,n);
  sqlite3_finalize(st);
  *phash=h; *pn=n;
  return dt;
}
/* Read through the incremental-BLOB API with one long-lived handle.  The
** BtCursor -- and therefore its aOverflow[] chain cache -- stays alive across
** reps, so combining this with --cold isolates "chain already known, data cold"
** from "chain unknown" (the speculative-batching path). */
static sqlite3_int64 gBlobOff = 0;
static double read_once_blob(sqlite3_blob *bh, unsigned char *buf, int n,
                             uint64_t *phash){
  double t0=now_us();
  if( sqlite3_blob_read(bh,buf,n,(int)gBlobOff)!=SQLITE_OK ){
    fprintf(stderr,"blob_read failed\n"); exit(1);
  }
  double dt=now_us()-t0;
  *phash=hash64(buf,n);
  return dt;
}
static int cmd_read(int argc, char **argv){
  const char *path=argv[0];
  int reps=20, first=0, cold=0, useblob=0, i;
  sqlite3_int64 rdOff=0, rdLen=-1;        /* partial read window, blob API */
  int bScan=0;                            /* scan every row with one cursor */
  for(i=1;i<argc;i++){
    if( !strcmp(argv[i],"--reps") && i+1<argc ) reps=atoi(argv[++i]);
    else if( !strcmp(argv[i],"--first") ) first=1;
    else if( !strcmp(argv[i],"--scan") ) bScan=1;
    else if( !strcmp(argv[i],"--blob") ) useblob=1;
    else if( !strcmp(argv[i],"--offset") && i+1<argc ){ rdOff=parse_size(argv[++i]); useblob=1; }
    else if( !strcmp(argv[i],"--len") && i+1<argc ){ rdLen=parse_size(argv[++i]); useblob=1; }
    else if( !strcmp(argv[i],"--cold") ) cold=1;
    else { fprintf(stderr,"read: unknown arg %s\n",argv[i]); return 1; }
  }
  gScan = bScan;
  if( bScan ) useblob = 0;
  if( cold && !useblob ) first=1;   /* without a live handle, cold needs a
                                    ** fresh connection to drop SQLite's cache */
  double *t = malloc(sizeof(double)*reps);
  uint64_t h=0, h0=0; sqlite3_int64 n=0;
  sqlite3 *db=0;
  if( useblob ){
    sqlite3_blob *bh; sqlite3_int64 n;
    if( sqlite3_open(path,&db)!=SQLITE_OK ) die(db,"open");
    sqlite3_busy_timeout(db,10000);
    if( sqlite3_blob_open(db,"main","target","v",TARGET_ID,0,&bh)!=SQLITE_OK )
      die(db,"blob_open");
    n = sqlite3_blob_bytes(bh);
    /* A partial read starting part-way into the BLOB exercises the paths where
    ** the first overflow page is entered at a non-zero offset, which the
    ** vectored read must decline. */
    if( rdOff>n ) rdOff = n;
    if( rdLen<0 || rdOff+rdLen>n ) rdLen = n-rdOff;
    n = rdLen;
    unsigned char *buf = malloc(n?n:1);
    gBlobOff = rdOff;
    read_once_blob(bh,buf,(int)n,&h0);          /* warm-up: populate aOverflow[] */
    for(i=0;i<reps;i++){
      if( cold ) evict(path);
      t[i]=read_once_blob(bh,buf,(int)n,&h);
      if( h!=h0 ){ fprintf(stderr,"HASH MISMATCH at rep %d\n",i); return 2; }
    }
    sqlite3_blob_close(bh); sqlite3_close(db); free(buf);
    qsort(t,reps,sizeof(double),cmp_double);
    printf("read %-34s reps=%-4d mode=%-6s median=%9.3f ms  p10=%9.3f  p90=%9.3f  bytes=%lld  hash=%016llx\n",
           path,reps, cold?"bcold":"bwarm", t[reps/2]/1000.0, t[reps/10]/1000.0,
           t[reps*9/10]/1000.0,(long long)n,(unsigned long long)h0);
    free(t); return 0;
  }
  if( !first && sqlite3_open(path,&db)!=SQLITE_OK ) die(db,"open");
  if( !first ) sqlite3_busy_timeout(db,10000);
  if( !first ){ double w=read_once(db,&h0,&n); (void)w; }   /* warm-up read */
  for(i=0;i<reps;i++){
    if( first ){
      if( cold ) evict(path);
      if( sqlite3_open(path,&db)!=SQLITE_OK ) die(db,"open");
      sqlite3_busy_timeout(db,10000);
    }
    t[i]=read_once(db,&h,&n);
    if( h0==0 ) h0=h;
    if( h!=h0 ){ fprintf(stderr,"HASH MISMATCH at rep %d\n",i); return 2; }
    if( first ){ sqlite3_close(db); db=0; }
  }
  if( db ) sqlite3_close(db);
  qsort(t,reps,sizeof(double),cmp_double);
  printf("read %-34s reps=%-4d mode=%-6s median=%9.3f ms  p10=%9.3f  p90=%9.3f  bytes=%lld  hash=%016llx\n",
         path,reps, cold?"cold":(first?"first":"warm"),
         t[reps/2]/1000.0, t[reps/10]/1000.0, t[reps*9/10]/1000.0,
         (long long)n,(unsigned long long)h0);
  free(t);
  return 0;
}

int main(int argc, char **argv){
  if( argc<3 ){
    fprintf(stderr,
      "usage: %s gen|read|layout <db> [options]\n"
      "  gen    <db> --layout fresh|fragK|churn --size 8M [--pagesize 4096]\n"
      "  read   <db> --reps 20 [--first] [--cold]\n"
      "  layout <db>\n", argv[0]);
    return 1;
  }
  if( !strcmp(argv[1],"gen") )    return cmd_gen(argc-2,argv+2);
  if( !strcmp(argv[1],"read") )   return cmd_read(argc-2,argv+2);
  if( !strcmp(argv[1],"layout") ) return cmd_layout(argc-2,argv+2);
  fprintf(stderr,"unknown command %s\n",argv[1]);
  return 1;
}
