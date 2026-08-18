from http.server import BaseHTTPRequestHandler, HTTPServer
class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        body = f"backend received {self.path}\n".encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *_):
        pass
HTTPServer(("127.0.0.1", 9000), Handler).serve_forever()
