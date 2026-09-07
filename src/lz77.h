#pragma once
//
// LZ77: replace repeated data with references to where it appeared before.
//
// Huffman (Phase 1) is an order-0 coder -- it only knows how often each byte occurs, so
// it cannot tell that a 40-character phrase appeared twice. LZ77 attacks exactly that
// blind spot. The input becomes a stream of tokens, each either a literal byte or a
// (distance, length) pair meaning "copy `length` bytes from `distance` bytes back".
//
// The whole difficulty is match finding: at every position, out of a 32 KiB history,
// which earlier position gives the longest match? Searching it exhaustively is quadratic.
// The standard answer, and the one used here, is a hash table over short prefixes that
// turns "where might a match be?" into a short list of candidates.
//
// SEARCH STRUCTURE. Two arrays, the same scheme zlib uses:
//   head[hash]  -- the most recent position whose next 3 bytes hash to `hash`
//   prev[pos]   -- the position before that one with the same hash
// Together they form a linked list per hash bucket, threaded through a ring buffer
// indexed by position modulo the window size. Walking that list gives candidates newest
// first, which matters because nearer matches encode into smaller distances.
//
// The knobs below are the ratio-versus-speed dial, and measuring what each one does is
// half the point of this phase. `maxChainLength` in particular is essentially what
// gzip's -1 through -9 levels control.
//
#include <cstddef>
#include <cstdint>
#include <vector>

#include "format.h"

namespace cmpr {
namespace lz77 {

struct Config {
  // log2 of the sliding window. 15 (32 KiB) matches DEFLATE. Bigger windows find more
  // matches but cost a larger distance field and more cache misses walking chains.
  int windowBits = 15;

  // Shorter matches than this are not worth encoding: a 2-byte match costs 3 bytes to
  // express, so emitting it would make the file bigger.
  int minMatch = 3;

  // How many candidates to examine per position before giving up. The single most
  // important speed knob: 1 is a greedy first-hit search, thousands is near-exhaustive.
  int maxChainLength = 128;

  // Before committing to a match, check whether the *next* position has a longer one.
  // If it does, emit a literal and take the better match instead. Cheap, and it
  // meaningfully improves ratio -- zlib turns this on from level 4 up.
  bool lazyMatching = true;

  // Derived, not configurable: the serialised length field is one byte holding
  // (length - minMatch), which caps a match at minMatch + 255. At the default minMatch
  // that is 258, the same ceiling DEFLATE uses -- not a coincidence, it comes from the
  // same reasoning about field widths.
  int maxMatch() const { return minMatch + 255; }
  std::size_t windowSize() const { return std::size_t{1} << windowBits; }

  // Throws std::invalid_argument if the settings cannot be encoded.
  void validate() const;
};

// A literal, or a back-reference. `distance == 0` marks a literal -- distance zero is
// otherwise meaningless (nothing is zero bytes back), so it costs no extra field.
struct Token {
  std::uint16_t distance = 0;
  std::uint16_t length = 0;
  std::uint8_t literal = 0;

  bool isMatch() const { return distance != 0; }
};

// Descriptive counters. Not needed to decode anything -- they exist because "why is the
// ratio what it is" is answered by the match distribution, not by the ratio.
struct Stats {
  std::uint64_t literals = 0;
  std::uint64_t matches = 0;
  std::uint64_t matchedBytes = 0;   // input bytes covered by matches
  std::uint64_t candidatesExamined = 0;  // total hash-chain steps: the real cost measure

  double averageMatchLength() const {
    return matches == 0 ? 0.0 : static_cast<double>(matchedBytes) / static_cast<double>(matches);
  }
};

std::vector<Token> tokenize(const std::uint8_t* data, std::size_t size, const Config& config,
                            Stats* stats = nullptr);

// Replays tokens back into bytes. `expectedSize` is used to size the output buffer and
// to bound how much a corrupt token stream can produce.
std::vector<std::uint8_t> detokenize(const std::vector<Token>& tokens, std::size_t expectedSize);

}  // namespace lz77
}  // namespace cmpr
