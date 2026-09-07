#include "lz77.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace cmpr {
namespace lz77 {

void Config::validate() const {
  // 16 is the ceiling because a distance is stored in a uint16_t as (distance - 1).
  if (windowBits < 8 || windowBits > 16) {
    throw std::invalid_argument("windowBits must be between 8 and 16");
  }
  // Below 3 the hash covers more bytes than the match needs; above 8 there is no reason
  // to go and the ceiling keeps the length field honest.
  if (minMatch < 3 || minMatch > 8) {
    throw std::invalid_argument("minMatch must be between 3 and 8");
  }
  if (maxChainLength < 1) {
    throw std::invalid_argument("maxChainLength must be at least 1");
  }
}

namespace {

// The hash covers exactly three bytes regardless of minMatch. Three is the shortest
// useful match, so this is the finest granularity the table can offer; with a larger
// minMatch the extra bytes simply get verified during the comparison. A hash hit is only
// ever a *candidate* -- every match is confirmed byte by byte before it is used, so a
// collision costs time and never correctness.
constexpr int kHashBytes = 3;

struct Match {
  std::uint16_t distance = 0;
  std::uint16_t length = 0;
};

class MatchFinder {
 public:
  MatchFinder(const std::uint8_t* data, std::size_t size, const Config& config)
      : data_(data),
        size_(size),
        config_(config),
        windowSize_(config.windowSize()),
        windowMask_(config.windowSize() - 1),
        hashShift_(32 - config.windowBits),
        head_(config.windowSize(), kNone),
        prev_(config.windowSize(), kNone) {}

  // Makes every position up to and including `position` findable. Called before any
  // search from that position, and idempotent -- which is what lets the lazy-matching
  // path look ahead one byte without inserting anything twice and corrupting a chain
  // into a self-referencing loop.
  void insertUpTo(std::size_t position) {
    while (inserted_ <= position) {
      insertOne(inserted_);
      ++inserted_;
    }
  }

  Match findLongest(std::size_t position, Stats& stats) const {
    Match best;
    if (position + static_cast<std::size_t>(config_.minMatch) > size_) return best;

    const std::size_t remaining = size_ - position;
    const std::size_t limit =
        std::min(remaining, static_cast<std::size_t>(config_.maxMatch()));

    // insertUpTo() has already put `position` at the head of its own bucket, so the
    // walk starts one link in -- otherwise the first candidate would be the position
    // being searched from, at distance zero. Safe to index prev_ directly here:
    // findLongest returns above unless position + minMatch <= size, and minMatch is
    // never below kHashBytes, so this position was definitely hashed and inserted.
    std::size_t candidate = prev_[position & windowMask_];
    int chain = config_.maxChainLength;

    while (candidate != kNone && chain-- > 0) {
      // Chain entries live in a ring buffer, so an entry can be stale -- older than the
      // window and therefore unreachable. The distance check is what retires it, and
      // the bound is strict: a candidate exactly windowSize back shares a ring slot
      // with the current position, whose own insertion has already overwritten that
      // slot's prev_ link. Following it would walk a corrupted chain. Costs one byte
      // of reach (max distance 32767 rather than 32768) and removes a whole class of
      // aliasing bug.
      const std::size_t distance = position - candidate;
      if (distance >= windowSize_) break;

      ++stats.candidatesExamined;

      // Check the byte just past the current best first. If a candidate cannot beat the
      // best match already found, that single comparison rejects it, instead of a full
      // scan that was doomed from the first byte. On repetitive input most candidates
      // die here, and this is the difference between a usable match finder and a slow one.
      if (best.length == 0 || data_[candidate + best.length] == data_[position + best.length]) {
        const std::size_t length = matchLength(position, candidate, limit);
        if (length > best.length) {
          best.length = static_cast<std::uint16_t>(length);
          best.distance = static_cast<std::uint16_t>(distance);
          // Nothing can beat the ceiling, so stop rather than walk the rest of the chain.
          if (length >= limit) break;
        }
      }
      candidate = prev_[candidate & windowMask_];
    }

    if (best.length < static_cast<std::uint16_t>(config_.minMatch)) return Match{};
    return best;
  }

 private:
  static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

  std::uint32_t hashAt(std::size_t position) const {
    const std::uint32_t value = (static_cast<std::uint32_t>(data_[position]) << 16) |
                                (static_cast<std::uint32_t>(data_[position + 1]) << 8) |
                                static_cast<std::uint32_t>(data_[position + 2]);
    // Knuth multiplicative hash: multiply by a constant near 2^32/phi and keep the high
    // bits, which mixes all three input bytes into every output bit. Keeping the *high*
    // bits matters -- the low bits of a product are barely mixed at all.
    return (value * 2654435761u) >> hashShift_;
  }

  void insertOne(std::size_t position) {
    if (position + kHashBytes > size_) return;  // no room to hash near the end
    const std::uint32_t hash = hashAt(position);
    prev_[position & windowMask_] = head_[hash];
    head_[hash] = position;
  }

  std::size_t matchLength(std::size_t a, std::size_t b, std::size_t limit) const {
    std::size_t length = 0;
    while (length < limit && data_[a + length] == data_[b + length]) ++length;
    return length;
  }

  const std::uint8_t* data_;
  std::size_t size_;
  Config config_;
  std::size_t windowSize_;
  std::size_t windowMask_;
  int hashShift_;
  std::vector<std::size_t> head_;
  std::vector<std::size_t> prev_;
  std::size_t inserted_ = 0;
};

}  // namespace

std::vector<Token> tokenize(const std::uint8_t* data, std::size_t size, const Config& config,
                            Stats* statsOut) {
  config.validate();

  Stats stats;
  std::vector<Token> tokens;
  if (size == 0) {
    if (statsOut != nullptr) *statsOut = stats;
    return tokens;
  }
  // Worst case is one token per byte; anything better is a bonus. Reserving a fraction of
  // that avoids most of the reallocation without assuming the input compresses.
  tokens.reserve(size / 2 + 16);

  MatchFinder finder(data, size, config);

  const auto emitLiteral = [&](std::size_t position) {
    Token token;
    token.literal = data[position];
    tokens.push_back(token);
    ++stats.literals;
  };

  std::size_t position = 0;
  while (position < size) {
    finder.insertUpTo(position);
    Match match = finder.findLongest(position, stats);

    if (match.length != 0 && config.lazyMatching && position + 1 < size) {
      // Lazy matching. A greedy coder takes the match at `position` immediately; but if
      // starting one byte later yields a strictly longer match, spending one byte on a
      // literal buys more than it costs. Classic local-optimum-versus-slightly-less-local
      // tradeoff, and it is why gzip's higher levels beat its lower ones on text.
      finder.insertUpTo(position + 1);
      const Match next = finder.findLongest(position + 1, stats);
      if (next.length > match.length) {
        emitLiteral(position);
        ++position;
        continue;
      }
    }

    if (match.length == 0) {
      emitLiteral(position);
      ++position;
      continue;
    }

    Token token;
    token.distance = match.distance;
    token.length = match.length;
    tokens.push_back(token);
    ++stats.matches;
    stats.matchedBytes += match.length;
    position += match.length;
  }

  if (statsOut != nullptr) *statsOut = stats;
  return tokens;
}

std::vector<std::uint8_t> detokenize(const std::vector<Token>& tokens, std::size_t expectedSize) {
  std::vector<std::uint8_t> out;
  out.reserve(expectedSize);

  for (const Token& token : tokens) {
    if (!token.isMatch()) {
      out.push_back(token.literal);
      continue;
    }
    if (token.distance > out.size()) {
      throw CorruptInput("back-reference points before the start of the output");
    }
    const std::size_t start = out.size() - token.distance;
    // Byte at a time, deliberately. When distance < length the source region overlaps the
    // destination, and each copied byte is read back as the source for a later one. That
    // is not a bug to guard against, it is a feature: "distance 1, length 200" is a
    // 200-byte run of one value, so run-length encoding falls out of LZ77 for free. A
    // memcpy would read the pre-copy contents and get this wrong.
    for (std::uint16_t i = 0; i < token.length; ++i) {
      out.push_back(out[start + i]);
    }
  }
  return out;
}

}  // namespace lz77
}  // namespace cmpr
