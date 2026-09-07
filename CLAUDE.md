# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

A DEFLATE-style compressor (LZ77 → Huffman) written from scratch, built as an SDE
portfolio piece. All eight planned phases are complete; see `README.md` for the results
and `NOTES.md` for the measurement log. Two consequences shape every decision here:

- **No libraries do the core work.** Hand-rolled data structures are the point. Libraries
  are acceptable only in the service layer and the measurement harnesses.
- **The owner must be able to defend every line in an interview.** Explain the reasoning
  behind code as it is written, and prefer the clear implementation over the clever one.
  Comments carry the *why* — the tradeoff, the alternative rejected, what a real
  implementation does differently — not a restatement of the code.

The differentiator is not the algorithm (that is textbook). It is the benchmarking, the
parallelization, and being able to explain why gzip wins where it wins. Keep that framing.

## Build and test

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

Release with `-O2` is the default build type, deliberately — benchmarking a `-O0` binary
would be meaningless.

```bash
./build/tests.exe              # all 67 tests
./build/tests.exe Lz77         # substring filter on the test name
./build/compressor.exe t <file>                     # roundtrip + ratio + throughput + memory
./build/compressor.exe c <in> <out> --stream --threads 8
./build/sweep.exe <file>       # window, search depth, block size, thread sweeps
python tools/benchmark.py corpus --out docs/benchmark.md   # the gzip comparison
python tools/plot.py docs      # regenerate the README figures
python service/server.py       # the HTTP service (needs the cmpr_c target built)
```

**Run built executables through the PowerShell tool, not the Bash tool.** The Bash
sandbox segfaults freshly built `.exe` files when they open files on disk. The binaries
are fine; only that execution path is broken. `gzip`, `git`, `cmake`, `ffmpeg` and
`python` all work normally under Bash.

**Heredocs mangle backslash escapes.** Writing C++ or Python containing `\n` through a
Bash heredoc silently turns it into a real newline and corrupts the file. Use the Write
and Edit tools for anything containing escape sequences.

**The corpus is not in git** (`corpus/`, ~350 MB). It is the Silesia corpus plus generated
files; `tools/make_corpus.py` regenerates the text ones, and the media files come from
ffmpeg. The scratchpad directory is periodically cleaned by the OS — put anything that
must survive a few minutes under `corpus/` instead.

## Architecture

Layered strictly, each layer testable alone:

```
bitio.h          bit-level read/write, MSB-first, inline on the hot path
huffman.*        canonical codes over a runtime-sized alphabet
lz77.*           sliding window, hash-chain match finder, token stream
deflate.*        two alphabets + RFC 1951 length/distance code tables
block.*          encode/decode ONE independent block -- the shared primitive
codec.*          container framing, blocking, streaming, parallel assembly
threadpool.*     fixed-size pool, shared work queue
capi.*           flat C ABI for the shared library
format.h         the format spec, written out as a comment block
```

`format.h` is the contract, not just constants — the byte layout of every method is
documented there and that comment block is the spec both encoder and decoder implement.
Read it first. `CorruptInput` lives there rather than beside the bit reader because it is
part of what `decompress()` promises its callers.

**`block.cpp` is the hinge.** `encodeBlock`/`decodeBlock` compress one block with no
reference to any other, and the single-block, streaming and parallel paths in `codec.cpp`
all drive that one primitive. New algorithms plug in there; new container behaviour goes
in `codec.cpp`.

A compressed file is either a single block (methods 1–3) or a sequence of independent
blocks (method 4, nine bytes of framing each). `Method::kStored` is not a failure path:
incompressible input legitimately takes it, and every encoder decides by *arithmetic on
the symbol statistics* rather than by encoding and measuring.

### Adding a compression method

1. Add the enum value and document the payload layout in `format.h`.
2. Add `encodeX`/`decodeX` in `block.cpp` and wire both switch statements there.
3. Extend `Algorithm` in `block.h`.
4. Add the generic roundtrip battery via `checkRoundtripEveryAlgorithm` in
   `test_roundtrip.cpp`, plus method-specific tests naming the algorithm explicitly.
5. Record measurements in `NOTES.md` before moving on.

## Invariants

- **Byte-identical roundtrip is the gate for every change.** No optimisation lands before
  the format it optimises is proven correct.
- **Output must not depend on thread count, or on which path produced it.** In-memory and
  streaming, 1 thread and 8, must all emit identical bytes. Tests assert this; without it
  no benchmark comparing them means anything.
- **Output must be deterministic.** Frequency ties in tree construction break on symbol
  index for exactly this reason.
- **The decoder never trusts the header.** A file can claim any size; every allocation is
  bounded by what the payload could actually encode, and every read is bounds-checked into
  a `CorruptInput` throw.
- **No exception may cross the C ABI.** Every `capi.cpp` entry point catches everything
  and returns a status code; unwinding into a non-C++ caller is undefined behaviour.
- **File I/O is binary mode.** On Windows a text-mode read translates CRLF and breaks the
  roundtrip on any file containing `0x0D`.

## NOTES.md and README.md are part of the deliverable

`NOTES.md` is the running measurement log: numbers as measured, what they mean, decisions
with their tradeoffs, and what went wrong. `README.md` is the portfolio artifact built
from it. Any change that moves a number invalidates both — re-run
`tools/benchmark.py` and `tools/plot.py` and update them rather than leaving stale figures.

Record results when they are measured; do not plan to reconstruct them later.

## Known-open work

Listed in README under "What I would do next", in value order: table-driven Huffman
decoding, compressing the code-length tables, a two-byte-at-a-time match loop, adaptive
block boundaries, and RFC 1951 bit-compatibility. The first two are the largest wins and
are deliberately left undone so the before/after stays measurable.
