#pragma once
//
// The container format.
//
// This is a bespoke format, not RFC 1951. Matching DEFLATE bit-for-bit is a later goal;
// getting a correct, explainable format working first is this one. Everything a decoder
// needs is in the file -- there are no implicit parameters shared out of band.
//
//   offset  size  field
//   0       4     magic "CMPR"
//   4       1     format version
//   5       1     method (see Method below)
//   6       8     uncompressed size, little-endian uint64
//   14      ..    method-specific payload
//
// Storing the uncompressed size costs 8 bytes and buys three things: the decoder can
// allocate the output buffer exactly once, it knows when to stop without an
// end-of-stream symbol in the alphabet, and the zero padding in the final byte can never
// be mistaken for data. DEFLATE instead spends an alphabet slot on an end-of-block
// symbol, because it is designed for streams whose length is not known up front. Here it
// always is.
//
// METHOD 0 -- STORED: the payload is the input, verbatim.
// METHOD 1 -- HUFFMAN:
//   +0      1     (number of distinct symbols) - 1
//   +1      2n    n pairs of (symbol byte, code length byte), ascending by symbol
//   +1+2n   ..    the bit stream, MSB-first, zero-padded to a byte boundary
// METHOD 2 -- LZ77:
//   +0      1     window bits (log2 of the sliding window used by the encoder)
//   +1      1     minimum match length
//   +2      ..    LZSS-framed token stream (below)
//
// The LZ77 token stream is byte-aligned, in groups of eight tokens. Each group opens
// with one flag byte whose bits, most significant first, say whether the corresponding
// token is a match (1) or a literal (0). A literal is one byte; a match is a
// little-endian uint16 of (distance - 1) followed by one byte of (length - minMatch).
//
// Byte alignment is deliberate and temporary. Phase 3 replaces this framing entirely by
// Huffman-coding the token streams, at which point the flag bits disappear -- a literal
// and a length share one alphabet, so which of the two a symbol is becomes implicit.
// Framing it in whole bytes here keeps Phase 2 independently verifiable and makes the
// improvement Phase 3 delivers directly measurable against it.
//
// The symbol table is sparse -- pairs rather than a flat 256-byte array of lengths --
// because small files are the case where header overhead actually matters. A file of
// five distinct bytes carries an 11-byte table instead of 256.
//
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace cmpr {

constexpr std::uint8_t kMagic[4] = {'C', 'M', 'P', 'R'};
constexpr std::uint8_t kFormatVersion = 1;
constexpr std::size_t kHeaderSize = 14;

// Thrown whenever input violates the contract above: bad magic, a truncated payload,
// a bit pattern that is not a code in the tree. Declared here rather than beside the
// bit reader because it is part of what decompress() promises its callers.
class CorruptInput : public std::runtime_error {
 public:
  explicit CorruptInput(const std::string& what) : std::runtime_error(what) {}
};

enum class Method : std::uint8_t {
  kStored = 0,
  kHuffman = 1,
  kLz77 = 2,
  // Reserved for later phases: 3 = LZ77 + Huffman, 4 = parallel blocks.
};

}  // namespace cmpr
