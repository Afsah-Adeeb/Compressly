#!/usr/bin/env python3
"""
HTTP service around the compressor.

  POST /compress    body = file bytes  ->  compressed bytes
  POST /decompress  body = compressed  ->  original bytes
  GET  /health      liveness and configuration
  GET  /            a small upload page, so the thing can be demonstrated in a browser

Query parameters on /compress: `threads` (0 = one per core) and `block` (bytes per block,
0 for the default). They are exposed because the load test needs to compare a
single-threaded server against a parallel one on the same code path.

Only the Python standard library is used. The rule for this project is that no library
does the compression work; the service layer is allowed dependencies, but a web framework
here would earn nothing — this is four routes over a byte stream — and would put an
install step between a reader and a running demo.

Concurrency: ThreadingHTTPServer runs a thread per request, and ctypes releases the GIL
for the duration of each foreign call, so several requests genuinely compress in parallel
inside the C++ library. That is what makes the single-threaded-versus-parallel comparison
in the load test measure anything.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cmpr import Compressor, CompressionError  # noqa: E402

# A request larger than this is refused rather than buffered. Without a cap, one client
# can decide how much memory the server allocates.
MAX_UPLOAD = 256 * 1024 * 1024

# The demo page lives in index.html rather than in a string literal here: markup does not
# belong inside a Python file, and keeping it separate means it can be opened, diffed and
# edited as HTML. Read once at import -- it never changes at runtime, and re-reading it
# per request would put a disk hit inside the latency the load test measures.
INDEX_PAGE = (Path(__file__).resolve().parent / "index.html").read_text(encoding="utf-8")


class Handler(BaseHTTPRequestHandler):
    server_version = "cmpr/1.0"
    protocol_version = "HTTP/1.1"  # keep-alive, so the load test is not measuring TCP setup

    codec: Compressor = None  # set on the class at startup
    default_threads: int = 0
    quiet: bool = False

    def log_message(self, fmt, *args):  # noqa: A003 - overriding the base class
        if not self.quiet:
            sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _send(self, status: HTTPStatus, body: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_json(self, status: HTTPStatus, payload: dict) -> None:
        self._send(status, json.dumps(payload).encode(), "application/json")

    def _read_body(self) -> bytes | None:
        length = int(self.headers.get("Content-Length") or 0)
        if length <= 0:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": "empty body"})
            return None
        if length > MAX_UPLOAD:
            self._send_json(HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                            {"error": f"body exceeds {MAX_UPLOAD} bytes"})
            return None
        return self.rfile.read(length)

    def do_GET(self) -> None:  # noqa: N802 - the base class dictates the name
        route = urlparse(self.path).path
        if route == "/health":
            self._send_json(HTTPStatus.OK, {
                "status": "ok",
                "version": self.codec.version(),
                "default_threads": self.default_threads,
                "max_upload_bytes": MAX_UPLOAD,
                # The demo page shows this so the thread selector can say plainly when the
                # container has too few cores for the speedup to appear. A benchmark number
                # without the hardware it was taken on is not a number.
                "cpus": os.cpu_count(),
            })
        elif route == "/":
            self._send(HTTPStatus.OK, INDEX_PAGE.encode(), "text/html; charset=utf-8")
        else:
            self._send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})

    def do_POST(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        route = parsed.path
        if route not in ("/compress", "/decompress"):
            self._send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})
            return

        body = self._read_body()
        if body is None:
            return

        query = parse_qs(parsed.query)
        try:
            threads = int(query.get("threads", [self.default_threads])[0])
            block = int(query.get("block", [0])[0])
        except ValueError:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": "threads and block must be integers"})
            return

        started = time.perf_counter()
        try:
            if route == "/compress":
                result = self.codec.compress(body, threads=threads, block_size=block)
            else:
                result = self.codec.decompress(body)
        except CompressionError as error:
            # A corrupt upload is the client's problem, not a server fault.
            status = (HTTPStatus.BAD_REQUEST if error.status == 2
                      else HTTPStatus.INTERNAL_SERVER_ERROR)
            self._send_json(status, {"error": str(error)})
            return
        elapsed = time.perf_counter() - started

        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(result)))
        # Handy for the load test and for anyone poking at it with curl.
        self.send_header("X-Original-Bytes", str(len(body)))
        self.send_header("X-Result-Bytes", str(len(result)))
        self.send_header("X-Elapsed-Ms", f"{elapsed * 1000:.2f}")
        self.end_headers()
        self.wfile.write(result)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--threads", type=int, default=0,
                        help="default worker threads per request; 0 means one per core")
    parser.add_argument("--library", default=None, help="path to the shared library")
    parser.add_argument("--quiet", action="store_true", help="suppress the per-request log")
    args = parser.parse_args()

    Handler.codec = Compressor(args.library)
    Handler.default_threads = args.threads
    Handler.quiet = args.quiet

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.daemon_threads = True
    print(f"{Handler.codec.version()}")
    print(f"listening on http://{args.host}:{args.port}  "
          f"(default threads per request: {args.threads or 'one per core'})")
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        while thread.is_alive():
            thread.join(0.5)
    except KeyboardInterrupt:
        print("\nshutting down")
        server.shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
