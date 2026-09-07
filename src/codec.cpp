#include "codec.h"

#include <algorithm>
#include <cstring>
#include <istream>
#include <ostream>
#include <stdexcept>

#include "threadpool.h"

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

std::uint32_t readU32LE(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void appendU32LE(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
}

// Per-block framing for method 4: uncompressed length, payload length, method byte.
// Nine bytes per block -- 0.001% at the default 1 MiB block size.
constexpr std::size_t kBlockHeaderSize = 9;

void appendBlock(std::vector<std::uint8_t>& out, std::size_t uncompressedSize,
                 const EncodedBlock& block) {
  appendU32LE(out, static_cast<std::uint32_t>(uncompressedSize));
  appendU32LE(out, static_cast<std::uint32_t>(block.payload.size()));
  out.push_back(static_cast<std::uint8_t>(block.method));
  out.insert(out.end(), block.payload.begin(), block.payload.end());
}

std::size_t blockCountFor(std::uint64_t size, std::size_t blockSize) {
  return static_cast<std::size_t>((size + blockSize - 1) / blockSize);
}

bool shouldBlock(std::size_t size, const Options& options) {
  return options.blockSize > 0 && size > options.blockSize;
}

// The most expansive thing any method can do is turn one bit into kMaxMatch output bytes.
// Used to bound allocations against a header claiming an implausible size.
constexpr std::uint64_t kMaxExpansionPerPayloadByte = 8 * 258;

std::vector<std::uint8_t> compressSingleBlock(const std::uint8_t* data, std::size_t size,
                                              const Options& options) {
  const EncodedBlock block = encodeBlock(data, size, options.algorithm, options.lz77);
  std::vector<std::uint8_t> out;
  out.reserve(kHeaderSize + block.payload.size());
  putHeader(out, block.method, size);
  out.insert(out.end(), block.payload.begin(), block.payload.end());
  return out;
}

std::vector<std::uint8_t> compressBlocked(const std::uint8_t* data, std::size_t size,
                                          const Options& options) {
  const std::size_t blocks = blockCountFor(size, options.blockSize);

  // Blocks are compressed in whatever order threads pick them up, but assembled strictly
  // in index order, so the output does not depend on the thread count or on how the
  // scheduler interleaved things. That is what lets a 1-thread and an 8-thread run be
  // compared byte for byte -- and a test asserts exactly that.
  std::vector<EncodedBlock> encoded(blocks);
  const auto compressOne = [&](std::size_t index) {
    const std::size_t offset = index * options.blockSize;
    const std::size_t length = std::min(options.blockSize, size - offset);
    encoded[index] = encodeBlock(data + offset, length, options.algorithm, options.lz77);
  };

  const int workers = resolveThreadCount(options.threads);
  if (workers <= 1) {
    for (std::size_t index = 0; index < blocks; ++index) compressOne(index);
  } else {
    ThreadPool pool(workers);
    pool.forEach(blocks, compressOne);
  }

  std::size_t total = kHeaderSize;
  for (const EncodedBlock& block : encoded) total += kBlockHeaderSize + block.payload.size();

  std::vector<std::uint8_t> out;
  out.reserve(total);
  putHeader(out, Method::kBlocked, size);
  for (std::size_t index = 0; index < blocks; ++index) {
    const std::size_t offset = index * options.blockSize;
    appendBlock(out, std::min(options.blockSize, size - offset), encoded[index]);
  }
  return out;
}

std::vector<std::uint8_t> decompressBlocked(const std::uint8_t* payload, std::size_t payloadSize,
                                            std::uint64_t originalSize) {
  std::vector<std::uint8_t> out;
  out.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(
      originalSize, static_cast<std::uint64_t>(payloadSize) * kMaxExpansionPerPayloadByte)));

  std::size_t at = 0;
  while (out.size() < originalSize) {
    if (payloadSize - at < kBlockHeaderSize) throw CorruptInput("truncated block header");
    const std::uint32_t uncompressed = readU32LE(payload + at);
    const std::uint32_t compressed = readU32LE(payload + at + 4);
    const auto method = static_cast<Method>(payload[at + 8]);
    at += kBlockHeaderSize;

    if (payloadSize - at < compressed) throw CorruptInput("truncated block payload");
    if (uncompressed > originalSize - out.size()) {
      throw CorruptInput("block overruns the declared output size");
    }

    const std::vector<std::uint8_t> decoded =
        decodeBlock(method, payload + at, compressed, uncompressed);
    if (decoded.size() != uncompressed) throw CorruptInput("block decoded to the wrong size");
    out.insert(out.end(), decoded.begin(), decoded.end());
    at += compressed;
  }
  return out;
}

// Reads up to `count` bytes; returns how many actually arrived.
std::size_t readFully(std::istream& in, std::uint8_t* buffer, std::size_t count) {
  if (count == 0) return 0;
  in.read(reinterpret_cast<char*>(buffer), static_cast<std::streamsize>(count));
  return static_cast<std::size_t>(in.gcount());
}

void writeAll(std::ostream& out, const std::uint8_t* data, std::size_t size) {
  if (size == 0) return;
  out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
  if (!out) throw std::runtime_error("write failed");
}

}  // namespace

std::vector<std::uint8_t> compress(const std::uint8_t* data, std::size_t size,
                                   const Options& options) {
  if (size == 0) {
    std::vector<std::uint8_t> out;
    putHeader(out, Method::kStored, 0);
    return out;
  }
  if (!shouldBlock(size, options)) return compressSingleBlock(data, size, options);
  return compressBlocked(data, size, options);
}

std::vector<std::uint8_t> decompress(const std::uint8_t* data, std::size_t size) {
  if (size < kHeaderSize) throw CorruptInput("input is shorter than the header");
  if (std::memcmp(data, kMagic, sizeof(kMagic)) != 0) throw CorruptInput("bad magic");
  if (data[4] != kFormatVersion) throw CorruptInput("unsupported format version");

  const auto method = static_cast<Method>(data[5]);
  const std::uint64_t originalSize = readU64LE(data + 6);
  const std::uint8_t* payload = data + kHeaderSize;
  const std::size_t payloadSize = size - kHeaderSize;

  if (method == Method::kBlocked) return decompressBlocked(payload, payloadSize, originalSize);
  return decodeBlock(method, payload, payloadSize, originalSize);
}

std::vector<std::uint8_t> compress(const std::vector<std::uint8_t>& input, const Options& options) {
  return compress(input.data(), input.size(), options);
}

std::vector<std::uint8_t> decompress(const std::vector<std::uint8_t>& input) {
  return decompress(input.data(), input.size());
}

void compressStream(std::istream& in, std::ostream& out, std::uint64_t totalSize,
                    const Options& options) {
  const std::size_t blockSize = options.blockSize > 0 ? options.blockSize : kDefaultBlockSize;

  // Small enough to be one block: read it and take the same path the in-memory function
  // takes, so the two produce identical bytes. Peak memory is one input plus one output,
  // which is what a single-block encode costs however it is invoked.
  if (totalSize <= blockSize) {
    std::vector<std::uint8_t> input(static_cast<std::size_t>(totalSize));
    if (readFully(in, input.data(), input.size()) != input.size()) {
      throw std::runtime_error("input ended before the declared size");
    }
    const std::vector<std::uint8_t> compressed = compress(input, options);
    writeAll(out, compressed.data(), compressed.size());
    return;
  }

  std::vector<std::uint8_t> header;
  putHeader(header, Method::kBlocked, totalSize);
  writeAll(out, header.data(), header.size());

  const std::size_t blocks = blockCountFor(totalSize, blockSize);
  const int workers = resolveThreadCount(options.threads);

  // A batch of blocks is read, compressed together, then written in order. Memory stays
  // bounded by (workers x block size) rather than by the file, and the output matches the
  // sequential path because ordering is imposed at write time rather than left to
  // whichever thread finished first.
  const std::size_t batch = workers <= 1 ? 1 : static_cast<std::size_t>(workers);

  std::vector<std::vector<std::uint8_t>> inputs(batch);
  std::vector<EncodedBlock> encoded(batch);
  ThreadPool pool(workers > 1 ? workers : 1);

  std::vector<std::uint8_t> framed;
  std::size_t done = 0;
  while (done < blocks) {
    const std::size_t here = std::min(batch, blocks - done);
    for (std::size_t i = 0; i < here; ++i) {
      const std::uint64_t offset = static_cast<std::uint64_t>(done + i) * blockSize;
      const std::size_t length =
          static_cast<std::size_t>(std::min<std::uint64_t>(blockSize, totalSize - offset));
      inputs[i].resize(length);
      if (readFully(in, inputs[i].data(), length) != length) {
        throw std::runtime_error("input ended before the declared size");
      }
    }

    const auto compressOne = [&](std::size_t i) {
      encoded[i] = encodeBlock(inputs[i].data(), inputs[i].size(), options.algorithm, options.lz77);
    };
    if (workers <= 1) {
      for (std::size_t i = 0; i < here; ++i) compressOne(i);
    } else {
      pool.forEach(here, compressOne);
    }

    for (std::size_t i = 0; i < here; ++i) {
      framed.clear();
      appendBlock(framed, inputs[i].size(), encoded[i]);
      writeAll(out, framed.data(), framed.size());
    }
    done += here;
  }
}

void decompressStream(std::istream& in, std::ostream& out) {
  std::uint8_t header[kHeaderSize];
  if (readFully(in, header, kHeaderSize) != kHeaderSize) {
    throw CorruptInput("input is shorter than the header");
  }
  if (std::memcmp(header, kMagic, sizeof(kMagic)) != 0) throw CorruptInput("bad magic");
  if (header[4] != kFormatVersion) throw CorruptInput("unsupported format version");

  const auto method = static_cast<Method>(header[5]);
  const std::uint64_t originalSize = readU64LE(header + 6);

  if (method != Method::kBlocked) {
    // A single block cannot be decoded incrementally: its Huffman codes and its back
    // references span the whole payload. Reading it whole is the only option -- and it is
    // precisely the situation blocking exists to avoid.
    std::vector<std::uint8_t> payload((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
    const std::vector<std::uint8_t> decoded =
        decodeBlock(method, payload.data(), payload.size(), originalSize);
    writeAll(out, decoded.data(), decoded.size());
    return;
  }

  std::uint64_t produced = 0;
  std::vector<std::uint8_t> payload;
  while (produced < originalSize) {
    std::uint8_t blockHeader[kBlockHeaderSize];
    if (readFully(in, blockHeader, kBlockHeaderSize) != kBlockHeaderSize) {
      throw CorruptInput("truncated block header");
    }
    const std::uint32_t uncompressed = readU32LE(blockHeader);
    const std::uint32_t compressed = readU32LE(blockHeader + 4);
    const auto blockMethod = static_cast<Method>(blockHeader[8]);

    if (uncompressed > originalSize - produced) {
      throw CorruptInput("block overruns the declared output size");
    }
    payload.resize(compressed);
    if (readFully(in, payload.data(), compressed) != compressed) {
      throw CorruptInput("truncated block payload");
    }

    const std::vector<std::uint8_t> decoded =
        decodeBlock(blockMethod, payload.data(), payload.size(), uncompressed);
    if (decoded.size() != uncompressed) throw CorruptInput("block decoded to the wrong size");
    writeAll(out, decoded.data(), decoded.size());
    produced += uncompressed;
  }
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
    case Method::kDeflate: return "deflate";
    case Method::kBlocked: return "blocked";
    default: return "unknown";
  }
}

}  // namespace cmpr
