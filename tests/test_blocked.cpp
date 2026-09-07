//
// Blocked container, streaming, and parallel compression.
//
// The three share one format and one decoder by design, so most of what is checked here
// is agreement: streaming must produce the same bytes as the in-memory path, and eight
// threads must produce the same bytes as one. Two code paths that are supposed to agree
// drift apart unless something asserts that they do not.
//
#include <random>
#include <sstream>
#include <string>

#include "codec.h"
#include "testing.h"

using cmpr::Algorithm;
using cmpr::CorruptInput;
using cmpr::Method;
using cmpr::Options;

namespace {

using Bytes = std::vector<std::uint8_t>;

// Deliberately heterogeneous: a stretch of repetitive text, a stretch of random bytes
// that must fall back to STORED, then more text. Blocks land on different content, so the
// per-block method choice is exercised rather than being uniform.
Bytes mixedContent(std::size_t targetSize, unsigned seed) {
  static const char* const words[] = {"compress", "block",  "stream", "thread", "window",
                                      "huffman",  "deflate", "token", "length", "buffer"};
  std::mt19937 rng(seed);
  std::uniform_int_distribution<std::size_t> pick(0, 9);
  std::uniform_int_distribution<int> byteDist(0, 255);

  Bytes out;
  out.reserve(targetSize);
  bool textual = true;
  while (out.size() < targetSize) {
    const std::size_t stretch = 40000;
    const std::size_t end = std::min(targetSize, out.size() + stretch);
    if (textual) {
      while (out.size() < end) {
        const char* word = words[pick(rng)];
        for (const char* c = word; *c != 0; ++c) out.push_back(static_cast<std::uint8_t>(*c));
        out.push_back(' ');
      }
    } else {
      while (out.size() < end) out.push_back(static_cast<std::uint8_t>(byteDist(rng)));
    }
    textual = !textual;
  }
  out.resize(targetSize);
  return out;
}

Options blocked(std::size_t blockSize, int threads = 1) {
  Options options;
  options.blockSize = blockSize;
  options.threads = threads;
  return options;
}

Bytes streamCompress(const Bytes& input, const Options& options) {
  std::stringstream in;
  in.write(reinterpret_cast<const char*>(input.data()), static_cast<std::streamsize>(input.size()));
  std::stringstream out;
  cmpr::compressStream(in, out, input.size(), options);
  const std::string produced = out.str();
  return Bytes(produced.begin(), produced.end());
}

Bytes streamDecompress(const Bytes& compressed) {
  std::stringstream in;
  in.write(reinterpret_cast<const char*>(compressed.data()),
           static_cast<std::streamsize>(compressed.size()));
  std::stringstream out;
  cmpr::decompressStream(in, out);
  const std::string produced = out.str();
  return Bytes(produced.begin(), produced.end());
}

}  // namespace

TEST(BlockedRoundtripAcrossBlockSizes) {
  const Bytes input = mixedContent(300000, 1);
  // Sizes chosen to land the last block exactly on, just over, and just under a boundary.
  for (std::size_t blockSize : {std::size_t{1024}, std::size_t{4096}, std::size_t{50000},
                                std::size_t{100000}, std::size_t{150000}, std::size_t{299999},
                                std::size_t{300000}, std::size_t{400000}}) {
    const Bytes compressed = cmpr::compress(input, blocked(blockSize));
    CHECK(cmpr::decompress(compressed) == input);
  }
}

TEST(BlockingOnlyKicksInAboveTheBlockSize) {
  const Bytes input = mixedContent(100000, 2);

  // Exactly one block's worth stays a plain single-block file: no framing overhead for
  // blocks that do not exist.
  const Bytes single = cmpr::compress(input, blocked(100000));
  CHECK(cmpr::methodOf(single) != Method::kBlocked);

  const Bytes multiple = cmpr::compress(input, blocked(99999));
  CHECK(cmpr::methodOf(multiple) == Method::kBlocked);

  // Blocking disabled entirely.
  const Bytes unblocked = cmpr::compress(input, blocked(0));
  CHECK(cmpr::methodOf(unblocked) != Method::kBlocked);
}

TEST(BlocksChooseTheirOwnMethod) {
  // The random stretches of the fixture cannot be compressed and must be stored, while
  // the textual stretches deflate. A per-block method choice is the point: one global
  // decision would either bloat the random parts or give up on the text.
  const Bytes input = mixedContent(240000, 3);
  const Bytes compressed = cmpr::compress(input, blocked(20000));
  CHECK(cmpr::methodOf(compressed) == Method::kBlocked);

  bool sawStored = false;
  bool sawDeflate = false;
  std::size_t at = cmpr::kHeaderSize;
  while (at + 9 <= compressed.size()) {
    const std::uint32_t payloadLength = static_cast<std::uint32_t>(compressed[at + 4]) |
                                        (static_cast<std::uint32_t>(compressed[at + 5]) << 8) |
                                        (static_cast<std::uint32_t>(compressed[at + 6]) << 16) |
                                        (static_cast<std::uint32_t>(compressed[at + 7]) << 24);
    const auto method = static_cast<Method>(compressed[at + 8]);
    if (method == Method::kStored) sawStored = true;
    if (method == Method::kDeflate) sawDeflate = true;
    at += 9 + payloadLength;
  }
  CHECK(sawStored);
  CHECK(sawDeflate);
  CHECK(cmpr::decompress(compressed) == input);
}

TEST(ThreadCountNeverChangesTheOutput) {
  // Parallelism is an implementation detail of the encoder, not a property of the format.
  // If eight threads produced different bytes from one, no benchmark comparing them would
  // mean anything, and reproducing a bug would depend on the scheduler.
  const Bytes input = mixedContent(500000, 4);
  const Bytes reference = cmpr::compress(input, blocked(50000, 1));

  for (int threads : {2, 3, 4, 8, 0}) {  // 0 means "as many as the hardware reports"
    const Bytes produced = cmpr::compress(input, blocked(50000, threads));
    CHECK(produced == reference);
    CHECK(cmpr::decompress(produced) == input);
  }
}

TEST(StreamingProducesIdenticalBytesToInMemory) {
  for (std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{40000},
                           std::size_t{100000}, std::size_t{250000}}) {
    const Bytes input = mixedContent(size, 5);
    for (std::size_t blockSize : {std::size_t{4096}, std::size_t{60000}, std::size_t{1 << 20}}) {
      for (int threads : {1, 4}) {
        const Options options = blocked(blockSize, threads);
        const Bytes inMemory = cmpr::compress(input, options);
        const Bytes streamed = streamCompress(input, options);
        CHECK(streamed == inMemory);
      }
    }
  }
}

TEST(StreamingRoundtrip) {
  const Bytes input = mixedContent(400000, 6);
  for (std::size_t blockSize : {std::size_t{8192}, std::size_t{100000}, std::size_t{1 << 20}}) {
    const Bytes compressed = streamCompress(input, blocked(blockSize, 4));
    CHECK(streamDecompress(compressed) == input);
    // ... and the streaming decoder must read what the in-memory encoder wrote, and the
    // other way round. One format, four combinations.
    CHECK(cmpr::decompress(compressed) == input);
    CHECK(streamDecompress(cmpr::compress(input, blocked(blockSize))) == input);
  }
}

TEST(StreamingHandlesEveryAlgorithm) {
  const Bytes input = mixedContent(150000, 7);
  for (Algorithm algorithm : {Algorithm::kHuffman, Algorithm::kLz77, Algorithm::kDeflate}) {
    Options options = blocked(30000, 2);
    options.algorithm = algorithm;
    const Bytes compressed = streamCompress(input, options);
    CHECK(streamDecompress(compressed) == input);
    CHECK(cmpr::decompress(compressed) == input);
  }
}

TEST(CorruptBlockedFramingIsRejected) {
  const Bytes input = mixedContent(200000, 8);
  const Bytes compressed = cmpr::compress(input, blocked(30000));
  CHECK(cmpr::methodOf(compressed) == Method::kBlocked);

  // A block claiming to decode to more than the file's declared size.
  Bytes oversizeBlock = compressed;
  for (int i = 0; i < 4; ++i) oversizeBlock[cmpr::kHeaderSize + i] = 0xFF;
  CHECK_THROWS(cmpr::decompress(oversizeBlock), CorruptInput);

  // A block whose payload runs off the end of the file.
  Bytes oversizePayload = compressed;
  for (int i = 0; i < 4; ++i) oversizePayload[cmpr::kHeaderSize + 4 + i] = 0xFF;
  CHECK_THROWS(cmpr::decompress(oversizePayload), CorruptInput);

  // Blocks do not nest: a block claiming to be method 4 is not something we ever write.
  Bytes nested = compressed;
  nested[cmpr::kHeaderSize + 8] = 4;
  CHECK_THROWS(cmpr::decompress(nested), CorruptInput);

  Bytes truncated(compressed.begin(), compressed.begin() + compressed.size() / 2);
  CHECK_THROWS(cmpr::decompress(truncated), CorruptInput);
  CHECK_THROWS(streamDecompress(truncated), CorruptInput);
}

TEST(BlockedRatioIsCloseToUnblocked) {
  // Blocking exists for memory and parallelism, but it must not quietly wreck the ratio.
  // At the default block size the difference should be small in either direction -- and
  // on heterogeneous input it tends to go the encoder's way, because per-block trees fit
  // better than one global set. NOTES.md has the measured curve.
  const Bytes input = mixedContent(600000, 9);
  const std::size_t unblocked = cmpr::compress(input, blocked(0)).size();
  const std::size_t withBlocks = cmpr::compress(input, blocked(200000)).size();
  CHECK(withBlocks < unblocked * 105 / 100);
}
