#!/usr/bin/env python3
"""A small demo backend for the EdgeFlow interview demo (standard library only).

  GET /            {"backend": "<NAME>", "path": "/"}        answers with the backend's name, so load balancing is visible
  GET /health      200 {"status":"UP"}                        what EdgeFlow's health checker probes
  GET /cacheable   same JSON, Cache-Control: public, max-age=30 (EdgeFlow's cache may store it); "served_at" changes
                   on every real call, so a cached answer is recognisable
  GET /fail        always 503 (to show retries and the circuit breaker)
  POST /__mode     body "ok" | "fail" | "slow": switches every later request to normal / 503 / 3 s delay (health stays 200)
Environment: NAME (default "backend"), PORT (default 8080).
"""
import http.server
import json
import os
import threading
import time

NAME = os.environ.get("NAME", "backend")
PORT = int(os.environ.get("PORT", "8080"))
mode = "ok"
lock = threading.Lock()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send(self, code, body, extra=None):
        data = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/health":
            return self._send(200, {"status": "UP"})
        with lock:
            current = mode
        if current == "slow":
            time.sleep(3)
        if current == "fail" or self.path == "/fail":
            return self._send(503, {"backend": NAME, "error": "induced failure"})
        extra = {"Cache-Control": "public, max-age=30"} if self.path.startswith("/cacheable") else None
        body = {"backend": NAME, "path": self.path}
        if extra:
            body["served_at"] = time.strftime("%H:%M:%S")
        self._send(200, body, extra)

    def do_POST(self):
        global mode
        length = int(self.headers.get("Content-Length") or 0)
        text = self.rfile.read(length).decode().strip()
        if self.path == "/__mode" and text in ("ok", "fail", "slow"):
            with lock:
                mode = text
            return self._send(200, {"backend": NAME, "mode": mode})
        self._send(404, {"error": "unknown"})

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    print(f"demo backend {NAME} listening on :{PORT}", flush=True)
    http.server.ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
