#include "block.h"

#include <algorithm>
#include <cstring>

#include "bitio.h"
#include "deflate.h"
#include "huffman.h"

namespace cmpr {
namespace {

// ---------------------------------------------------------------- Huffman (method 1)

// Returns an empty payload when the encoding would reach `budgetBytes` or more, the same
// "not worth it" convention deflate::encode uses.
std::vector<std::uint8_t> encodeHuffman(const std::uint8_t* data, std::size_t size,
                                        std::size_t budgetBytes) {
  const huffman::FreqTable freq = huffman::countFrequencies(data, size);
  const huffman::LengthTable lengths = huffman::buildLengths(freq);
  const huffman::CodeTable codes = huffman::buildCanonicalCodes(lengths);

  std::size_t distinct = 0;
  std::uint64_t payloadBits = 0;
  for (int symbol = 0; symbol < huffman::kByteAlphabetSize; ++symbol) {
    const std::uint8_t length = lengths[static_cast<std::size_t>(symbol)];
    if (length == 0) continue;
    ++distinct;
    payloadBits += freq[static_cast<std::size_t>(symbol)] * length;
  }

  // The exact compressed size is known before encoding anything: the code lengths and
  // the frequencies are all it depends on. So the "would this even help?" question gets
  // answered by arithmetic instead of by encoding and measuring, which matters for
  // already-compressed input where the encode would be pure waste.
  const std::size_t tableSize = 1 + 2 * distinct;
  const std::size_t total = tableSize + static_cast<std::size_t>((payloadBits + 7) / 8);
  if (total >= budgetBytes) return {};

  std::vector<std::uint8_t> out;
  out.reserve(total);
  out.push_back(static_cast<std::uint8_t>(distinct - 1));  // 1..256 stored as 0..255
  for (int symbol = 0; symbol < huffman::kByteAlphabetSize; ++symbol) {
    const std::uint8_t length = lengths[static_cast<std::size_t>(symbol)];
    if (length == 0) continue;
    out.push_back(static_cast<std::uint8_t>(symbol));
    out.push_back(length);
  }

  BitWriter writer(out);
  for (std::size_t i = 0; i < size; ++i) {
    const huffman::Code& code = codes[data[i]];
    writer.writeBits(code.bits, code.length);
  }
  writer.flush();
  return out;
}

std::vector<std::uint8_t> decodeHuffman(const std::uint8_t* payload, std::size_t payloadSize,
                                        std::uint64_t originalSize) {
  if (payloadSize < 1) throw CorruptInput("missing symbol table");

  const std::size_t distinct = static_cast<std::size_t>(payload[0]) + 1;
  const std::size_t tableSize = 1 + 2 * distinct;
  if (payloadSize < tableSize) throw CorruptInput("truncated symbol table");

  huffman::LengthTable lengths(huffman::kByteAlphabetSize, 0);
  for (std::size_t i = 0; i < distinct; ++i) {
    const std::uint8_t symbol = payload[1 + 2 * i];
    const std::uint8_t length = payload[2 + 2 * i];
    if (length == 0 || length > huffman::kMaxCodeLength) throw CorruptInput("illegal code length");
    if (lengths[symbol] != 0) throw CorruptInput("duplicate symbol in table");
    lengths[symbol] = length;
  }

  const huffman::DecodeTree tree(lengths);
  const std::uint8_t* bits = payload + tableSize;
  const std::size_t bitsSize = payloadSize - tableSize;

  std::vector<std::uint8_t> out;
  // Every symbol costs at least one bit, so the payload bounds how many symbols can
  // possibly follow. Reserving the smaller of the two stops a corrupt header claiming a
  // 2^64-byte block from turning into an allocation the size of that claim.
  out.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(originalSize, bitsSize * 8)));

  BitReader reader(bits, bitsSize);
  for (std::uint64_t i = 0; i < originalSize; ++i) {
    int node = huffman::DecodeTree::kRoot;
    do {
      const int next = tree.step(node, reader.readBit());
      // A missing edge means the bit stream took a path the encoder never wrote. Only
      // reachable with a corrupted file -- and it is reachable, because a length-limited
      // code can leave part of the code space unused.
      if (next < 0) throw CorruptInput("bit pattern is not a valid code");
      node = next;
    } while (!tree.isLeaf(node));
    out.push_back(static_cast<std::uint8_t>(tree.symbol(node)));
  }
  return out;
}

// ------------------------------------------------------------------- LZ77 (method 2)

constexpr std::size_t kLz77ParamBytes = 2;  // window bits, min match

std::vector<std::uint8_t> encodeLz77(const std::uint8_t* data, std::size_t size,
                                     const lz77::Config& config, std::size_t budgetBytes) {
  const std::vector<lz77::Token> tokens = lz77::tokenize(data, size, config);

  std::size_t literals = 0;
  std::size_t matches = 0;
  for (const lz77::Token& token : tokens) {
    if (token.isMatch()) {
      ++matches;
    } else {
      ++literals;
    }
  }
  const std::size_t flagBytes = (tokens.size() + 7) / 8;
  const std::size_t total = kLz77ParamBytes + flagBytes + literals + 3 * matches;
  if (total >= budgetBytes) return {};

  std::vector<std::uint8_t> out;
  out.reserve(total);
  out.push_back(static_cast<std::uint8_t>(config.windowBits));
  out.push_back(static_cast<std::uint8_t>(config.minMatch));

  // Flag bits go into a byte reserved eight tokens in advance and patched once the group
  // is complete. The alternative -- two passes, or a separate flag buffer stitched on
  // afterwards -- costs either time or a second allocation for no benefit.
  std::size_t flagIndex = 0;
  std::uint8_t flags = 0;
  int inGroup = 0;

  for (const lz77::Token& token : tokens) {
    if (inGroup == 0) {
      flagIndex = out.size();
      out.push_back(0);
      flags = 0;
    }
    if (token.isMatch()) {
      flags |= static_cast<std::uint8_t>(0x80u >> inGroup);
      const std::uint16_t encodedDistance = static_cast<std::uint16_t>(token.distance - 1);
      out.push_back(static_cast<std::uint8_t>(encodedDistance & 0xFF));
      out.push_back(static_cast<std::uint8_t>(encodedDistance >> 8));
      out.push_back(static_cast<std::uint8_t>(token.length - config.minMatch));
    } else {
      out.push_back(token.literal);
    }
    if (++inGroup == 8) {
      out[flagIndex] = flags;
      inGroup = 0;
    }
  }
  if (inGroup != 0) out[flagIndex] = flags;
  return out;
}

std::vector<std::uint8_t> decodeLz77(const std::uint8_t* payload, std::size_t payloadSize,
                                     std::uint64_t originalSize) {
  if (payloadSize < kLz77ParamBytes) throw CorruptInput("truncated LZ77 parameters");
  const int windowBits = payload[0];
  const int minMatch = payload[1];
  if (windowBits < 8 || windowBits > 16) throw CorruptInput("illegal window size");
  if (minMatch < 3 || minMatch > 8) throw CorruptInput("illegal minimum match length");
  const std::size_t windowSize = std::size_t{1} << windowBits;

  std::size_t at = kLz77ParamBytes;
  const auto take = [&](std::size_t count) {
    if (payloadSize - at < count) throw CorruptInput("truncated LZ77 token stream");
    const std::uint8_t* p = payload + at;
    at += count;
    return p;
  };

  std::vector<std::uint8_t> out;
  const std::uint64_t expansionBound =
      static_cast<std::uint64_t>(payloadSize) * static_cast<std::uint64_t>(minMatch + 255);
  out.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(originalSize, expansionBound)));

  std::uint8_t flags = 0;
  int remainingInGroup = 0;

  while (out.size() < originalSize) {
    if (remainingInGroup == 0) {
      flags = *take(1);
      remainingInGroup = 8;
    }
    const bool isMatch = (flags & 0x80u) != 0;
    flags = static_cast<std::uint8_t>(flags << 1);
    --remainingInGroup;

    if (!isMatch) {
      out.push_back(*take(1));
      continue;
    }

    const std::uint8_t* p = take(3);
    const std::size_t distance =
        static_cast<std::size_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8)) + 1;
    const std::size_t length = static_cast<std::size_t>(p[2]) + static_cast<std::size_t>(minMatch);

    if (distance > out.size()) throw CorruptInput("back-reference points before the output");
    if (distance > windowSize) throw CorruptInput("back-reference exceeds the window");
    if (out.size() + length > originalSize) throw CorruptInput("token stream overruns the output");

    const std::size_t start = out.size() - distance;
    // Byte at a time, deliberately: when distance < length the source overlaps the
    // destination and each copied byte becomes the source for a later one, which is
    // exactly how a run is encoded. A memcpy would read the pre-copy contents.
    for (std::size_t i = 0; i < length; ++i) out.push_back(out[start + i]);
  }
  return out;
}

}  // namespace

EncodedBlock encodeBlock(const std::uint8_t* data, std::size_t size, Algorithm algorithm,
                         const lz77::Config& config) {
  EncodedBlock block;
  if (size == 0) return block;  // stored, empty

  std::vector<std::uint8_t> payload;
  Method method = Method::kStored;
  switch (algorithm) {
    case Algorithm::kHuffman:
      payload = encodeHuffman(data, size, size);
      method = Method::kHuffman;
      break;
    case Algorithm::kLz77:
      payload = encodeLz77(data, size, config, size);
      method = Method::kLz77;
      break;
    case Algorithm::kDeflate:
    default:
      payload = deflate::encode(data, size, config, size);
      method = Method::kDeflate;
      break;
  }

  // Incompressible input is a case to handle, not a failure: high-entropy data (JPEG,
  // MP4, an archive) has neither skew for Huffman nor repetition for LZ77. Storing raw is
  // what real compressors do here, and ties go to STORED because it decodes faster too.
  if (payload.empty()) {
    block.method = Method::kStored;
    block.payload.assign(data, data + size);
    return block;
  }
  block.method = method;
  block.payload = std::move(payload);
  return block;
}

std::vector<std::uint8_t> decodeBlock(Method method, const std::uint8_t* payload,
                                      std::size_t payloadSize, std::uint64_t uncompressedSize) {
  switch (method) {
    case Method::kStored:
      if (payloadSize != uncompressedSize) {
        throw CorruptInput("stored payload size disagrees with the framing");
      }
      return std::vector<std::uint8_t>(payload, payload + payloadSize);
    case Method::kHuffman:
      return decodeHuffman(payload, payloadSize, uncompressedSize);
    case Method::kLz77:
      return decodeLz77(payload, payloadSize, uncompressedSize);
    case Method::kDeflate:
      return deflate::decode(payload, payloadSize, uncompressedSize);
    default:
      throw CorruptInput("unknown compression method");
  }
}

}  // namespace cmpr
