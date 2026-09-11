# Compressly — a DEFLATE-style compressor, written from scratch

LZ77 followed by canonical Huffman coding — structurally the same algorithm gzip uses —
implemented in C++17 with no compression libraries, then benchmarked against gzip on
349 MB across 23 file types.

**Result: 53.02% reduction against gzip's 53.15%.** 0.13 percentage points apart over the
whole corpus, ahead of gzip on 9 of the 23 files, every roundtrip verified byte-identical.
Per thread gzip compresses 1.5–2.7× faster, and [the reasons are specific and
measurable](#why-gzip-is-faster-per-thread). With 8 threads this is faster than gzip on 19
of 23 files.

2,300 lines of library code, 1,500 lines of tests, no dependencies.

---

## Contents

- [Quick start](#quick-start)
- [Benchmark: Compressly vs gzip](#benchmark-compressly-vs-gzip)
- [Why gzip is faster per thread](#why-gzip-is-faster-per-thread)
- [Parallel speedup](#parallel-speedup)
- [Block size: the parameter that does three jobs](#block-size-the-parameter-that-does-three-jobs)
- [Memory](#memory)
- [The HTTP service](#the-http-service)
- [How it works](#how-it-works)
- [Design decisions](#design-decisions)
- [What I would do next](#what-i-would-do-next)
- [Project layout](#project-layout)

---

## Quick start

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/tests.exe
```

```bash
# compress and decompress
./build/compressor c input.txt output.cmpr --threads 8
./build/compressor d output.cmpr restored.txt

# roundtrip, ratio and throughput in one pass
./build/compressor t input.txt

# bounded-memory streaming, for files larger than RAM
./build/compressor c huge.bin huge.cmpr --stream --threads 8
```

```bash
# sweep the tuning parameters over a file
./build/sweep corpus/samba

# the full benchmark against gzip
python tools/benchmark.py corpus --out docs/benchmark.md

# the HTTP service, then a load test against it
python service/server.py --port 8080
python service/loadtest.py --payload corpus/dickens --clients 1,2,4,8,16
```

---

## Benchmark: Compressly vs gzip

The tables below label it `cmpr` — the short name it uses in code, and the extension it
writes.

23 files, 348,873,288 bytes: the complete [Silesia
corpus](https://sun.aei.polsl.pl//~sdeor/index.php?page=silesia), plus generated
JSON/CSV/log data, plus already-compressed media. `gzip -6`, its own default.

Both compressors are timed the same way — as processes, reading and writing real files,
best of three runs, each binary's measured startup subtracted. **The throughput comparison
is single-threaded**, because gzip is single-threaded and comparing it against 8 threads
would measure the core count rather than the implementation. The parallel figure sits in
its own column.

### Text and structured data

| File | What it is | Size | cmpr | gzip -6 | Diff | cmpr MB/s | gzip MB/s | cmpr 8t | Decomp |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `access.log` | Server access log | 12,655,301 | 88.07% | 87.68% | **+0.39** | 29.7 | 71.2 | 105.6 | 159.4 |
| `data.json` | JSON API records | 18,088,061 | 91.18% | 91.20% | −0.02 | 50.2 | 134.4 | 175.3 | 242.5 |
| `data.csv` | CSV export | 12,790,937 | 73.92% | 74.29% | −0.37 | 14.9 | 28.8 | 46.6 | 117.8 |
| `xml` | XML documents | 5,345,280 | 87.11% | 87.05% | **+0.06** | 31.7 | 64.3 | 73.4 | 49.0 |
| `nci` | Chemical database | 33,553,445 | 90.61% | 90.46% | **+0.15** | 39.0 | 89.6 | 131.9 | 223.8 |
| `dickens` | English prose | 10,192,446 | 61.91% | 62.04% | −0.14 | 9.9 | 16.8 | 30.4 | 73.0 |
| `webster` | Dictionary text | 41,458,703 | 70.56% | 70.57% | −0.01 | 17.1 | 30.5 | 67.2 | 36.8 |
| `reymont` | Polish prose, PDF | 6,627,202 | 71.82% | 71.95% | −0.13 | 9.6 | 19.4 | 34.6 | 105.0 |
| `samba` | Source tarball | 21,606,400 | 74.23% | 74.73% | −0.49 | 27.0 | 46.9 | 86.5 | 136.9 |
| `source.txt` | C++ source, this project | 92,330 | 71.33% | 71.54% | −0.21 | 8.7 | 36.3 | 9.6 | 12.3 |
| **total** | | **162,410,105** | **79.17%** | **79.22%** | **−0.05** | | | | |

### Binary and scientific data

| File | What it is | Size | cmpr | gzip -6 | Diff | cmpr MB/s | gzip MB/s | cmpr 8t | Decomp |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `osdb` | Database dump | 10,085,684 | 63.23% | 62.92% | **+0.30** | 28.3 | 35.6 | 84.1 | 105.1 |
| `mozilla` | Binary tarball | 51,220,480 | 62.06% | 62.81% | −0.75 | 22.2 | 27.7 | 81.6 | 103.1 |
| `ooffice` | Shared library | 6,152,192 | 49.56% | 49.66% | −0.10 | 16.7 | 21.6 | 57.4 | 53.2 |
| `shell32.dll` | Windows DLL | 7,947,424 | 53.86% | 54.19% | −0.33 | 16.5 | 21.2 | 61.9 | 54.3 |
| `sao` | Star catalogue, raw floats | 7,251,944 | 26.28% | 26.46% | −0.18 | 12.7 | 18.7 | 48.1 | 60.3 |
| `mr` | Medical MRI scan | 9,970,564 | 62.60% | 62.99% | −0.39 | 12.1 | 19.7 | 37.6 | 104.0 |
| `x-ray` | Medical X-ray image | 8,474,240 | 29.64% | 28.75% | **+0.89** | 22.8 | 26.7 | 73.7 | 68.2 |
| **total** | | **101,102,528** | **55.54%** | **55.90%** | **−0.36** | | | | |

### Already compressed

| File | What it is | Size | cmpr | gzip -6 | Diff | cmpr MB/s | gzip MB/s | cmpr 8t | Decomp |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `photo.jpg` | JPEG image | 298,161 | 4.57% | 4.46% | **+0.10** | 19.8 | 27.9 | 20.4 | 32.6 |
| `image.png` | PNG image | 880,926 | 0.15% | 0.10% | **+0.04** | 26.7 | 40.7 | 26.0 | 49.1 |
| `audio.mp3` | MP3 audio | 2,161,197 | 0.89% | 0.80% | **+0.09** | 27.9 | 37.5 | 49.1 | 54.6 |
| `video.mp4` | H.264 video | 10,837,627 | 0.03% | 0.02% | **+0.01** | 39.5 | 42.3 | 102.5 | 491.2 |
| `archive.zip` | ZIP archive | 68,182,744 | 0.29% | 0.33% | −0.04 | 37.9 | 38.7 | 116.7 | 147.8 |
| `random.bin` | Uniform random bytes | 3,000,000 | −0.00% | −0.02% | **+0.01** | 37.2 | 41.2 | 68.2 | 271.1 |
| **total** | | **85,360,655** | **0.28%** | **0.30%** | **−0.02** | | | | |

**Whole corpus: Compressly 53.02%, gzip -6 53.15%, a difference of −0.13 points.** All 23
roundtrips verified byte-identical by SHA-256.

### Reading the ratios

**Structured text is where compression earns its keep.** JSON 91%, chemical database 91%,
logs 88%, XML 87% — these formats repeat their own field names on every record, which is
exactly the redundancy LZ77 was invented for. Prose is far worse: `dickens` reaches only
62%, because English reuses words constantly but rarely repeats long exact phrases.

**Binary data spreads from 26% to 63%, and the spread is about what the bytes mean.**
`mr` (an MRI scan) hits 63% because medical images have large uniform regions. `sao` (a
star catalogue of raw floats) manages 26% because the mantissa bits of measured values
are close to random.

**Already-compressed input yields essentially nothing, correctly.** Every one of those
files is already entropy-coded, so there is no redundancy left by construction. The
property that matters is that none of them *grew*: a block the encoder cannot shrink is
stored verbatim, capping the worst case at 9 bytes of framing per block. On random data
Compressly adds 14 bytes where gzip adds 489.

**JPEG at 4.57% is not a paradox.** JPEG entropy-codes the image data but leaves headers,
quantisation tables and restart markers in the clear. That is what the 4.57% is.

**Where each side wins is not random.** Compressly comes out ahead on `x-ray` (+0.89),
`access.log` (+0.39) and `osdb` (+0.30) — large, internally uniform files, where 1 MiB
blocks give the Huffman trees several well-fitted sets. gzip wins on `mozilla` (−0.75) and
`samba` (−0.49) — heterogeneous archives, where its adaptive block splitting reacts to
content changes at the moment they happen rather than at a fixed boundary.

---

## Why gzip is faster per thread

gzip compresses 1.5–2.7× faster per thread. That is the honest headline, and the causes
are specific rather than mysterious. In the order the time actually goes:

1. **Its longest-match inner loop is hand-optimised C**, comparing two bytes at a time
   through a pointer-arithmetic trick, unrolled, with assembly variants for several
   architectures. Mine compares one byte at a time in a plain loop. This is the largest
   single gap and the least interesting one — it is thirty years of people profiling one
   function.
2. **Its Huffman decoder is table-driven.** Mine walks a binary trie one bit per step,
   which is a dependent pointer chase that cannot be prefetched. A 9-bit lookup table
   decodes a whole symbol per step. I know what this costs and left it undone
   deliberately, so the before/after is measurable rather than assumed.
3. **Its hash insertion is incremental.** zlib rolls the 3-byte hash forward one byte at a
   time; mine recomputes it. Small, but on the hottest path there is.
4. **Better constants throughout** — buffer sizes, an early-exit match threshold, skipping
   hash insertion inside long matches at low levels.

**None of that is algorithmic.** Same algorithm, same data structures, same 32 KiB window.
The difference is implementation quality in about three functions — which is exactly the
comparison this project was built to be able to make.

With 8 threads Compressly is faster than gzip on 19 of 23 files. That is a different claim, and
worth stating as one: **per core, gzip wins; per machine, blocking wins.** gzip cannot be
parallelised without changing its format. This was designed so it could be.

---

## Parallel speedup

![Speedup against thread count: 1.00x at 1 thread rising to 3.71x at 8, then falling to
2.87x at 16](docs/speedup.svg)

Blocks are independent — no shared history, no shared Huffman trees — so compressing them
concurrently needs no coordination beyond assembling the results in order. A hand-written
fixed-size thread pool (`std::thread`, `std::mutex`, `std::condition_variable`, ~80 lines)
hands out block indices to workers.

**3.71× at 8 threads on 4 physical cores.** Not 4×, and not 8×. Four causes, in order of
size:

1. **There are only four real cores.** The machine is an i5-10300H: 4 physical, 8 logical.
   The jump from 4 threads (2.62×) to 8 (3.71×) is hyperthreading, and 1.42× from the
   second thread on each core is about what SMT gives on this workload — hash-chain walking
   stalls on memory often enough that a second thread has gaps to fill, but the two still
   share one set of execution units. Past 8 the curve turns over: 16 threads is *slower*
   than 8, because oversubscribing 8 logical processors adds context switching and cache
   thrashing and buys nothing.
2. **Clock throttling.** The CPU boosts far higher with one active core than with four, so
   part of the missing scaling is simply that each core runs slower than the
   single-threaded baseline did. Invisible unless you go looking for it.
3. **Amdahl's law on what stays sequential.** Reading input, allocating the output buffer,
   and concatenating compressed blocks into it are all serial — that last one is a memcpy
   of the entire compressed output while every core waits. At 62% of theoretical 4-core
   scaling, Amdahl puts the serial fraction near 15%, which matches the I/O-plus-assembly
   share.
4. **Memory bandwidth.** Match finding is a random walk over a 32 KiB window through hash
   chains, with four cores sharing one L3 and one memory controller.

**Thread count never changes the output bytes.** Blocks are compressed in whatever order
workers pick them up but assembled by index, so a 1-thread and an 8-thread run produce
identical files. There is a test asserting it. Without that property no benchmark
comparing thread counts would mean anything, and any bug would reproduce only sometimes.

---

## Block size: the parameter that does three jobs

![Compression ratio against block size, peaking at 74.31% for 256 KiB blocks and falling
to 69.09% at 16 KiB](docs/blocksize.svg)

Splitting the input into independent blocks is what makes streaming and parallelism
possible. It is usually described as costing compression ratio, because matches cannot
cross a boundary. **Measured, that turns out to be wrong above about 256 KiB** — blocking
*improves* the ratio.

Two forces pull against each other. Every block pays for its own Huffman code length
tables, around 320 bytes; at 16 KiB blocks that overhead alone destroys 4 points. But one
set of trees stretched over a whole file fits none of it well. Past a quarter-megabyte the
second effect wins.

Across seven corpus files (140 MB), by total compressed size:

| Block size | Reduction |
|---|---|
| 64 KiB | 68.58% |
| unblocked | 69.41% |
| 256 KiB | 69.68% |
| 4 MiB | 69.78% |
| **1 MiB** | **69.82%** ← the default |

**64 KiB — the obvious round-number default — is the worst option on this corpus**, worse
even than not blocking at all.

The sign of the effect depends entirely on content. A source tarball gains 1.2 points from
blocking; a novel loses 0.6. Heterogeneous archives want small blocks so the trees can
track content that changes; homogeneous files want one tree fitted to the whole thing.

And the same parameter does a third job in the service, where it trades ratio against
**latency** rather than against ratio — see below.

---

## Memory

Peak working set compressing `mozilla` (51 MB), and then a 636 MB file with the same
binary:

| Path | Input | Peak memory | Throughput |
|---|---:|---:|---:|
| in-memory, unblocked | 51 MB | 151.0 MiB | 19.7 MB/s |
| in-memory, 1 MiB blocks | 51 MB | 90.6 MiB | 21.5 MB/s |
| **streaming, 1 thread** | 51 MB | **14.1 MiB** | 22.4 MB/s |
| streaming, 8 threads | 51 MB | 41.8 MiB | 83.7 MB/s |
| **streaming, 8 threads** | **636 MB** | **44.1 MiB** | 70.0 MB/s |
| streaming decompress | 636 MB | 8.1 MiB | 114.3 MB/s |

44.1 MiB for a 636 MB file, against 41.8 MiB for one twelve times smaller. **Memory is a
function of block size × thread count, not of input size** — which is the whole claim, and
what lets it handle files larger than RAM. The in-memory path peaks at roughly 3× the file
size (input, output, and the token vector), so that is the one that cannot.

All four paths produce byte-identical output. A test asserts it; two code paths meant to
agree drift apart unless something checks.

---

## The HTTP service

`POST /compress`, `POST /decompress`, `GET /health`, and a small upload page. Python
standard library only; the codec is loaded as a shared library through `ctypes`.

**Why a C ABI instead of shelling out to the binary:** process creation costs ~11 ms here,
and a 256 KB payload compresses in 25 ms. A third of every latency measurement would have
been Windows process creation, and the p99 would have described the operating system
rather than this codec. So the codec is exposed as a flat `extern "C"` API and built as a
DLL. `ctypes` releases the GIL during foreign calls, so request threads genuinely compress
in parallel.

### Throughput and latency, 256 KB payloads

| Clients | req/s | MiB/s | mean ms | p50 | p95 | **p99** |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 40.0 | 10.0 | 25.0 | 24.4 | 27.6 | **33.4** |
| 2 | 76.0 | 19.0 | 26.4 | 25.6 | 31.9 | **36.1** |
| 4 | 115.7 | 28.9 | 34.6 | 33.2 | 44.4 | **58.8** |
| 8 | 133.9 | 33.5 | 59.9 | 56.9 | 80.9 | **127.9** |
| 16 | 132.3 | 33.1 | 122.0 | 120.3 | 171.6 | **217.4** |

Zero errors throughout. Throughput saturates around 134 req/s between 4 and 8 clients —
the four physical cores filling up. Past that, throughput is flat and latency grows
linearly: 16 clients at 133 req/s implies 120 ms of queueing, and the measured mean is
122 ms. Little's law, visible in a table.

### Per-request parallelism helps latency, and only latency

Same test with 4 MiB payloads, so each request spans several blocks:

| Clients | 1 thread/request | 8 threads/request | |
|---:|---:|---:|---|
| 1 | 442.7 ms | **187.6 ms** | 2.36× faster |
| 2 | 484.7 ms | 269.2 ms | 1.80× faster |
| 4 | 665.3 ms | 511.9 ms | 1.30× faster |
| 8 | 1013.9 ms | 1023.9 ms | no difference |

The benefit vanishes exactly as concurrency rises, and the reason is worth stating
plainly: **there is one pool of cores and it does not care which layer keeps it busy.** At
one client the only way to use four cores is to split the request; at eight clients the
cores are already saturated by requests, and splitting each one further adds coordination
for nothing. At 8 clients the parallel configuration is marginally *slower* — that
overhead showing up.

The rule this produces: parallelise inside a request when requests are large and rare;
parallelise across requests when they are small and constant. A server that does both
without thinking gets the second case wrong.

### Block size as a latency dial

The 256 KB test showed no benefit from threads at all — because 256 KB is smaller than the
1 MiB default block, so every request was a single block with nothing to split. Dropping
the block size for that payload:

| Configuration | req/s | mean ms | p99 |
|---|---:|---:|---:|
| 1 MiB blocks, 1 thread | 40.0 | 25.0 | 33.4 |
| 64 KB blocks, 8 threads | **102.6** | **9.8** | **24.6** |

2.5× lower mean latency and 2.6× the throughput, at a cost of 2.3 points of compression
ratio. A service should set its block size from the payload sizes it actually receives,
not inherit the value that is optimal for files.

---

## How it works

```
input bytes
    │
    ├─ split into independent 1 MiB blocks ──────────► compressed in parallel
    │
    └─ per block:
         LZ77          sliding 32 KiB window, hash chains over 3-byte prefixes
           │           emits literals and (distance, length) back-references
           ▼
         two alphabets  literals + match lengths share one; distances get their own
           │            lengths and distances become range codes + raw extra bits
           ▼
         Huffman        canonical codes; the file carries code lengths, not tree shape
           │
           ▼
         bit stream     MSB-first, and a STORED fallback if none of that helped
```

**LZ77** replaces repetition with back-references. The hard part is match finding: at every
position, which of the previous 32 KiB gives the longest match? Two arrays make it
tractable — `head[hash]` holds the most recent position whose next three bytes hash to that
value, and `prev[position]` chains back through older ones. Walking that list gives
candidates newest-first, which matters because nearer matches encode to smaller distances.

**Huffman** then removes what LZ77 leaves. Without it every distance costs 16 bits whether
it is 4 or 30,000, every literal costs 8 whether it is a space or a tilde. Sharing one
alphabet between literals and match lengths is what removes the flag bits entirely: which
kind of thing a symbol is becomes implicit in the symbol, and the tree learns each file's
literal-to-match ratio for free.

The three stages measured separately, on this project's own source:

| | Reduction |
|---|---|
| Huffman alone (order-0) | 36.60% |
| LZ77 alone | 62.33% |
| **LZ77 + Huffman** | **71.33%** |
| gzip -6 | 71.54% |

All three remain reachable through `--huffman` / `--lz77` / `--deflate`, because the
comparison between them is half the point.

---

## Design decisions

Each of these was a real fork with a real cost. The full reasoning and the numbers behind
them are in **[NOTES.md](NOTES.md)**, the running measurement log kept phase by phase.

**Canonical Huffman.** The file carries one code length per symbol, not a tree shape; both
sides derive identical codes from the lengths alone. Smaller header, less code, and it is
what DEFLATE does — which makes the eventual RFC 1951 compatibility goal easier.

**Code lengths capped at 15 bits, with a Kraft-inequality repair pass.** Huffman puts no
bound on code length, and Fibonacci-distributed frequencies produce codes past 32 bits.
The cap never fires on real data but it bounds the code width for a future table decoder,
and there is a test that constructs exactly the input that triggers it.

**The uncompressed size in the header instead of an end-of-block symbol.** Costs 8 bytes,
buys exact preallocation and makes the final byte's zero padding unambiguous. DEFLATE
chooses the other way because it targets streams of unknown length; here it is always
known.

**RFC 1951's exact length and distance code tables.** A distance can be anything from 1 to
32768, so a symbol per distance would need a 32768-symbol alphabet whose code lengths alone
would dwarf the file. Range codes with raw extra bits reduce that to 30 symbols. Using
DEFLATE's own tables rather than inventing a scheme makes the gzip comparison about
implementation rather than format.

**Maximum match distance is windowSize − 1, not windowSize.** A candidate exactly one
window back shares a ring-buffer slot with the current position, whose own insertion has
already overwritten that slot's chain link. Following it walks a corrupted chain. Costs one
byte of reach; removes a whole class of aliasing bug. There is a test that sweeps distances
right up to that edge.

**Greedy parsing with a one-byte lazy lookahead, not optimal parsing.** Optimal parsing is
a shortest-path search over the token graph; real DEFLATE implementations decline it too.
A visible consequence: token count is not quite monotonic in search depth, because the
longest match now can leave the encoder at a worse position later.

**The "is this worth compressing?" question is answered by arithmetic, not by trying.** The
exact output size follows from the code lengths and the frequencies, so incompressible
input skips the encode entirely instead of encoding and discarding.

**A hand-written thread pool, not OpenMP.** `#pragma omp parallel for` would do it in one
line, and that is precisely the objection — the pragma would be doing the work and there
would be nothing to explain.

**No work stealing.** Every block is the same size and costs roughly the same, so there is
no skew for stealing to correct. It would add per-worker deques and a steal protocol to fix
a problem this workload does not have, and none of the four measured causes of the speedup
flattening is one stealing would touch.

**Decompression stays single-threaded.** The format permits parallel decode — blocks are
independent and their boundaries are walkable — but one decoder implementation means one
roundtrip test and no second correctness surface. Decompression already runs 4–5× faster
than compression, so it is not the bottleneck worth attacking.

**MSB-first bit packing, where DEFLATE is LSB-first for integers.** A hex dump then reads
left-to-right in write order, which makes a misaligned stream debuggable. Switching is a
rewrite of two classes and nothing else, which is exactly why they are isolated.

### Two things that went wrong, and what they taught

**A test fixture that was too repetitive made the tuning knobs measure as inert.** Cycling
a fixed paragraph produces matches that immediately hit the 258-byte ceiling, at which
point the match finder stops early — so search depth and lazy matching appeared to have no
effect at all. Real source code produces 5–20 byte matches, which is where those parameters
actually operate.

**Lazy matching emits *more* tokens while producing *fewer* bytes.** It trades one match
for a literal plus a longer match; a match serialises to 3 bytes and a literal to 1, so the
count rises while the size falls. Asserting on token count failed against a completely
correct encoder. Measure the quantity you actually care about.

---

## Correctness

**67 tests**, no framework — a ~50-line harness, because a dependency would be odd in a
project whose premise is implementing things yourself.

```bash
./build/tests.exe          # all of them
./build/tests.exe Lz77     # substring filter
```

Byte-identical roundtrip is the gate for every phase. What is covered beyond the obvious:

- **Both DEFLATE code tables exhaustively** — all 258 lengths and all 32,768 distances,
  checked for range, monotonicity, extra-bit width and exact reversibility. A typo in a
  transcribed constant table corrupts one narrow band of values that a roundtrip test
  catches only by luck.
- **Canonical-code and prefix-free properties verified independently of the encoder**, so
  a shared misunderstanding cannot pass.
- **Fibonacci frequencies**, to force the code-length limiter and produce an incomplete
  code with genuinely dangling decode-tree edges.
- **Every size from 0 to 200 bytes**, since bit-writer flush and flag-byte grouping bugs
  appear only at particular lengths.
- **Distances swept to the window edge**, where ring-buffer aliasing would show.
- **Streaming output byte-identical to in-memory** across sizes, block sizes, thread counts
  and all three algorithms, and all four encoder/decoder path combinations.
- **Thread count never changing the output**, at 2, 3, 4, 8 and "one per core".
- **The thread pool**: every index run exactly once, tasks genuinely overlapping, an
  exception propagating and leaving the pool reusable.
- **Corrupt input at every layer** — bad magic, wrong version, unknown method, illegal code
  lengths, back-references before the output start, blocks claiming to nest, and headers
  claiming implausible sizes (which must throw rather than attempt the allocation).

---

## What I would do next

In the order I would actually do them, by value per hour:

1. **Table-driven Huffman decoding.** A 9-bit lookup table decoding a whole symbol per step
   instead of a trie walk per bit. This is the single largest remaining speed win and the
   one gzip most clearly has.
2. **Compress the code length tables.** They are stored one byte per symbol — 318 bytes,
   1.2% of the output on small files. RFC 1951 runs a run-length encoder and a third
   Huffman code over them. Measured, that omission accounts for the entire ratio gap on
   small files.
3. **A two-byte-at-a-time match comparison loop.** The other half of gzip's speed advantage.
4. **Adaptive block boundaries**, splitting where the symbol statistics actually shift
   rather than at fixed offsets — which is what gzip's remaining ratio advantage on
   heterogeneous archives comes from.
5. **RFC 1951 bit-compatibility**, so `gzip -d` can read the output. Canonical coding, the
   code tables and the alphabet numbering already match; what remains is LSB-first bit
   packing and the block header format.

---

## Project layout

```
src/
  bitio.h         bit-level read/write, MSB-first, inline on the hot path
  huffman.*       canonical codes: frequencies → lengths → codes → decode trie
  lz77.*          sliding window, hash-chain match finder, token stream
  deflate.*       the two alphabets and RFC 1951's length/distance code tables
  block.*         encode/decode one independent block
  codec.*         container framing, blocking, streaming
  threadpool.*    fixed-size pool with a shared work queue
  capi.*          flat C ABI for the shared library
  format.h        the format specification, written out as a comment block
tools/
  cli.cpp         the command-line front end
  sweep.cpp       parameter sweeps: window, search depth, block size, threads
  benchmark.py    the gzip comparison across the corpus
  make_corpus.py  generates the structured-text corpus files
  plot.py         generates the two figures above
service/
  cmpr.py         ctypes binding to the shared library
  server.py       the HTTP service
  loadtest.py     concurrent load generator with latency percentiles
tests/            67 tests and a ~50-line harness
docs/             generated benchmark tables and figures
NOTES.md          the measurement log, phase by phase
```

`format.h` is the contract, not just constants — the byte layout of every method is
documented there, and that comment block is the specification both the encoder and the
decoder implement. It is the right file to read first.
