#!/usr/bin/env python3
import concurrent.futures
import http.client
import os
import statistics
import subprocess
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROXY_PORT = 18081
BACKEND_PORT = 19001
REQUESTS = int(os.environ.get("BENCH_REQUESTS", "1000"))
CONCURRENCY = int(os.environ.get("BENCH_CONCURRENCY", "20"))


def wait_ready(port, timeout=5):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            c = http.client.HTTPConnection("127.0.0.1", port, timeout=0.5)
            c.request("GET", "/ready")
            r = c.getresponse()
            r.read()
            c.close()
            if r.status == 200:
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"port {port} did not become ready")


def one_request(_):
    start = time.perf_counter()
    try:
        c = http.client.HTTPConnection("127.0.0.1", PROXY_PORT, timeout=5)
        c.request("GET", "/bench")
        r = c.getresponse()
        r.read()
        status = r.status
        c.close()
    except OSError:
        return 0, 0.0
    return status, (time.perf_counter() - start) * 1000.0


def percentile(values, p):
    values = sorted(values)
    if not values:
        return 0.0
    index = min(len(values) - 1, max(0, int((len(values) - 1) * p)))
    return values[index]


def main():
    if REQUESTS < 1:
        raise ValueError("BENCH_REQUESTS must be at least 1")
    if CONCURRENCY < 1:
        raise ValueError("BENCH_CONCURRENCY must be at least 1")

    backend = subprocess.Popen([
        "python3", "-c",
        "from http.server import BaseHTTPRequestHandler,HTTPServer; "
        "H=type('H',(BaseHTTPRequestHandler,),{'do_GET':lambda s:(s.send_response(200),s.send_header('Content-Length','2'),s.end_headers(),s.wfile.write(b'OK')),'log_message':lambda *a:None}); "
        f"HTTPServer(('127.0.0.1',{BACKEND_PORT}),H).serve_forever()"
    ], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    proxy = None
    try:
        wait_ready(BACKEND_PORT)
        proxy = subprocess.Popen([
            "./build/cinderproxy", "--listen", str(PROXY_PORT),
            "--backend-host", "127.0.0.1", "--backend-port", str(BACKEND_PORT),
            "--workers", str(max(8, CONCURRENCY)), "--rate-limit", str(REQUESTS + 100),
            "--rate-window", "60", "--health-interval", "1"
        ], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wait_ready(PROXY_PORT)

        start = time.perf_counter()
        with concurrent.futures.ThreadPoolExecutor(max_workers=CONCURRENCY) as pool:
            results = list(pool.map(one_request, range(REQUESTS)))
        elapsed = time.perf_counter() - start

        latencies = [lat for status, lat in results if status == 200]
        ok = len(latencies)
        failed = REQUESTS - ok
        rps = ok / elapsed if elapsed else 0.0

        print("CinderProxy local benchmark")
        print(f"requests:       {REQUESTS}")
        print(f"concurrency:    {CONCURRENCY}")
        print(f"successful:     {ok}")
        print(f"failed:         {failed}")
        print(f"elapsed_sec:    {elapsed:.3f}")
        print(f"requests_sec:   {rps:.1f}")
        if latencies:
            print(f"latency_avg_ms: {statistics.mean(latencies):.3f}")
            print(f"latency_p50_ms: {percentile(latencies, 0.50):.3f}")
            print(f"latency_p95_ms: {percentile(latencies, 0.95):.3f}")
            print(f"latency_p99_ms: {percentile(latencies, 0.99):.3f}")
    finally:
        if proxy is not None and proxy.poll() is None:
            proxy.terminate()
            proxy.wait(timeout=3)
        if backend.poll() is None:
            backend.terminate()
            backend.wait(timeout=3)


if __name__ == "__main__":
    main()
