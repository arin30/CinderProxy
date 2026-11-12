#include "http.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static http_parse_result_t parse(const char *s){ http_request_t r; size_t e=0; return http_parse_request(s,strlen(s),16384,&r,&e); }
int main(void){
  {
    const char *requests[]={
      "GET\0X / HTTP/1.1\r\nHost: localhost\r\n\r\n",
      "GET /ok\0hidden HTTP/1.1\r\nHost: localhost\r\n\r\n",
      "GET / HTTP/1.1\0hidden\r\nHost: localhost\r\n\r\n",
      "GET / HTTP/1.1\r\nHost\0hidden: localhost\r\n\r\n",
      "GET / HTTP/1.1\r\nHost: localhost\0hidden\r\n\r\n"
    };
    const size_t lengths[]={
      sizeof("GET\0X / HTTP/1.1\r\nHost: localhost\r\n\r\n")-1,
      sizeof("GET /ok\0hidden HTTP/1.1\r\nHost: localhost\r\n\r\n")-1,
      sizeof("GET / HTTP/1.1\0hidden\r\nHost: localhost\r\n\r\n")-1,
      sizeof("GET / HTTP/1.1\r\nHost\0hidden: localhost\r\n\r\n")-1,
      sizeof("GET / HTTP/1.1\r\nHost: localhost\0hidden\r\n\r\n")-1
    };
    http_request_t r; size_t e=0;
    for(size_t i=0;i<sizeof(requests)/sizeof(requests[0]);i++)
      assert(http_parse_request(requests[i],lengths[i],16384,&r,&e)==HTTP_PARSE_BAD_REQUEST);
    const char body[]="POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 3\r\n\r\na\0b";
    assert(http_parse_request(body,sizeof(body)-1,16384,&r,&e)==HTTP_PARSE_OK);
  }
  {
    /* Reject invalid HTTP method tokens without blocking valid extension methods. */
    assert(parse("G@T / HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
    assert(parse("GE/T / HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
    assert(parse("G\tET / HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
    assert(parse("M-SEARCH / HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_OK);
  }


  /* HTTP/1.0 permits an absent Host, unlike HTTP/1.1. */
  assert(parse("POST /submit HTTP/1.0\r\nContent-Length: 0\r\n\r\n")==HTTP_PARSE_OK);
  assert(parse("POST /submit HTTP/1.1\r\nContent-Length: 0\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);

  assert(parse("GET /ok HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_OK);
  assert(parse("GET /search?q=../safe HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_OK);
  /* Query text must not bypass request-target control/fragment validation. */
  assert(parse("GET /search?q=bad\tvalue HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_TRAVERSAL);
  assert(parse("GET /search?q=bad\x7fvalue HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_TRAVERSAL);
  assert(parse("GET /search?q=safe#fragment HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_TRAVERSAL);
  assert(parse("GET /search?q=%23safe HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_OK);
  assert(parse("GET / HTTP/1.1\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
  assert(parse("GET / HTTP/1.1\r\nhost: localhost\r\n\r\n")==HTTP_PARSE_OK);
  assert(parse("GET / HTTP/1.1\r\nHost: one\r\nhOST: two\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
  assert(parse("GET / HTTP/1.0\r\n\r\n")==HTTP_PARSE_OK);
  assert(parse("GET / HTTP/2.0\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_UNSUPPORTED);

  {
    const char *raw="GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    http_request_t req; size_t header_end=0;
    assert(http_parse_request(raw,strlen(raw),strlen(raw)-1,&req,&header_end)==HTTP_PARSE_TOO_LARGE);
    assert(http_parse_request(raw,strlen(raw),strlen(raw),&req,&header_end)==HTTP_PARSE_OK);
    assert(header_end==strlen(raw));
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
  assert(parse("GET /safe#fragment HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_TRAVERSAL);

  assert(parse("GET / HTTP/1.1\r\nHost: one\r\nHost: two\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
  assert(parse("GET / HTTP/1.1\r\nHost:\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
  assert(parse("GET / HTTP/1.1\r\nHost:   \r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
  assert(parse("GET / HTTP/1.1\r\nHost : localhost\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
  assert(parse("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: nope\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
  assert(parse("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length:\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
  assert(parse("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: +5\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
  assert(parse("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: -1\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
  assert(parse("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 999999999999999999999999999999\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
  assert(parse("POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: gzip\r\n\r\n")==HTTP_PARSE_UNSUPPORTED);
  assert(parse("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nContent-Length: 7\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
  assert(parse("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n")==HTTP_PARSE_OK);
  assert(parse("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\ncOnTeNt-LeNgTh: 6\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
  assert(parse("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0005\r\nContent-Length: 5\r\n\r\n")==HTTP_PARSE_OK);
  assert(parse("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0005\r\nContent-Length: 6\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
  assert(parse("POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n")==HTTP_PARSE_UNSUPPORTED);
  assert(parse("POST / HTTP/1.1\r\nHost: localhost\r\ntRaNsFeR-EnCoDiNg: ChUnKeD\r\n\r\n")==HTTP_PARSE_UNSUPPORTED);
  assert(parse("POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: identity\r\n\r\n")==HTTP_PARSE_OK);
  assert(parse("POST / HTTP/1.1\r\nHost: localhost\r\ntransfer-encoding: IDENTITY\r\n\r\n")==HTTP_PARSE_OK);

  http_request_t req; size_t header_end=0, out_len=0; char forwarded[4096];
  const char *raw="GET /ok HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\npRoXy-CoNnEcTiOn: keep-alive\r\nX-Forwarded-For: spoofed\r\nX-Test: yes\r\n\r\n";
  assert(http_parse_request(raw,strlen(raw),16384,&req,&header_end)==HTTP_PARSE_OK);
  assert(http_build_forward_request(&req,"198.51.100.7",forwarded,sizeof(forwarded),&out_len)==0);
  assert(strstr(forwarded,"Connection: keep-alive")==NULL);
  assert(strstr(forwarded,"pRoXy-CoNnEcTiOn: keep-alive")==NULL);
  assert(strstr(forwarded,"X-Forwarded-For: spoofed")==NULL);
  assert(strstr(forwarded,"X-Test: yes\r\n")!=NULL);
  assert(strstr(forwarded,"X-Forwarded-For: 198.51.100.7\r\n")!=NULL);
  assert(strstr(forwarded,"Connection: close\r\n")!=NULL);
  assert(out_len==strlen(forwarded));

  {
    const char *mixed="GET / HTTP/1.1\r\nHost: localhost\r\nx-fOrWaRdEd-FoR: 203.0.113.99\r\n\r\n";
    assert(http_parse_request(mixed,strlen(mixed),16384,&req,&header_end)==HTTP_PARSE_OK);
    assert(http_build_forward_request(&req,"198.51.100.8",forwarded,sizeof(forwarded),&out_len)==0);
    assert(strstr(forwarded,"203.0.113.99")==NULL);
    assert(strstr(forwarded,"X-Forwarded-For: 198.51.100.8\r\n")!=NULL);
    assert(http_build_forward_request(&req,"198.51.100.8",forwarded,16,&out_len)==-1);
  }

  {
    const char *hop_headers =
      "GET / HTTP/1.1\r\nHost: localhost\r\n"
      "kEeP-aLiVe: timeout=5\r\nTe: trailers\r\n"
      "tRaIlEr: X-Foo\r\nuPgRaDe: websocket\r\n"
      "X-End-To-End: preserved\r\n\r\n";
    assert(http_parse_request(hop_headers,strlen(hop_headers),16384,&req,&header_end)==HTTP_PARSE_OK);
    assert(http_build_forward_request(&req,"198.51.100.9",forwarded,sizeof(forwarded),&out_len)==0);
    assert(strstr(forwarded,"kEeP-aLiVe:")==NULL);
    assert(strstr(forwarded,"Te: trailers")==NULL);
    assert(strstr(forwarded,"tRaIlEr:")==NULL);
    assert(strstr(forwarded,"uPgRaDe:")==NULL);
    assert(strstr(forwarded,"X-End-To-End: preserved\r\n")!=NULL);
    assert(strstr(forwarded,"X-Forwarded-For: 198.51.100.9\r\n")!=NULL);
  }

  {
    /* Header names must be HTTP tokens; control bytes in values are invalid. */
    assert(parse("GET / HTTP/1.1\r\nHost: localhost\r\nX Bad: value\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
    assert(parse("GET / HTTP/1.1\r\nHost: localhost\r\nX-Bad\t: value\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
    assert(parse("GET / HTTP/1.1\r\nHost: localhost\r\nX-Test: yes\x01\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
    assert(parse("GET / HTTP/1.1\r\nHost: localhost\r\nX-Test: yes\x7f\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);
    assert(parse("GET / HTTP/1.1\r\nHost: localhost\r\nX-Trace_Id: allowed\r\n\r\n")==HTTP_PARSE_OK);
  }


  {
    /* Encoded controls and separators must never reach the upstream. */
    assert(parse("GET /safe/%0D%0Ainjected HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_TRAVERSAL);
    assert(parse("GET /safe/%7F HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_TRAVERSAL);
    assert(parse("GET /safe/%2Fsecret HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_TRAVERSAL);
    assert(parse("GET /safe/%5Csecret HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_TRAVERSAL);
    assert(parse("GET /safe/%41 HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_OK);
  }

  /* Reject encoded horizontal tabs in paths, but permit encoded unreserved bytes. */
  assert(parse("GET /safe/%09injected HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_TRAVERSAL);
  assert(parse("GET /safe/%7Euser HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_OK);

  /* Incomplete header lines must not be accepted as valid requests. */
  assert(parse("GET / HTTP/1.1\r\nHost: localhost\r\nX-Trace: pending\r\n")==HTTP_PARSE_BAD_REQUEST);
  assert(parse("GET / HTTP/1.1\r\nHost: localhost\r\nX-Trace: pending\r\n\r\n")==HTTP_PARSE_OK);

  /* Percent-encoded dot segments are traversal; encoded dots in filenames are safe. */
  assert(parse("GET /safe/%2e%2e/secret HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_TRAVERSAL);
  assert(parse("GET /safe/%2e/secret HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_TRAVERSAL);
  assert(parse("GET /safe/file%2etxt HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_OK);

  /* Optional whitespace around field values does not change their meaning. */
  assert(parse("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5 \t\r\n\r\n")==HTTP_PARSE_OK);
  assert(parse("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\t\r\nContent-Length: 5 \r\n\r\n")==HTTP_PARSE_OK);
  assert(parse("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5 6\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);

  /* Host header matching is case-insensitive, including duplicate detection. */
  assert(parse("GET / HTTP/1.1\r\nhOsT: localhost\r\n\r\n")==HTTP_PARSE_OK);
  assert(parse("GET / HTTP/1.1\r\nHOST: localhost\r\nhost: other\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);

  /* Terminal dot segments remain unsafe even when followed by a query. */
  assert(parse("GET /safe/..?q=1 HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_TRAVERSAL);
  assert(parse("GET /safe/%2e?q=1 HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_TRAVERSAL);
  assert(parse("GET /safe/file.txt?q=1 HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_OK);

  /* Valid token punctuation is allowed in extension methods; separators are not. */
  assert(parse("X_CHECK / HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_OK);
  assert(parse("X:CHECK / HTTP/1.1\r\nHost: localhost\r\n\r\n")==HTTP_PARSE_BAD_REQUEST);

  puts("test_http: ok"); return 0;
}
