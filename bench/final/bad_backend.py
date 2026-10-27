#!/usr/bin/env python3
"""A backend that passes its health check and fails every real request with 503 (an 'erroring' instance)."""
import http.server


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        ok = self.path == "/health"
        body = b'{"status":"UP"}' if ok else b'{"error":"induced failure"}'
        self.send_response(200 if ok else 503)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


http.server.ThreadingHTTPServer(("0.0.0.0", 8080), Handler).serve_forever()
