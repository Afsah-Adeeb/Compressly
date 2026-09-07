#!/usr/bin/env python3
"""
Generates the two figures in the README as standalone SVG.

Hand-written SVG rather than matplotlib, for the same reason the compressor has no
dependencies: two line charts do not justify a 40 MB install, and the output is a text
file that reviews cleanly in a diff.

Both figures carry their own light/dark palette via `prefers-color-scheme` inside the
SVG, which browsers apply even when the file is referenced from an `<img>` — so the
figures stay legible in a dark README instead of becoming a white slab.

Colours are the validated defaults: series #2a78d6 light / #3987e5 dark, both passing
the lightness, chroma and 3:1 contrast checks against their surface.

Usage:  python tools/plot.py docs
"""

from __future__ import annotations

import sys
from pathlib import Path

WIDTH, HEIGHT = 760, 400
LEFT, RIGHT, TOP, BOTTOM = 66, 26, 46, 56

# Measured in Phase 5: samba (21.6 MB), in-memory, 1 MiB blocks, i5-10300H.
SPEEDUP = [(1, 1.00), (2, 1.77), (3, 2.49), (4, 2.62), (6, 3.52), (8, 3.71),
           (12, 3.19), (16, 2.87)]
PHYSICAL_CORES = 4

# Measured in Phase 4: samba, reduction against block size. None = unblocked.
BLOCKS = [(16 * 1024, 69.09), (64 * 1024, 73.23), (256 * 1024, 74.31),
          (1024 * 1024, 74.23), (4 * 1024 * 1024, 73.96), (16 * 1024 * 1024, 73.31)]
UNBLOCKED = 73.14

STYLE = """
  .surface { fill: #fcfcfb; }
  .grid    { stroke: #e4e3df; stroke-width: 1; }
  .axis    { stroke: #b8b7b1; stroke-width: 1; }
  .title   { fill: #0b0b0b; font: 600 15px system-ui, -apple-system, Segoe UI, sans-serif; }
  .label   { fill: #52514e; font: 12px system-ui, -apple-system, Segoe UI, sans-serif; }
  .muted   { fill: #7a7975; font: 11px system-ui, -apple-system, Segoe UI, sans-serif; }
  .series  { stroke: #2a78d6; fill: none; stroke-width: 2; stroke-linejoin: round;
             stroke-linecap: round; }
  .dot     { fill: #2a78d6; stroke: #fcfcfb; stroke-width: 2; }
  .refline { stroke: #b8b7b1; stroke-width: 1.5; stroke-dasharray: 5 4; fill: none; }
  .callout { fill: #0b0b0b; font: 600 12px system-ui, -apple-system, Segoe UI, sans-serif; }
  @media (prefers-color-scheme: dark) {
    .surface { fill: #1a1a19; }
    .grid    { stroke: #33322f; }
    .axis    { stroke: #55534d; }
    .title   { fill: #ffffff; }
    .label   { fill: #c3c2b7; }
    .muted   { fill: #93918a; }
    .series  { stroke: #3987e5; }
    .dot     { fill: #3987e5; stroke: #1a1a19; }
    .refline { stroke: #55534d; }
    .callout { fill: #ffffff; }
  }
"""


def open_svg(title: str, subtitle: str) -> list[str]:
    return [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{HEIGHT}" '
        f'viewBox="0 0 {WIDTH} {HEIGHT}" role="img" aria-label="{title}">',
        f"<style>{STYLE}</style>",
        f'<rect class="surface" width="{WIDTH}" height="{HEIGHT}" rx="6"/>',
        f'<text class="title" x="{LEFT}" y="24">{title}</text>',
        f'<text class="muted" x="{LEFT}" y="40">{subtitle}</text>',
    ]


def frame() -> tuple[float, float, float, float]:
    return LEFT, WIDTH - RIGHT, TOP, HEIGHT - BOTTOM


def speedup_chart() -> str:
    x0, x1, y0, y1 = frame()
    max_threads, max_speed = 16, 4.4

    def px(threads: float) -> float:
        return x0 + (threads - 1) / (max_threads - 1) * (x1 - x0)

    def py(speed: float) -> float:
        return y1 - speed / max_speed * (y1 - y0)

    parts = open_svg(
        "Parallel speedup: compressing samba (21.6 MB)",
        "Intel i5-10300H, 4 physical cores / 8 logical. 1 MiB blocks, in-memory path.")

    for speed in (1, 2, 3, 4):
        y = py(speed)
        parts.append(f'<line class="grid" x1="{x0}" y1="{y:.1f}" x2="{x1}" y2="{y:.1f}"/>')
        parts.append(f'<text class="label" x="{x0 - 10:.1f}" y="{y + 4:.1f}" '
                     f'text-anchor="end">{speed}x</text>')

    # The ceiling this machine could reach if scaling were perfect across real cores.
    parts.append(f'<line class="refline" x1="{x0}" y1="{py(PHYSICAL_CORES):.1f}" '
                 f'x2="{x1}" y2="{py(PHYSICAL_CORES):.1f}"/>')
    parts.append(f'<text class="muted" x="{x1 - 4:.1f}" y="{py(PHYSICAL_CORES) - 8:.1f}" '
                 f'text-anchor="end">perfect scaling across 4 physical cores</text>')

    # Where the physical cores run out and hyperthreading takes over.
    parts.append(f'<line class="refline" x1="{px(PHYSICAL_CORES):.1f}" y1="{y0}" '
                 f'x2="{px(PHYSICAL_CORES):.1f}" y2="{y1}"/>')
    parts.append(f'<text class="muted" x="{px(PHYSICAL_CORES) + 6:.1f}" y="{y0 + 12}">'
                 f'4 physical cores &#8594; hyperthreading beyond</text>')

    parts.append(f'<line class="axis" x1="{x0}" y1="{y1}" x2="{x1}" y2="{y1}"/>')

    points = " ".join(f"{px(t):.1f},{py(s):.1f}" for t, s in SPEEDUP)
    parts.append(f'<polyline class="series" points="{points}"/>')

    for threads, speed in SPEEDUP:
        parts.append(f'<circle class="dot" cx="{px(threads):.1f}" cy="{py(speed):.1f}" r="4.5"/>')
        parts.append(f'<text class="label" x="{px(threads):.1f}" y="{y1 + 20:.1f}" '
                     f'text-anchor="middle">{threads}</text>')

    parts.append(f'<text class="label" x="{(x0 + x1) / 2:.1f}" y="{y1 + 42:.1f}" '
                 f'text-anchor="middle">worker threads</text>')

    # Two labels, not eight: the peak, and the point where more threads make it worse.
    peak_x, peak_y = px(8), py(3.71)
    parts.append(f'<text class="callout" x="{peak_x:.1f}" y="{peak_y - 12:.1f}" '
                 f'text-anchor="middle">3.71x</text>')
    end_x, end_y = px(16), py(2.87)
    parts.append(f'<text class="callout" x="{end_x - 6:.1f}" y="{end_y + 20:.1f}" '
                 f'text-anchor="end">2.87x &#8212; oversubscribed</text>')

    parts.append("</svg>")
    return "\n".join(parts)


def blocksize_chart() -> str:
    x0, x1, y0, y1 = frame()
    lo, hi = 68.5, 75.0
    import math

    logs = [math.log2(size) for size, _ in BLOCKS]
    log_lo, log_hi = min(logs) - 0.4, max(logs) + 0.4

    def px(size: int) -> float:
        return x0 + (math.log2(size) - log_lo) / (log_hi - log_lo) * (x1 - x0)

    def py(pct: float) -> float:
        return y1 - (pct - lo) / (hi - lo) * (y1 - y0)

    def size_label(size: int) -> str:
        return f"{size // 1024} KiB" if size < 1024 * 1024 else f"{size // (1024 * 1024)} MiB"

    parts = open_svg(
        "Compression ratio against block size: samba (21.6 MB)",
        "Small blocks each pay for their own Huffman tables; large ones stretch "
        "one set of trees too far.")

    for pct in (69, 70, 71, 72, 73, 74, 75):
        y = py(pct)
        parts.append(f'<line class="grid" x1="{x0}" y1="{y:.1f}" x2="{x1}" y2="{y:.1f}"/>')
        parts.append(f'<text class="label" x="{x0 - 10:.1f}" y="{y + 4:.1f}" '
                     f'text-anchor="end">{pct}%</text>')

    parts.append(f'<line class="refline" x1="{x0}" y1="{py(UNBLOCKED):.1f}" '
                 f'x2="{x1}" y2="{py(UNBLOCKED):.1f}"/>')
    parts.append(f'<text class="muted" x="{x1 - 4:.1f}" y="{py(UNBLOCKED) + 17:.1f}" '
                 f'text-anchor="end">unblocked, one tree for the whole file: '
                 f'{UNBLOCKED:.2f}%</text>')

    parts.append(f'<line class="axis" x1="{x0}" y1="{y1}" x2="{x1}" y2="{y1}"/>')

    points = " ".join(f"{px(size):.1f},{py(pct):.1f}" for size, pct in BLOCKS)
    parts.append(f'<polyline class="series" points="{points}"/>')

    best = max(BLOCKS, key=lambda item: item[1])
    for size, pct in BLOCKS:
        parts.append(f'<circle class="dot" cx="{px(size):.1f}" cy="{py(pct):.1f}" r="4.5"/>')
        parts.append(f'<text class="label" x="{px(size):.1f}" y="{y1 + 20:.1f}" '
                     f'text-anchor="middle">{size_label(size)}</text>')

    parts.append(f'<text class="label" x="{(x0 + x1) / 2:.1f}" y="{y1 + 42:.1f}" '
                 f'text-anchor="middle">block size</text>')
    parts.append(f'<text class="callout" x="{px(best[0]):.1f}" y="{py(best[1]) - 12:.1f}" '
                 f'text-anchor="middle">{best[1]:.2f}%</text>')
    worst = min(BLOCKS, key=lambda item: item[1])
    parts.append(f'<text class="callout" x="{px(worst[0]) + 10:.1f}" y="{py(worst[1]) - 10:.1f}">'
                 f'{worst[1]:.2f}% &#8212; table overhead dominates</text>')

    parts.append("</svg>")
    return "\n".join(parts)


def main() -> int:
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "docs")
    out.mkdir(parents=True, exist_ok=True)
    (out / "speedup.svg").write_text(speedup_chart(), encoding="utf-8")
    (out / "blocksize.svg").write_text(blocksize_chart(), encoding="utf-8")
    print(f"wrote {out / 'speedup.svg'} and {out / 'blocksize.svg'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
