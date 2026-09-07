#!/usr/bin/env python3
"""
Load test for the compression service: throughput and latency percentiles under
concurrency.

  python service/loadtest.py --payload corpus/dickens --duration 15 --clients 8

Why a home-made generator rather than wrk or Locust: both would work and either is a
drop-in replacement, but wrk has no Windows build and Locust is a sizeable install for
what this needs, which is N threads issuing keep-alive POSTs and a percentile summary.
The standard library covers it in a page, and the numbers are the same numbers.

What is measured, and why each choice:

- LATENCY IS PER REQUEST, wall clock, from the moment the request is written to the moment
  the last response byte is read. That is what a client experiences.
- p99 IS REPORTED ALONGSIDE THE MEAN because the mean hides the thing that matters. A
  service with a 20 ms mean and a 900 ms p99 is a service where one request in a hundred
  looks broken.
- A WARM-UP PERIOD IS DISCARDED. The first requests pay for connection setup, page faults
  on the payload, and the CPU still being at its idle clock.
- CONNECTIONS ARE REUSED (HTTP keep-alive). Otherwise a large share of every measurement
  is TCP and it swamps the compression.
"""

from __future__ import annotations

import argparse
import http.client
import statistics
import threading
import time
from pathlib import Path
from urllib.parse import urlparse

WARMUP_SECONDS = 2.0


class Worker(threading.Thread):
    """One simulated client: a persistent connection issuing requests back to back."""

    def __init__(self, url: str, path: str, payload: bytes, stop: threading.Event,
                 warm_at: float):
        super().__init__(daemon=True)
        parsed = urlparse(url)
        self.host = parsed.hostname
        self.port = parsed.port or 80
        self.path = path
        self.payload = payload
        self.stop = stop
        self.warm_at = warm_at
        self.latencies: list[float] = []
        self.bytes_in = 0
        self.bytes_out = 0
        self.errors = 0
        self.warmup_requests = 0

    def run(self) -> None:
        connection = http.client.HTTPConnection(self.host, self.port, timeout=120)
        headers = {"Content-Type": "application/octet-stream",
                   "Content-Length": str(len(self.payload))}
        while not self.stop.is_set():
            started = time.perf_counter()
            try:
                connection.request("POST", self.path, body=self.payload, headers=headers)
                response = connection.getresponse()
                body = response.read()
                elapsed = time.perf_counter() - started
                if response.status != 200:
                    self.errors += 1
                    continue
            except Exception:
                self.errors += 1
                # A dropped connection must not end the worker: reconnect and carry on,
                # or one blip would silently reduce the offered load for the whole run.
                try:
                    connection.close()
                except Exception:
                    pass
                connection = http.client.HTTPConnection(self.host, self.port, timeout=120)
                continue

            if time.perf_counter() < self.warm_at:
                self.warmup_requests += 1
                continue
            self.latencies.append(elapsed)
            self.bytes_in += len(self.payload)
            self.bytes_out += len(body)
        connection.close()


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round(fraction * (len(ordered) - 1)))))
    return ordered[index]


def run_case(url: str, path: str, payload: bytes, clients: int, duration: float) -> dict:
    stop = threading.Event()
    warm_at = time.perf_counter() + WARMUP_SECONDS
    workers = [Worker(url, path, payload, stop, warm_at) for _ in range(clients)]

    for worker in workers:
        worker.start()
    time.sleep(WARMUP_SECONDS)
    measured_start = time.perf_counter()
    time.sleep(duration)
    stop.set()
    for worker in workers:
        worker.join(timeout=180)
    wall = time.perf_counter() - measured_start

    latencies = [value for worker in workers for value in worker.latencies]
    bytes_in = sum(worker.bytes_in for worker in workers)
    bytes_out = sum(worker.bytes_out for worker in workers)
    errors = sum(worker.errors for worker in workers)

    return {
        "clients": clients,
        "requests": len(latencies),
        "errors": errors,
        "rps": len(latencies) / wall if wall > 0 else 0.0,
        "mib_s": bytes_in / (1024 * 1024) / wall if wall > 0 else 0.0,
        "mean_ms": 1000 * statistics.fmean(latencies) if latencies else 0.0,
        "p50_ms": 1000 * percentile(latencies, 0.50),
        "p95_ms": 1000 * percentile(latencies, 0.95),
        "p99_ms": 1000 * percentile(latencies, 0.99),
        "max_ms": 1000 * max(latencies) if latencies else 0.0,
        "ratio": 100 * (1 - bytes_out / bytes_in) if bytes_in else 0.0,
    }


def print_table(rows: list[dict], title: str) -> str:
    lines = [f"\n### {title}\n",
             "| Clients | req/s | MiB/s in | mean ms | p50 | p95 | p99 | max | errors |",
             "|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for row in rows:
        lines.append(
            f"| {row['clients']} | {row['rps']:.1f} | {row['mib_s']:.1f} | "
            f"{row['mean_ms']:.1f} | {row['p50_ms']:.1f} | {row['p95_ms']:.1f} | "
            f"{row['p99_ms']:.1f} | {row['max_ms']:.1f} | {row['errors']} |")
    table = "\n".join(lines)
    print(table)
    return table


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:8080")
    parser.add_argument("--payload", required=True, help="file to send as the request body")
    parser.add_argument("--payload-bytes", type=int, default=256 * 1024,
                        help="truncate the payload to this many bytes (0 = whole file)")
    parser.add_argument("--duration", type=float, default=10.0, help="seconds per case")
    parser.add_argument("--clients", default="1,2,4,8,16",
                        help="comma-separated concurrency levels to sweep")
    parser.add_argument("--server-threads", default="1,0",
                        help="comma-separated per-request thread counts; 0 = one per core")
    parser.add_argument("--block", type=int, default=0,
                        help="bytes per block on the server; 0 uses the server default")
    parser.add_argument("--out", default=None, help="write the markdown tables here")
    args = parser.parse_args()

    payload = Path(args.payload).read_bytes()
    if args.payload_bytes and len(payload) > args.payload_bytes:
        payload = payload[: args.payload_bytes]

    # Confirm the service is up and answering before starting a timed run.
    parsed = urlparse(args.url)
    probe = http.client.HTTPConnection(parsed.hostname, parsed.port or 80, timeout=10)
    probe.request("GET", "/health")
    health = probe.getresponse()
    if health.status != 200:
        print(f"service is not healthy: HTTP {health.status}")
        return 1
    print(health.read().decode())
    probe.close()

    concurrency = [int(value) for value in args.clients.split(",")]
    server_threads = [int(value) for value in args.server_threads.split(",")]

    print(f"\npayload {len(payload):,} bytes from {args.payload}, "
          f"{args.duration:.0f}s per case after a {WARMUP_SECONDS:.0f}s warm-up")

    sections = []
    for threads in server_threads:
        block_note = "" if args.block == 0 else f", block={args.block:,} bytes"
        label = (f"single-threaded compression per request{block_note}" if threads == 1
                 else f"parallel compression per request "
                      f"(threads={threads or 'one per core'}{block_note})")
        rows = []
        for clients in concurrency:
            route = f"/compress?threads={threads}&block={args.block}"
            rows.append(run_case(args.url, route, payload, clients, args.duration))
        sections.append(print_table(rows, label))

    if args.out:
        Path(args.out).write_text("\n".join(sections) + "\n", encoding="utf-8")
        print(f"\nwrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
