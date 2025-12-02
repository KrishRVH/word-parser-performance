#include <sys/mman.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#define B struct
#define R return
#define Z sizeof
#define O void
#define Q size_t
#define T typedef
#define E stderr
#define S static
#define A(x)__attribute__((x))
#define K 16384
#define F(i,n)for(i=0;i<n;i++)
#define W while
#define G(a,b)((a)>(b)?(a):(b))
#define Y(x)fprintf(E,"%s\n",x)
#define H0 2166136261u
#define HP 16777619u
#define LTR(c)(((c)|32u)-'a'<26u)
T char C;T int I;T unsigned U;
T B N{B N*n;Q c;U h;C w[];}N;
T B{const C*w;Q c;}V;
S N*H[K];S C*M,*P;S Q L,J,GT,GU;

I X(const O*a,const O*b){
 const V*x=a,*y=b;
 R x->c<y->c?1:x->c>y->c?-1:strcmp(x->w,y->w);
}

I main(I c,C**v){
 I f;B stat s;
 if(c-2)R Y("Usage: <file>"),1;
 if((f=open(1[v],0))<0||fstat(f,&s))R Y("IO Err"),1;
 L=s.st_size;
 if(!L)R Y("Empty"),1;
 if((M=mmap(0,L,1,2,f,0))==MAP_FAILED)R Y("Map Err"),1;
 J=G(L/4,4096);
 if((P=mmap(0,J,3,34,-1,0))==MAP_FAILED)R Y("Mem Err"),1;
 Q i=0,l=0,u=0;U h=H0;C b[64];
 F(i,L+1){
  C k=i<L?(M[i]|32):0;
  I g=LTR(k);
  if(g){
   if(l<63)b[l++]=k,h=(h^k)*HP;
   u=1;
  }else if(u){
   b[l]=0;
   U x=h&(K-1);N*n=x[H];
   W(n&&(n->h!=h||memcmp(n->w,b,l)))n=n->n;
   if(n)n->c++;
   else{
    Q z=Z(N)+l+1,a=7,o=(a-(GU&a))&a;
    if(GU+o+z>J)break;
    n=(N*)(P+GU+o);GU+=o+z;
    memcpy(n->w,b,l+1);
    n->h=h;n->c=1;n->n=x[H];x[H]=n;
   }
   GT++;u=0;h=H0;l=0;
  }
 }
 V*D=malloc(GU*Z(V));if(!D)R Y("OOM"),1;
 Q k=0;
 F(i,K)for(N*n=i[H];n;n=n->n)D[k++]=(V){n->w,n->c};
 qsort(D,k,Z(V),X);
 puts("\n=== Top 10 ===");
 u=k<10?k:10;
 F(i,u)printf("%4zu %-15s %9zu %5.2f\n",
              i+1,D[i].w,D[i].c,100.0*D[i].c/GT);
 printf("\nStats: %.2fMB words:%zu uniq:%zu\n",
        (double)L/1048576,GT,k);
 puts("\n=== Statistics ===");
 printf("Total words:     %zu\n", GT);
 printf("Unique words:    %zu\n", k);
 munmap(M,L);munmap(P,J);close(f);free(D);
 R 0;
}