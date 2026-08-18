#ifndef CINDER_HTTP_H
#define CINDER_HTTP_H
#include <stddef.h>
#define HTTP_MAX_HEADERS 64
#define HTTP_MAX_METHOD 16
#define HTTP_MAX_TARGET 2048
#define HTTP_MAX_VERSION 16
#define HTTP_MAX_NAME 128
#define HTTP_MAX_VALUE 2048

typedef struct { char name[HTTP_MAX_NAME]; char value[HTTP_MAX_VALUE]; } http_header_t;
typedef struct {
  char method[HTTP_MAX_METHOD];
  char target[HTTP_MAX_TARGET];
  char version[HTTP_MAX_VERSION];
  http_header_t headers[HTTP_MAX_HEADERS];
  size_t header_count;
  long content_length;
} http_request_t;

typedef enum { HTTP_PARSE_OK=0, HTTP_PARSE_BAD_REQUEST, HTTP_PARSE_TOO_LARGE, HTTP_PARSE_UNSUPPORTED, HTTP_PARSE_TRAVERSAL } http_parse_result_t;
http_parse_result_t http_parse_request(const char *buf,size_t len,size_t max_header_bytes,http_request_t *out,size_t *header_end);
int http_build_forward_request(const http_request_t *req,const char *client_ip,char *out,size_t out_cap,size_t *out_len);
const char *http_result_message(http_parse_result_t r);
#endif
