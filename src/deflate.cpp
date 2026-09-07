#include "deflate.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

#include "bitio.h"
#include "huffman.h"

namespace cmpr {
namespace deflate {
namespace {

// RFC 1951 section 3.2.5. Symbol 257 + i covers lengths starting at kLengthBase[i], with
// kLengthExtra[i] bits selecting within that range. Note the ranges double in width every
// four symbols, and that 258 gets its own zero-extra-bit symbol at the end -- the maximum
// match is common enough to be worth a dedicated code rather than the tail of a range.
constexpr int kLengthCodes = 29;
constexpr int kLengthBase[kLengthCodes] = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                           15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                           67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr int kLengthExtra[kLengthCodes] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                            2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};

constexpr int kDistBase[kDistAlphabetSize] = {
    1,    2,    3,    4,    5,    7,     9,     13,    17,   25,
    33,   49,   65,   97,   129,  193,   257,   385,   513,  769,
    1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
constexpr int kDistExtra[kDistAlphabetSize] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                                               4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                                               9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// Reverse lookups, so encoding a length or distance is an array index rather than a scan
// over 30 ranges on the hot path.
struct ReverseTables {
  std::array<std::uint8_t, kMaxMatch + 1> lengthIndex{};  // indexed by length, 3..258

  // Distances split in two levels rather than one 32768-entry table. Above 256 every
  // code's range is a whole number of 128-byte buckets, so the top bits alone identify
  // the code. 512 bytes total instead of 32 KiB, which is the difference between living
  // in L1 and not. zlib uses exactly this trick.
  std::array<std::uint8_t, 256> distLow{};   // distance 1..256, indexed by (d - 1)
  std::array<std::uint8_t, 256> distHigh{};  // distance 257..32768, by (d - 1) >> 7

  ReverseTables() {
    for (int i = 0; i < kLengthCodes; ++i) {
      const int last = (i + 1 < kLengthCodes) ? kLengthBase[i + 1] - 1 : kMaxMatch;
      for (int length = kLengthBase[i]; length <= last; ++length) {
        lengthIndex[static_cast<std::size_t>(length)] = static_cast<std::uint8_t>(i);
      }
    }
    for (int code = 0; code < kDistAlphabetSize; ++code) {
      const int last =
          (code + 1 < kDistAlphabetSize) ? kDistBase[code + 1] - 1 : (1 << kMaxWindowBits);
      for (int distance = kDistBase[code]; distance <= last; ++distance) {
        if (distance <= 256) {
          distLow[static_cast<std::size_t>(distance - 1)] = static_cast<std::uint8_t>(code);
        } else {
          distHigh[static_cast<std::size_t>((distance - 1) >> 7)] = static_cast<std::uint8_t>(code);
        }
      }
    }
  }
};

const ReverseTables& tables() {
  static const ReverseTables instance;
  return instance;
}

constexpr std::size_t kParamBytes = 4;  // window bits, HLIT (2), HDIST

void writeU16LE(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFF));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
}

// Number of leading entries that must be stored: everything after the last symbol that
// actually got a code is zero and can be dropped. This is what RFC 1951 calls HLIT/HDIST.
int usedLength(const huffman::LengthTable& lengths) {
  int used = 0;
  for (int symbol = 0; symbol < static_cast<int>(lengths.size()); ++symbol) {
    if (lengths[static_cast<std::size_t>(symbol)] != 0) used = symbol + 1;
  }
  return used;
}

}  // namespace

Split splitLength(int length) {
  if (length < kMinMatch || length > kMaxMatch) {
    throw std::invalid_argument("match length outside the DEFLATE code table");
  }
  const int index = tables().lengthIndex[static_cast<std::size_t>(length)];
  Split split;
  split.symbol = kFirstLengthSymbol + index;
  split.extraBits = kLengthExtra[index];
  split.extraValue = static_cast<std::uint32_t>(length - kLengthBase[index]);
  return split;
}

Split splitDistance(int distance) {
  if (distance < 1 || distance > (1 << kMaxWindowBits)) {
    throw std::invalid_argument("match distance outside the DEFLATE code table");
  }
  const int code = distance <= 256
                       ? tables().distLow[static_cast<std::size_t>(distance - 1)]
                       : tables().distHigh[static_cast<std::size_t>((distance - 1) >> 7)];
  Split split;
  split.symbol = code;
  split.extraBits = kDistExtra[code];
  split.extraValue = static_cast<std::uint32_t>(distance - kDistBase[code]);
  return split;
}

int lengthBase(int symbol) { return kLengthBase[symbol - kFirstLengthSymbol]; }
int lengthExtraBits(int symbol) { return kLengthExtra[symbol - kFirstLengthSymbol]; }
int distanceBase(int code) { return kDistBase[code]; }
int distanceExtraBits(int code) { return kDistExtra[code]; }

std::vector<std::uint8_t> encode(const std::uint8_t* data, std::size_t size,
                                 const lz77::Config& config, std::size_t budgetBytes) {
  // The code tables are the constraint, not a preference. Phase 2's sweep found minMatch
  // 3 and 4 within 0.02% of each other and everything above that worse, so being pinned
  // to 3 costs nothing measurable.
  if (config.minMatch != kMinMatch) {
    throw std::invalid_argument("DEFLATE length codes require minMatch == 3");
  }
  if (config.windowBits > kMaxWindowBits) {
    throw std::invalid_argument("DEFLATE distance codes reach 32768; windowBits must be <= 15");
  }

  const std::vector<lz77::Token> tokens = lz77::tokenize(data, size, config);

  huffman::FreqTable litLenFreq(kLitLenAlphabetSize, 0);
  huffman::FreqTable distFreq(kDistAlphabetSize, 0);
  std::uint64_t extraBits = 0;

  for (const lz77::Token& token : tokens) {
    if (!token.isMatch()) {
      ++litLenFreq[token.literal];
      continue;
    }
    const Split length = splitLength(token.length);
    const Split distance = splitDistance(token.distance);
    ++litLenFreq[static_cast<std::size_t>(length.symbol)];
    ++distFreq[static_cast<std::size_t>(distance.symbol)];
    extraBits += static_cast<std::uint64_t>(length.extraBits) +
                 static_cast<std::uint64_t>(distance.extraBits);
  }

  const huffman::LengthTable litLenLengths = huffman::buildLengths(litLenFreq);
  const huffman::LengthTable distLengths = huffman::buildLengths(distFreq);
  const huffman::CodeTable litLenCodes = huffman::buildCanonicalCodes(litLenLengths);
  const huffman::CodeTable distCodes = huffman::buildCanonicalCodes(distLengths);

  const int hlit = usedLength(litLenLengths);
  const int hdist = usedLength(distLengths);

  // Same discipline as the earlier phases: the exact output size follows from the code
  // lengths and the frequencies, so whether this is worth doing at all is settled by
  // arithmetic before a single bit is written.
  std::uint64_t payloadBits = extraBits;
  for (int symbol = 0; symbol < kLitLenAlphabetSize; ++symbol) {
    payloadBits += litLenFreq[static_cast<std::size_t>(symbol)] *
                   litLenLengths[static_cast<std::size_t>(symbol)];
  }
  for (int symbol = 0; symbol < kDistAlphabetSize; ++symbol) {
    payloadBits +=
        distFreq[static_cast<std::size_t>(symbol)] * distLengths[static_cast<std::size_t>(symbol)];
  }

  const std::size_t tableBytes = kParamBytes + static_cast<std::size_t>(hlit) +
                                 static_cast<std::size_t>(hdist);
  const std::size_t total = tableBytes + static_cast<std::size_t>((payloadBits + 7) / 8);
  if (total >= budgetBytes) return {};

  std::vector<std::uint8_t> out;
  out.reserve(total);
  out.push_back(static_cast<std::uint8_t>(config.windowBits));
  writeU16LE(out, static_cast<std::uint16_t>(hlit));
  out.push_back(static_cast<std::uint8_t>(hdist));
  for (int symbol = 0; symbol < hlit; ++symbol) {
    out.push_back(litLenLengths[static_cast<std::size_t>(symbol)]);
  }
  for (int symbol = 0; symbol < hdist; ++symbol) {
    out.push_back(distLengths[static_cast<std::size_t>(symbol)]);
  }

  BitWriter writer(out);
  for (const lz77::Token& token : tokens) {
    if (!token.isMatch()) {
      const huffman::Code& code = litLenCodes[token.literal];
      writer.writeBits(code.bits, code.length);
      continue;
    }
    const Split length = splitLength(token.length);
    const huffman::Code& lengthCode = litLenCodes[static_cast<std::size_t>(length.symbol)];
    writer.writeBits(lengthCode.bits, lengthCode.length);
    writer.writeBits(length.extraValue, length.extraBits);

    const Split distance = splitDistance(token.distance);
    const huffman::Code& distanceCode = distCodes[static_cast<std::size_t>(distance.symbol)];
    writer.writeBits(distanceCode.bits, distanceCode.length);
    writer.writeBits(distance.extraValue, distance.extraBits);
  }
  writer.flush();

  return out;
}

std::vector<std::uint8_t> decode(const std::uint8_t* payload, std::size_t payloadSize,
                                 std::uint64_t originalSize) {
  if (payloadSize < kParamBytes) throw CorruptInput("truncated DEFLATE parameters");

  const int windowBits = payload[0];
  const int hlit = static_cast<int>(payload[1]) | (static_cast<int>(payload[2]) << 8);
  const int hdist = payload[3];

  if (windowBits < 8 || windowBits > kMaxWindowBits) throw CorruptInput("illegal window size");
  if (hlit < 1 || hlit > kLitLenAlphabetSize) throw CorruptInput("illegal literal/length table size");
  if (hdist > kDistAlphabetSize) throw CorruptInput("illegal distance table size");

  const std::size_t tableBytes =
      kParamBytes + static_cast<std::size_t>(hlit) + static_cast<std::size_t>(hdist);
  if (payloadSize < tableBytes) throw CorruptInput("truncated code length tables");

  huffman::LengthTable litLenLengths(kLitLenAlphabetSize, 0);
  huffman::LengthTable distLengths(kDistAlphabetSize, 0);
  for (int symbol = 0; symbol < hlit; ++symbol) {
    const std::uint8_t length = payload[kParamBytes + static_cast<std::size_t>(symbol)];
    if (length > huffman::kMaxCodeLength) throw CorruptInput("illegal code length");
    litLenLengths[static_cast<std::size_t>(symbol)] = length;
  }
  for (int symbol = 0; symbol < hdist; ++symbol) {
    const std::uint8_t length =
        payload[kParamBytes + static_cast<std::size_t>(hlit) + static_cast<std::size_t>(symbol)];
    if (length > huffman::kMaxCodeLength) throw CorruptInput("illegal code length");
    distLengths[static_cast<std::size_t>(symbol)] = length;
  }

  const huffman::DecodeTree litLenTree(litLenLengths);
  const huffman::DecodeTree distTree(distLengths);

  const std::uint8_t* bits = payload + tableBytes;
  const std::size_t bitsSize = payloadSize - tableBytes;
  BitReader reader(bits, bitsSize);

  std::vector<std::uint8_t> out;
  // A single symbol can expand to kMaxMatch output bytes and costs at least one bit, so
  // the payload bounds the output. Without this a corrupt header could request an
  // arbitrary allocation.
  const std::uint64_t expansionBound =
      static_cast<std::uint64_t>(bitsSize) * 8 * static_cast<std::uint64_t>(kMaxMatch);
  out.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(originalSize, expansionBound)));

  const std::size_t windowSize = std::size_t{1} << windowBits;

  const auto decodeSymbol = [&reader](const huffman::DecodeTree& tree) {
    int node = huffman::DecodeTree::kRoot;
    do {
      const int next = tree.step(node, reader.readBit());
      if (next < 0) throw CorruptInput("bit pattern is not a valid code");
      node = next;
    } while (!tree.isLeaf(node));
    return tree.symbol(node);
  };

  while (out.size() < originalSize) {
    const int symbol = decodeSymbol(litLenTree);

    if (symbol < 256) {
      out.push_back(static_cast<std::uint8_t>(symbol));
      continue;
    }
    // The slot is kept for RFC 1951 numbering but this encoder never emits it, so seeing
    // one means the stream is not ours.
    if (symbol == kEndOfBlockSymbol) throw CorruptInput("unexpected end-of-block symbol");
    if (symbol >= kLitLenAlphabetSize) throw CorruptInput("literal/length symbol out of range");

    const int length =
        lengthBase(symbol) + static_cast<int>(reader.readBits(lengthExtraBits(symbol)));

    const int distanceCode = decodeSymbol(distTree);
    if (distanceCode >= kDistAlphabetSize) throw CorruptInput("distance symbol out of range");
    const std::size_t distance = static_cast<std::size_t>(distanceBase(distanceCode)) +
                                 reader.readBits(distanceExtraBits(distanceCode));

    if (distance > out.size()) throw CorruptInput("back-reference points before the output");
    if (distance > windowSize) throw CorruptInput("back-reference exceeds the window");
    if (out.size() + static_cast<std::size_t>(length) > originalSize) {
      throw CorruptInput("token stream overruns the output");
    }

    // Byte at a time: when distance < length the source overlaps the destination, and
    // each copied byte becomes the source for a later one. That is how a run is encoded.
    const std::size_t start = out.size() - distance;
    for (int i = 0; i < length; ++i) out.push_back(out[start + static_cast<std::size_t>(i)]);
  }
  return out;
}

}  // namespace deflate
}  // namespace cmpr
