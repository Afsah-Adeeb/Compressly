#pragma once
//
// The public API: container framing over the block codec in block.h.
//
// A compressed file is either a single block (methods 1-3, the whole input as one unit)
// or a sequence of independent blocks (method 4). Blocking is what makes bounded-memory
// streaming and parallel compression possible, and -- above a certain block size -- it
// improves the ratio rather than costing one, because each block gets Huffman trees
// fitted to its own contents. NOTES.md has the curve.
//
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <vector>

#include "block.h"
#include "format.h"
#include "lz77.h"

namespace cmpr {

// Chosen by measurement, not by round-number appeal: see the block size sweep in
// NOTES.md. Small blocks pay for a set of Huffman tables each; large ones make one set of
// trees cover content it does not fit.
constexpr std::size_t kDefaultBlockSize = std::size_t{1} << 20;  // 1 MiB

struct Options {
  Algorithm algorithm = Algorithm::kDeflate;
  lz77::Config lz77;

  // Input larger than this is cut into independent blocks. 0 disables blocking entirely,
  // which is how the single-block methods stay reachable for comparison.
  std::size_t blockSize = kDefaultBlockSize;

  // Worker threads for block compression. 1 is sequential; 0 means "as many as the
  // hardware reports". Only meaningful when the input spans more than one block, and it
  // never changes the bytes produced -- see NOTES.md on why that determinism matters.
  int threads = 1;
};

// Compresses `size` bytes. Never larger than the input plus kHeaderSize plus per-block
// framing: any block the algorithm cannot shrink is stored verbatim.
std::vector<std::uint8_t> compress(const std::uint8_t* data, std::size_t size,
                                   const Options& options = Options{});

// Reverses compress(), whichever method it chose -- the file says which. Throws
// CorruptInput on anything this codec did not produce.
std::vector<std::uint8_t> decompress(const std::uint8_t* data, std::size_t size);

std::vector<std::uint8_t> compress(const std::vector<std::uint8_t>& input,
                                   const Options& options = Options{});
std::vector<std::uint8_t> decompress(const std::vector<std::uint8_t>& input);

// Streaming forms. Memory use is bounded by the block size rather than the file size, so
// these handle inputs larger than RAM. They produce byte-identical output to the
// in-memory functions above for the same options -- that equivalence is asserted by a
// test, because two code paths that are supposed to agree tend to drift apart.
//
// `totalSize` is the uncompressed length, which the container header carries. Callers
// reading a regular file get it from the filesystem.
void compressStream(std::istream& in, std::ostream& out, std::uint64_t totalSize,
                    const Options& options = Options{});
void decompressStream(std::istream& in, std::ostream& out);

// Which branch compress() took. Useful for tests and for reporting in benchmarks.
Method methodOf(const std::vector<std::uint8_t>& compressed);
const char* methodName(Method method);

}  // namespace cmpr
