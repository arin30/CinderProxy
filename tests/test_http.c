#include "http.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static http_parse_result_t parse(const char *s){ http_request_t r; size_t e=0; return http_parse_request(s,strlen(s),16384,&r,&e); }
int main(void){
  assert(parse("GET /ok HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_OK);
  assert(parse("GET /search?q=../safe HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_OK);
  assert(parse("GET / HTTP/1.1\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
  assert(parse("GET / HTTP/1.0\r\n\r\n")==HTTP_PARSE_OK);

  {
    const char *raw="GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    http_request_t req; size_t header_end=0;
    assert(http_parse_request(raw,strlen(raw),strlen(raw)-1,&req,&header_end)==HTTP_PARSE_TOO_LARGE);
  }

  assert(parse("GET /a/../b HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_TRAVERSAL);
  assert(parse("GET /a/./b HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_TRAVERSAL);
  assert(parse("GET /a/%2e%2e/b HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_TRAVERSAL);
  assert(parse("GET /a/%2E./b HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_TRAVERSAL);
  assert(parse("GET /a%2fb HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_TRAVERSAL);
  assert(parse("GET /a%5cb HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_TRAVERSAL);
  assert(parse("GET /a%00b HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_TRAVERSAL);
  assert(parse("GET /bad%zz HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_TRAVERSAL);
  assert(parse("GET /bad% HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_TRAVERSAL);

  assert(parse("GET / HTTP/1.1\r\nHost: one\r\nHost: two\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
  assert(parse("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: nope\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
  assert(parse("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: +5\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
  assert(parse("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nContent-Length: 7\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
  assert(parse("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n")==HTTP_PARSE_OK);
  assert(parse("POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n")==HTTP_PARSE_UNSUPPORTED);

  http_request_t req; size_t header_end=0, out_len=0; char forwarded[4096];
  const char *raw="GET /ok HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\nX-Forwarded-For: spoofed\r\nX-Test: yes\r\n\r\n";
  assert(http_parse_request(raw,strlen(raw),16384,&req,&header_end)==HTTP_PARSE_OK);
  assert(http_build_forward_request(&req,"198.51.100.7",forwarded,sizeof(forwarded),&out_len)==0);
  assert(strstr(forwarded,"Connection: keep-alive")==NULL);
  assert(strstr(forwarded,"X-Forwarded-For: spoofed")==NULL);
  assert(strstr(forwarded,"X-Test: yes\r\n")!=NULL);
  assert(strstr(forwarded,"X-Forwarded-For: 198.51.100.7\r\n")!=NULL);
  assert(strstr(forwarded,"Connection: close\r\n")!=NULL);
  assert(out_len==strlen(forwarded));

  puts("test_http: ok"); return 0;
}
