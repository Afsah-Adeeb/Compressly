#pragma once
//
// LZ77 and Huffman combined -- structurally what gzip does.
//
// Phase 2 wrote the token stream in raw bytes, which wastes most of what LZ77 found: a
// distance of 4 and a distance of 30,000 both cost 16 bits, every literal costs 8 whether
// it is a space or a tilde, and every token pays a flag bit. Huffman-coding the token
// stream fixes all three at once.
//
// TWO ALPHABETS. Literals and match lengths share one alphabet; distances get their own.
//
//   literal/length   0-255  the literal byte
//                    256    end-of-block (reserved, see below)
//                    257-285  match length, via the code table
//   distance         0-29   match distance, via the code table
//
// Sharing one alphabet between literals and lengths is what removes the flag bits: which
// kind of thing a symbol is has become implicit in the symbol itself, and the Huffman
// tree learns the literal-versus-match ratio of this particular file for free. Distances
// need a separate alphabet because they are only meaningful after a length, and their
// distribution is completely different (heavily biased towards short distances).
//
// CODE TABLES WITH EXTRA BITS. A distance can be anything from 1 to 32768, so a symbol
// per distance would mean a 32768-symbol alphabet whose code lengths alone would dwarf
// the file. Instead each symbol covers a *range* whose width doubles as distances grow,
// and the position within the range is written as raw, uncoded "extra" bits. So distance
// 5 is symbol 4 plus one extra bit, and distance 20000 is symbol 28 plus 13 extra bits.
// This is a deliberate approximation to the real distribution: near distances are common
// and get precise, cheap codes; far distances are rare and get coarse ones.
//
// These are RFC 1951's exact tables rather than a scheme of my own. Two reasons: the
// comparison against gzip becomes about implementation rather than format, and the later
// bit-compatibility goal needs them anyway.
//
// Symbol 256 (end-of-block) is reserved and never emitted -- the container header carries
// the uncompressed size, so the decoder already knows when to stop. It costs nothing to
// keep the slot (an unused symbol gets no code at all) and keeps the numbering identical
// to RFC 1951.
//
#include <cstddef>
#include <cstdint>
#include <vector>

#include "format.h"
#include "lz77.h"

namespace cmpr {
namespace deflate {

constexpr int kLitLenAlphabetSize = 286;
constexpr int kDistAlphabetSize = 30;
constexpr int kEndOfBlockSymbol = 256;
constexpr int kFirstLengthSymbol = 257;

// Fixed by the code tables above, not by preference.
constexpr int kMinMatch = 3;
constexpr int kMaxMatch = 258;
constexpr int kMaxWindowBits = 15;  // distance codes reach exactly 32768

// A value split into the Huffman symbol that covers its range and the raw bits that
// locate it inside that range.
struct Split {
  int symbol = 0;
  int extraBits = 0;
  std::uint32_t extraValue = 0;
};

Split splitLength(int length);      // length in [3, 258]
Split splitDistance(int distance);  // distance in [1, 32768]

// Inverses, for the decoder: given the symbol, what range does it start at and how many
// bits locate a value within it.
int lengthBase(int symbol);       // symbol in [257, 285]
int lengthExtraBits(int symbol);
int distanceBase(int code);       // code in [0, 29]
int distanceExtraBits(int code);

// Builds the complete method-3 payload -- everything after the 14-byte container header.
//
// Returns an empty vector when the encoding would come to `budgetBytes` or more, meaning
// the caller should store the input raw instead. An empty payload is never otherwise
// valid for non-empty input, so it is an unambiguous signal.
//
// Throws std::invalid_argument if the config cannot be expressed: the length code table
// fixes the match range at [3, 258], and the distance code table stops at 32768.
std::vector<std::uint8_t> encode(const std::uint8_t* data, std::size_t size,
                                 const lz77::Config& config, std::size_t budgetBytes);

std::vector<std::uint8_t> decode(const std::uint8_t* payload, std::size_t payloadSize,
                                 std::uint64_t originalSize);

}  // namespace deflate
}  // namespace cmpr
