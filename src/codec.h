#pragma once
//
// The top-level compress/decompress entry points, and the container framing that decides
// between an actual encode and storing the input raw.
//
#include <cstdint>
#include <vector>

#include "deflate.h"
#include "format.h"
#include "lz77.h"

namespace cmpr {

enum class Algorithm {
  kHuffman,  // Phase 1: order-0 Huffman coding
  kLz77,     // Phase 2: LZ77 with byte-aligned LZSS framing
  kDeflate,  // Phase 3: LZ77 then Huffman -- structurally what gzip does
};

struct Options {
  // Defaults to the combined codec: it beats the other two on every input measured, and
  // the earlier ones are kept reachable because the comparison between them is the point.
  Algorithm algorithm = Algorithm::kDeflate;
  lz77::Config lz77;
};

// Compresses `size` bytes. Never larger than the input plus kHeaderSize: if the chosen
// algorithm would not pay for itself the input is stored verbatim instead.
std::vector<std::uint8_t> compress(const std::uint8_t* data, std::size_t size,
                                   const Options& options = Options{});

// Reverses compress(), whichever method it chose -- the file says which. Throws
// CorruptInput on anything this codec did not produce.
std::vector<std::uint8_t> decompress(const std::uint8_t* data, std::size_t size);

std::vector<std::uint8_t> compress(const std::vector<std::uint8_t>& input,
                                   const Options& options = Options{});
std::vector<std::uint8_t> decompress(const std::vector<std::uint8_t>& input);

// Which branch compress() took. Useful for tests and for reporting in benchmarks.
Method methodOf(const std::vector<std::uint8_t>& compressed);
const char* methodName(Method method);

}  // namespace cmpr
