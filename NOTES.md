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
