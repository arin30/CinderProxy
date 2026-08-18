# CinderProxy

CinderProxy is a small reverse proxy written in C.

I built it to get more hands on with socket programming, HTTP request handling, concurrency, and defensive input validation. The proxy accepts HTTP/1.0 and HTTP/1.1 requests, validates them, applies basic protections, forwards accepted traffic to a backend server, and returns the backend response to the client.

The project is intentionally smaller than production proxies such as nginx, HAProxy, or Envoy. I wanted the codebase to stay understandable while still dealing with real network behavior and attacker-controlled input.

## Features

- HTTP/1.0 and HTTP/1.1 parsing
- configurable reverse proxying
- optional TLS termination with OpenSSL
- bounded worker pool and fixed-capacity connection queue
- per-IP rate limiting
- periodic backend health checks
- request body streaming in 16 KB chunks
- request and header size limits
- malformed header rejection
- duplicate `Host` header rejection
- `Content-Length` validation
- unsupported transfer encoding rejection
- path traversal checks
- hop-by-hop header filtering
- `X-Forwarded-For` insertion
- socket timeouts and backend failure handling
- AddressSanitizer and UndefinedBehaviorSanitizer testing
- libFuzzer harness for the HTTP parser
- end-to-end integration testing
- repeatable local throughput and latency benchmark

## Build

Build the normal HTTP proxy:

```bash
make
```

The binary is written to:

```text
build/cinderproxy
```

Build the TLS-enabled version:

```bash
make tls
```

On macOS, the Makefile automatically uses Homebrew's `openssl@3` path when available.

## Run locally

Start the included backend:

```bash
python3 examples/backend.py
```

Start CinderProxy in another terminal:

```bash
./build/cinderproxy \
  --listen 8080 \
  --backend-host 127.0.0.1 \
  --backend-port 9000
```

Send a request through the proxy:

```bash
curl -v http://127.0.0.1:8080/hello
```

## TLS

The TLS build terminates HTTPS at CinderProxy and forwards the decrypted HTTP request to the configured backend.

For local testing, generate a temporary self-signed certificate:

```bash
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout key.pem \
  -out cert.pem \
  -days 1 \
  -subj "/CN=localhost"
```

Then start the TLS-enabled proxy:

```bash
./build/cinderproxy-tls \
  --listen 8443 \
  --backend-host 127.0.0.1 \
  --backend-port 9000 \
  --tls-cert cert.pem \
  --tls-key key.pem
```

Test it with:

```bash
curl -k https://127.0.0.1:8443/hello
```

TLS is optional. The normal `make` target does not require OpenSSL.

## Configuration

```text
--listen PORT
--backend-host HOST
--backend-port PORT
--timeout SECONDS
--rate-limit REQUESTS
--rate-window SECONDS
--workers THREADS
--health-interval SECONDS
--tls-cert FILE
--tls-key FILE
```

The worker pool defaults to eight threads. Accepted connections are placed into a bounded queue. If the queue is full, the proxy rejects additional work instead of creating threads indefinitely.

## Backend health checks

A background thread periodically attempts to connect to the configured backend. If the backend is unavailable, the proxy returns `503 Backend Unavailable` rather than repeatedly attempting to forward requests to a server it already knows is down.

When the backend becomes reachable again, normal forwarding resumes automatically.

## Request body streaming

CinderProxy validates request headers first, then forwards request bodies in 16 KB chunks instead of buffering the entire body in memory.

The current implementation accepts bodies up to 1 MB. Chunked request bodies are intentionally rejected because chunked decoding has not been implemented yet.

## Defensive request handling

The parser rejects malformed header names, control characters in header values, duplicate `Host` headers, invalid or conflicting request metadata, unsupported transfer encodings, oversized requests, and obvious path traversal patterns.

Before forwarding a valid request, CinderProxy removes hop-by-hop headers and adds its own `X-Forwarded-For` value.

## Tests

Run the parser tests:

```bash
make test
```

Run the full proxy integration test:

```bash
make integration
```

The integration test verifies a normal GET request, streams a 256 KB POST through the proxy, shuts down the backend, and confirms the health checker causes the proxy to return a 503 response.

Run parser tests with AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
make sanitize
```

## Fuzzing

The HTTP parser has a libFuzzer harness because it directly handles untrusted network input.

On Linux with Clang:

```bash
make fuzz
```

On macOS with Homebrew LLVM:

```bash
make fuzz FUZZ_CC="$(brew --prefix llvm)/bin/clang"
```

The default fuzz run lasts about 20 seconds and is built with libFuzzer, AddressSanitizer, and UndefinedBehaviorSanitizer.

## Benchmarking

Run the local benchmark with:

```bash
make benchmark
```

The default benchmark sends 1,000 requests using 20 concurrent clients and reports throughput plus average, p50, p95, and p99 latency.

A local macOS run produced:

```text
requests:       1000
concurrency:    20
successful:     1000
failed:         0
requests_sec:   5682.6
latency_avg_ms: 2.827
latency_p50_ms: 0.999
latency_p95_ms: 3.599
latency_p99_ms: 56.317
```

These numbers are a local development benchmark, not a production capacity claim. Results vary by hardware, operating system, backend behavior, and benchmark configuration.

The workload can be adjusted with environment variables:

```bash
BENCH_REQUESTS=5000 BENCH_CONCURRENCY=50 make benchmark
```

## Project layout

```text
include/http.h             HTTP request structures and parser interface
src/http.c                 HTTP parsing and request forwarding logic
src/cinderproxy.c          sockets, TLS, workers, health checks, streaming, and rate limiting
tests/test_http.c          parser unit tests
tests/fuzz_http.c          libFuzzer entry point
tests/test_integration.py  end-to-end proxy test
tools/benchmark.py         local throughput and latency benchmark
examples/backend.py        backend used for local and integration testing
```

## Next steps

The main areas I still want to explore are connection reuse, stricter URI normalization, and support for multiple backend targets.
