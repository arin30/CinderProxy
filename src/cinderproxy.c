#define _POSIX_C_SOURCE 200809L
#include "http.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_REQ (1024*1024)
#define MAX_HDR (16*1024)
#define RATE_SLOTS 1024
#define DEFAULT_WORKERS 8
#define QUEUE_CAPACITY 256
#define BODY_CHUNK 16384

typedef struct { char ip[64]; time_t start; int count; int used; } rate_entry_t;
typedef struct { rate_entry_t e[RATE_SLOTS]; pthread_mutex_t lock; int limit; int window; } limiter_t;
typedef struct backend_health backend_health_t;
typedef struct { int fd; char ip[64]; char backend[256]; int backend_port; int timeout; limiter_t *limiter; backend_health_t *health; } client_t;

typedef struct {
  client_t *items[QUEUE_CAPACITY];
  size_t head;
  size_t tail;
  size_t count;
  pthread_mutex_t lock;
  pthread_cond_t not_empty;
} work_queue_t;

struct backend_health {
  char host[256];
  int port;
  int timeout;
  int interval;
  int healthy;
  pthread_mutex_t lock;
};

static work_queue_t workq = {
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .not_empty = PTHREAD_COND_INITIALIZER
};

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

static int queue_push(client_t *c){
  int ok=0;
  pthread_mutex_lock(&workq.lock);
  if(workq.count<QUEUE_CAPACITY){
    workq.items[workq.tail]=c;
    workq.tail=(workq.tail+1)%QUEUE_CAPACITY;
    workq.count++;
    ok=1;
    pthread_cond_signal(&workq.not_empty);
  }
  pthread_mutex_unlock(&workq.lock);
  return ok;
}

static client_t *queue_pop(void){
  pthread_mutex_lock(&workq.lock);
  while(workq.count==0) pthread_cond_wait(&workq.not_empty,&workq.lock);
  client_t *c=workq.items[workq.head];
  workq.head=(workq.head+1)%QUEUE_CAPACITY;
  workq.count--;
  pthread_mutex_unlock(&workq.lock);
  return c;
}

static int timeout_fd(int fd,int sec){ struct timeval tv={.tv_sec=sec,.tv_usec=0}; return setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv))||setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv)); }
static int send_all(int fd,const void *buf,size_t n){ const char *p=buf; size_t off=0; while(off<n){ ssize_t x=send(fd,p+off,n-off,0); if(x<=0) return -1; off+=(size_t)x; } return 0; }
static void error_resp(int fd,int code,const char *reason){ char body[128],resp[512]; int bl=snprintf(body,sizeof(body),"%d %s\n",code,reason); int n=snprintf(resp,sizeof(resp),"HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",code,reason,bl,body); if(n>0) send_all(fd,resp,(size_t)n); }

static int connect_backend(const char *host,int port,int timeout){
  char ps[16]; struct addrinfo hints={0},*res=NULL,*it; hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM;
  snprintf(ps,sizeof(ps),"%d",port);
  if(getaddrinfo(host,ps,&hints,&res)) return -1;
  int fd=-1;
  for(it=res;it;it=it->ai_next){
    fd=socket(it->ai_family,it->ai_socktype,it->ai_protocol);
    if(fd<0) continue;
    timeout_fd(fd,timeout);
    if(connect(fd,it->ai_addr,it->ai_addrlen)==0) break;
    close(fd); fd=-1;
  }
  freeaddrinfo(res);
  return fd;
}

static void set_health(backend_health_t *h,int healthy){
  pthread_mutex_lock(&h->lock);
  int changed=(h->healthy!=healthy);
  h->healthy=healthy;
  pthread_mutex_unlock(&h->lock);
  if(changed){ fprintf(stdout,"backend=%s:%d health=%s\n",h->host,h->port,healthy?"up":"down"); fflush(stdout); }
}

static int get_health(backend_health_t *h){ int healthy; pthread_mutex_lock(&h->lock); healthy=h->healthy; pthread_mutex_unlock(&h->lock); return healthy; }

static void *health_main(void *arg){
  backend_health_t *h=arg;
  for(;;){
    int fd=connect_backend(h->host,h->port,h->timeout);
    set_health(h,fd>=0);
    if(fd>=0) close(fd);
    sleep((unsigned int)h->interval);
  }
  return NULL;
}

static void log_line(const char *ip,const char *method,const char *target,int status){ fprintf(stdout,"client=%s method=%s target=\"%s\" status=%d\n",ip,method?method:"-",target?target:"-",status); fflush(stdout); }

static int stream_body(int client_fd,int backend_fd,const char *already,size_t already_len,size_t total){
  size_t sent=0;
  size_t first=already_len<total?already_len:total;
  if(first>0 && send_all(backend_fd,already,first)!=0) return -1;
  sent=first;
  char chunk[BODY_CHUNK];
  while(sent<total){
    size_t need=total-sent;
    size_t want=need<sizeof(chunk)?need:sizeof(chunk);
    ssize_t n=recv(client_fd,chunk,want,0);
    if(n<=0) return -1;
    if(send_all(backend_fd,chunk,(size_t)n)!=0) return -1;
    sent+=(size_t)n;
  }
  return 0;
}

static void handle_client(client_t *c){
  int fd=c->fd; timeout_fd(fd,c->timeout);
  if(!allow(c->limiter,c->ip)){ error_resp(fd,429,"Too Many Requests"); close(fd); free(c); return; }
  if(!get_health(c->health)){ error_resp(fd,503,"Backend Unavailable"); log_line(c->ip,"-","-",503); close(fd); free(c); return; }

  char *buf=calloc(1,MAX_HDR+1); if(!buf){ close(fd); free(c); return; }
  size_t used=0;
  while(used<MAX_HDR){
    ssize_t n=recv(fd,buf+used,MAX_HDR-used,0);
    if(n<=0){ free(buf); close(fd); free(c); return; }
    used+=(size_t)n; buf[used]='\0';
    if(strstr(buf,"\r\n\r\n")) break;
  }
  if(!strstr(buf,"\r\n\r\n")){ error_resp(fd,431,"Request Header Fields Too Large"); free(buf); close(fd); free(c); return; }

  http_request_t req; size_t h_end=0; http_parse_result_t pr=http_parse_request(buf,used,MAX_HDR,&req,&h_end);
  if(pr!=HTTP_PARSE_OK){ int code=(pr==HTTP_PARSE_TOO_LARGE)?431:(pr==HTTP_PARSE_UNSUPPORTED)?501:400; error_resp(fd,code,http_result_message(pr)); log_line(c->ip,"-","-",code); free(buf); close(fd); free(c); return; }
  if(req.content_length<0 || (size_t)req.content_length>MAX_REQ){ error_resp(fd,413,"Payload Too Large"); free(buf); close(fd); free(c); return; }

  int bfd=connect_backend(c->backend,c->backend_port,c->timeout);
  if(bfd<0){ set_health(c->health,0); error_resp(fd,502,"Bad Gateway"); log_line(c->ip,req.method,req.target,502); free(buf); close(fd); free(c); return; }

  char fwd[MAX_HDR+1024]; size_t fwd_len=0;
  if(http_build_forward_request(&req,c->ip,fwd,sizeof(fwd),&fwd_len)){ error_resp(fd,500,"Internal Server Error"); close(bfd); free(buf); close(fd); free(c); return; }
  if(send_all(bfd,fwd,fwd_len)!=0){ error_resp(fd,502,"Bad Gateway"); close(bfd); free(buf); close(fd); free(c); return; }

  size_t body_have=used>h_end?used-h_end:0;
  if(req.content_length>0 && stream_body(fd,bfd,buf+h_end,body_have,(size_t)req.content_length)!=0){ error_resp(fd,502,"Bad Gateway"); close(bfd); free(buf); close(fd); free(c); return; }

  char rbuf[16384]; int status=200,first=1;
  for(;;){
    ssize_t n=recv(bfd,rbuf,sizeof(rbuf),0);
    if(n<=0) break;
    if(first){ first=0; int s=0; if(sscanf(rbuf,"HTTP/%*s %d",&s)==1) status=s; }
    if(send_all(fd,rbuf,(size_t)n)) break;
  }
  log_line(c->ip,req.method,req.target,status);
  close(bfd); free(buf); close(fd); free(c);
}

static void *worker_main(void *unused){ (void)unused; for(;;) handle_client(queue_pop()); return NULL; }

static int listener(int port){ int fd=socket(AF_INET6,SOCK_STREAM,0); if(fd<0) return -1; int one=1,off=0; setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one)); setsockopt(fd,IPPROTO_IPV6,IPV6_V6ONLY,&off,sizeof(off)); struct sockaddr_in6 a={0}; a.sin6_family=AF_INET6; a.sin6_addr=in6addr_any; a.sin6_port=htons((unsigned short)port); if(bind(fd,(struct sockaddr*)&a,sizeof(a))||listen(fd,128)){ close(fd); return -1; } return fd; }

int main(int argc,char **argv){
  signal(SIGPIPE,SIG_IGN);
  int listen_port=8080,backend_port=9000,timeout=10,rate=60,window=10,workers=DEFAULT_WORKERS,health_interval=5;
  char backend[256]="127.0.0.1";
  for(int i=1;i<argc;i++){
    if(i+1>=argc){fprintf(stderr,"bad args\n");return 2;}
    char *k=argv[i],*v=argv[++i];
    if(!strcmp(k,"--listen")) listen_port=atoi(v);
    else if(!strcmp(k,"--backend-host")) snprintf(backend,sizeof(backend),"%s",v);
    else if(!strcmp(k,"--backend-port")) backend_port=atoi(v);
    else if(!strcmp(k,"--timeout")) timeout=atoi(v);
    else if(!strcmp(k,"--rate-limit")) rate=atoi(v);
    else if(!strcmp(k,"--rate-window")) window=atoi(v);
    else if(!strcmp(k,"--workers")) workers=atoi(v);
    else if(!strcmp(k,"--health-interval")) health_interval=atoi(v);
    else {fprintf(stderr,"unknown option: %s\n",k);return 2;}
  }
  if(listen_port<1||listen_port>65535||backend_port<1||backend_port>65535||timeout<1||rate<1||window<1||workers<1||workers>128||health_interval<1||health_interval>300){fprintf(stderr,"invalid config\n");return 2;}

  int lfd=listener(listen_port); if(lfd<0){perror("listen");return 1;}
  limiter_t lim={0}; pthread_mutex_init(&lim.lock,NULL); lim.limit=rate; lim.window=window;

  backend_health_t health={0};
  snprintf(health.host,sizeof(health.host),"%s",backend); health.port=backend_port; health.timeout=timeout; health.interval=health_interval; health.healthy=0; pthread_mutex_init(&health.lock,NULL);
  int probe=connect_backend(backend,backend_port,timeout); set_health(&health,probe>=0); if(probe>=0) close(probe);
  pthread_t health_thread; if(pthread_create(&health_thread,NULL,health_main,&health)!=0){ fprintf(stderr,"failed to create health thread\n"); close(lfd); return 1; } pthread_detach(health_thread);

  pthread_t *pool=calloc((size_t)workers,sizeof(*pool)); if(!pool){ close(lfd); return 1; }
  for(int i=0;i<workers;i++){
    if(pthread_create(&pool[i],NULL,worker_main,NULL)!=0){ fprintf(stderr,"failed to create worker thread\n"); close(lfd); free(pool); return 1; }
    pthread_detach(pool[i]);
  }

  printf("CinderProxy listening on :%d -> %s:%d workers=%d queue=%d health_interval=%ds\n",listen_port,backend,backend_port,workers,QUEUE_CAPACITY,health_interval);
  for(;;){
    struct sockaddr_storage ss; socklen_t sl=sizeof(ss); int cfd=accept(lfd,(struct sockaddr*)&ss,&sl); if(cfd<0) continue;
    client_t *c=calloc(1,sizeof(*c)); if(!c){close(cfd);continue;}
    c->fd=cfd;c->backend_port=backend_port;c->timeout=timeout;c->limiter=&lim;c->health=&health;snprintf(c->backend,sizeof(c->backend),"%s",backend);
    void *addr=NULL; if(ss.ss_family==AF_INET) addr=&((struct sockaddr_in*)&ss)->sin_addr; else addr=&((struct sockaddr_in6*)&ss)->sin6_addr;
    if(!inet_ntop(ss.ss_family,addr,c->ip,sizeof(c->ip))) snprintf(c->ip,sizeof(c->ip),"unknown");
    if(!queue_push(c)){ error_resp(cfd,503,"Service Unavailable"); close(cfd); free(c); }
  }
}
