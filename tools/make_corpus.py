#!/usr/bin/env python3
"""
Generates the structured-text half of the benchmark corpus: JSON, CSV and a server log.

The Silesia corpus covers prose, source code, binaries and scientific data, but not the
machine-generated formats that dominate real compression workloads. Those are worth
measuring separately because they compress far better than prose does -- they are
extremely repetitive at the token level (the same field names on every record) which is
exactly what LZ77 is for, and the compression numbers in the writeup would be misleading
without them.

Everything is generated from a fixed seed so the corpus is reproducible.
"""

import json
import random
import sys
from datetime import datetime, timedelta
from pathlib import Path

SEED = 20260907
TARGET = 12 * 1024 * 1024  # ~12 MB each, big enough for stable throughput timing

FIRST = ["ana", "ben", "chen", "dara", "eli", "fatima", "gus", "hana", "ivan", "jo",
         "kiran", "lena", "mo", "nina", "omar", "pia", "quinn", "raj", "sam", "tara"]
LAST = ["alvarez", "brooks", "chen", "diallo", "eriksen", "faulkner", "gupta", "hassan",
        "ito", "jensen", "kowalski", "lindqvist", "mbeki", "novak", "okafor", "petrov"]
CITIES = ["Lagos", "Karachi", "Osaka", "Lima", "Warsaw", "Nairobi", "Toronto", "Manila",
          "Bogota", "Hanoi", "Cairo", "Dublin", "Seoul", "Quito", "Perth", "Tbilisi"]
PLANS = ["free", "starter", "team", "business", "enterprise"]
PATHS = ["/", "/login", "/api/v1/users", "/api/v1/orders", "/api/v1/orders/search",
         "/static/app.js", "/static/app.css", "/health", "/metrics", "/api/v1/session"]
AGENTS = [
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Safari/605.1.15",
    "Mozilla/5.0 (X11; Linux x86_64; rv:121.0) Gecko/20100101 Firefox/121.0",
    "curl/8.4.0",
]


def write_json(path: Path, rng: random.Random) -> None:
    """Nested records with repeated keys -- the shape an API actually returns."""
    records = []
    size = 0
    user_id = 100000
    while size < TARGET:
        user_id += 1
        record = {
            "id": user_id,
            "name": {"first": rng.choice(FIRST), "last": rng.choice(LAST)},
            "email": f"{rng.choice(FIRST)}.{rng.choice(LAST)}@example.com",
            "city": rng.choice(CITIES),
            "plan": rng.choice(PLANS),
            "active": rng.random() > 0.15,
            "seats": rng.randint(1, 250),
            "balance": round(rng.uniform(0, 5000), 2),
            "tags": rng.sample(["beta", "vip", "trial", "churn-risk", "referred"],
                               k=rng.randint(0, 3)),
            "created_at": (datetime(2023, 1, 1) + timedelta(minutes=rng.randint(0, 1500000))).isoformat(),
        }
        records.append(record)
        size += 260  # rough per-record estimate; the loop below trims to the real target
        if size >= TARGET:
            break
    path.write_text(json.dumps({"users": records}, indent=2), encoding="utf-8")


def write_csv(path: Path, rng: random.Random) -> None:
    """Columnar data: short fields, heavy repetition down each column."""
    lines = ["id,first,last,city,plan,active,seats,balance,signup_date"]
    size = len(lines[0])
    user_id = 100000
    while size < TARGET:
        user_id += 1
        date = (datetime(2023, 1, 1) + timedelta(days=rng.randint(0, 1000))).date().isoformat()
        line = (f"{user_id},{rng.choice(FIRST)},{rng.choice(LAST)},{rng.choice(CITIES)},"
                f"{rng.choice(PLANS)},{'true' if rng.random() > 0.15 else 'false'},"
                f"{rng.randint(1, 250)},{rng.uniform(0, 5000):.2f},{date}")
        lines.append(line)
        size += len(line) + 1
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_log(path: Path, rng: random.Random) -> None:
    """Combined-format access log: the most repetitive text most systems produce."""
    lines = []
    size = 0
    stamp = datetime(2026, 3, 1, 0, 0, 0)
    while size < TARGET:
        stamp += timedelta(milliseconds=rng.randint(1, 900))
        ip = f"{rng.randint(1, 223)}.{rng.randint(0, 255)}.{rng.randint(0, 255)}.{rng.randint(1, 254)}"
        method = rng.choices(["GET", "POST", "PUT", "DELETE"], weights=[80, 14, 4, 2])[0]
        status = rng.choices([200, 201, 204, 302, 400, 401, 404, 500],
                             weights=[70, 6, 4, 5, 4, 3, 6, 2])[0]
        line = (f'{ip} - - [{stamp.strftime("%d/%b/%Y:%H:%M:%S +0000")}] '
                f'"{method} {rng.choice(PATHS)} HTTP/1.1" {status} {rng.randint(120, 90000)} '
                f'"-" "{rng.choice(AGENTS)}" {rng.randint(1, 4000)}ms')
        lines.append(line)
        size += len(line) + 1
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "corpus")
    out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(SEED)
    write_json(out / "data.json", rng)
    write_csv(out / "data.csv", rng)
    write_log(out / "access.log", rng)
    for name in ("data.json", "data.csv", "access.log"):
        print(f"{name:14} {(out / name).stat().st_size:>12,} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
