#!/usr/bin/env python3
"""Malformed-input and resource-exhaustion checks against a RUNNING EdgeFlow (Phase 10).

    abuse.py --port 8080 --max-connections 200 --request-timeout 2 --keep-alive-timeout 3 [--pid PID]

Each check sends something hostile over a raw socket (the HTTP libraries would repair most of it), prints one
line `PASS|FAIL  name  detail`, and then proves the gateway is still serving (GET /health). With --pid
(a native gateway) the number of open file descriptors and the resident memory are compared before and
after the whole run: a leak would show as growth. Exit status 1 if any check failed. Standard library only.
"""
import argparse
import os
import socket
import sys
import time

results = []


def connect(port, timeout=5.0):
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    s.settimeout(timeout)
    return s


def read_response(s, limit=65536):
    """Everything the server sends until it closes or goes quiet; returns bytes."""
    data = b""
    try:
        while len(data) < limit:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
    except (socket.timeout, ConnectionResetError, OSError):
        pass
    return data


def status_of(data):
    if not data.startswith(b"HTTP/"):
        return None
    try:
        return int(data.split(b" ", 2)[1])
    except (IndexError, ValueError):
        return None


def healthy(port):
    try:
        s = connect(port, 3.0)
        s.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        data = read_response(s)
        s.close()
        return status_of(data) == 200
    except OSError:
        return False


def record(name, ok, detail, port):
    alive = healthy(port)
    flag = "PASS" if ok and alive else "FAIL"
    results.append(flag == "PASS")
    print(f"{flag}  {name:<46} {detail}{'' if alive else '  [GATEWAY NOT HEALTHY AFTERWARDS]'}", flush=True)


def one_shot(port, payload, wait=5.0):
    s = connect(port, wait)
    s.sendall(payload)
    data = read_response(s)
    s.close()
    return data


def fds(pid):
    return len(os.listdir(f"/proc/{pid}/fd"))


def rss_kb(pid):
    for line in open(f"/proc/{pid}/status"):
        if line.startswith("VmRSS:"):
            return int(line.split()[1])
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--max-connections", type=int, default=200)
    ap.add_argument("--request-timeout", type=float, default=2.0)
    ap.add_argument("--keep-alive-timeout", type=float, default=3.0)
    ap.add_argument("--max-header-bytes", type=int, default=8192)
    ap.add_argument("--max-body-bytes", type=int, default=1048576)
    ap.add_argument("--pid", type=int, default=0, help="gateway pid (native process): compare fds and RSS")
    a = ap.parse_args()
    port = a.port

    if not healthy(port):
        print("the gateway is not answering /health", file=sys.stderr)
        return 2
    if a.pid:
        fd0, rss0 = fds(a.pid), rss_kb(a.pid)
        print(f"before: fds={fd0} rss={rss0} kB")

    # ---- malformed requests: each must be answered with a 4xx (or the connection closed), never a crash
    d = one_shot(port, b"THIS IS NOT HTTP\r\n\r\n")
    record("garbage request line", status_of(d) == 400 or d == b"", f"-> {status_of(d)}", port)

    d = one_shot(port, b"GET / HTTP/9.9\r\nHost: x\r\n\r\n")
    record("unsupported HTTP version", status_of(d) in (400, 505) or d == b"", f"-> {status_of(d)}", port)

    d = one_shot(port, b"FROB /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
    record("unknown method", status_of(d) in (400, 405, 501) or d == b"", f"-> {status_of(d)}", port)

    d = one_shot(port, b"GET /" + b"a" * 20000 + b" HTTP/1.1\r\nHost: x\r\n\r\n")
    record("20 KB request target", status_of(d) in (400, 414, 431) or d == b"", f"-> {status_of(d)}", port)

    big = b"X-Big: " + b"a" * (a.max_header_bytes + 2048) + b"\r\n"
    d = one_shot(port, b"GET /health HTTP/1.1\r\nHost: x\r\n" + big + b"\r\n")
    record("header block over max_header_bytes", status_of(d) == 431, f"-> {status_of(d)} (expect 431)", port)

    many = b"".join(b"X-H%d: v\r\n" % i for i in range(5000))
    d = one_shot(port, b"GET /health HTTP/1.1\r\nHost: x\r\n" + many + b"\r\n")
    record("5000 headers", status_of(d) in (400, 431) or d == b"", f"-> {status_of(d)}", port)

    d = one_shot(port, b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\nConnection: close\r\n\r\n" % (a.max_body_bytes + 1))
    record("Content-Length over max_request_body_bytes", status_of(d) == 413, f"-> {status_of(d)} (expect 413, no body sent)", port)

    d = one_shot(port, b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nContent-Length: 5\r\n\r\nabcde")
    record("two different Content-Length headers", status_of(d) == 400 or d == b"", f"-> {status_of(d)}", port)

    d = one_shot(port, b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n")
    record("Content-Length + Transfer-Encoding (smuggling)", status_of(d) == 400 or d == b"", f"-> {status_of(d)}", port)

    d = one_shot(port, b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n")
    record("negative Content-Length", status_of(d) == 400 or d == b"", f"-> {status_of(d)}", port)

    d = one_shot(port, b"GET /health\x00injected HTTP/1.1\r\nHost: x\r\n\r\n")
    record("NUL byte in the request target", status_of(d) in (400, 404) or d == b"", f"-> {status_of(d)}", port)

    d = one_shot(port, b"GET /health HTTP/1.1\r\nHost: x\r\nBad Header Name: v\r\n\r\n")
    record("space in a header name", status_of(d) == 400 or d == b"", f"-> {status_of(d)}", port)

    d = one_shot(port, b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nZZZ\r\nabc\r\n0\r\n\r\n")
    record("malformed chunk size", status_of(d) == 400 or d == b"", f"-> {status_of(d)}", port)

    for target in (b"/proxy/", b"/proxy//", b"/proxy/nosuch/x", b"/proxy/../../etc/passwd", b"/services/..%2f..%2fetc",
                   b"/proxy/%00/x", b"/services/a%0d%0aX-Evil:1/instances"):
        d = one_shot(port, b"GET " + target + b" HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        ok = status_of(d) in (400, 401, 404, 503) or d == b""
        record(f"odd path {target.decode('latin1')}", ok and b"root:" not in d, f"-> {status_of(d)}", port)

    d = one_shot(port, b"GET /proxy/a/x HTTP/1.1\r\nHost: x\r\nX-Forwarded-For: " + b"1.1.1.1, " * 2000 + b"2.2.2.2\r\nConnection: close\r\n\r\n")
    record("giant X-Forwarded-For", status_of(d) in (400, 404, 431, 503) or d == b"", f"-> {status_of(d)}", port)

    # ---- slow clients: the server must give up on them, not wait forever
    s = connect(port, a.request_timeout + 6)
    s.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\nX-Slow: ")  # headers never finish
    t0 = time.time()
    d = read_response(s)
    took = time.time() - t0
    s.close()
    record("slowloris (header never finished)", took < a.request_timeout + 3 and (status_of(d) in (408, None)),
           f"closed after {took:.1f}s -> {status_of(d)} (request_timeout {a.request_timeout}s)", port)

    s = connect(port, a.keep_alive_timeout + 6)
    s.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n")
    first = s.recv(4096)
    t0 = time.time()
    rest = read_response(s)
    took = time.time() - t0
    s.close()
    record("idle keep-alive connection is closed", status_of(first) == 200 and rest == b"" and took < a.keep_alive_timeout + 3,
           f"closed after {took:.1f}s idle (keep_alive_timeout {a.keep_alive_timeout}s)", port)

    s = connect(port, a.request_timeout + 6)
    s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1000\r\n\r\nabc")  # body never completes
    t0 = time.time()
    d = read_response(s)
    took = time.time() - t0
    s.close()
    record("body that never completes", took < a.request_timeout + 3 and status_of(d) in (408, None),
           f"closed after {took:.1f}s -> {status_of(d)}", port)

    # ---- connection exhaustion: more idle connections than max_connections
    held = []
    refused = 0
    for _ in range(a.max_connections + 100):
        try:
            c = connect(port, 2.0)
            held.append(c)
        except OSError:
            refused += 1
    time.sleep(0.5)
    # connections beyond the limit are accepted by the kernel and closed at once by the server: probe for that
    closed_by_server = 0
    for c in held[a.max_connections:]:
        c.settimeout(0.5)
        try:
            if c.recv(1) == b"":
                closed_by_server += 1
        except (socket.timeout, OSError):
            pass
    served_while_full = healthy(port)
    for c in held:
        c.close()
    time.sleep(1.0)
    record("more idle connections than max_connections", closed_by_server >= 50 and not served_while_full or closed_by_server >= 50,
           f"opened {len(held)}, closed by the server beyond the limit: {closed_by_server}, /health while full: {served_while_full}", port)
    time.sleep(a.keep_alive_timeout + 1)
    record("serving again once the idle connections are gone", healthy(port), "", port)

    # ---- pipelining flood: thousands of requests in one write
    n = 3000
    s = connect(port, 15.0)
    s.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n" * n)
    got = b""
    t0 = time.time()
    while got.count(b"HTTP/1.1 200") < n and time.time() - t0 < 12:
        try:
            chunk = s.recv(65536)
        except (socket.timeout, OSError):
            break
        if not chunk:
            break
        got += chunk
    s.close()
    answered = got.count(b"HTTP/1.1 200")
    record("3000 pipelined requests in one write", answered == n, f"{answered}/{n} answered in {time.time() - t0:.2f}s", port)

    # ---- connection churn: open/send/close very fast, and half-open resets
    ok = True
    for i in range(500):
        try:
            c = connect(port, 2.0)
            c.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n" if i % 2 else b"GET /hea")
            if i % 3 == 0:
                c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00")  # RST on close
            c.close()
        except OSError:
            ok = False
    record("500 connections opened and abandoned (some with RST)", ok, "", port)

    if a.pid:
        time.sleep(a.keep_alive_timeout + 2)
        fd1, rss1 = fds(a.pid), rss_kb(a.pid)
        leak = fd1 - fd0
        print(f"after:  fds={fd1} rss={rss1} kB  (fds {leak:+d}, rss {rss1 - rss0:+d} kB)")
        results.append(leak <= 2)
        print(f"{'PASS' if leak <= 2 else 'FAIL'}  no file-descriptor leak                        fds {fd0} -> {fd1}")

    failed = results.count(False)
    print(f"\n{len(results) - failed}/{len(results)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
