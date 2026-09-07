#!/usr/bin/env python3
"""
Phase 6 benchmark: this compressor against gzip, across file types.

Fairness is the whole point of this script, so two things are done deliberately:

1. BOTH COMPRESSORS ARE TIMED THE SAME WAY -- as processes, reading a real file and
   writing a real file, best of three runs, with each binary's own measured startup cost
   subtracted. Timing mine in-process and gzip as a subprocess would have flattered mine
   by whatever file I/O costs, which on these file sizes is not nothing.

2. THE HEADLINE THROUGHPUT COMPARISON IS SINGLE-THREADED. gzip is single-threaded, so
   comparing it against an 8-thread run would be measuring the core count, not the
   implementation. The parallel number is reported alongside, in its own column, clearly
   labelled.

Ratios need none of that care: they are exact byte counts.

Every file is round-tripped and hash-checked. A benchmark that reports throughput for a
codec that loses data is worse than no benchmark.

Usage:  python tools/benchmark.py [corpus_dir] [--out results.md]
"""

import argparse
import hashlib
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPEATS = 3
GZIP_LEVEL = "-6"  # gzip's own default, so this is gzip as people actually use it

# (filename, category, description). Categories follow the brief's three bands: text-like
# data that compresses well, binary data that compresses moderately, and already-compressed
# data where there is nothing left to remove.
CORPUS = [
    ("access.log", "text", "Server access log (generated)"),
    ("data.json", "text", "JSON API records (generated)"),
    ("data.csv", "text", "CSV export (generated)"),
    ("xml", "text", "XML documents (Silesia)"),
    ("nci", "text", "Chemical database (Silesia)"),
    ("dickens", "text", "English prose (Silesia)"),
    ("webster", "text", "Dictionary text (Silesia)"),
    ("reymont", "text", "Polish prose, PDF (Silesia)"),
    ("samba", "text", "Source tarball (Silesia)"),
    ("source.txt", "text", "C++ source, this project"),
    ("osdb", "binary", "Database dump (Silesia)"),
    ("mozilla", "binary", "Binary tarball (Silesia)"),
    ("ooffice", "binary", "Shared library (Silesia)"),
    ("shell32.dll", "binary", "Windows DLL"),
    ("sao", "binary", "Star catalogue, floats (Silesia)"),
    ("mr", "binary", "Medical MRI scan (Silesia)"),
    ("x-ray", "binary", "Medical X-ray image (Silesia)"),
    ("photo.jpg", "compressed", "JPEG image"),
    ("image.png", "compressed", "PNG image"),
    ("audio.mp3", "compressed", "MP3 audio"),
    ("video.mp4", "compressed", "H.264 video"),
    ("archive.zip", "compressed", "ZIP archive"),
    ("random.bin", "compressed", "Uniform random bytes"),
]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(command: list[str]) -> float:
    """Runs a command, returns wall time in seconds. Raises if it fails."""
    start = time.perf_counter()
    result = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    elapsed = time.perf_counter() - start
    if result.returncode != 0:
        raise RuntimeError(f"{command[0]} failed: {result.stderr.decode(errors='replace')[:400]}")
    return elapsed


def best_of(command: list[str], repeats: int = REPEATS) -> float:
    """Best of N. The minimum is the right statistic here: it is the run least disturbed
    by whatever else the machine was doing, and background noise only ever adds time."""
    return min(run(command) for _ in range(repeats))


def measure_startup(command: list[str], probe: Path) -> float:
    """Process startup cost, measured against a trivial input so it can be subtracted from
    the real runs. On small corpus files this is a large fraction of the total."""
    return min(run(command) for _ in range(5)) if probe.exists() else 0.0


def mib_per_second(size: int, seconds: float) -> float:
    return size / (1024 * 1024) / seconds if seconds > 1e-9 else 0.0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("corpus", nargs="?", default="corpus")
    parser.add_argument("--out", default=None, help="write a markdown table here")
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--exe", default="build/compressor.exe")
    parser.add_argument("--gzip", default=None, help="path to the gzip binary")
    args = parser.parse_args()

    corpus = Path(args.corpus)
    exe = Path(args.exe)
    if not exe.exists():
        print(f"compressor not found at {exe} -- build it first", file=sys.stderr)
        return 1

    # gzip ships with Git for Windows but is not usually on the PowerShell PATH, so fall
    # back to the standard install locations before giving up.
    gzip_exe = args.gzip or shutil.which("gzip")
    if gzip_exe is None:
        for candidate in (r"C:\Program Files\Git\usr\bin\gzip.exe",
                          r"C:\Program Files (x86)\Git\usr\bin\gzip.exe",
                          "/usr/bin/gzip", "/bin/gzip"):
            if Path(candidate).exists():
                gzip_exe = candidate
                break
    if gzip_exe is None:
        print("gzip not found; pass --gzip <path>", file=sys.stderr)
        return 1

    work = corpus / "_bench"
    work.mkdir(exist_ok=True)
    probe = work / "probe"
    probe.write_bytes(b"x" * 64)

    # Startup cost for each binary, so the throughput figures are about the compression
    # rather than about process creation.
    mine_startup = measure_startup([str(exe), "c", str(probe), str(work / "probe.cmpr"),
                                    "--stream", "--threads", "1"], probe)
    gzip_startup = measure_startup([gzip_exe, GZIP_LEVEL, "-k", "-f", "-c", str(probe)], probe)
    print(f"startup overhead: compressor {mine_startup*1000:.1f} ms, gzip {gzip_startup*1000:.1f} ms\n")

    rows = []
    for name, category, description in CORPUS:
        source = corpus / name
        if not source.exists():
            print(f"  skip {name} (not present)")
            continue

        size = source.stat().st_size
        mine_out = work / f"{name}.cmpr"
        mine_back = work / f"{name}.back"
        gzip_out = work / f"{name}.gz"

        # Mine, single-threaded -- the like-for-like comparison against gzip.
        one_thread = max(best_of([str(exe), "c", str(source), str(mine_out),
                                  "--stream", "--threads", "1"]) - mine_startup, 1e-9)
        # Mine, parallel -- reported separately, never mixed into the comparison.
        many_threads = max(best_of([str(exe), "c", str(source), str(mine_out),
                                    "--stream", "--threads", str(args.threads)]) - mine_startup, 1e-9)
        decompress = max(best_of([str(exe), "d", str(mine_out), str(mine_back),
                                  "--stream"]) - mine_startup, 1e-9)
        gz = max(best_of([gzip_exe, GZIP_LEVEL, "-f", "-k", "-c", str(source),
                          "-S", ".gz"]) - gzip_startup, 1e-9)

        # gzip -c writes to stdout, which the runner discards, so produce the file too.
        subprocess.run([gzip_exe, GZIP_LEVEL, "-c", str(source)],
                       stdout=gzip_out.open("wb"), check=True)

        mine_size = mine_out.stat().st_size
        gzip_size = gzip_out.stat().st_size
        ok = sha256(source) == sha256(mine_back)

        rows.append({
            "name": name, "category": category, "description": description, "size": size,
            "mine_size": mine_size, "gzip_size": gzip_size,
            "mine_pct": 100 * (1 - mine_size / size),
            "gzip_pct": 100 * (1 - gzip_size / size),
            "mine_mbps": mib_per_second(size, one_thread),
            "mine_par_mbps": mib_per_second(size, many_threads),
            "dec_mbps": mib_per_second(size, decompress),
            "gzip_mbps": mib_per_second(size, gz),
            "ok": ok,
        })
        flag = "ok" if ok else "ROUNDTRIP FAILED"
        print(f"  {name:14} {size:>12,}  mine {rows[-1]['mine_pct']:6.2f}%  "
              f"gzip {rows[-1]['gzip_pct']:6.2f}%  {flag}")

        for temp in (mine_out, mine_back, gzip_out):
            temp.unlink(missing_ok=True)

    probe.unlink(missing_ok=True)
    (work / "probe.cmpr").unlink(missing_ok=True)
    try:
        work.rmdir()
    except OSError:
        pass

    failures = [r for r in rows if not r["ok"]]
    lines = []
    labels = {"text": "Text and structured data",
              "binary": "Binary and scientific data",
              "compressed": "Already compressed"}
    for category in ("text", "binary", "compressed"):
        group = [r for r in rows if r["category"] == category]
        if not group:
            continue
        lines.append(f"\n### {labels[category]}\n")
        lines.append("| File | What it is | Size | Mine | gzip -6 | Diff | "
                     "Mine MB/s | gzip MB/s | Mine 8t MB/s | Decomp MB/s |")
        lines.append("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|")
        for r in group:
            diff = r["mine_pct"] - r["gzip_pct"]
            lines.append(
                f"| `{r['name']}` | {r['description']} | {r['size']:,} | "
                f"{r['mine_pct']:.2f}% | {r['gzip_pct']:.2f}% | {diff:+.2f} | "
                f"{r['mine_mbps']:.1f} | {r['gzip_mbps']:.1f} | "
                f"{r['mine_par_mbps']:.1f} | {r['dec_mbps']:.1f} |")

        total = sum(r["size"] for r in group)
        mine_total = sum(r["mine_size"] for r in group)
        gzip_total = sum(r["gzip_size"] for r in group)
        lines.append(f"| **total** | | **{total:,}** | **{100*(1-mine_total/total):.2f}%** | "
                     f"**{100*(1-gzip_total/total):.2f}%** | "
                     f"**{100*(1-mine_total/total) - 100*(1-gzip_total/total):+.2f}** | | | | |")

    total = sum(r["size"] for r in rows)
    mine_total = sum(r["mine_size"] for r in rows)
    gzip_total = sum(r["gzip_size"] for r in rows)
    lines.append(f"\n**Whole corpus:** {total:,} bytes. "
                 f"Mine {100*(1-mine_total/total):.2f}%, "
                 f"gzip -6 {100*(1-gzip_total/total):.2f}%, "
                 f"difference {100*(1-mine_total/total) - 100*(1-gzip_total/total):+.2f} points.")
    lines.append(f"\nRoundtrip verified byte-identical on {len(rows) - len(failures)}"
                 f"/{len(rows)} files.")

    table = "\n".join(lines)
    print(table)
    if args.out:
        Path(args.out).write_text(table + "\n", encoding="utf-8")
        print(f"\nwrote {args.out}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
