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
