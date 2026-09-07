#pragma once
//
// Bit-level I/O.
//
// Huffman codes are not byte-aligned: a code might be 3 bits or 11 bits, and the next
// code starts immediately after it. Everything else in this project reads and writes
// bytes, so these two classes are the boundary between "stream of bits" and "vector of
// bytes".
//
// BIT ORDER: this project packs bits MSB-first. The first bit written lands in bit 7 of
// the first byte, the second in bit 6, and so on. That means a hex dump of the output
// reads left-to-right in the same order the bits were written, which makes debugging a
// misaligned stream tractable.
//
// Note for the future RFC 1951 compatibility mode: real DEFLATE is *LSB-first* for
// integers but packs Huffman codes starting from the code's most significant bit. That
// mixture is a well-known source of confusion, and it is a deliberate choice not to
// inherit it before the format is proven correct. Switching costs one rewrite of these
// two classes and nothing else -- that is why they are isolated here.
//
// The methods on the hot path are defined inline in the header on purpose: they are
// called once per symbol (millions of times per file) and are a few instructions each,
// so a function call per bit would dominate the runtime.
//
#include <cstddef>
#include <cstdint>
#include <vector>

#include "format.h"

namespace cmpr {

class BitWriter {
 public:
  // Appends to `out` rather than owning a buffer, so the caller can lay a bit stream
  // down directly after a header it has already written.
  explicit BitWriter(std::vector<std::uint8_t>& out) : out_(out) {}

  // Writes the low `count` bits of `value`, most significant of those first.
  // `count` must be in [0, 32].
  void writeBits(std::uint32_t value, int count) {
    if (count == 0) return;
    // Mask off anything above `count` bits so a caller passing a dirty value cannot
    // corrupt bits that were already buffered.
    const std::uint32_t masked =
        (count >= 32) ? value : (value & ((std::uint32_t{1} << count) - 1));

    // acc_ holds up to 7 leftover bits in its low positions; shifting left and OR-ing
    // appends the new bits below them, keeping the whole buffer MSB-first.
    // Worst case here is 7 + 32 = 39 bits, which is why acc_ is 64 bits wide.
    acc_ = (acc_ << count) | masked;
    nbits_ += count;
    written_ += static_cast<std::uint64_t>(count);

    while (nbits_ >= 8) {
      nbits_ -= 8;
      out_.push_back(static_cast<std::uint8_t>((acc_ >> nbits_) & 0xFF));
    }
  }

  void writeBit(int bit) { writeBits(static_cast<std::uint32_t>(bit & 1), 1); }

  // Pads the final partial byte with zeros. Those padding bits are never mistaken for
  // data because the container header stores the exact uncompressed size, so the decoder
  // stops after decoding that many symbols and never looks at them.
  void flush() {
    if (nbits_ > 0) {
      out_.push_back(static_cast<std::uint8_t>((acc_ << (8 - nbits_)) & 0xFF));
      nbits_ = 0;
      acc_ = 0;
    }
  }

  // Bits written since construction, padding excluded.
  std::uint64_t bitsWritten() const { return written_; }

 private:
  std::vector<std::uint8_t>& out_;
  std::uint64_t acc_ = 0;   // pending bits, right-aligned in the low `nbits_` positions
  int nbits_ = 0;           // how many of those are valid (always < 8 between calls)
  std::uint64_t written_ = 0;
};

class BitReader {
 public:
  BitReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  // Reads one bit. Throws CorruptInput past the end of the buffer rather than returning
  // a sentinel: a truncated file is not a value the caller can do anything sensible with.
  int readBit() {
    if (nbits_ == 0) refill();
    --nbits_;
    return static_cast<int>((acc_ >> nbits_) & 1);
  }

  // Reads `count` bits, MSB-first, in the same order writeBits laid them down.
  std::uint32_t readBits(int count) {
    std::uint32_t value = 0;
    for (int i = 0; i < count; ++i) value = (value << 1) | static_cast<std::uint32_t>(readBit());
    return value;
  }

 private:
  void refill() {
    if (pos_ >= size_) throw CorruptInput("bit stream ended early");
    acc_ = data_[pos_++];
    nbits_ = 8;
  }

  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
  std::uint64_t acc_ = 0;
  int nbits_ = 0;
};

}  // namespace cmpr
