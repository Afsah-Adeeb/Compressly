#pragma once
//
// Canonical Huffman coding over the 256-symbol byte alphabet.
//
// Two things here are worth knowing before reading the implementation:
//
// 1. CANONICAL CODES. A Huffman tree is not unique -- swapping any node's children gives
//    a different set of codes with identical compressed size. Canonical coding pins down
//    one specific choice, derived purely from each symbol's CODE LENGTH. That means the
//    compressed file only has to carry the lengths, not the tree shape, and the decoder
//    rebuilds byte-identical codes from them. Cheaper header, less code, and it is what
//    real DEFLATE does.
//
// 2. LENGTH LIMITING. Huffman puts no bound on code length; a sufficiently skewed input
//    (Fibonacci-like frequencies) produces codes longer than 32 bits, which would not fit
//    the writer's registers or a future decode table. Codes are therefore capped at
//    kMaxCodeLength, which costs a negligible amount of compression ratio and is again
//    what real implementations do.
//
#include <array>
#include <cstdint>
#include <vector>

namespace cmpr {
namespace huffman {

constexpr int kAlphabetSize = 256;

// 15 bits matches DEFLATE. It is comfortably enough that no realistic input gets near
// it: reaching depth L needs roughly fib(L+2) symbols, so depth 15 requires an
// adversarially constructed frequency distribution, never natural data.
constexpr int kMaxCodeLength = 15;

using FreqTable = std::array<std::uint64_t, kAlphabetSize>;

// Code length per symbol. 0 means "symbol does not occur in the input".
using LengthTable = std::array<std::uint8_t, kAlphabetSize>;

struct Code {
  std::uint32_t bits = 0;  // right-aligned; only the low `length` bits are meaningful
  std::uint8_t length = 0; // 0 for absent symbols
};
using CodeTable = std::array<Code, kAlphabetSize>;

FreqTable countFrequencies(const std::uint8_t* data, std::size_t size);

// Builds the Huffman tree, derives a code length per symbol, and enforces
// kMaxCodeLength. Guarantees: every symbol with a non-zero frequency gets a length in
// [1, kMaxCodeLength]; every symbol with zero frequency gets 0.
LengthTable buildLengths(const FreqTable& freq);

// Assigns the canonical code for each length, using the RFC 1951 procedure: codes are
// handed out in increasing order of (length, symbol).
CodeTable buildCanonicalCodes(const LengthTable& lengths);

// A binary trie for decoding, rebuilt from the lengths alone. Walking it one bit at a
// time is the straightforward decoder; a lookup table would be faster and is a deliberate
// later optimisation, not something to do before the format is proven correct.
class DecodeTree {
 public:
  explicit DecodeTree(const LengthTable& lengths);

  // Returns the index of the child reached by `bit`, or -1 if there is no such edge
  // (only reachable on corrupt input, because the encoder never emits that bit pattern).
  int step(int node, int bit) const { return nodes_[static_cast<std::size_t>(node)].child[bit]; }
  bool isLeaf(int node) const { return nodes_[static_cast<std::size_t>(node)].symbol >= 0; }
  int symbol(int node) const { return nodes_[static_cast<std::size_t>(node)].symbol; }
  static constexpr int kRoot = 0;

 private:
  struct Node {
    int child[2] = {-1, -1};
    int symbol = -1;  // >= 0 marks a leaf
  };
  std::vector<Node> nodes_;
};

}  // namespace huffman
}  // namespace cmpr
