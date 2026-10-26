import http.client
import os
import socket
import subprocess
import time
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROXY_PORT = 18080
BACKEND_PORT = 19000


def wait_for_port(port, timeout=5):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError(f"port {port} did not become ready")


def stop_process(process, timeout=3):
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=timeout)


def request(method, path, body=None):
    conn = http.client.HTTPConnection("127.0.0.1", PROXY_PORT, timeout=3)
    conn.request(method, path, body=body)
    response = conn.getresponse()
    data = response.read().decode()
    status = response.status
    conn.close()
    return status, data


def oversized_header_status():
    request_bytes = (
        b"GET /oversized HTTP/1.1\r\n"
        b"Host: localhost\r\n"
        b"X-Fill: " + b"a" * (16 * 1024) + b"\r\n\r\n"
    )
    with socket.create_connection(("127.0.0.1", PROXY_PORT), timeout=3) as sock:
        sock.sendall(request_bytes)
        response = sock.recv(1024)
    return response.split(b" ", 2)[1]


def test_nul_headers_never_reach_backend():
    received = []

    class RecordingHandler(BaseHTTPRequestHandler):
        def do_GET(self):
            received.append(self.path)
            self.send_response(200)
            self.send_header("Content-Length", "0")
            self.end_headers()

        def log_message(self, *_):
            pass

    backend = ThreadingHTTPServer(("127.0.0.1", 0), RecordingHandler)
    thread = threading.Thread(target=backend.serve_forever, daemon=True)
    thread.start()
    proxy = None
    try:
        proxy = subprocess.Popen([
            "./build/cinderproxy", "--listen", str(PROXY_PORT),
            "--backend-host", "127.0.0.1", "--backend-port", str(backend.server_port),
            "--timeout", "2",
        ], cwd=ROOT)
        wait_for_port(PROXY_PORT)
        assert proxy.poll() is None, "test proxy did not start"
        malformed = [
            b"GET\x00X /nul HTTP/1.1\r\nHost: localhost\r\n\r\n",
            b"GET /nul\x00hidden HTTP/1.1\r\nHost: localhost\r\n\r\n",
            b"GET /nul HTTP/1.1\x00hidden\r\nHost: localhost\r\n\r\n",
            b"GET /nul HTTP/1.1\r\nHost\x00hidden: localhost\r\n\r\n",
            b"GET /nul HTTP/1.1\r\nHost: localhost\x00hidden\r\n\r\n",
        ]
        for payload in malformed:
            with socket.create_connection(("127.0.0.1", PROXY_PORT), timeout=3) as sock:
                sock.sendall(payload)
                response = http.client.HTTPResponse(sock)
                response.begin()
                response.read()
                assert response.status == 400, (payload, response.status)
        assert received == [], received
        assert request("GET", "/control")[0] == 200
        assert received == ["/control"], received
        print("test_nul_headers_never_reach_backend: ok")
    finally:
        stop_process(proxy)
        backend.shutdown()
        backend.server_close()
        thread.join(timeout=3)


def main():
    backend = subprocess.Popen([
        "python3",
        "examples/backend.py",
        str(BACKEND_PORT),
    ], cwd=ROOT)
    proxy = None
    try:
        wait_for_port(BACKEND_PORT)
        proxy = subprocess.Popen([
            "./build/cinderproxy",
            "--listen", str(PROXY_PORT),
            "--backend-host", "127.0.0.1",
            "--backend-port", str(BACKEND_PORT),
            "--health-interval", "1",
            "--timeout", "2",
        ], cwd=ROOT)

        wait_for_port(PROXY_PORT)
        if proxy.poll() is not None:
            raise RuntimeError("CinderProxy exited before the integration test started")

        status, data = request("GET", "/hello")
        assert status == 200, (status, data)
        assert "/hello" in data, data

        status, data = request("GET", "/search?q=../safe")
        assert status == 200, (status, data)
        assert "/search?q=../safe" in data, data

        payload = b"x" * (256 * 1024)
        status, data = request("POST", "/upload", payload)
        assert status == 200, (status, data)
        assert "262144 bytes" in data, data

        assert oversized_header_status() == b"431"

        stop_process(backend)
        time.sleep(1.5)

        status, _ = request("GET", "/after-backend-stop")
        assert status == 503, status

        print("test_integration: ok")
    finally:
        stop_process(proxy)
        stop_process(backend)


if __name__ == "__main__":
    test_nul_headers_never_reach_backend()
    main()
