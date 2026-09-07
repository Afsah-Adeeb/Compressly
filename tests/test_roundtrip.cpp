//
// The correctness gate: decompress(compress(x)) == x, byte for byte, for every input
// shape that has ever broken a compressor. Ratio is a separate question -- an
// implementation that loses a single byte is worthless no matter how small its output.
//
// Every generic case runs under every algorithm. Tests that assert *which* method the
// encoder picked name the algorithm explicitly, because that choice is the thing under
// test rather than incidental.
//
#include <random>
#include <string>

#include "codec.h"
#include "testing.h"

using cmpr::Algorithm;
using cmpr::CorruptInput;
using cmpr::Method;
using cmpr::Options;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes bytesOf(const std::string& text) { return Bytes(text.begin(), text.end()); }

Options with(Algorithm algorithm) {
  Options options;
  options.algorithm = algorithm;
  return options;
}

// Returns the compressed form so callers can additionally assert on size or method.
Bytes checkRoundtrip(const Bytes& input, const Options& options) {
  const Bytes compressed = cmpr::compress(input, options);
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

void checkRoundtripEveryAlgorithm(const Bytes& input) {
  checkRoundtrip(input, with(Algorithm::kHuffman));
  checkRoundtrip(input, with(Algorithm::kLz77));
}

}  // namespace

TEST(EmptyFile) {
  const Bytes compressed = checkRoundtrip({}, Options{});
  CHECK_EQ(compressed.size(), cmpr::kHeaderSize);
  CHECK(cmpr::methodOf(compressed) == Method::kStored);
  checkRoundtripEveryAlgorithm({});
}

TEST(SingleByte) {
  checkRoundtripEveryAlgorithm({0x42});
}

TEST(EveryPossibleSingleByteValue) {
  // A one-byte file for each of the 256 values, including 0x00 and 0xFF, which are the
  // ones that tend to expose sign-extension and terminator bugs.
  for (int value = 0; value < 256; ++value) {
    checkRoundtripEveryAlgorithm({static_cast<std::uint8_t>(value)});
  }
}

TEST(AllIdenticalBytes) {
  const Bytes input(100000, 'A');
  checkRoundtripEveryAlgorithm(input);

  // Huffman: the degenerate single-symbol tree, one bit per byte.
  const Bytes viaHuffman = checkRoundtrip(input, with(Algorithm::kHuffman));
  CHECK(cmpr::methodOf(viaHuffman) == Method::kHuffman);
  CHECK(viaHuffman.size() < input.size() / 7);

  // LZ77: the same file is a handful of overlapping back-references, which is a
  // different order of magnitude entirely. Worth seeing the two side by side.
  const Bytes viaLz77 = checkRoundtrip(input, with(Algorithm::kLz77));
  CHECK(cmpr::methodOf(viaLz77) == Method::kLz77);
  CHECK(viaLz77.size() < 3000);
}

TEST(TwoSymbolsSkewed) {
  Bytes input;
  for (int i = 0; i < 10000; ++i) input.push_back(i % 100 == 0 ? 'b' : 'a');
  checkRoundtripEveryAlgorithm(input);
}

TEST(EveryByteValueOnceEach) {
  // A flat distribution over the full alphabet with no repetition: nothing for either
  // algorithm to exploit, so both must fall back to STORED rather than grow the file.
  Bytes input;
  for (int value = 0; value < 256; ++value) input.push_back(static_cast<std::uint8_t>(value));

  for (Algorithm algorithm : {Algorithm::kHuffman, Algorithm::kLz77}) {
    const Bytes compressed = checkRoundtrip(input, with(algorithm));
    CHECK(cmpr::methodOf(compressed) == Method::kStored);
    CHECK_EQ(compressed.size(), input.size() + cmpr::kHeaderSize);
  }
}

TEST(EnglishText) {
  std::string text;
  while (text.size() < 200000) {
    text += "the quick brown fox jumps over the lazy dog; ";
    text += "pack my box with five dozen liquor jugs. ";
  }
  const Bytes input = bytesOf(text);
  checkRoundtripEveryAlgorithm(input);

  // Order-0 Huffman on English text lands near 4.2 bits per character, so roughly half.
  const Bytes viaHuffman = checkRoundtrip(input, with(Algorithm::kHuffman));
  CHECK(cmpr::methodOf(viaHuffman) == Method::kHuffman);
  CHECK(viaHuffman.size() < input.size() * 60 / 100);

  // LZ77 sees the repeated phrases Huffman is blind to, and must do substantially better
  // on text this repetitive.
  const Bytes viaLz77 = checkRoundtrip(input, with(Algorithm::kLz77));
  CHECK(cmpr::methodOf(viaLz77) == Method::kLz77);
  CHECK(viaLz77.size() < viaHuffman.size());
}

TEST(HighlySkewedTextCompressesHard) {
  std::string text(100000, 'a');
  for (std::size_t i = 0; i < text.size(); i += 1000) text[i] = 'z';
  const Bytes input = bytesOf(text);
  checkRoundtripEveryAlgorithm(input);
  CHECK(checkRoundtrip(input, with(Algorithm::kHuffman)).size() < input.size() / 5);
}

TEST(IncompressibleRandomDataFallsBackToStored) {
  // Stands in for the already-compressed inputs in the benchmark suite (JPEG, MP4, ZIP):
  // a near-uniform byte distribution has neither skew for Huffman nor repetition for
  // LZ77. Growing the file here would be a correctness problem, not just a ratio one.
  std::mt19937 rng(999);
  std::uniform_int_distribution<int> dist(0, 255);
  Bytes input;
  input.reserve(100000);
  for (int i = 0; i < 100000; ++i) input.push_back(static_cast<std::uint8_t>(dist(rng)));

  for (Algorithm algorithm : {Algorithm::kHuffman, Algorithm::kLz77}) {
    const Bytes compressed = checkRoundtrip(input, with(algorithm));
    CHECK(cmpr::methodOf(compressed) == Method::kStored);
    CHECK(compressed.size() <= input.size() + cmpr::kHeaderSize);
  }
}

TEST(BinaryDataWithNullsAndHighBytes) {
  Bytes input;
  for (int i = 0; i < 50000; ++i) {
    input.push_back(0x00);
    input.push_back(static_cast<std::uint8_t>(i & 0xFF));
    input.push_back(0xFF);
    input.push_back(0x00);
  }
  checkRoundtripEveryAlgorithm(input);
}

TEST(SizesAroundByteBoundaries) {
  // Off-by-one bugs in the bit writer's final flush, and in the LZ77 flag-byte grouping,
  // show up only at particular sizes -- so sweep a range rather than test one convenient
  // number. 200 covers well past both an 8-token flag group and a 64-bit accumulator.
  for (std::size_t size = 0; size < 200; ++size) {
    Bytes input;
    input.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
      input.push_back(static_cast<std::uint8_t>('a' + (i * i) % 7));
    }
    checkRoundtripEveryAlgorithm(input);
  }
}

TEST(FibonacciFrequenciesRoundtrip) {
  // The Huffman length-limiter path end to end: with the limiter engaged the code becomes
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
  checkRoundtripEveryAlgorithm(input);
}

TEST(Lz77ConfigsRoundtripThroughTheContainer) {
  // The window size and minimum match length are written into the file and read back by
  // the decoder, so a non-default encoder setting has to survive the container.
  std::string text;
  while (text.size() < 80000) text += "repetition is what LZ77 eats for breakfast. ";
  const Bytes input = bytesOf(text);

  for (int windowBits : {8, 11, 15, 16}) {
    for (int minMatch : {3, 5, 8}) {
      Options options = with(Algorithm::kLz77);
      options.lz77.windowBits = windowBits;
      options.lz77.minMatch = minMatch;
      checkRoundtrip(input, options);
    }
  }
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

TEST(CorruptLz77StreamIsRejected) {
  std::string text;
  while (text.size() < 20000) text += "back-references everywhere, back-references everywhere. ";
  Bytes compressed = cmpr::compress(bytesOf(text), with(Algorithm::kLz77));
  CHECK(cmpr::methodOf(compressed) == Method::kLz77);

  Bytes badWindow = compressed;
  badWindow[cmpr::kHeaderSize] = 99;  // window bits outside the legal range
  CHECK_THROWS(cmpr::decompress(badWindow), CorruptInput);

  Bytes badMinMatch = compressed;
  badMinMatch[cmpr::kHeaderSize + 1] = 1;
  CHECK_THROWS(cmpr::decompress(badMinMatch), CorruptInput);

  Bytes truncated(compressed.begin(), compressed.begin() + compressed.size() / 2);
  CHECK_THROWS(cmpr::decompress(truncated), CorruptInput);
}

TEST(CorruptHeaderDoesNotCauseAHugeAllocation) {
  // A hostile file can claim any uncompressed size it likes. The decoder must not trust
  // it: bounding the reservation by what the payload could possibly encode keeps this a
  // thrown exception rather than an out-of-memory kill.
  for (Algorithm algorithm : {Algorithm::kHuffman, Algorithm::kLz77}) {
    Bytes compressed = cmpr::compress(bytesOf("abracadabra abracadabra abracadabra"), with(algorithm));
    for (int i = 0; i < 8; ++i) compressed[6 + i] = 0xFF;  // uncompressed size = 2^64 - 1
    CHECK_THROWS(cmpr::decompress(compressed), CorruptInput);
  }
}
