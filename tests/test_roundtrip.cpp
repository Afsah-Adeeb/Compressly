//
// The correctness gate for Phase 1: decompress(compress(x)) == x, byte for byte, for
// every input shape that has ever broken a compressor. Ratio is a separate question --
// an implementation that loses a single byte is worthless no matter how small its output.
//
#include <random>
#include <string>

#include "codec.h"
#include "testing.h"

using cmpr::CorruptInput;
using cmpr::Method;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes bytesOf(const std::string& text) {
  return Bytes(text.begin(), text.end());
}

// Returns the compressed form so callers can additionally assert on size or method.
Bytes checkRoundtrip(const Bytes& input) {
  const Bytes compressed = cmpr::compress(input);
  const Bytes restored = cmpr::decompress(compressed);
  CHECK_EQ(restored.size(), input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (restored[i] != input[i]) {
      ::testing::fail(__FILE__, __LINE__,
                      "byte " + std::to_string(i) + " differs: expected " +
                          std::to_string(input[i]) + ", got " + std::to_string(restored[i]));
    }
  }
  return compressed;
}

}  // namespace

TEST(EmptyFile) {
  const Bytes compressed = checkRoundtrip({});
  CHECK_EQ(compressed.size(), cmpr::kHeaderSize);
  CHECK(cmpr::methodOf(compressed) == Method::kStored);
}

TEST(SingleByte) {
  checkRoundtrip({0x42});
}

TEST(EveryPossibleSingleByteValue) {
  // A one-byte file for each of the 256 values, including 0x00 and 0xFF, which are the
  // ones that tend to expose sign-extension and terminator bugs.
  for (int value = 0; value < 256; ++value) {
    checkRoundtrip({static_cast<std::uint8_t>(value)});
  }
}

TEST(AllIdenticalBytes) {
  // The degenerate single-symbol tree, at a size where a broken decoder loops forever
  // rather than merely returning the wrong answer.
  const Bytes input(100000, 'A');
  const Bytes compressed = checkRoundtrip(input);
  CHECK(cmpr::methodOf(compressed) == Method::kHuffman);
  // One bit per byte plus a small header: within a whisker of an 8x reduction.
  CHECK(compressed.size() < input.size() / 7);
}

TEST(TwoSymbolsSkewed) {
  Bytes input;
  for (int i = 0; i < 10000; ++i) input.push_back(i % 100 == 0 ? 'b' : 'a');
  checkRoundtrip(input);
}

TEST(EveryByteValueOnceEach) {
  // A flat distribution over the full alphabet: every code comes out 8 bits, so Huffman
  // cannot win and the symbol table is pure overhead. Must fall back to STORED.
  Bytes input;
  for (int value = 0; value < 256; ++value) input.push_back(static_cast<std::uint8_t>(value));
  const Bytes compressed = checkRoundtrip(input);
  CHECK(cmpr::methodOf(compressed) == Method::kStored);
  CHECK_EQ(compressed.size(), input.size() + cmpr::kHeaderSize);
}

TEST(EnglishText) {
  std::string text;
  while (text.size() < 200000) {
    text += "the quick brown fox jumps over the lazy dog; ";
    text += "pack my box with five dozen liquor jugs. ";
  }
  const Bytes input = bytesOf(text);
  const Bytes compressed = checkRoundtrip(input);
  CHECK(cmpr::methodOf(compressed) == Method::kHuffman);
  // Order-0 Huffman on English text lands near 4.2 bits per character. Anything above
  // 60% of the original means something regressed -- and note how far this is from what
  // gzip achieves, which is precisely what LZ77 buys in Phase 2.
  CHECK(compressed.size() < input.size() * 60 / 100);
}

TEST(HighlySkewedTextCompressesHard) {
  std::string text(100000, 'a');
  for (std::size_t i = 0; i < text.size(); i += 1000) text[i] = 'z';
  const Bytes input = bytesOf(text);
  const Bytes compressed = checkRoundtrip(input);
  CHECK(compressed.size() < input.size() / 5);
}

TEST(IncompressibleRandomDataFallsBackToStored) {
  // Stands in for the already-compressed inputs in the benchmark suite (JPEG, MP4, ZIP):
  // a near-uniform byte distribution has no redundancy for an order-0 coder to remove.
  // Growing the file here would be a correctness problem, not just a ratio one.
  std::mt19937 rng(999);
  std::uniform_int_distribution<int> dist(0, 255);
  Bytes input;
  input.reserve(100000);
  for (int i = 0; i < 100000; ++i) input.push_back(static_cast<std::uint8_t>(dist(rng)));

  const Bytes compressed = checkRoundtrip(input);
  CHECK(cmpr::methodOf(compressed) == Method::kStored);
  CHECK(compressed.size() <= input.size() + cmpr::kHeaderSize);
}

TEST(BinaryDataWithNullsAndHighBytes) {
  Bytes input;
  for (int i = 0; i < 50000; ++i) {
    input.push_back(0x00);
    input.push_back(static_cast<std::uint8_t>(i & 0xFF));
    input.push_back(0xFF);
    input.push_back(0x00);
  }
  checkRoundtrip(input);
}

TEST(SizesAroundByteBoundaries) {
  // Off-by-one bugs in the bit writer's final flush show up as a wrong length only at
  // particular sizes, so sweep a range rather than testing one convenient number.
  for (std::size_t size = 0; size < 200; ++size) {
    Bytes input;
    input.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
      input.push_back(static_cast<std::uint8_t>('a' + (i * i) % 7));
    }
    checkRoundtrip(input);
  }
}

TEST(FibonacciFrequenciesRoundtrip) {
  // The length-limited code path, end to end: with the limiter engaged the code becomes
  // incomplete, so this is the case where the decode tree genuinely has dangling edges.
  Bytes input;
  std::uint64_t a = 1;
  std::uint64_t b = 1;
  for (int symbol = 0; symbol < 32; ++symbol) {
    for (std::uint64_t i = 0; i < a; ++i) {
      input.push_back(static_cast<std::uint8_t>(symbol));
    }
    const std::uint64_t next = a + b;
    a = b;
    b = next;
  }
  checkRoundtrip(input);
}

TEST(CorruptInputIsRejected) {
  const Bytes compressed = cmpr::compress(bytesOf("some reasonably compressible text, repeated"));

  CHECK_THROWS(cmpr::decompress(Bytes{}), CorruptInput);
  CHECK_THROWS(cmpr::decompress(Bytes{'C', 'M', 'P', 'R'}), CorruptInput);

  Bytes badMagic = compressed;
  badMagic[0] = 'X';
  CHECK_THROWS(cmpr::decompress(badMagic), CorruptInput);

  Bytes badVersion = compressed;
  badVersion[4] = 99;
  CHECK_THROWS(cmpr::decompress(badVersion), CorruptInput);

  Bytes badMethod = compressed;
  badMethod[5] = 77;
  CHECK_THROWS(cmpr::decompress(badMethod), CorruptInput);

  // Truncating the payload must fail loudly rather than returning a short buffer.
  Bytes truncated(compressed.begin(), compressed.begin() + cmpr::kHeaderSize + 4);
  CHECK_THROWS(cmpr::decompress(truncated), CorruptInput);
}

TEST(CorruptHeaderDoesNotCauseAHugeAllocation) {
  // A hostile file can claim any uncompressed size it likes. The decoder must not trust
  // it: bounding the reservation by what the payload could possibly encode keeps this a
  // thrown exception rather than an out-of-memory kill.
  Bytes compressed = cmpr::compress(bytesOf("abracadabra"));
  for (int i = 0; i < 8; ++i) compressed[6 + i] = 0xFF;  // uncompressed size = 2^64 - 1
  CHECK_THROWS(cmpr::decompress(compressed), CorruptInput);
}
