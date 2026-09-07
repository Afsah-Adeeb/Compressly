//
// Tests for the DEFLATE-style combination.
//
// The code tables get exhaustive coverage rather than spot checks. There are only 258
// lengths and 32768 distances, the tables are transcribed constants where a single typo
// silently corrupts one narrow range of values, and a roundtrip test would only catch it
// if some input happened to produce a match of exactly that length. Checking all of them
// costs milliseconds.
//
#include <random>
#include <stdexcept>
#include <string>

#include "codec.h"
#include "deflate.h"
#include "testing.h"

using cmpr::Algorithm;
using cmpr::CorruptInput;
using cmpr::Method;
using cmpr::Options;
using cmpr::deflate::Split;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes bytesOf(const std::string& text) { return Bytes(text.begin(), text.end()); }

Options with(Algorithm algorithm) {
  Options options;
  options.algorithm = algorithm;
  return options;
}

Bytes sourceLikeText(std::size_t targetSize, unsigned seed) {
  static const char* const vocabulary[] = {
      "const",    "std",     "size_t",  "return", "if",     "else",   "for",
      "while",    "vector",  "uint8_t", "length", "buffer", "token",  "match",
      "position", "encoder", "window",  "hash",   "chain",  "stream", "config"};
  constexpr std::size_t kVocabularySize = sizeof(vocabulary) / sizeof(vocabulary[0]);

  std::mt19937 rng(seed);
  std::uniform_int_distribution<std::size_t> pick(0, kVocabularySize - 1);
  std::uniform_int_distribution<int> mark(0, 9);

  std::string text;
  while (text.size() < targetSize) {
    text += vocabulary[pick(rng)];
    const int punctuation = mark(rng);
    text += (punctuation < 6) ? " " : (punctuation < 8 ? ", " : ";\n  ");
  }
  text.resize(targetSize);
  return bytesOf(text);
}

}  // namespace

TEST(DeflateLengthCodesCoverEveryLengthExactlyOnce) {
  int previousSymbol = 0;
  for (int length = cmpr::deflate::kMinMatch; length <= cmpr::deflate::kMaxMatch; ++length) {
    const Split split = cmpr::deflate::splitLength(length);

    CHECK(split.symbol >= cmpr::deflate::kFirstLengthSymbol);
    CHECK(split.symbol < cmpr::deflate::kLitLenAlphabetSize);
    // Symbols must not go backwards as the value grows, or the ranges overlap.
    CHECK(split.symbol >= previousSymbol);
    previousSymbol = split.symbol;

    CHECK_EQ(split.extraBits, cmpr::deflate::lengthExtraBits(split.symbol));
    // The extra value has to fit in the bits allotted, or writing it would corrupt the
    // following code.
    CHECK(split.extraBits == 0 ? split.extraValue == 0
                               : split.extraValue < (std::uint32_t{1} << split.extraBits));
    // And the split must be exactly reversible -- this is what the decoder does.
    CHECK_EQ(cmpr::deflate::lengthBase(split.symbol) + static_cast<int>(split.extraValue), length);
  }
}

TEST(DeflateDistanceCodesCoverEveryDistanceExactlyOnce) {
  int previousSymbol = 0;
  for (int distance = 1; distance <= (1 << cmpr::deflate::kMaxWindowBits); ++distance) {
    const Split split = cmpr::deflate::splitDistance(distance);

    CHECK(split.symbol >= 0);
    CHECK(split.symbol < cmpr::deflate::kDistAlphabetSize);
    CHECK(split.symbol >= previousSymbol);
    previousSymbol = split.symbol;

    CHECK_EQ(split.extraBits, cmpr::deflate::distanceExtraBits(split.symbol));
    CHECK(split.extraBits == 0 ? split.extraValue == 0
                               : split.extraValue < (std::uint32_t{1} << split.extraBits));
    CHECK_EQ(cmpr::deflate::distanceBase(split.symbol) + static_cast<int>(split.extraValue),
             distance);
  }
  // The last distance code must reach exactly the top of the window, no further and no
  // shorter -- this is the check that the table was not truncated or padded.
  CHECK_EQ(previousSymbol, cmpr::deflate::kDistAlphabetSize - 1);
}

TEST(DeflateRejectsValuesOutsideTheCodeTables) {
  CHECK_THROWS(cmpr::deflate::splitLength(2), std::invalid_argument);
  CHECK_THROWS(cmpr::deflate::splitLength(259), std::invalid_argument);
  CHECK_THROWS(cmpr::deflate::splitDistance(0), std::invalid_argument);
  CHECK_THROWS(cmpr::deflate::splitDistance(32769), std::invalid_argument);
}

TEST(DeflateRejectsConfigsItsTablesCannotExpress) {
  const Bytes input = sourceLikeText(4000, 3);

  Options wideWindow = with(Algorithm::kDeflate);
  wideWindow.lz77.windowBits = 16;  // distance codes stop at 32768
  CHECK_THROWS(cmpr::compress(input, wideWindow), std::invalid_argument);

  Options longMinMatch = with(Algorithm::kDeflate);
  longMinMatch.lz77.minMatch = 4;  // would push maxMatch to 259, past the length table
  CHECK_THROWS(cmpr::compress(input, longMinMatch), std::invalid_argument);
}

TEST(DeflateBeatsBothOfItsHalves) {
  // The whole justification for this phase: Huffman alone cannot see repetition, LZ77
  // alone spends a fixed 16 bits on every distance and 8 on every literal. Together they
  // must beat either one, on any input where either one does anything at all.
  const Bytes input = sourceLikeText(200000, 5);

  const std::size_t huffman = cmpr::compress(input, with(Algorithm::kHuffman)).size();
  const std::size_t lz77 = cmpr::compress(input, with(Algorithm::kLz77)).size();
  const Bytes deflated = cmpr::compress(input, with(Algorithm::kDeflate));

  CHECK(cmpr::methodOf(deflated) == Method::kDeflate);
  CHECK(deflated.size() < lz77);
  CHECK(deflated.size() < huffman);
}

TEST(DeflateIsTheDefaultAlgorithm) {
  const Bytes input = sourceLikeText(50000, 6);
  const Bytes compressed = cmpr::compress(input);
  CHECK(cmpr::methodOf(compressed) == Method::kDeflate);
  CHECK(cmpr::decompress(compressed) == input);
}

TEST(DeflateFallsBackToStoredOnRandomData) {
  std::mt19937 rng(31337);
  std::uniform_int_distribution<int> dist(0, 255);
  Bytes input;
  input.reserve(100000);
  for (int i = 0; i < 100000; ++i) input.push_back(static_cast<std::uint8_t>(dist(rng)));

  const Bytes compressed = cmpr::compress(input, with(Algorithm::kDeflate));
  CHECK(cmpr::methodOf(compressed) == Method::kStored);
  CHECK(cmpr::decompress(compressed) == input);
}

TEST(DeflateHandlesMatchesAtTheTableExtremes) {
  // A maximum-length match at a maximum distance exercises the last entry of both tables,
  // which the code-table sweeps above check in isolation but not through the encoder.
  const std::size_t distance = (1 << cmpr::deflate::kMaxWindowBits) - 1;
  Bytes input;
  input.reserve(distance + 600);

  std::mt19937 rng(8);
  std::uniform_int_distribution<int> byteDist(0, 255);
  Bytes prefix;
  for (int i = 0; i < 300; ++i) prefix.push_back(static_cast<std::uint8_t>(byteDist(rng)));
  input = prefix;
  while (input.size() < distance) input.push_back(static_cast<std::uint8_t>(byteDist(rng)));
  // Repeat the opening 300 bytes from as far back as the window reaches.
  input.insert(input.end(), prefix.begin(), prefix.end());

  const Bytes compressed = cmpr::compress(input, with(Algorithm::kDeflate));
  CHECK(cmpr::decompress(compressed) == input);
}

TEST(DeflateHandlesLongRuns) {
  // Runs become distance-1 matches at the maximum length, repeatedly -- the overlapping
  // copy path, through the full encoder this time.
  Bytes input(200000, 0x5A);
  const Bytes compressed = cmpr::compress(input, with(Algorithm::kDeflate));
  CHECK(cmpr::decompress(compressed) == input);
  // 200 KB of one byte should collapse to almost nothing.
  CHECK(compressed.size() < 1000);
}

TEST(CorruptDeflateStreamIsRejected) {
  const Bytes input = sourceLikeText(40000, 9);
  const Bytes compressed = cmpr::compress(input, with(Algorithm::kDeflate));
  CHECK(cmpr::methodOf(compressed) == Method::kDeflate);
  const std::size_t payload = cmpr::kHeaderSize;

  Bytes badWindow = compressed;
  badWindow[payload] = 99;
  CHECK_THROWS(cmpr::decompress(badWindow), CorruptInput);

  Bytes badHlit = compressed;
  badHlit[payload + 1] = 0xFF;
  badHlit[payload + 2] = 0xFF;
  CHECK_THROWS(cmpr::decompress(badHlit), CorruptInput);

  Bytes zeroHlit = compressed;
  zeroHlit[payload + 1] = 0;
  zeroHlit[payload + 2] = 0;
  CHECK_THROWS(cmpr::decompress(zeroHlit), CorruptInput);

  Bytes badHdist = compressed;
  badHdist[payload + 3] = 31;
  CHECK_THROWS(cmpr::decompress(badHdist), CorruptInput);

  Bytes truncated(compressed.begin(), compressed.begin() + compressed.size() / 2);
  CHECK_THROWS(cmpr::decompress(truncated), CorruptInput);
}

TEST(DeflateTableSizesShrinkWithTheAlphabetUsed) {
  // HLIT/HDIST drop trailing unused symbols. A file with no matches at all uses no
  // distance codes, so its distance table should be empty rather than 30 zero bytes.
  Bytes input;
  for (int value = 0; value < 200; ++value) input.push_back(static_cast<std::uint8_t>(value));
  // 200 distinct bytes, no repetition: literals only.
  const Bytes compressed = cmpr::compress(input, with(Algorithm::kDeflate));
  if (cmpr::methodOf(compressed) == Method::kDeflate) {
    CHECK_EQ(static_cast<int>(compressed[cmpr::kHeaderSize + 3]), 0);  // HDIST
  }
  CHECK(cmpr::decompress(compressed) == input);
}
