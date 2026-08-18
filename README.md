# CinderProxy

CinderProxy is a small reverse proxy written in C.

I built it to get more hands on with socket programming, HTTP request handling, concurrency, and defensive input validation. It accepts HTTP/1.1 requests from clients, validates them, applies a few basic protections, forwards accepted requests to a backend server, and streams the response back to the client.

This is not meant to replace nginx, HAProxy, or Envoy. I wanted the code to stay small enough that I could understand the full request path while still dealing with untrusted input and real network behavior.

## Current features

* HTTP/1.0 and HTTP/1.1 request parsing
* reverse proxying to a configurable backend
* one thread per client connection
* request header size limits
* path traversal checks
* malformed header rejection
* duplicate `Host` header rejection
* `Content-Length` validation
* unsupported transfer encoding rejection
* per-IP rate limiting
* socket timeouts
* hop-by-hop header filtering
* `X-Forwarded-For` insertion
* simple request logging
* backend failure handling

## Build

On macOS or Linux:

```bash
make
```

The binary is written to `build/cinderproxy`.

## Run it locally

Start the included backend in one terminal:

```bash
python3 examples/backend.py
```

Then start CinderProxy in another terminal:

```bash
./build/cinderproxy --listen 8080 --backend-host 127.0.0.1 --backend-port 9000
```

Send a request through the proxy:

```bash
curl -v http://127.0.0.1:8080/hello
```

You should receive a response from the backend through CinderProxy.

## Options

```text
--listen PORT
--backend-host HOST
--backend-port PORT
--timeout SECONDS
--rate-limit REQUESTS
--rate-window SECONDS
```

For example:

```bash
./build/cinderproxy \
  --listen 8080 \
  --backend-host 127.0.0.1 \
  --backend-port 9000 \
  --rate-limit 30 \
  --rate-window 10
```

## Request handling

The parser rejects a few cases that are easy to get wrong when processing raw HTTP, including malformed header names, control characters in header values, duplicate `Host` headers, invalid `Content-Length` values, unsupported transfer encodings, and obvious path traversal patterns.

Before forwarding a request, the proxy removes hop-by-hop headers and adds its own `X-Forwarded-For` value.

Chunked request bodies are not supported yet. I reject them instead of attempting to partially implement the format.

## Tests

```bash
make test
```

The current tests cover valid request parsing, duplicate `Host` headers, traversal attempts, invalid content lengths, and unsupported transfer encoding.

## Project layout

```text
include/http.h       HTTP request structures and parser interface
src/http.c           parser and forwarding logic
src/cinderproxy.c    sockets, proxying, threading, and rate limiting
tests/test_http.c    parser tests
examples/backend.py  tiny local backend for manual testing
```

## Next steps

A few things I want to add next:

* bounded worker pool instead of one thread per connection
* TLS termination through OpenSSL
* backend health checks
* fuzz testing for the HTTP parser
* connection reuse
* stronger request body streaming
* more precise URI normalization
