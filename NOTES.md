# Measurement log

Running notes and numbers, phase by phase. Raw material for the Phase 8 writeup — the
point is to record results when they are measured, not reconstruct them later.

Machine: Windows 11, MinGW-w64 g++ (WinLibs UCRT), `-O2`, single-threaded.
gzip baseline: `gzip -6` (its default level).

---

## Phase 1 — Huffman only

Order-0 Huffman: one code per byte, one tree for the whole file, no LZ77 yet.

| File | Size | Mine | Mine % | gzip -6 % | Compress MB/s | Decompress MB/s |
|---|---|---|---|---|---|---|
| Source text (2.0 MB) | 2,023,760 | 1,275,363 | 37.0% | 72.7% | 177 | 53 |
| Windows .exe (142 KB) | 141,655 | 98,250 | 30.6% | 66.7% | 123 | 57 |
| Random bytes (3.0 MB) | 3,000,000 | 3,000,014 | −0.0% (stored) | −0.0% | 1098 | 5602 |

### What the numbers say

**The 37% vs 72.7% gap on text is the whole argument for Phase 2.** Huffman is an
order-0 coder: it exploits how often each byte occurs and nothing else. It cannot see
that `std::size_t` appeared 300 times — to it that is just some e's and t's with good
odds. English text has roughly 4.2 bits of order-0 entropy per character, and 37% of 8
bits is 5.0 bits, so this implementation is within spitting distance of the theoretical
limit *for this class of algorithm*. The remaining 35 points are repetition, and only
LZ77 can reach them. Expect this gap to close in Phase 3, not Phase 2.

**Decompression is 3x slower than compression** (53 vs 177 MB/s). Compression looks up a
code per byte in a flat array; decompression walks a binary trie one bit at a time, which
is a dependent pointer chase per bit — about 5 cache-accesses per output byte, none of
them predictable. The standard fix is a lookup table keyed on the next N bits, decoding a
whole symbol per step. Deliberately not done yet: the format has to be proven correct
before it is worth making fast, and the before/after measurement is more interesting than
the after alone.

**On random data the fallback beats gzip** (+14 bytes vs +489). Not a real win — gzip
frames its output in DEFLATE blocks and pays a few hundred bytes for the framing, while
the STORED path here is a 14-byte header and a memcpy. Worth knowing, not worth bragging
about.

### Decisions made in this phase

- **Canonical Huffman.** The file carries one code length per symbol, not a tree shape;
  both sides derive identical codes from the lengths. Smaller header, less code, and it
  is what DEFLATE does, so the later RFC 1951 compatibility goal gets easier.
- **Sparse symbol table** — (symbol, length) pairs rather than a flat 256-byte array.
  Wins on small files, loses ~256 bytes on files using the whole alphabet, and small
  files are where header overhead actually shows up in the ratio.
- **Uncompressed size in the header instead of an end-of-block symbol.** Costs 8 bytes
  per file, buys exact preallocation and makes the final byte's zero padding
  unambiguous. DEFLATE chooses the other way because it targets streams of unknown
  length; here the length is always known.
- **Code lengths capped at 15 bits** with a Kraft-inequality repair pass. Never triggers
  on real data — reaching depth 15 needs roughly Fibonacci-distributed frequencies — but
  it bounds the code width for the future lookup-table decoder and is directly tested.
- **MSB-first bit packing.** A hex dump then reads left-to-right in write order, which
  makes a misaligned stream debuggable. Real DEFLATE is LSB-first for integers; switching
  is a rewrite of `BitWriter`/`BitReader` and nothing else, which is why they are isolated.
- **STORED fallback decided by arithmetic, not by trying.** The exact compressed size is a
  function of the frequencies and code lengths, so incompressible input skips the encode
  entirely rather than encoding and discarding.

### Correctness

29 tests, all passing: bit-I/O roundtrips at random widths, canonical-code and
prefix-free properties checked independently of the encoder, Fibonacci frequencies to
force the length limiter, and full roundtrips over empty files, single bytes, all 256
byte values, all-identical bytes, every size from 0 to 200 bytes, and corrupt/truncated
input. Plus byte-identical file roundtrips through disk on the files in the table above.

---

## Phase 2 — LZ77

Sliding window with hash-chain match finding, byte-aligned LZSS framing (one flag bit per
token, 1 byte per literal, 3 per match). No Huffman on top yet — that is Phase 3.

Corpus is small and local (project source, a Windows binary, a system DLL, random bytes).
The Silesia corpus arrives in Phase 6; these are for tuning, not for the final table.

| File | Size | Huffman | LZ77 | gzip -6 |
|---|---|---|---|---|
| Project source | 92,330 | 36.6% | **62.3%** | 71.5% |
| compressor.exe | 141,655 | 30.6% | **56.4%** | 66.7% |
| shell32.dll | 7,947,424 | 19.9% | **42.2%** | 54.2% |
| Random bytes | 3,000,000 | stored | stored | −0.02% |

Throughput, compress / decompress, MB/s:

| File | Huffman | LZ77 | gzip -6 (compress) |
|---|---|---|---|
| Project source | 170 / 50 | 21 / 385 | ~13 |
| shell32.dll | 169 / 52 | 17 / 355 | ~20 |

Caveat on the gzip column: it is measured by timing the `gzip` process and subtracting
measured startup (~39 ms), so it includes file I/O where my numbers are in-memory. Close
enough to say the two are in the same range, not close enough to claim a winner. Phase 6
needs a fairer harness.

### What the numbers say

**LZ77 roughly doubles what Huffman achieves, and it is still 9-12 points behind gzip.**
That residual gap is the entire content of Phase 3: gzip is LZ77 *and* Huffman, and right
now the token stream is written out in raw bytes. Every distance spends 16 bits whether it
is 4 or 30,000, every literal spends 8 whether it is a space or a `~`, and one flag bit
per token is paid regardless. Huffman-coding those streams should close most of the gap.

**Compression got ~8x slower and decompression ~7x faster.** Both are structural, and they
are the same fact seen from two directions: LZ77 is asymmetric by construction. The
encoder searches — walking hash chains, comparing candidate strings, at the default 128
candidates per position. The decoder just copies bytes it is told to copy, with no search
at all. Asymmetry like this is why compressed formats are worth it: you pay once to
compress and save every time you decompress.

**Compression throughput is already in gzip's range** (17-25 MB/s against gzip's ~13-20).
Do not read that as beating gzip. gzip at level 6 is doing strictly more work per byte,
because it is also building and applying Huffman trees, and its numbers here include file
I/O. The honest claim is "same order of magnitude", and the interesting version of it is
that decades of optimised C is not automatically faster than a straightforward
implementation of the same data structure — the algorithm dominates.

### What each knob does (project source, one knob moved at a time)

Full sweep in `tools/sweep.cpp`; the shape of each curve is what matters.

**Window size** — the strongest single lever, and it costs speed monotonically:

| Window | Reduction | Compress MB/s |
|---|---|---|
| 256 B | 22.9% | 50.5 |
| 1 KiB | 39.1% | 52.7 |
| 4 KiB | 50.7% | 44.8 |
| 32 KiB (default) | 62.3% | 21.2 |
| 64 KiB | 63.9% | 15.7 |

Doubling the window keeps paying, but with sharply diminishing returns past 32 KiB: the
last doubling buys 1.6 points and costs a quarter of the throughput. That is why DEFLATE
settled on 32 KiB, and measuring it independently is more convincing than citing it.

**Search depth (chain length)** — the classic ratio/speed dial, and the returns die fast:

| Chain | Reduction | Compress MB/s |
|---|---|---|
| 1 | 48.7% | 102.2 |
| 4 | 56.3% | 51.0 |
| 16 | 60.4% | 30.3 |
| 128 (default) | 62.3% | 21.2 |
| 1024 | 62.6% | 14.3 |
| 4096 | 62.6% | 14.5 |

Going from 1 to 16 buys 12 points for 3x the time. Going from 128 to 4096 buys 0.25 of a
point for another 30%. This is what gzip's -1..-9 levels are actually adjusting, and the
curve explains why -9 is so rarely worth it.

**Minimum match length** — 3 and 4 are a wash, then it degrades. Raising it makes each
match longer on average (9.3 to 15.7 bytes) but leaves more bytes uncovered (94.8% down to
69.2%), and the literals cost more than the saved match overhead.

**Lazy matching** — 60.6% to 62.3% for roughly half the throughput. Worth it here.

An honest note on lazy matching, because it broke a test and the reason is the
interesting part: it emits *more* tokens than greedy parsing, not fewer. It trades one
match for a literal plus a longer match, and since a match serialises to 3 bytes and a
literal to 1, the byte count falls while the token count rises. Asserting on token count
failed against a correct encoder. Measure the quantity you actually care about.

### Decisions made in this phase

- **Hash chains (`head[]` + `prev[]`) over a single-slot hash table.** The single slot
  finds only the most recent candidate; chains make search depth a tunable, which is what
  produced the curve above.
- **Hash over 3 bytes regardless of minMatch.** Three is the shortest useful match. A hash
  hit is only ever a candidate — every match is confirmed byte by byte — so a collision
  costs time and never correctness.
- **Check `data[candidate + bestLength]` before comparing.** One byte rejects any
  candidate that cannot beat the current best. On repetitive input most candidates die
  there, and it is the difference between a usable match finder and a slow one.
- **Maximum distance is windowSize − 1, not windowSize.** A candidate exactly one window
  back shares a ring-buffer slot with the current position, whose own insertion has
  already overwritten that slot's chain link. Following it walks a corrupted chain. Costs
  one byte of reach and removes a class of aliasing bug.
- **Greedy parsing with a one-byte lazy lookahead, not optimal parsing.** Optimal parsing
  is a shortest-path search over the token graph. Real DEFLATE implementations decline it
  too. A visible consequence: token count is not quite monotonic in search depth, because
  the longest match now can leave the encoder at a worse position.
- **Byte-aligned LZSS framing, deliberately temporary.** Phase 3 deletes it — once the
  streams are Huffman-coded, literal-versus-match becomes implicit in the alphabet and the
  flag bits disappear. Framing in whole bytes here keeps Phase 2 independently verifiable
  and makes Phase 3's improvement directly measurable against it.

### Correctness

42 tests. New in this phase: token-stream invariants checked independently of the decoder
(every distance inside the window, every length within bounds, no back-reference before
the output start), overlapping matches (distance 1 as run-length encoding), a distance
sweep right up to the window edge where ring-buffer aliasing would show, roundtrips across
the full config grid (4 window sizes x 3 min-match values x 3 chain depths x lazy on/off,
on both text and binary), and corrupt LZ77 streams — bad window bits, bad minimum match,
truncation.

---

## Phase 3 — DEFLATE-style (LZ77 + Huffman)

The token stream from Phase 2, Huffman-coded over two alphabets using RFC 1951's length
and distance code tables. Structurally what gzip does.

| File | Size | Huffman | LZ77 | **DEFLATE** | gzip -6 | Gap |
|---|---|---|---|---|---|---|
| Project source | 92,330 | 36.6% | 62.3% | **71.33%** | 71.54% | 0.21 |
| compressor.exe | 141,655 | 30.6% | 56.4% | **66.17%** | 66.67% | 0.50 |
| shell32.dll | 7,947,424 | 19.9% | 42.2% | **53.52%** | 54.19% | 0.67 |
| Random bytes | 3,000,000 | stored | stored | stored | −0.02% | — |

Throughput: 17–23 MB/s compressing, 88–127 MB/s decompressing.

### What the numbers say

**Within 0.7 points of gzip on every file, and the remaining gap is now fully accounted
for.** Two separate causes, measured rather than guessed:

*Cause 1 — the code length tables are stored raw.* HLIT/HDIST trim trailing unused
symbols but the lengths themselves are one byte per symbol: 318 bytes on the source file,
1.20% of the output. RFC 1951 runs a run-length encoder and then a *third* Huffman code
over those lengths, which typically gets them to 80–120 bytes. On the source file gzip is
ahead by 190 bytes and my table overhead is 318 — so that one omission explains the entire
gap on small files. It matters less as files grow (0.01% on the DLL).

*Cause 2 — one tree for the whole file.* gzip emits multiple blocks, each with its own
Huffman trees, so the codes track content that changes as the file goes on. I build one
global tree. Tested directly by compressing shell32.dll in independent blocks:

| Block size | Blocks | Result | vs whole file |
|---|---|---|---|
| 64 KiB | 122 | 52.75% | −0.77 |
| 256 KiB | 31 | 53.79% | +0.27 |
| 1 MiB | 8 | 53.86% | +0.34 |
| whole file | 1 | 53.52% | — |
| gzip -6 | — | 54.19% | +0.67 |

**Splitting into blocks makes compression better, not worse — up to a point.** This
contradicts what I expected going into Phase 4, and the shape of the curve says why. Two
forces pull in opposite directions:

- Every block pays for its own code length tables (~320 bytes here). At 64 KiB that is 39
  KB of pure overhead across 122 blocks, and the ratio gets *worse* by 0.77 points.
- But a single tree averaged over a whole 7.9 MB DLL fits none of it well. Executable
  code, resource tables and string blobs have completely different byte distributions.
  Per-block trees adapt; one global tree compromises.

Past roughly 256 KiB the second effect wins, and 1 MiB blocks recover half the distance to
gzip. Matches lost at block boundaries are real but tiny at that size — one boundary per
million bytes.

This reframes Phase 4 and 5. Chunking was going in as a memory-bound necessity with an
expected ratio cost; it turns out to be a ratio *improvement* at the right block size,
which also happens to be the thing that makes Phase 5's parallelism possible. Worth
sweeping block size properly in Phase 4 rather than assuming 64 KiB.

**Decompression dropped from 385 MB/s to 127.** Phase 2's decoder read whole bytes and
copied; this one walks a Huffman trie one bit at a time for every symbol, which is a
dependent pointer chase with no prefetching possible. Still faster than the Phase 1 decoder
(48 MB/s) because matches expand many output bytes per symbol decoded. The lookup-table
decoder is now clearly the highest-value optimisation available, and it is deliberately
still on the shelf so the before/after can be measured.

**Compression speed barely moved** (18.9 → 18.1 MB/s on source). Match finding dominates
and always did; adding two Huffman passes over the token stream is noise against walking
hash chains.

### What each knob does now

Same sweep, run on the combined codec. The curves keep their shape but compress:

| Window | Reduction | | Chain | Reduction | Comp MB/s |
|---|---|---|---|---|---|
| 256 B | 53.93% | | 1 | 65.14% | 57.7 |
| 1 KiB | 61.57% | | 4 | 68.70% | 53.2 |
| 4 KiB | 66.52% | | 16 | 70.54% | 27.5 |
| 32 KiB | 71.33% | | 128 | 71.33% | 19.8 |
| | | | 4096 | 71.42% | 11.8 |

Note how much the Huffman stage flattens the search-depth curve. In Phase 2 going from
chain 1 to chain 128 bought 13.6 points; here it buys 6.2. Entropy coding recovers a good
part of what a lazier search gives up, because the shorter matches it settles for are
themselves more predictable and get shorter codes. That makes fast settings more
attractive than the Phase 2 numbers suggested.

### Decisions made in this phase

- **RFC 1951's exact length and distance code tables**, not a scheme of my own. The
  comparison against gzip becomes about implementation rather than format, and the later
  bit-compatibility goal needs them regardless.
- **Literals and lengths share one alphabet.** This is what deletes Phase 2's flag bits:
  which kind of thing a symbol is has become implicit in the symbol, and the tree learns
  this file's literal-to-match ratio for free.
- **Symbol 256 (end-of-block) reserved but never emitted.** The container header carries
  the uncompressed size. Keeping the slot costs nothing — an unused symbol gets no code —
  and keeps the numbering identical to RFC 1951.
- **Extra bits written raw, not entropy coded.** Within a length or distance range the
  values are close to uniform, so there is nothing for a Huffman code to exploit.
- **minMatch pinned to 3 and windowBits to 15**, because the code tables fix the ranges at
  [3, 258] and [1, 32768]. Justified by the Phase 2 sweep, where minMatch 3 and 4 were
  within 0.02% and a 64 KiB window bought 1.6 points for a quarter of the throughput.
- **Code lengths stored one byte per symbol**, knowing it costs ~1.2% on small files. It
  is measured and quantified above rather than assumed away; encoding them properly is the
  single highest-ratio improvement left, and it belongs after Phase 4 has settled the
  block structure it would be applied to.

### Correctness

53 tests. New: exhaustive verification of both code tables — all 258 lengths and all
32,768 distances checked for range, monotonicity, extra-bit width, and exact
reversibility, because a transcription typo in a constant table corrupts one narrow range
of values that a roundtrip test would only catch by luck. Plus matches at the table
extremes through the full encoder, 200 KB runs, config rejection for settings the tables
cannot express, and corrupt HLIT/HDIST/window fields.

---

## Phase 4 — Blocks and streaming

The input is cut into independent blocks, each compressed with no reference to any other:
no shared history, no shared Huffman trees. That is what makes bounded-memory streaming
and (Phase 5) parallel compression possible.

Container method 4, nine bytes of framing per block. Each block carries its own lengths
rather than relying on a block size recorded once — four extra bytes, in exchange for a
decoder that never needs to know the encoder's block size and can walk block boundaries
without decoding anything.

### Block size is a real optimum, and 64 KiB would have been the worst choice

Total compressed size over seven corpus files (140.5 MB of Silesia plus samba):

| Block size | Total output | Reduction |
|---|---|---|
| 64 KiB | 44,133,751 | 68.58% |
| unblocked | 42,970,414 | 69.41% |
| 256 KiB | 42,595,887 | 69.68% |
| 4 MiB | 42,452,843 | 69.78% |
| **1 MiB** | **42,393,087** | **69.82%** |

Two forces pull against each other. Every block pays for its own Huffman code length
tables — around 320 bytes — so at 64 KiB that overhead alone is 0.5% of the output. But
one set of trees stretched over a whole file fits none of it well. 1 MiB is where the
curve turns, and it is the default.

The effect is strongly content-dependent, which the aggregate hides:

| File | What it is | Unblocked | 256 KiB | 1 MiB | Best |
|---|---|---|---|---|---|
| samba | source tarball, many file types | 73.14% | 74.31% | 74.23% | 256 KiB |
| mozilla | binary tarball | 61.28% | 62.16% | 62.06% | 256 KiB |
| dickens | one plain-text book | 62.05% | 61.42% | 61.91% | unblocked |
| nci | uniform chemical database | 90.68% | 90.39% | 90.61% | unblocked |

Heterogeneous archives want small blocks so the trees can track content that changes;
homogeneous files want one tree fitted to the whole thing. A tarball of source code gains
1.2 points from blocking; a novel loses 0.6. Same knob, opposite signs, and the reason is
entirely about what is in the file.

### Memory is bounded by block size, not file size

Peak working set, compressing `mozilla` (51 MB):

| Path | Peak memory | Throughput | Output |
|---|---|---|---|
| in-memory, unblocked | 151.0 MiB | 19.7 MB/s | 19,834,809 |
| in-memory, 1 MiB blocks | 90.6 MiB | 21.5 MB/s | 19,431,440 |
| streaming, 1 thread | **14.1 MiB** | 22.4 MB/s | 19,431,440 |
| streaming, 8 threads | 41.8 MiB | 83.7 MB/s | 19,431,440 |
| streaming decompress | 7.0 MiB | 104.9 MB/s | — |

Then the same binary on a 636 MB file — 12x larger:

| | Peak memory | Throughput |
|---|---|---|
| streaming compress, 8 threads | 44.1 MiB | 70.0 MB/s |
| streaming decompress | 8.1 MiB | 114.3 MB/s |

44.1 MiB against 41.8 MiB for a file twelve times smaller. Memory is a function of
(block size x threads), not of input size, which is the whole claim. The in-memory path
peaks at roughly 3x the file size — input, plus output, plus the token vector — so it is
the one that cannot survive a file larger than RAM.

**All four paths produce byte-identical output.** In-memory and streaming, one thread and
eight. Asserted by a test, not just observed here: two code paths meant to agree drift
apart unless something checks.

### Decisions

- **Per-block lengths rather than an index at the front.** An index would need either two
  passes or a seekable output to patch afterwards. Per-block framing streams in one pass
  in both directions and still lets a reader find block boundaries cheaply.
- **Blocking only above the block size.** A file that fits in one block is written as a
  plain single-block file, so small files pay no framing at all.
- **Blocks do not nest.** A block claiming method 4 is rejected, which closes off a
  recursive-expansion attack on the decoder.
- **The streaming and in-memory paths converge for small inputs**: below one block, the
  streaming function reads the input and calls the in-memory one. Two implementations of
  the same framing is one too many.

---

## Phase 5 — Parallel block compression

A fixed-size thread pool with a shared work queue, hand-written: `std::thread`,
`std::mutex`, `std::condition_variable`, about 80 lines. Blocks are handed out by index,
compressed concurrently, and assembled strictly in order.

### Speedup

`samba` (21.6 MB), in-memory, 1 MiB blocks. Machine: Intel i5-10300H, **4 physical cores,
8 logical**.

| Threads | MB/s | Speedup |
|---|---|---|
| 1 | 27.4 | 1.00x |
| 2 | 48.5 | 1.77x |
| 3 | 68.2 | 2.49x |
| 4 | 71.9 | 2.62x |
| 6 | 96.5 | 3.52x |
| 8 | 101.8 | **3.71x** |
| 12 | 87.3 | 3.19x |
| 16 | 78.6 | 2.87x |

Streaming `mozilla` shows the same shape: 22.4 MB/s at one thread, 83.7 at eight, 3.74x.

### Why it flattens — four causes, in order of size

**1. There are only four real cores.** The jump from 4 threads (2.62x) to 8 (3.71x) is
hyperthreading, and 1.42x from the second thread on each core is about what SMT gives on
a workload like this — hash chain walking stalls on memory often enough that a second
thread has gaps to fill, but the two threads still share one set of execution units. Past
8 the curve turns over: 16 threads is slower than 8, because oversubscribing 8 logical
processors adds context switching and cache thrashing and buys nothing.

**2. Clock throttling.** The i5-10300H runs 2.5 GHz base and boosts far higher on one
active core than on four. A meaningful slice of the missing scaling at 4 threads is simply
that each core is running slower than the single-threaded baseline did. This is invisible
unless you go looking for it, and it is why "why isn't it 4x?" has no single answer.

**3. Amdahl's law on the parts that stay sequential.** Reading the input, allocating the
output, and concatenating the compressed blocks into it are all serial. Concatenation is a
memcpy of the entire compressed output — 5.5 MB for samba — while every core waits. At 62%
of theoretical 4-core scaling, Amdahl puts the serial fraction near 15%, which matches the
I/O-plus-assembly share.

**4. Memory bandwidth.** Match finding is a random walk over a 32 KiB window through hash
chains, four cores sharing one L3 and one memory controller. This is the least separable
of the four and the hardest to prove without hardware counters.

### Decisions

- **A hand-written pool, not OpenMP.** `#pragma omp parallel for` would do this in one
  line, and that is exactly the objection: the pragma would be doing the work and there
  would be nothing to explain.
- **No work stealing.** Every block is the same size and costs roughly the same to
  compress, so there is no skew for stealing to correct. It would add per-worker deques
  and a steal protocol to fix a problem this workload does not have, and none of the four
  causes above is one stealing would touch.
- **Thread count never changes the output.** Blocks are compressed in whatever order
  threads pick them up but assembled by index, so a 1-thread and an 8-thread run produce
  identical bytes. Without that, no benchmark comparing them would mean anything and any
  bug would reproduce only sometimes. There is a test.
- **A task that throws is still counted as finished.** Otherwise `forEach` waits forever
  for a task that is never coming back — a deadlock rather than an error. The first
  exception is captured and rethrown on the calling thread once every task has finished.
- **Decompression stays single-threaded.** The format permits parallel decode — blocks are
  independent and their boundaries are walkable — but one decoder implementation means one
  roundtrip test and no second correctness surface. Decompression already runs at
  105-145 MB/s, four to five times faster than compression, so it is not the bottleneck
  worth attacking.
- **The pool is constructed per call in the in-memory path**, which costs a few hundred
  microseconds of thread creation. Irrelevant against multi-second compressions, visible
  on small ones. The streaming path builds it once outside the loop.

### Correctness

67 tests. New: the pool runs every index exactly once at 1, 2, 4 and 8 workers; tasks
genuinely overlap; an exception propagates and leaves the pool reusable; blocked
roundtrips across eight block sizes including exact boundary multiples; per-block method
selection verified by walking the framing (a mixed file must contain both stored and
deflate blocks); streaming output byte-identical to in-memory across sizes, block sizes,
thread counts and all three algorithms; all four encoder/decoder path combinations; and
corrupt block framing — oversized lengths, nested blocks, truncation.
