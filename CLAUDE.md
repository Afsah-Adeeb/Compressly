# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

A DEFLATE-style compressor (LZ77 → Huffman) written from scratch, built as an SDE
portfolio piece. Two consequences shape every decision here:

- **No libraries do the core work.** Hand-rolled data structures are the point. Libraries
  are acceptable only for the eventual web service layer and load testing.
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
./build/tests.exe              # all tests
./build/tests.exe Lz77         # substring filter on the test name
./build/compressor.exe t <file>          # roundtrip + ratio + throughput in one pass
./build/compressor.exe c <in> <out> --lz77
./build/sweep.exe <file>       # LZ77 tuning sweep: one knob at a time, ratio and MB/s
```

**Run built executables through the PowerShell tool, not the Bash tool.** The Bash
sandbox segfaults freshly built `.exe` files when they open files on disk. The binaries
are fine; only that execution path is broken. `gzip`, `git`, `cmake`, and `python` all
work normally under Bash.

`compressor t` is the measurement that matters — it verifies correctness and produces the
ratio and throughput numbers in one pass, with no file I/O in the timed region.

## Architecture

Layered strictly, each layer testable alone:

```
bitio.h          bit-level read/write, MSB-first, inline on the hot path
huffman.*        canonical codes: frequencies → lengths → codes → decode trie
lz77.*           sliding window, hash-chain match finder, token stream
codec.*          container framing, method dispatch, STORED fallback
format.h         the format spec, written out as a comment block
tools/           cli.cpp (front end), sweep.cpp (measurement instrument)
```

`format.h` is the contract, not just constants — the byte layout of every method is
documented there and that comment block is the spec the encoder and decoder both
implement. Read it first. `CorruptInput` lives there rather than beside the bit reader
because it is part of what `decompress()` promises its callers.

A compressed file always begins with a 14-byte header whose method byte selects the
decoder. `Method::kStored` is not a failure path: incompressible input (already-compressed
files, random data) legitimately takes it, and both encoders decide by *arithmetic on the
symbol statistics* rather than by encoding and measuring — so incompressible input skips
the encode entirely.

Which algorithm the encoder uses comes from `Options`, so every codec stays reachable and
testable as new ones are added. Decompression takes no options: the file says which method
produced it.

### Adding a compression method

1. Add the enum value and document the payload layout in `format.h`.
2. Add `compressX` / `decompressX` in `codec.cpp` and wire both switch statements.
3. Extend `Algorithm` and `Options` in `codec.h`.
4. Add the generic roundtrip battery for it in `test_roundtrip.cpp` (via
   `checkRoundtripEveryAlgorithm`), plus method-specific tests naming the algorithm
   explicitly.
5. Record measurements in `NOTES.md` before moving on.

## Invariants

- **Byte-identical roundtrip is the gate for every phase.** No optimisation lands before
  the format it optimises is proven correct.
- **Output must be deterministic.** Frequency ties in tree construction break on symbol
  index for exactly this reason: benchmark runs across settings are only comparable if the
  same input always produces the same bytes.
- **The decoder never trusts the header.** A file can claim any uncompressed size; every
  allocation is bounded by what the payload could actually encode, and every read is
  bounds-checked into a `CorruptInput` throw.
- **File I/O is binary mode.** On Windows a text-mode read translates CRLF and breaks the
  roundtrip on any file containing `0x0D`.

## NOTES.md is part of the deliverable

`NOTES.md` is the running measurement log and the raw material for the Phase 8 writeup.
Every phase appends: the numbers as measured, what the numbers *mean*, the design
decisions with their tradeoffs, and — importantly — what went wrong and why. Two Phase 2
test failures are recorded there because the reasons were more instructive than the fix.
Record results when they are measured; do not plan to reconstruct them later.

## Phase plan

1. Huffman encoder + decoder — **done**
2. LZ77 — **done**
3. Combine: LZ77 output Huffman-coded, two alphabets (literals/lengths, distances)
4. Streaming / chunked processing, bounded memory
5. Parallel block compression across a thread pool; parallel output must stay decodable by
   the plain single-threaded decoder
6. Benchmark suite against gzip on the Silesia corpus
7. Web service + load test
8. README writeup: benchmark table, speedup graph, design decisions

Format compatibility with RFC 1951 is a stretch goal after Phase 6, not a constraint now.
`bitio.h` is isolated specifically so that switching to DEFLATE's LSB-first bit order is a
rewrite of two classes and nothing else.
