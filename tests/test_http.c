#include "http.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static http_parse_result_t parse(const char *s){ http_request_t r; size_t e=0; return http_parse_request(s,strlen(s),16384,&r,&e); }
int main(void){
  assert(parse("GET /ok HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_OK);
  assert(parse("GET /a/../b HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_TRAVERSAL);
  assert(parse("GET /a/%2e%2e/b HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_TRAVERSAL);
  assert(parse("GET / HTTP/1.1\r\nHost: one\r\nHost: two\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
  assert(parse("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: nope\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
  assert(parse("POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n")==HTTP_PARSE_UNSUPPORTED);
  puts("test_http: ok"); return 0;
}
