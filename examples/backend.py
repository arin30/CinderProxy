from http.server import BaseHTTPRequestHandler, HTTPServer
import sys


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        body = f"backend received {self.path}\n".encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        remaining = length
        while remaining:
            chunk = self.rfile.read(min(16384, remaining))
            if not chunk:
                break
            remaining -= len(chunk)
        received = length - remaining
        body = f"backend received {received} bytes\n".encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


port = int(sys.argv[1]) if len(sys.argv) > 1 else 9000
HTTPServer(("127.0.0.1", port), Handler).serve_forever()
