#include "codec.h"

#include <algorithm>
#include <cstring>

#include "bitio.h"
#include "huffman.h"

namespace cmpr {
namespace {

void putHeader(std::vector<std::uint8_t>& out, Method method, std::uint64_t originalSize) {
  out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
  out.push_back(kFormatVersion);
  out.push_back(static_cast<std::uint8_t>(method));
  // Written a byte at a time rather than memcpy'ing a uint64: the format is
  // little-endian regardless of what the host happens to be.
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>((originalSize >> (8 * i)) & 0xFF));
  }
}

std::uint64_t readU64LE(const std::uint8_t* p) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(p[i]) << (8 * i);
  return value;
}

std::vector<std::uint8_t> storeRaw(const std::uint8_t* data, std::size_t size) {
  std::vector<std::uint8_t> out;
  out.reserve(kHeaderSize + size);
  putHeader(out, Method::kStored, size);
  out.insert(out.end(), data, data + size);
  return out;
}

// ---------------------------------------------------------------- Huffman (method 1)

std::vector<std::uint8_t> compressHuffman(const std::uint8_t* data, std::size_t size) {
  const huffman::FreqTable freq = huffman::countFrequencies(data, size);
  const huffman::LengthTable lengths = huffman::buildLengths(freq);
  const huffman::CodeTable codes = huffman::buildCanonicalCodes(lengths);

  std::size_t distinct = 0;
  std::uint64_t payloadBits = 0;
  for (int symbol = 0; symbol < huffman::kAlphabetSize; ++symbol) {
    const std::uint8_t length = lengths[static_cast<std::size_t>(symbol)];
    if (length == 0) continue;
    ++distinct;
    payloadBits += freq[static_cast<std::size_t>(symbol)] * length;
  }

  // The exact compressed size is known before encoding anything: the code lengths and
  // the frequencies are all it depends on. So the "would this even help?" question gets
  // answered by arithmetic instead of by encoding the file and measuring, which matters
  // for already-compressed input where the encode would be pure waste.
  const std::size_t tableSize = 1 + 2 * distinct;
  const std::size_t total =
      kHeaderSize + tableSize + static_cast<std::size_t>((payloadBits + 7) / 8);
  if (total >= kHeaderSize + size) return storeRaw(data, size);

  std::vector<std::uint8_t> out;
  out.reserve(total);
  putHeader(out, Method::kHuffman, size);

  out.push_back(static_cast<std::uint8_t>(distinct - 1));  // 1..256 stored as 0..255
  for (int symbol = 0; symbol < huffman::kAlphabetSize; ++symbol) {
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

std::vector<std::uint8_t> decompressHuffman(const std::uint8_t* payload, std::size_t payloadSize,
                                            std::uint64_t originalSize) {
  if (payloadSize < 1) throw CorruptInput("missing symbol table");

  const std::size_t distinct = static_cast<std::size_t>(payload[0]) + 1;
  const std::size_t tableSize = 1 + 2 * distinct;
  if (payloadSize < tableSize) throw CorruptInput("truncated symbol table");

  huffman::LengthTable lengths{};
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
  // 2^64-byte file from turning into an allocation the size of that claim.
  out.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(originalSize, bitsSize * 8)));

  BitReader reader(bits, bitsSize);
  for (std::uint64_t i = 0; i < originalSize; ++i) {
    int node = huffman::DecodeTree::kRoot;
    do {
      const int next = tree.step(node, reader.readBit());
      // A missing edge means the bit stream took a path the encoder never wrote. That is
      // only reachable with a corrupted file -- and it is reachable, because a
      // length-limited code can leave part of the code space unused.
      if (next < 0) throw CorruptInput("bit pattern is not a valid code");
      node = next;
    } while (!tree.isLeaf(node));
    out.push_back(static_cast<std::uint8_t>(tree.symbol(node)));
  }
  return out;
}

// ------------------------------------------------------------------- LZ77 (method 2)

constexpr std::size_t kLz77ParamBytes = 2;  // window bits, min match

std::vector<std::uint8_t> compressLz77(const std::uint8_t* data, std::size_t size,
                                       const lz77::Config& config) {
  const std::vector<lz77::Token> tokens = lz77::tokenize(data, size, config);

  // As with Huffman, the serialised size is known from the token counts alone, so the
  // stored-versus-encoded decision costs no extra work.
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
  const std::size_t total = kHeaderSize + kLz77ParamBytes + flagBytes + literals + 3 * matches;
  if (total >= kHeaderSize + size) return storeRaw(data, size);

  std::vector<std::uint8_t> out;
  out.reserve(total);
  putHeader(out, Method::kLz77, size);
  out.push_back(static_cast<std::uint8_t>(config.windowBits));
  out.push_back(static_cast<std::uint8_t>(config.minMatch));

  // Flag bits are written into a byte reserved eight tokens in advance and patched once
  // the group is complete. The alternative -- two passes, or a separate flag buffer
  // stitched on afterwards -- costs either time or a second allocation for no benefit.
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

std::vector<std::uint8_t> decompressLz77(const std::uint8_t* payload, std::size_t payloadSize,
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
  // A match expands at most (minMatch + 255) output bytes from 3 payload bytes, so the
  // payload bounds the output the same way it does for Huffman. Without this a corrupt
  // header could ask for a 16-exabyte reservation.
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
    // Byte at a time, deliberately. When distance < length the source overlaps the
    // destination and each copied byte becomes the source for a later one -- which is
    // exactly how "distance 1, length 200" expresses a 200-byte run. A memcpy would read
    // the pre-copy contents and get this wrong.
    for (std::size_t i = 0; i < length; ++i) out.push_back(out[start + i]);
  }
  return out;
}

}  // namespace

std::vector<std::uint8_t> compress(const std::uint8_t* data, std::size_t size,
                                   const Options& options) {
  if (size == 0) return storeRaw(data, 0);
  switch (options.algorithm) {
    case Algorithm::kLz77:
      return compressLz77(data, size, options.lz77);
    case Algorithm::kHuffman:
    default:
      return compressHuffman(data, size);
  }
}

std::vector<std::uint8_t> decompress(const std::uint8_t* data, std::size_t size) {
  if (size < kHeaderSize) throw CorruptInput("input is shorter than the header");
  if (std::memcmp(data, kMagic, sizeof(kMagic)) != 0) throw CorruptInput("bad magic");
  if (data[4] != kFormatVersion) throw CorruptInput("unsupported format version");

  const auto method = static_cast<Method>(data[5]);
  const std::uint64_t originalSize = readU64LE(data + 6);
  const std::uint8_t* payload = data + kHeaderSize;
  const std::size_t payloadSize = size - kHeaderSize;

  switch (method) {
    case Method::kStored:
      if (payloadSize != originalSize) throw CorruptInput("stored payload size disagrees with header");
      return std::vector<std::uint8_t>(payload, payload + payloadSize);
    case Method::kHuffman:
      return decompressHuffman(payload, payloadSize, originalSize);
    case Method::kLz77:
      return decompressLz77(payload, payloadSize, originalSize);
    default:
      throw CorruptInput("unknown compression method");
  }
}

std::vector<std::uint8_t> compress(const std::vector<std::uint8_t>& input, const Options& options) {
  return compress(input.data(), input.size(), options);
}

std::vector<std::uint8_t> decompress(const std::vector<std::uint8_t>& input) {
  return decompress(input.data(), input.size());
}

Method methodOf(const std::vector<std::uint8_t>& compressed) {
  if (compressed.size() < kHeaderSize) throw CorruptInput("input is shorter than the header");
  return static_cast<Method>(compressed[5]);
}

const char* methodName(Method method) {
  switch (method) {
    case Method::kStored: return "stored";
    case Method::kHuffman: return "huffman";
    case Method::kLz77: return "lz77";
    default: return "unknown";
  }
}

}  // namespace cmpr
