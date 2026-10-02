/* Ceiling estimate: replay a known overflow chain against the raw db file,
** (a) one pread per page, (b) coalescing maximal physically-contiguous runs
** in EITHER direction into one preadv.  Page cache evicted before each trial. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>
#include <time.h>
#define PG 4096
#define MAXIOV 128
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e6+t.tv_nsec/1e3;}
static int cmpd(const void*a,const void*b){double x=*(double*)a,y=*(double*)b;return x<y?-1:x>y;}
static void evict(int fd){ fsync(fd); posix_fadvise(fd,0,0,POSIX_FADV_DONTNEED); }
int main(int argc,char**argv){
  FILE *f=fopen(argv[2],"r"); long *pg=malloc(sizeof(long)*1<<20); int n=0;
  while(fscanf(f,"%ld",&pg[n])==1) n++;
  fclose(f);
  int fd=open(argv[1],O_RDONLY); if(fd<0){perror("open");return 1;}
  char *buf=aligned_alloc(4096,(size_t)PG*n);
  int trials=9; double ta[9],tb[9];
  for(int t=0;t<trials;t++){
    evict(fd);
    double t0=now();
    for(int i=0;i<n;i++) if(pread(fd,buf+(size_t)i*PG,PG,(off_t)(pg[i]-1)*PG)!=PG) return 2;
    ta[t]=now()-t0;
    evict(fd);
    t0=now();
    int i=0, nio=0;
    while(i<n){
      int j=i+1, dir=0;
      if( j<n && pg[j]==pg[i]+1 ) dir=1; else if( j<n && pg[j]==pg[i]-1 ) dir=-1;
      while( dir && j<n && pg[j]==pg[j-1]+dir && (j-i)<MAXIOV ) j++;
      int cnt=j-i;
      struct iovec iv[MAXIOV];
      /* lowest page in the run starts the single contiguous read; iovecs are
      ** assigned so each page lands in its chain-order slot, which for a
      ** descending run simply means filling the iovec array backwards */
      for(int k=0;k<cnt;k++){
        int slot = (dir>=0) ? i+k : i+cnt-1-k;
        iv[k].iov_base = buf+(size_t)slot*PG; iv[k].iov_len = PG;
      }
      long first = (dir>=0) ? pg[i] : pg[j-1];
      if( preadv(fd,iv,cnt,(off_t)(first-1)*PG) != (ssize_t)PG*cnt ) return 3;
      nio++; i=j;
    }
    tb[t]=now()-t0;
    if(t==0) printf("pages=%d   per-page I/Os=%d   coalesced I/Os=%d\n",n,n,nio);
  }
  qsort(ta,trials,sizeof(double),cmpd); qsort(tb,trials,sizeof(double),cmpd);
  printf("%-28s per-page %8.2f ms   bidirectional-coalesced %8.2f ms   %+.1f%%\n",
         argv[1], ta[trials/2]/1000, tb[trials/2]/1000,
         (tb[trials/2]-ta[trials/2])/ta[trials/2]*100);
  return 0;
}
