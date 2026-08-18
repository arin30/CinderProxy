#define _POSIX_C_SOURCE 200809L
#include "http.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_REQ (1024*1024)
#define MAX_HDR (16*1024)
#define RATE_SLOTS 1024

typedef struct { char ip[64]; time_t start; int count; int used; } rate_entry_t;
typedef struct { rate_entry_t e[RATE_SLOTS]; pthread_mutex_t lock; int limit; int window; } limiter_t;
typedef struct { int fd; char ip[64]; char backend[256]; int backend_port; int timeout; limiter_t *limiter; } client_t;

static unsigned long hash_ip(const char *s){ unsigned long h=5381; for(;*s;s++) h=((h<<5)+h)^(unsigned char)*s; return h; }
static int allow(limiter_t *r,const char *ip){
  time_t now=time(NULL); size_t start=hash_ip(ip)%RATE_SLOTS; pthread_mutex_lock(&r->lock);
  for(size_t i=0;i<RATE_SLOTS;i++){
    rate_entry_t *e=&r->e[(start+i)%RATE_SLOTS];
    if(!e->used){ e->used=1; snprintf(e->ip,sizeof(e->ip),"%s",ip); e->start=now; e->count=1; pthread_mutex_unlock(&r->lock); return 1; }
    if(!strncmp(e->ip,ip,sizeof(e->ip))){ if(now-e->start>=r->window){ e->start=now; e->count=1; pthread_mutex_unlock(&r->lock); return 1; } if(e->count>=r->limit){ pthread_mutex_unlock(&r->lock); return 0; } e->count++; pthread_mutex_unlock(&r->lock); return 1; }
  }
  pthread_mutex_unlock(&r->lock); return 0;
}
static int timeout_fd(int fd,int sec){ struct timeval tv={.tv_sec=sec,.tv_usec=0}; return setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv))||setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv)); }
static int send_all(int fd,const void *buf,size_t n){ const char *p=buf; size_t off=0; while(off<n){ ssize_t x=send(fd,p+off,n-off,0); if(x<=0) return -1; off+=(size_t)x; } return 0; }
static void error_resp(int fd,int code,const char *reason){ char body[128],resp[512]; int bl=snprintf(body,sizeof(body),"%d %s\n",code,reason); int n=snprintf(resp,sizeof(resp),"HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",code,reason,bl,body); if(n>0) send_all(fd,resp,(size_t)n); }
static int connect_backend(const char *host,int port,int timeout){ char ps[16]; struct addrinfo hints={0},*res=NULL,*it; hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM; snprintf(ps,sizeof(ps),"%d",port); if(getaddrinfo(host,ps,&hints,&res)) return -1; int fd=-1; for(it=res;it;it=it->ai_next){ fd=socket(it->ai_family,it->ai_socktype,it->ai_protocol); if(fd<0) continue; timeout_fd(fd,timeout); if(connect(fd,it->ai_addr,it->ai_addrlen)==0) break; close(fd); fd=-1; } freeaddrinfo(res); return fd; }
static void log_line(const char *ip,const char *method,const char *target,int status){ fprintf(stdout,"client=%s method=%s target=\"%s\" status=%d\n",ip,method?method:"-",target?target:"-",status); fflush(stdout); }
static void *handle(void *arg){
  client_t *c=arg; int fd=c->fd; timeout_fd(fd,c->timeout);
  if(!allow(c->limiter,c->ip)){ error_resp(fd,429,"Too Many Requests"); close(fd); free(c); return NULL; }
  char *buf=calloc(1,MAX_REQ+1); if(!buf){ close(fd); free(c); return NULL; }
  size_t used=0; while(used<MAX_HDR){ ssize_t n=recv(fd,buf+used,MAX_REQ-used,0); if(n<=0){ free(buf); close(fd); free(c); return NULL; } used+=(size_t)n; buf[used]='\0'; if(strstr(buf,"\r\n\r\n")) break; }
  if(!strstr(buf,"\r\n\r\n")){ error_resp(fd,431,"Request Header Fields Too Large"); free(buf); close(fd); free(c); return NULL; }
  http_request_t req; size_t h_end=0; http_parse_result_t pr=http_parse_request(buf,used,MAX_HDR,&req,&h_end);
  if(pr!=HTTP_PARSE_OK){ int code=(pr==HTTP_PARSE_TOO_LARGE)?431:(pr==HTTP_PARSE_UNSUPPORTED)?501:400; error_resp(fd,code,http_result_message(pr)); log_line(c->ip,"-","-",code); free(buf); close(fd); free(c); return NULL; }
  if((size_t)req.content_length>MAX_REQ-h_end){ error_resp(fd,413,"Payload Too Large"); free(buf); close(fd); free(c); return NULL; }
  size_t body_have=used>h_end?used-h_end:0; while(body_have<(size_t)req.content_length){ ssize_t n=recv(fd,buf+used,MAX_REQ-used,0); if(n<=0){ free(buf); close(fd); free(c); return NULL; } used+=(size_t)n; body_have+=(size_t)n; }
  int bfd=connect_backend(c->backend,c->backend_port,c->timeout); if(bfd<0){ error_resp(fd,502,"Bad Gateway"); log_line(c->ip,req.method,req.target,502); free(buf); close(fd); free(c); return NULL; }
  char fwd[MAX_HDR+1024]; size_t fwd_len=0; if(http_build_forward_request(&req,c->ip,fwd,sizeof(fwd),&fwd_len)){ error_resp(fd,500,"Internal Server Error"); close(bfd); free(buf); close(fd); free(c); return NULL; }
  if(send_all(bfd,fwd,fwd_len)|| (req.content_length>0 && send_all(bfd,buf+h_end,(size_t)req.content_length))){ error_resp(fd,502,"Bad Gateway"); close(bfd); free(buf); close(fd); free(c); return NULL; }
  char rbuf[16384]; int status=200,first=1; for(;;){ ssize_t n=recv(bfd,rbuf,sizeof(rbuf),0); if(n<=0) break; if(first){ first=0; int s=0; if(sscanf(rbuf,"HTTP/%*s %d",&s)==1) status=s; } if(send_all(fd,rbuf,(size_t)n)) break; }
  log_line(c->ip,req.method,req.target,status); close(bfd); free(buf); close(fd); free(c); return NULL;
}
static int listener(int port){ int fd=socket(AF_INET6,SOCK_STREAM,0); if(fd<0) return -1; int one=1,off=0; setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one)); setsockopt(fd,IPPROTO_IPV6,IPV6_V6ONLY,&off,sizeof(off)); struct sockaddr_in6 a={0}; a.sin6_family=AF_INET6; a.sin6_addr=in6addr_any; a.sin6_port=htons((unsigned short)port); if(bind(fd,(struct sockaddr*)&a,sizeof(a))||listen(fd,128)){ close(fd); return -1; } return fd; }
int main(int argc,char **argv){
  signal(SIGPIPE,SIG_IGN); int listen_port=8080,backend_port=9000,timeout=10,rate=60,window=10; char backend[256]="127.0.0.1";
  for(int i=1;i<argc;i++){ if(i+1>=argc){fprintf(stderr,"bad args\n");return 2;} char *k=argv[i],*v=argv[++i]; if(!strcmp(k,"--listen")) listen_port=atoi(v); else if(!strcmp(k,"--backend-host")) snprintf(backend,sizeof(backend),"%s",v); else if(!strcmp(k,"--backend-port")) backend_port=atoi(v); else if(!strcmp(k,"--timeout")) timeout=atoi(v); else if(!strcmp(k,"--rate-limit")) rate=atoi(v); else if(!strcmp(k,"--rate-window")) window=atoi(v); else {fprintf(stderr,"unknown option: %s\n",k);return 2;} }
  if(listen_port<1||listen_port>65535||backend_port<1||backend_port>65535||timeout<1||rate<1||window<1){fprintf(stderr,"invalid config\n");return 2;}
  int lfd=listener(listen_port); if(lfd<0){perror("listen");return 1;} limiter_t lim={0}; pthread_mutex_init(&lim.lock,NULL); lim.limit=rate; lim.window=window;
  printf("CinderProxy listening on :%d -> %s:%d\n",listen_port,backend,backend_port);
  for(;;){ struct sockaddr_storage ss; socklen_t sl=sizeof(ss); int cfd=accept(lfd,(struct sockaddr*)&ss,&sl); if(cfd<0) continue; client_t *c=calloc(1,sizeof(*c)); if(!c){close(cfd);continue;} c->fd=cfd;c->backend_port=backend_port;c->timeout=timeout;c->limiter=&lim;snprintf(c->backend,sizeof(c->backend),"%s",backend); void *addr=NULL; if(ss.ss_family==AF_INET) addr=&((struct sockaddr_in*)&ss)->sin_addr; else addr=&((struct sockaddr_in6*)&ss)->sin6_addr; if(!inet_ntop(ss.ss_family,addr,c->ip,sizeof(c->ip))) snprintf(c->ip,sizeof(c->ip),"unknown"); pthread_t t; if(pthread_create(&t,NULL,handle,c)){free(c);close(cfd);continue;} pthread_detach(t); }
}
