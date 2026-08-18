#include "http.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int token_char(unsigned char c){
  if(isalnum(c)) return 1;
  const char *ok="!#$%&'*+-.^_`|~";
  return strchr(ok,(int)c)!=NULL;
}
static int has_ctl(const char *s){
  for(;*s;s++){ unsigned char c=(unsigned char)*s; if((c<32&&c!='\t')||c==127) return 1; }
  return 0;
}
static const char *find_crlf(const char *s,const char *end){
  for(const char *p=s;p+1<end;p++) if(p[0]=='\r'&&p[1]=='\n') return p;
  return NULL;
}
static int copy_span(char *dst,size_t cap,const char *a,const char *b){
  size_t n=(size_t)(b-a); if(n+1>cap) return -1; memcpy(dst,a,n); dst[n]='\0'; return 0;
}
static int hex_value(unsigned char c){
  if(c>='0'&&c<='9') return c-'0';
  if(c>='a'&&c<='f') return c-'a'+10;
  if(c>='A'&&c<='F') return c-'A'+10;
  return -1;
}
static int safe_target(const char *t){
  if(!t[0]||t[0]!='/') return 0;

  char segment[1024];
  size_t seg_len=0;
  for(size_t i=1;t[i]&&t[i]!='?';i++){
    unsigned char c=(unsigned char)t[i];
    if(c=='#'||c=='\\'||c<32||c==127) return 0;

    if(c=='%'){
      if(!t[i+1]||!t[i+2]) return 0;
      int hi=hex_value((unsigned char)t[i+1]);
      int lo=hex_value((unsigned char)t[i+2]);
      if(hi<0||lo<0) return 0;
      c=(unsigned char)((hi<<4)|lo);
      i+=2;
      if(c==0||c=='/'||c=='\\'||c<32||c==127) return 0;
    }

    if(c=='/'){
      if((seg_len==1&&segment[0]=='.') ||
         (seg_len==2&&segment[0]=='.'&&segment[1]=='.')) return 0;
      seg_len=0;
      continue;
    }

    if(seg_len+1>=sizeof(segment)) return 0;
    segment[seg_len++]=(char)c;
  }

  if((seg_len==1&&segment[0]=='.') ||
     (seg_len==2&&segment[0]=='.'&&segment[1]=='.')) return 0;
  return 1;
}
static int hop(const char *name){
  const char *h[]={"Connection","Proxy-Connection","Keep-Alive","TE","Trailer","Transfer-Encoding","Upgrade"};
  for(size_t i=0;i<sizeof(h)/sizeof(h[0]);i++) if(strcasecmp(name,h[i])==0) return 1;
  return 0;
}
http_parse_result_t http_parse_request(const char *buf,size_t len,size_t max_header_bytes,http_request_t *out,size_t *header_end){
  if(!buf||!out||!header_end) return HTTP_PARSE_BAD_REQUEST;
  if(len>max_header_bytes) return HTTP_PARSE_TOO_LARGE;
  memset(out,0,sizeof(*out)); out->content_length=0;
  const char *end=buf+len,*line=find_crlf(buf,end); if(!line) return HTTP_PARSE_BAD_REQUEST;
  const char *sp1=memchr(buf,' ',(size_t)(line-buf)); if(!sp1) return HTTP_PARSE_BAD_REQUEST;
  const char *sp2=memchr(sp1+1,' ',(size_t)(line-(sp1+1))); if(!sp2) return HTTP_PARSE_BAD_REQUEST;
  if(memchr(sp2+1,' ',(size_t)(line-(sp2+1)))) return HTTP_PARSE_BAD_REQUEST;
  if(copy_span(out->method,sizeof(out->method),buf,sp1)||copy_span(out->target,sizeof(out->target),sp1+1,sp2)||copy_span(out->version,sizeof(out->version),sp2+1,line)) return HTTP_PARSE_TOO_LARGE;
  for(const unsigned char *p=(unsigned char*)out->method;*p;p++) if(!token_char(*p)) return HTTP_PARSE_BAD_REQUEST;
  if(!safe_target(out->target)) return HTTP_PARSE_TRAVERSAL;
  if(strcmp(out->version,"HTTP/1.1")&&strcmp(out->version,"HTTP/1.0")) return HTTP_PARSE_UNSUPPORTED;
  int host_count=0,cl_seen=0; const char *p=line+2;
  while(p<end){
    const char *eol=find_crlf(p,end); if(!eol) return HTTP_PARSE_BAD_REQUEST;
    if(eol==p){ *header_end=(size_t)((eol+2)-buf); if(!strcmp(out->version,"HTTP/1.1")&&host_count!=1) return HTTP_PARSE_BAD_REQUEST; return HTTP_PARSE_OK; }
    if(out->header_count>=HTTP_MAX_HEADERS) return HTTP_PARSE_TOO_LARGE;
    const char *colon=memchr(p,':',(size_t)(eol-p)); if(!colon||colon==p) return HTTP_PARSE_BAD_REQUEST;
    for(const char *q=p;q<colon;q++) if(!token_char((unsigned char)*q)) return HTTP_PARSE_BAD_REQUEST;
    const char *v=colon+1; while(v<eol&&(*v==' '||*v=='\t')) v++;
    http_header_t *h=&out->headers[out->header_count++];
    if(copy_span(h->name,sizeof(h->name),p,colon)||copy_span(h->value,sizeof(h->value),v,eol)||has_ctl(h->value)) return HTTP_PARSE_BAD_REQUEST;
    if(!strcasecmp(h->name,"Host")){ if(++host_count>1) return HTTP_PARSE_BAD_REQUEST; }
    else if(!strcasecmp(h->name,"Content-Length")){
      char *ep=NULL; long x=strtol(h->value,&ep,10); if(!h->value[0]||!ep||*ep||x<0) return HTTP_PARSE_BAD_REQUEST;
      if(cl_seen&&x!=out->content_length) return HTTP_PARSE_BAD_REQUEST;
      out->content_length=x;
      cl_seen=1;
    } else if(!strcasecmp(h->name,"Transfer-Encoding") && strcasecmp(h->value,"identity")) return HTTP_PARSE_UNSUPPORTED;
    p=eol+2;
  }
  return HTTP_PARSE_BAD_REQUEST;
}
int http_build_forward_request(const http_request_t *req,const char *client_ip,char *out,size_t cap,size_t *out_len){
  size_t used=0; int n=snprintf(out,cap,"%s %s %s\r\n",req->method,req->target,req->version); if(n<0||(size_t)n>=cap) return -1; used=(size_t)n;
  for(size_t i=0;i<req->header_count;i++){
    const http_header_t *h=&req->headers[i]; if(hop(h->name)||!strcasecmp(h->name,"X-Forwarded-For")) continue;
    n=snprintf(out+used,cap-used,"%s: %s\r\n",h->name,h->value); if(n<0||(size_t)n>=cap-used) return -1; used+=(size_t)n;
  }
  n=snprintf(out+used,cap-used,"X-Forwarded-For: %s\r\nConnection: close\r\n\r\n",client_ip?client_ip:"unknown"); if(n<0||(size_t)n>=cap-used) return -1; used+=(size_t)n; *out_len=used; return 0;
}
const char *http_result_message(http_parse_result_t r){
  switch(r){case HTTP_PARSE_OK:return "ok";case HTTP_PARSE_BAD_REQUEST:return "bad request";case HTTP_PARSE_TOO_LARGE:return "request too large";case HTTP_PARSE_UNSUPPORTED:return "unsupported request";case HTTP_PARSE_TRAVERSAL:return "unsafe request target";default:return "parse error";}
}
