#include "yyjson.h"  /* fetched by run.sh */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(int argc,char**argv){FILE*f=fopen(argv[1],"rb");fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);char*b=malloc(n);fread(b,1,n,f);fclose(f);int it=atoi(argv[2]);
double bp=1e9,bs=1e9;size_t ol=0;yyjson_doc*keep=NULL;
for(int i=0;i<it;i++){double t0=now();yyjson_doc*d=yyjson_read(b,n,0);double dt=now()-t0;if(dt<bp)bp=dt;if(keep)yyjson_doc_free(keep);keep=d;}
for(int i=0;i<it;i++){double t0=now();char*s=yyjson_write(keep,0,&ol);double dt=now()-t0;if(dt<bs)bs=dt;free(s);}
/* mutable doc parse (closer to a DOM users mutate) */
double bm=1e9;for(int i=0;i<it;i++){double t0=now();yyjson_doc*d=yyjson_read(b,n,0);yyjson_mut_doc*m=yyjson_doc_mut_copy(d,NULL);double dt=now()-t0;if(dt<bm)bm=dt;yyjson_mut_doc_free(m);yyjson_doc_free(d);}
double mb=n/1048576.0;printf("%s parse %.2f ms %.0f MB/s | serialize %.2f ms %.0f MB/s | parse+mutable copy %.0f MB/s\n",argv[1],bp*1000,mb/bp,bs*1000,ol/1048576.0/bs,mb/bm);}
