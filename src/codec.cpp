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

}  // namespace

std::vector<std::uint8_t> compress(const std::uint8_t* data, std::size_t size) {
  if (size == 0) return storeRaw(data, 0);

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
  const std::size_t huffmanSize = kHeaderSize + tableSize + static_cast<std::size_t>((payloadBits + 7) / 8);

  // Incompressible input is not a failure case, it is a case to handle: high-entropy
  // data (JPEG, MP4, an archive) has a near-flat byte distribution, every code comes out
  // 8 bits, and the symbol table is pure overhead. Real compressors fall back to storing
  // raw for exactly this reason, and the benchmark suite deliberately includes such
  // files. Ties go to STORED because it is also faster to decode.
  if (huffmanSize >= kHeaderSize + size) return storeRaw(data, size);

  std::vector<std::uint8_t> out;
  out.reserve(huffmanSize);
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

std::vector<std::uint8_t> decompress(const std::uint8_t* data, std::size_t size) {
  if (size < kHeaderSize) throw CorruptInput("input is shorter than the header");
  if (std::memcmp(data, kMagic, sizeof(kMagic)) != 0) throw CorruptInput("bad magic");
  if (data[4] != kFormatVersion) throw CorruptInput("unsupported format version");

  const auto method = static_cast<Method>(data[5]);
  const std::uint64_t originalSize = readU64LE(data + 6);

  const std::uint8_t* payload = data + kHeaderSize;
  const std::size_t payloadSize = size - kHeaderSize;

  if (method == Method::kStored) {
    if (payloadSize != originalSize) throw CorruptInput("stored payload size disagrees with header");
    return std::vector<std::uint8_t>(payload, payload + payloadSize);
  }
  if (method != Method::kHuffman) throw CorruptInput("unknown compression method");
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
  // 2^64-byte file from turning into an allocation the size of the header claims.
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

std::vector<std::uint8_t> compress(const std::vector<std::uint8_t>& input) {
  return compress(input.data(), input.size());
}

std::vector<std::uint8_t> decompress(const std::vector<std::uint8_t>& input) {
  return decompress(input.data(), input.size());
}

Method methodOf(const std::vector<std::uint8_t>& compressed) {
  if (compressed.size() < kHeaderSize) throw CorruptInput("input is shorter than the header");
  return static_cast<Method>(compressed[5]);
}

}  // namespace cmpr
