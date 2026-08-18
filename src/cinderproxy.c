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
#ifdef CINDER_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#define MAX_REQ (1024*1024)
#define MAX_HDR (16*1024)
#define RATE_SLOTS 1024
#define DEFAULT_WORKERS 8
#define QUEUE_CAPACITY 256
#define BODY_CHUNK 16384

typedef struct { char ip[64]; time_t start; int count; int used; } rate_entry_t;
typedef struct { rate_entry_t e[RATE_SLOTS]; pthread_mutex_t lock; int limit; int window; } limiter_t;
typedef struct backend_health backend_health_t;
typedef struct { int fd; char ip[64]; char backend[256]; int backend_port; int timeout; limiter_t *limiter; backend_health_t *health;
#ifdef CINDER_TLS
SSL_CTX *tls_ctx;
#endif
} client_t;
typedef struct { client_t *items[QUEUE_CAPACITY]; size_t head,tail,count; pthread_mutex_t lock; pthread_cond_t not_empty; } work_queue_t;
struct backend_health { char host[256]; int port,timeout,interval,healthy; pthread_mutex_t lock; };
static work_queue_t workq={.lock=PTHREAD_MUTEX_INITIALIZER,.not_empty=PTHREAD_COND_INITIALIZER};

static unsigned long hash_ip(const char *s){ unsigned long h=5381; for(;*s;s++) h=((h<<5)+h)^(unsigned char)*s; return h; }
static int allow(limiter_t *r,const char *ip){ time_t now=time(NULL); size_t start=hash_ip(ip)%RATE_SLOTS; pthread_mutex_lock(&r->lock); for(size_t i=0;i<RATE_SLOTS;i++){ rate_entry_t *e=&r->e[(start+i)%RATE_SLOTS]; if(!e->used){e->used=1;snprintf(e->ip,sizeof(e->ip),"%s",ip);e->start=now;e->count=1;pthread_mutex_unlock(&r->lock);return 1;} if(!strncmp(e->ip,ip,sizeof(e->ip))){if(now-e->start>=r->window){e->start=now;e->count=1;pthread_mutex_unlock(&r->lock);return 1;} if(e->count>=r->limit){pthread_mutex_unlock(&r->lock);return 0;} e->count++;pthread_mutex_unlock(&r->lock);return 1;}} pthread_mutex_unlock(&r->lock);return 0; }
static int queue_push(client_t *c){int ok=0;pthread_mutex_lock(&workq.lock);if(workq.count<QUEUE_CAPACITY){workq.items[workq.tail]=c;workq.tail=(workq.tail+1)%QUEUE_CAPACITY;workq.count++;ok=1;pthread_cond_signal(&workq.not_empty);}pthread_mutex_unlock(&workq.lock);return ok;}
static client_t *queue_pop(void){pthread_mutex_lock(&workq.lock);while(!workq.count)pthread_cond_wait(&workq.not_empty,&workq.lock);client_t*c=workq.items[workq.head];workq.head=(workq.head+1)%QUEUE_CAPACITY;workq.count--;pthread_mutex_unlock(&workq.lock);return c;}
static int timeout_fd(int fd,int sec){struct timeval tv={.tv_sec=sec};return setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv))||setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));}
static int send_all(int fd,const void*buf,size_t n){const char*p=buf;size_t off=0;while(off<n){ssize_t x=send(fd,p+off,n-off,0);if(x<=0)return-1;off+=(size_t)x;}return 0;}
#ifdef CINDER_TLS
static int tls_send_all(SSL*s,const void*buf,size_t n){const char*p=buf;size_t off=0;while(off<n){int x=SSL_write(s,p+off,(int)(n-off));if(x<=0)return-1;off+=(size_t)x;}return 0;}
#endif
static int client_send(int fd,
#ifdef CINDER_TLS
SSL *ssl,
#endif
const void*buf,size_t n){
#ifdef CINDER_TLS
if(ssl)return tls_send_all(ssl,buf,n);
#endif
return send_all(fd,buf,n);}
static ssize_t client_recv(int fd,
#ifdef CINDER_TLS
SSL *ssl,
#endif
void*buf,size_t n){
#ifdef CINDER_TLS
if(ssl)return SSL_read(ssl,buf,(int)n);
#endif
return recv(fd,buf,n,0);}
static void error_resp(int fd,
#ifdef CINDER_TLS
SSL *ssl,
#endif
int code,const char*reason){char body[128],resp[512];int bl=snprintf(body,sizeof(body),"%d %s\n",code,reason);int n=snprintf(resp,sizeof(resp),"HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",code,reason,bl,body);if(n>0)client_send(fd,
#ifdef CINDER_TLS
ssl,
#endif
resp,(size_t)n);}
static int connect_backend(const char*h,int p,int t){char ps[16];struct addrinfo hints={0},*res=NULL,*it;hints.ai_family=AF_UNSPEC;hints.ai_socktype=SOCK_STREAM;snprintf(ps,sizeof(ps),"%d",p);if(getaddrinfo(h,ps,&hints,&res))return-1;int fd=-1;for(it=res;it;it=it->ai_next){fd=socket(it->ai_family,it->ai_socktype,it->ai_protocol);if(fd<0)continue;timeout_fd(fd,t);if(connect(fd,it->ai_addr,it->ai_addrlen)==0)break;close(fd);fd=-1;}freeaddrinfo(res);return fd;}
static void set_health(backend_health_t*h,int v){pthread_mutex_lock(&h->lock);int changed=h->healthy!=v;h->healthy=v;pthread_mutex_unlock(&h->lock);if(changed){printf("backend=%s:%d health=%s\n",h->host,h->port,v?"up":"down");fflush(stdout);}}
static int get_health(backend_health_t*h){int v;pthread_mutex_lock(&h->lock);v=h->healthy;pthread_mutex_unlock(&h->lock);return v;}
static void*health_main(void*a){backend_health_t*h=a;for(;;){int fd=connect_backend(h->host,h->port,h->timeout);set_health(h,fd>=0);if(fd>=0)close(fd);sleep((unsigned)h->interval);}return NULL;}
static void log_line(const char*ip,const char*m,const char*t,int s){fprintf(stdout,"client=%s method=%s target=\"%s\" status=%d\n",ip,m?m:"-",t?t:"-",s);fflush(stdout);}
static int stream_body(int cfd,
#ifdef CINDER_TLS
SSL *ssl,
#endif
int bfd,const char*already,size_t have,size_t total){size_t sent=0,first=have<total?have:total;if(first&&send_all(bfd,already,first))return-1;sent=first;char chunk[BODY_CHUNK];while(sent<total){size_t need=total-sent,want=need<sizeof(chunk)?need:sizeof(chunk);ssize_t n=client_recv(cfd,
#ifdef CINDER_TLS
ssl,
#endif
chunk,want);if(n<=0||send_all(bfd,chunk,(size_t)n))return-1;sent+=(size_t)n;}return 0;}
static void handle_client(client_t*c){int fd=c->fd;timeout_fd(fd,c->timeout);
#ifdef CINDER_TLS
SSL *ssl=NULL;if(c->tls_ctx){ssl=SSL_new(c->tls_ctx);if(!ssl){close(fd);free(c);return;}SSL_set_fd(ssl,fd);if(SSL_accept(ssl)<=0){SSL_free(ssl);close(fd);free(c);return;}}
#endif
#define ERESP(code,msg) error_resp(fd, \
#ifdef CINDER_TLS
ssl, \
#endif
code,msg)
if(!allow(c->limiter,c->ip)){ERESP(429,"Too Many Requests");goto done;}if(!get_health(c->health)){ERESP(503,"Backend Unavailable");log_line(c->ip,"-","-",503);goto done;}char*buf=calloc(1,MAX_HDR+1);if(!buf)goto done;size_t used=0;while(used<MAX_HDR){ssize_t n=client_recv(fd,
#ifdef CINDER_TLS
ssl,
#endif
buf+used,MAX_HDR-used);if(n<=0){free(buf);goto done;}used+=(size_t)n;buf[used]=0;if(strstr(buf,"\r\n\r\n"))break;}if(!strstr(buf,"\r\n\r\n")){ERESP(431,"Request Header Fields Too Large");free(buf);goto done;}http_request_t req;size_t h_end=0;http_parse_result_t pr=http_parse_request(buf,used,MAX_HDR,&req,&h_end);if(pr!=HTTP_PARSE_OK){int code=pr==HTTP_PARSE_TOO_LARGE?431:pr==HTTP_PARSE_UNSUPPORTED?501:400;ERESP(code,http_result_message(pr));free(buf);goto done;}if(req.content_length<0||(size_t)req.content_length>MAX_REQ){ERESP(413,"Payload Too Large");free(buf);goto done;}int bfd=connect_backend(c->backend,c->backend_port,c->timeout);if(bfd<0){set_health(c->health,0);ERESP(502,"Bad Gateway");free(buf);goto done;}char fwd[MAX_HDR+1024];size_t fl=0;if(http_build_forward_request(&req,c->ip,fwd,sizeof(fwd),&fl)||send_all(bfd,fwd,fl)){ERESP(502,"Bad Gateway");close(bfd);free(buf);goto done;}size_t have=used>h_end?used-h_end:0;if(req.content_length>0&&stream_body(fd,
#ifdef CINDER_TLS
ssl,
#endif
bfd,buf+h_end,have,(size_t)req.content_length)){ERESP(502,"Bad Gateway");close(bfd);free(buf);goto done;}char rb[16384];int status=200,first=1;for(;;){ssize_t n=recv(bfd,rb,sizeof(rb),0);if(n<=0)break;if(first){first=0;int s=0;if(sscanf(rb,"HTTP/%*s %d",&s)==1)status=s;}if(client_send(fd,
#ifdef CINDER_TLS
ssl,
#endif
rb,(size_t)n))break;}log_line(c->ip,req.method,req.target,status);close(bfd);free(buf);
done:
#ifdef CINDER_TLS
if(ssl){SSL_shutdown(ssl);SSL_free(ssl);}
#endif
close(fd);free(c);
#undef ERESP
}
static void*worker_main(void*x){(void)x;for(;;)handle_client(queue_pop());return NULL;}
static int listener(int p){int fd=socket(AF_INET6,SOCK_STREAM,0);if(fd<0)return-1;int one=1,off=0;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));setsockopt(fd,IPPROTO_IPV6,IPV6_V6ONLY,&off,sizeof(off));struct sockaddr_in6 a={0};a.sin6_family=AF_INET6;a.sin6_addr=in6addr_any;a.sin6_port=htons((unsigned short)p);if(bind(fd,(struct sockaddr*)&a,sizeof(a))||listen(fd,128)){close(fd);return-1;}return fd;}
int main(int argc,char**argv){signal(SIGPIPE,SIG_IGN);int lp=8080,bp=9000,to=10,rate=60,win=10,workers=DEFAULT_WORKERS,hi=5;char backend[256]="127.0.0.1",cert[512]="",key[512]="";for(int i=1;i<argc;i++){if(i+1>=argc){fprintf(stderr,"bad args\n");return 2;}char*k=argv[i],*v=argv[++i];if(!strcmp(k,"--listen"))lp=atoi(v);else if(!strcmp(k,"--backend-host"))snprintf(backend,sizeof(backend),"%s",v);else if(!strcmp(k,"--backend-port"))bp=atoi(v);else if(!strcmp(k,"--timeout"))to=atoi(v);else if(!strcmp(k,"--rate-limit"))rate=atoi(v);else if(!strcmp(k,"--rate-window"))win=atoi(v);else if(!strcmp(k,"--workers"))workers=atoi(v);else if(!strcmp(k,"--health-interval"))hi=atoi(v);else if(!strcmp(k,"--tls-cert"))snprintf(cert,sizeof(cert),"%s",v);else if(!strcmp(k,"--tls-key"))snprintf(key,sizeof(key),"%s",v);else{fprintf(stderr,"unknown option: %s\n",k);return 2;}}if(lp<1||lp>65535||bp<1||bp>65535||to<1||rate<1||win<1||workers<1||workers>128||hi<1||hi>300){fprintf(stderr,"invalid config\n");return 2;}int lfd=listener(lp);if(lfd<0){perror("listen");return 1;}limiter_t lim={0};pthread_mutex_init(&lim.lock,NULL);lim.limit=rate;lim.window=win;backend_health_t health={0};snprintf(health.host,sizeof(health.host),"%s",backend);health.port=bp;health.timeout=to;health.interval=hi;pthread_mutex_init(&health.lock,NULL);int probe=connect_backend(backend,bp,to);set_health(&health,probe>=0);if(probe>=0)close(probe);pthread_t ht;pthread_create(&ht,NULL,health_main,&health);pthread_detach(ht);
#ifdef CINDER_TLS
SSL_CTX*tls_ctx=NULL;if(cert[0]||key[0]){if(!cert[0]||!key[0]){fprintf(stderr,"both --tls-cert and --tls-key are required\n");return 2;}OPENSSL_init_ssl(0,NULL);tls_ctx=SSL_CTX_new(TLS_server_method());if(!tls_ctx||SSL_CTX_use_certificate_file(tls_ctx,cert,SSL_FILETYPE_PEM)<=0||SSL_CTX_use_PrivateKey_file(tls_ctx,key,SSL_FILETYPE_PEM)<=0||!SSL_CTX_check_private_key(tls_ctx)){ERR_print_errors_fp(stderr);return 1;}SSL_CTX_set_min_proto_version(tls_ctx,TLS1_2_VERSION);}
#else
if(cert[0]||key[0]){fprintf(stderr,"TLS support is not compiled in; run make tls\n");return 2;}
#endif
pthread_t*pool=calloc((size_t)workers,sizeof(*pool));if(!pool)return 1;for(int i=0;i<workers;i++){if(pthread_create(&pool[i],NULL,worker_main,NULL)){fprintf(stderr,"failed to create worker thread\n");return 1;}pthread_detach(pool[i]);}printf("CinderProxy listening on :%d -> %s:%d workers=%d queue=%d health_interval=%ds%s\n",lp,backend,bp,workers,QUEUE_CAPACITY,hi,cert[0]?" tls=on":"");for(;;){struct sockaddr_storage ss;socklen_t sl=sizeof(ss);int cfd=accept(lfd,(struct sockaddr*)&ss,&sl);if(cfd<0)continue;client_t*c=calloc(1,sizeof(*c));if(!c){close(cfd);continue;}c->fd=cfd;c->backend_port=bp;c->timeout=to;c->limiter=&lim;c->health=&health;snprintf(c->backend,sizeof(c->backend),"%s",backend);
#ifdef CINDER_TLS
c->tls_ctx=tls_ctx;
#endif
void*addr=ss.ss_family==AF_INET?(void*)&((struct sockaddr_in*)&ss)->sin_addr:(void*)&((struct sockaddr_in6*)&ss)->sin6_addr;if(!inet_ntop(ss.ss_family,addr,c->ip,sizeof(c->ip)))snprintf(c->ip,sizeof(c->ip),"unknown");if(!queue_push(c)){
#ifdef CINDER_TLS
if(tls_ctx){close(cfd);free(c);continue;}
#endif
error_resp(cfd,
#ifdef CINDER_TLS
NULL,
#endif
503,"Service Unavailable");close(cfd);free(c);}}
}
