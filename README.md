# CinderProxy

CinderProxy is a reverse proxy I wrote in C to get deeper into socket programming, HTTP, concurrency, and the kinds of edge cases that show up when a service is handling untrusted input.

The proxy sits in front of a backend server, checks incoming requests, applies a few security controls, and forwards valid traffic. I have kept the project fairly small on purpose. It is not meant to compete with nginx or HAProxy; the point is to build the important pieces myself and understand what is happening between the client and backend.

## What is implemented

- HTTP/1.0 and HTTP/1.1 request parsing
- reverse proxying to a configurable backend
- optional TLS termination with OpenSSL
- fixed worker pool with a bounded connection queue
- per-IP rate limiting
- backend health checks
- request body streaming in 16 KB chunks
- request/header size limits and socket timeouts
- validation for malformed headers, duplicate `Host`, and conflicting `Content-Length`
- rejection of unsupported transfer encodings
- path checks for traversal and ambiguous percent-encoded input
- hop-by-hop header filtering and `X-Forwarded-For`
- unit and integration tests
- ASan/UBSan testing and a libFuzzer harness
- a small local benchmark harness

## Build and run

```bash
make
python3 examples/backend.py
```

In another terminal:

```bash
./build/cinderproxy \
  --listen 8080 \
  --backend-host 127.0.0.1 \
  --backend-port 9000
```

Then send something through it:

```bash
curl -v http://127.0.0.1:8080/hello
```

The normal build has no OpenSSL dependency. Run `make clean` to remove generated build artifacts before rebuilding from scratch.

## TLS

To build the TLS version:

```bash
make tls
```

For a quick local test, create a self-signed certificate:

```bash
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout key.pem \
  -out cert.pem \
  -days 1 \
  -subj "/CN=localhost"
```

Start the TLS build:

```bash
./build/cinderproxy-tls \
  --listen 8443 \
  --backend-host 127.0.0.1 \
  --backend-port 9000 \
  --tls-cert cert.pem \
  --tls-key key.pem
```

And test it:

```bash
curl -k https://127.0.0.1:8443/hello
```

On macOS, the Makefile will use Homebrew's `openssl@3` path when it is available.

## Options

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

The default worker pool has eight threads. Connections wait in a fixed-size queue, so a burst of traffic cannot cause the process to keep creating threads indefinitely.

The health-check thread periodically tests the backend. If it goes down, new requests receive a 503 until the backend is reachable again.

Request headers are validated before forwarding. Bodies are then streamed in 16 KB chunks rather than being buffered in full. The current body limit is 1 MB. Chunked request bodies are not supported yet.

## Request validation

Most of the security work in the project is in the HTTP parser. It rejects malformed header names, control characters in header values, duplicate `Host` headers, conflicting request metadata, oversized input, and request targets that can be interpreted ambiguously.

For paths, each segment is checked after percent decoding. That catches plain and encoded `.`/`..` traversal as well as encoded separators, backslashes, NUL/control bytes, malformed escapes, and fragments. Query values are left alone, so something like `?q=../example` is still valid.

## Testing

Parser tests:

```bash
make test
```

End-to-end test:

```bash
make integration
```

The integration test starts a backend and proxy, checks a normal GET, sends a 256 KB POST through the streaming path, takes the backend down, and verifies that the health check causes a 503.

Sanitizers:

```bash
make sanitize
```

Fuzzing on Linux with Clang:

```bash
make fuzz
```

On macOS I use Homebrew LLVM:

```bash
make fuzz FUZZ_CC="$(brew --prefix llvm)/bin/clang"
```

The default fuzz run is 20 seconds and uses libFuzzer with AddressSanitizer and UndefinedBehaviorSanitizer enabled.

## Benchmark

```bash
make benchmark-suite
```

I added the benchmark mainly so I could track whether changes to the request path were making the proxy noticeably slower. The suite runs the same workload several times and reports median throughput and p95 latency instead of relying on one run.

On my Mac, five runs of 1,000 requests at concurrency 20 produced a median of **5,193 requests/sec** and **6.516 ms p95 latency**, with no failed requests. These are local development numbers, not a production capacity claim.

The workload can be changed, for example:

```bash
BENCH_RUNS=7 BENCH_REQUESTS=5000 BENCH_CONCURRENCY=50 make benchmark-suite
```

## Layout

```text
include/http.h               request structures and parser interface
src/http.c                   parsing, path validation, forwarding logic
src/cinderproxy.c            sockets, TLS, workers, health checks, streaming, rate limiting
tests/test_http.c            parser tests
tests/fuzz_http.c            libFuzzer target
tests/test_integration.py    end-to-end test
tools/benchmark.py           single benchmark run
tools/benchmark_suite.py     repeated benchmark summary
examples/backend.py          small backend used for local testing
```

## Things I still want to try

Connection reuse is probably next. I also want to experiment with multiple backend targets instead of keeping the proxy tied to one upstream server.
