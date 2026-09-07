//
// Tokeniser-level tests. The end-to-end roundtrip lives in test_roundtrip.cpp; what is
// checked here is that the token stream itself obeys the invariants the format relies on
// -- because a token stream can replay correctly while still being wrong in ways that
// only bite later (a distance outside the window survives detokenize() in memory and
// then fails to decode from a file).
//
#include <random>
#include <stdexcept>
#include <string>

#include "lz77.h"
#include "testing.h"

using cmpr::lz77::Config;
using cmpr::lz77::Stats;
using cmpr::lz77::Token;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes bytesOf(const std::string& text) { return Bytes(text.begin(), text.end()); }

std::vector<Token> tokenizeBytes(const Bytes& input, const Config& config, Stats* stats = nullptr) {
  return cmpr::lz77::tokenize(input.data(), input.size(), config, stats);
}

void checkTokenRoundtrip(const Bytes& input, const Config& config) {
  const std::vector<Token> tokens = tokenizeBytes(input, config);
  const Bytes restored = cmpr::lz77::detokenize(tokens, input.size());
  CHECK_EQ(restored.size(), input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (restored[i] != input[i]) {
      ::testing::fail(__FILE__, __LINE__, "byte " + std::to_string(i) + " differs");
    }
  }
}

// Everything downstream -- the serialised distance field, the window bound the decoder
// enforces, the length byte -- depends on these holding for every token.
void checkTokenInvariants(const std::vector<Token>& tokens, const Config& config) {
  std::size_t produced = 0;
  for (const Token& token : tokens) {
    if (!token.isMatch()) {
      ++produced;
      continue;
    }
    CHECK(token.length >= config.minMatch);
    CHECK(token.length <= config.maxMatch());
    CHECK(token.distance >= 1);
    CHECK(token.distance < config.windowSize());
    // A back-reference may never point before the data produced so far.
    CHECK(token.distance <= produced);
    produced += token.length;
  }
}

Bytes repetitiveText(std::size_t targetSize) {
  std::string text;
  while (text.size() < targetSize) {
    text += "the quick brown fox jumps over the lazy dog; ";
    text += "compression works by finding repetition, and this sentence repeats. ";
  }
  text.resize(targetSize);
  return bytesOf(text);
}

// What a token stream costs once serialised, per the LZSS framing in format.h: one flag
// bit per token, one byte per literal, three per match.
//
// This, not the token count, is the quantity the tuning knobs are trying to minimise, and
// the two genuinely disagree. Lazy matching spends a literal to buy a longer match, which
// can *raise* the token count while lowering the byte count -- because a match costs three
// times what a literal does. Asserting on token count made this test fail against an
// encoder that was working correctly.
std::size_t serialisedSize(const std::vector<Token>& tokens) {
  std::size_t literals = 0;
  std::size_t matches = 0;
  for (const Token& token : tokens) {
    if (token.isMatch()) {
      ++matches;
    } else {
      ++literals;
    }
  }
  return (tokens.size() + 7) / 8 + literals + 3 * matches;
}

// Text with *realistic* repetition, as opposed to the block-repeated kind above.
//
// This distinction turned out to matter. Cycling a fixed paragraph produces matches that
// immediately hit the 258-byte ceiling, and once a match is at the ceiling the match
// finder stops early -- so the search-depth and lazy-matching knobs have nothing left to
// do and measure as having no effect. Drawing words from a vocabulary gives matches in
// the 5-20 byte range, which is where those knobs actually operate, and matches what the
// sweep measures on real source code.
Bytes wordSaladText(std::size_t targetSize, unsigned seed) {
  static const char* const vocabulary[] = {
      "const",   "std",      "size_t",  "return",   "if",       "else",    "for",
      "while",   "vector",   "uint8_t", "length",   "distance", "token",   "match",
      "position", "compress", "buffer",  "window",   "hash",     "chain",   "literal",
      "stream",  "config",   "static",  "inline",   "throw",    "encoder", "decoder"};
  constexpr std::size_t kVocabularySize = sizeof(vocabulary) / sizeof(vocabulary[0]);

  std::mt19937 rng(seed);
  std::uniform_int_distribution<std::size_t> wordPick(0, kVocabularySize - 1);
  std::uniform_int_distribution<int> punctuation(0, 9);

  std::string text;
  while (text.size() < targetSize) {
    text += vocabulary[wordPick(rng)];
    const int mark = punctuation(rng);
    text += (mark < 6) ? " " : (mark < 8 ? ", " : ";\n  ");
  }
  text.resize(targetSize);
  return bytesOf(text);
}

}  // namespace

TEST(Lz77EmptyAndTinyInputs) {
  const Config config;
  CHECK(tokenizeBytes({}, config).empty());
  // Anything shorter than minMatch cannot contain a match; it must come out as literals.
  for (std::size_t size = 1; size < 8; ++size) {
    const Bytes input(size, 'a');
    const std::vector<Token> tokens = tokenizeBytes(input, config);
    if (size < static_cast<std::size_t>(config.minMatch) + 1) {
      for (const Token& token : tokens) CHECK(!token.isMatch());
    }
    checkTokenRoundtrip(input, config);
  }
}

TEST(Lz77FindsAnObviousRepeat) {
  const Config config;
  const Bytes input = bytesOf("abcdefghij" "abcdefghij");
  const std::vector<Token> tokens = tokenizeBytes(input, config);

  bool foundMatch = false;
  for (const Token& token : tokens) {
    if (token.isMatch()) {
      foundMatch = true;
      CHECK_EQ(static_cast<int>(token.distance), 10);
      CHECK_EQ(static_cast<int>(token.length), 10);
    }
  }
  CHECK(foundMatch);
  checkTokenInvariants(tokens, config);
  checkTokenRoundtrip(input, config);
}

TEST(Lz77EncodesRunsAsOverlappingMatches) {
  // The case worth understanding: a run of identical bytes becomes distance 1, and the
  // copy overlaps itself. LZ77 gets run-length encoding for free, provided the decoder
  // copies byte at a time rather than block-copying.
  const Config config;
  const Bytes input(1000, 'x');
  const std::vector<Token> tokens = tokenizeBytes(input, config);

  CHECK(tokens.size() < 10);  // ~1000 bytes expressed in a handful of tokens
  bool sawOverlap = false;
  for (const Token& token : tokens) {
    if (token.isMatch() && token.distance < token.length) sawOverlap = true;
  }
  CHECK(sawOverlap);
  checkTokenInvariants(tokens, config);
  checkTokenRoundtrip(input, config);
}

TEST(Lz77RespectsTheWindow) {
  // A repeat further back than the window must not be found: the decoder enforces the
  // window bound, so a token violating it would produce a file that fails to decode.
  Config config;
  config.windowBits = 9;  // 512 bytes
  const std::size_t gap = 4000;

  Bytes input = bytesOf("UNIQUEMARKERSEQUENCE");
  std::mt19937 rng(7);
  std::uniform_int_distribution<int> dist('a', 'z');
  for (std::size_t i = 0; i < gap; ++i) input.push_back(static_cast<std::uint8_t>(dist(rng)));
  const Bytes marker = bytesOf("UNIQUEMARKERSEQUENCE");
  input.insert(input.end(), marker.begin(), marker.end());

  const std::vector<Token> tokens = tokenizeBytes(input, config);
  checkTokenInvariants(tokens, config);
  checkTokenRoundtrip(input, config);
}

TEST(Lz77MatchesAtEveryDistanceUpToTheWindow) {
  // Sweeps the distance right up to the window edge, which is where the ring-buffer
  // aliasing in the hash chains would show up if the bound were off by one.
  Config config;
  config.windowBits = 9;
  const std::size_t window = config.windowSize();

  for (std::size_t distance = 3; distance < window + 4; distance += 7) {
    Bytes input;
    input.reserve(distance + 32);
    std::mt19937 rng(static_cast<unsigned>(distance));
    std::uniform_int_distribution<int> dist(0, 255);
    for (std::size_t i = 0; i < distance; ++i) input.push_back(static_cast<std::uint8_t>(dist(rng)));
    // Repeat the first 16 bytes at exactly `distance` away.
    for (std::size_t i = 0; i < 16; ++i) input.push_back(input[i]);

    const std::vector<Token> tokens = tokenizeBytes(input, config);
    checkTokenInvariants(tokens, config);
    checkTokenRoundtrip(input, config);
  }
}

TEST(Lz77RoundtripsAcrossTheWholeConfigSpace) {
  // Every knob interacts with the match finder, so correctness is checked across the
  // grid rather than at the defaults only.
  const Bytes text = repetitiveText(60000);

  Bytes binary;
  std::mt19937 rng(4242);
  std::uniform_int_distribution<int> byteDist(0, 255);
  std::uniform_int_distribution<int> runDist(1, 40);
  while (binary.size() < 60000) {
    const int run = runDist(rng);
    const auto value = static_cast<std::uint8_t>(byteDist(rng));
    for (int i = 0; i < run; ++i) binary.push_back(value);
  }

  for (const Bytes& input : {text, binary}) {
    for (int windowBits : {8, 10, 15, 16}) {
      for (int minMatch : {3, 4, 6}) {
        for (int chain : {1, 8, 256}) {
          for (bool lazy : {false, true}) {
            Config config;
            config.windowBits = windowBits;
            config.minMatch = minMatch;
            config.maxChainLength = chain;
            config.lazyMatching = lazy;

            const std::vector<Token> tokens = tokenizeBytes(input, config);
            checkTokenInvariants(tokens, config);
            const Bytes restored = cmpr::lz77::detokenize(tokens, input.size());
            CHECK_EQ(restored.size(), input.size());
            CHECK(restored == input);
          }
        }
      }
    }
  }
}

TEST(Lz77LongerChainsFindMoreMatches) {
  // The chain length is the ratio-versus-speed dial: it caps how many candidates the
  // match finder examines per position, and it is essentially what gzip's -1 through -9
  // control.
  //
  // Note what is NOT asserted here: that token count decreases monotonically at every
  // step. It very nearly does, but it is not guaranteed, because this is a *greedy*
  // parser. Taking the longest match at the current position can leave the encoder at a
  // worse position than a shorter match would have -- finding a better match locally can
  // cost more globally. Optimal parsing needs a shortest-path search over the token
  // graph, which real DEFLATE implementations also decline to do. So the endpoints are
  // compared, where the effect swamps the noise.
  const Bytes input = wordSaladText(200000, 11);
  Config shallow;
  shallow.maxChainLength = 1;
  Config deep;
  deep.maxChainLength = 1024;

  Stats shallowStats;
  Stats deepStats;
  const std::size_t shallowBytes = serialisedSize(tokenizeBytes(input, shallow, &shallowStats));
  const std::size_t deepBytes = serialisedSize(tokenizeBytes(input, deep, &deepStats));

  CHECK(deepBytes < shallowBytes);
  CHECK(deepStats.averageMatchLength() > shallowStats.averageMatchLength());
  // And it is not free: the deeper search examines strictly more candidates. That cost
  // is the other half of the tradeoff, and it is what the benchmark sweep measures.
  CHECK(deepStats.candidatesExamined > shallowStats.candidatesExamined);
}

TEST(Lz77LazyMatchingImprovesTheRatio) {
  // Worth asserting rather than assuming: the heuristic spends a literal to buy a longer
  // match. Note it comes out ahead on *bytes* while emitting more tokens than the greedy
  // parser -- see serialisedSize() above for why those two disagree.
  const Bytes input = wordSaladText(200000, 22);
  Config greedy;
  greedy.lazyMatching = false;
  Config lazy;
  lazy.lazyMatching = true;

  CHECK(serialisedSize(tokenizeBytes(input, lazy)) <
        serialisedSize(tokenizeBytes(input, greedy)));
}

TEST(Lz77StatsAddUp) {
  const Bytes input = repetitiveText(50000);
  Stats stats;
  const std::vector<Token> tokens = tokenizeBytes(input, Config{}, &stats);

  CHECK_EQ(stats.literals + stats.matches, static_cast<std::uint64_t>(tokens.size()));
  // Every input byte is either emitted as a literal or covered by a match.
  CHECK_EQ(stats.literals + stats.matchedBytes, static_cast<std::uint64_t>(input.size()));
  CHECK(stats.averageMatchLength() >= Config{}.minMatch);
}

TEST(Lz77RejectsImpossibleConfigs) {
  const Bytes input = bytesOf("anything");
  Config tooWide;
  tooWide.windowBits = 17;  // a distance no longer fits in the uint16 field
  CHECK_THROWS(tokenizeBytes(input, tooWide), std::invalid_argument);

  Config tooShort;
  tooShort.minMatch = 2;  // a 2-byte match costs more to encode than to spell out
  CHECK_THROWS(tokenizeBytes(input, tooShort), std::invalid_argument);

  Config noChain;
  noChain.maxChainLength = 0;
  CHECK_THROWS(tokenizeBytes(input, noChain), std::invalid_argument);
}

TEST(Lz77DetokenizeRejectsABadBackReference) {
  std::vector<Token> tokens;
  Token bad;
  bad.distance = 5;  // nothing has been produced yet, so there is nothing 5 bytes back
  bad.length = 3;
  tokens.push_back(bad);
  CHECK_THROWS(cmpr::lz77::detokenize(tokens, 3), cmpr::CorruptInput);
}
