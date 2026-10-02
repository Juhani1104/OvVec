/* First-read yield: replay a chain respecting the pointer dependency (page i+1
** is only knowable after page i has been read), under three policies:
**   oracle    whole chain known in advance, maximal bidirectional coalescing
**   fwd       OvVec as it stands: confirm two consecutive +1 links, then batch
**             8->16->...->128, validate, revert to 8 on a break
**   bidir     same policy but a run may be ascending OR descending
** Speculative reads pay for every page in the window, even the discarded tail. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>
#include <time.h>
#include <sys/stat.h>
#define PG 4096
#define MAXW 128
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e6+t.tv_nsec/1e3;}
static int cmpd(const void*a,const void*b){double x=*(double*)a,y=*(double*)b;return x<y?-1:x>y;}
static long *pg; static int n; static char *buf; static int fd; static long nFilePg;
static long nio, npageio;

static void rd1(int i){
  if( pread(fd,buf+(size_t)i*PG,PG,(off_t)(pg[i]-1)*PG)!=PG ) exit(2);
  nio++; npageio++;
}
/* one vectored read of cnt physically contiguous pages starting (in chain
** order) at index i and running in direction dir */
static void rdv(int i,int cnt,int dir){
  struct iovec iv[MAXW];
  for(int k=0;k<cnt;k++){
    int slot = (dir>0) ? i+k : i+cnt-1-k;
    iv[k].iov_base = buf+(size_t)slot*PG; iv[k].iov_len = PG;
  }
  long first = (dir>0) ? pg[i] : pg[i]-(cnt-1);
  if( preadv(fd,iv,cnt,(off_t)(first-1)*PG)!=(ssize_t)PG*cnt ) exit(3);
  nio++; npageio+=cnt;
}
static void run_spec(int bidir){
  int i=0, w=8;
  nio=0; npageio=0;
  while( i<n ){
    /* how many consecutive same-direction links have we just confirmed? */
    int dir=0;
    if( i>=2 ){
      long d1=pg[i-1]-pg[i-2], d2=pg[i]-pg[i-1];
      if( d1==1 && d2==1 ) dir=1;
      else if( bidir && d1==-1 && d2==-1 ) dir=-1;
    }
    if( dir==0 ){ rd1(i); i++; continue; }
    int cnt = w; if( i+cnt>n ) cnt = n-i;
    /* the speculative window must stay inside the file in that direction */
    if( dir<0 ){ if( pg[i]-(cnt-1) < 2 ) cnt = (int)(pg[i]-1); }
    else { if( pg[i]+(cnt-1) > nFilePg ) cnt = (int)(nFilePg-pg[i]+1); }
    if( cnt<1 ){ rd1(i); i++; continue; }
    rdv(i,cnt,dir);
    /* validate: how far does the guess actually hold? */
    int k=1;
    while( k<cnt && pg[i+k]==pg[i+k-1]+dir ) k++;
    i += k;
    if( k==cnt ){ w = w<MAXW ? w*2 : MAXW; if(w>MAXW) w=MAXW; }
    else w = 8;
  }
}
/* Policy 3: bidirectional, plus run-structure learning.  When a run breaks we
** already hold the first page of the NEXT run (it is the pointer stored in the
** page that ended the previous one), so if a previous run has told us how long
** runs tend to be we can open the next window immediately, with no page-by-page
** reconfirmation.  A wrong guess is still validated by the pointers we read and
** costs at most one window of over-read. */
static void run_learn(void){
  int i=0, runLen=0, lastDir=0, w=8;
  nio=0; npageio=0;
  while( i<n ){
    int dir=0;
    /* two confirmed links: a run is definitely in progress */
    if( i>=2 ){
      long d1=pg[i-1]-pg[i-2], d2=pg[i]-pg[i-1];
      if( d1==d2 && (d2==1||d2==-1) ){ dir=(int)d2; if(w<8) w=8; }
    }
    /* one confirmed link is enough to risk a small window once we have seen
    ** that this chain is built out of runs at all -- this is what lets
    ** 2-page runs be batched, which two-link confirmation can never do */
    if( dir==0 && i>=1 ){
      long d2=pg[i]-pg[i-1];
      /* runLen==0 means we have not yet seen any run: probe with the smallest
      ** useful window, which costs at most one wasted page and is the only way
      ** a chain built from 2-page runs can ever be batched */
      if( d2==1||d2==-1 ){ dir=(int)d2; w = runLen>=2 ? runLen : 2; }
    }
    /* a run has just ended and we hold the next run's first page: open the
    ** next window straight away, sized by what runs have looked like so far */
    if( dir==0 && runLen>=2 && lastDir!=0 ){ dir=lastDir; w=runLen; }
    if( dir==0 ){ rd1(i); i++; continue; }
    int cnt=w; if(cnt<2) cnt=2;
    if( i+cnt>n ) cnt=n-i;
    if( dir<0 ){ if( pg[i]-(cnt-1) < 2 ) cnt=(int)(pg[i]-1); }
    else { if( pg[i]+(cnt-1) > nFilePg ) cnt=(int)(nFilePg-pg[i]+1); }
    if( cnt<2 ){ rd1(i); i++; continue; }
    rdv(i,cnt,dir);
    int k=1;
    while( k<cnt && pg[i+k]==pg[i+k-1]+dir ) k++;
    if( k>=2 ){ if(k>runLen) runLen=k; lastDir=dir; }
    if( k==cnt ){ w = w<MAXW ? w*2 : MAXW; if(w>MAXW) w=MAXW; }  /* keep doubling */
    else w = runLen>8 ? runLen : 8;
    i+=k;
  }
}
static void run_oracle(void){
  int i=0; nio=0; npageio=0;
  while(i<n){
    int j=i+1, dir=0;
    if( j<n && pg[j]==pg[i]+1 ) dir=1; else if( j<n && pg[j]==pg[i]-1 ) dir=-1;
    while( dir && j<n && pg[j]==pg[j-1]+dir && (j-i)<MAXW ) j++;
    int cnt=j-i;
    if(cnt==1) rd1(i); else rdv(i,cnt,dir);
    i=j;
  }
}
int main(int argc,char**argv){
  FILE *f=fopen(argv[2],"r"); pg=malloc(sizeof(long)*1<<20); n=0;
  while(fscanf(f,"%ld",&pg[n])==1) n++;
  fclose(f);
  fd=open(argv[1],O_RDONLY); if(fd<0){perror("open");return 1;}
  { struct stat st; fstat(fd,&st); nFilePg=st.st_size/PG; }
  buf=aligned_alloc(4096,(size_t)PG*(n+MAXW));
  const char *nm[4]={"per-page (stock)","fwd only (OvVec v1)","bidirectional","bidir + run learning"};
  double res[4][9]; long io[4],pio[4];
  for(int t=0;t<9;t++){
    for(int m=0;m<4;m++){
      fsync(fd); posix_fadvise(fd,0,0,POSIX_FADV_DONTNEED);
      double t0=now();
      if(m==0){ nio=0;npageio=0; for(int i=0;i<n;i++) rd1(i); }
      else if(m==3) run_learn();
      else run_spec(m==2);
      res[m][t]=now()-t0; io[m]=nio; pio[m]=npageio;
    }
  }
  double base=0;
  for(int m=0;m<4;m++){
    qsort(res[m],9,sizeof(double),cmpd);
    double v=res[m][4]/1000.0; if(m==0) base=v;
    printf("  %-22s %8.2f ms  %+6.1f%%   I/Os=%-6ld pages_read=%ld\n",
           nm[m],v,(v-base)/base*100,io[m],pio[m]);
  }
  fsync(fd); posix_fadvise(fd,0,0,POSIX_FADV_DONTNEED);
  double t0=now(); run_oracle(); double v=(now()-t0)/1000.0;
  printf("  %-22s %8.2f ms  %+6.1f%%   I/Os=%-6ld pages_read=%ld\n",
         "oracle (chain known)",v,(v-base)/base*100,nio,npageio);
  return 0;
}
