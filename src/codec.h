#pragma once
//
// The top-level compress/decompress entry points, and the container framing that decides
// between an actual Huffman encode and storing the input raw.
//
#include <cstdint>
#include <vector>

#include "format.h"

namespace cmpr {

// Compresses `size` bytes. Never larger than the input plus kHeaderSize: if Huffman
// coding would not pay for itself the input is stored verbatim instead.
std::vector<std::uint8_t> compress(const std::uint8_t* data, std::size_t size);

// Reverses compress(). Throws CorruptInput on anything this codec did not produce.
std::vector<std::uint8_t> decompress(const std::uint8_t* data, std::size_t size);

std::vector<std::uint8_t> compress(const std::vector<std::uint8_t>& input);
std::vector<std::uint8_t> decompress(const std::vector<std::uint8_t>& input);

// Which branch compress() took. Useful for tests and for reporting in benchmarks.
Method methodOf(const std::vector<std::uint8_t>& compressed);

}  // namespace cmpr
