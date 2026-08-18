import http.client
import os
import signal
import subprocess
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def wait_for_port(port, timeout=5):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=0.2)
            conn.request("GET", "/health-probe")
            conn.getresponse().read()
            conn.close()
            return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError(f"port {port} did not become ready")


def request(method, path, body=None):
    conn = http.client.HTTPConnection("127.0.0.1", 8080, timeout=3)
    conn.request(method, path, body=body)
    response = conn.getresponse()
    data = response.read().decode()
    status = response.status
    conn.close()
    return status, data


def main():
    backend = subprocess.Popen(["python3", "examples/backend.py"], cwd=ROOT)
    proxy = None
    try:
        time.sleep(0.2)
        proxy = subprocess.Popen([
            "./build/cinderproxy",
            "--listen", "8080",
            "--backend-host", "127.0.0.1",
            "--backend-port", "9000",
            "--health-interval", "1",
            "--timeout", "2",
        ], cwd=ROOT)

        wait_for_port(8080)

        status, data = request("GET", "/hello")
        assert status == 200, (status, data)

        payload = b"x" * (256 * 1024)
        status, data = request("POST", "/upload", payload)
        assert status == 200, (status, data)
        assert "262144 bytes" in data, data

        backend.terminate()
        backend.wait(timeout=3)
        time.sleep(1.5)

        status, _ = request("GET", "/after-backend-stop")
        assert status == 503, status

        print("test_integration: ok")
    finally:
        if proxy is not None and proxy.poll() is None:
            proxy.terminate()
            proxy.wait(timeout=3)
        if backend.poll() is None:
            backend.terminate()
            backend.wait(timeout=3)


if __name__ == "__main__":
    main()
