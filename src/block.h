#pragma once
//
// Encoding and decoding of a single block, independent of any container framing.
//
// This is the unit everything above is built from. One block is compressed with no
// reference to any other -- no shared history, no shared Huffman trees -- which is what
// makes the later phases possible: streaming can hold one block in memory at a time, and
// parallel compression can hand different blocks to different threads without any
// coordination beyond assembling the results in order.
//
// Independence is not free. Matches cannot cross a block boundary, so some redundancy
// goes unexploited. It buys something back, though: each block gets Huffman trees fitted
// to its own contents rather than to the average of the whole file. Which effect wins
// depends on the block size, and NOTES.md has the measurement.
//
#include <cstddef>
#include <cstdint>
#include <vector>

#include "format.h"
#include "lz77.h"

namespace cmpr {

enum class Algorithm {
  kHuffman,  // Phase 1: order-0 Huffman coding
  kLz77,     // Phase 2: LZ77 with byte-aligned LZSS framing
  kDeflate,  // Phase 3: LZ77 then Huffman -- structurally what gzip does
};

struct EncodedBlock {
  Method method = Method::kStored;
  std::vector<std::uint8_t> payload;
};

// Compresses one block. Falls back to Method::kStored -- payload identical to the input --
// whenever the chosen algorithm would not come out smaller, so a block never grows.
EncodedBlock encodeBlock(const std::uint8_t* data, std::size_t size, Algorithm algorithm,
                         const lz77::Config& config);

// Reverses encodeBlock. `uncompressedSize` is the size the block decodes to, which the
// caller knows from the framing.
std::vector<std::uint8_t> decodeBlock(Method method, const std::uint8_t* payload,
                                      std::size_t payloadSize, std::uint64_t uncompressedSize);

}  // namespace cmpr
