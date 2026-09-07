#include "huffman.h"

#include <algorithm>
#include <cassert>
#include <queue>

namespace cmpr {
namespace huffman {

FreqTable countFrequencies(const std::uint8_t* data, std::size_t size) {
  FreqTable freq{};
  for (std::size_t i = 0; i < size; ++i) ++freq[data[i]];
  return freq;
}

namespace {

// Nodes live in one vector and refer to each other by index rather than by pointer.
// Indices survive the vector reallocating, there is nothing to free, and the whole tree
// sits in a couple of cache lines.
struct Node {
  std::uint64_t freq;
  int left;   // -1 for a leaf
  int right;
};

// Depth of every leaf, bucketed by depth. Depths beyond kMaxCodeLength are clamped into
// the last bucket here and repaired by enforceLengthLimit below.
std::array<std::uint32_t, kMaxCodeLength + 1> leafDepthHistogram(
    const std::vector<Node>& nodes, int root) {
  std::array<std::uint32_t, kMaxCodeLength + 1> count{};
  // Iterative traversal: a recursive one would be fine at these depths, but the depth
  // bound is exactly the thing being measured here, so it should not be assumed.
  std::vector<std::pair<int, int>> stack{{root, 0}};
  while (!stack.empty()) {
    const int index = stack.back().first;
    const int depth = stack.back().second;
    stack.pop_back();
    const Node& node = nodes[static_cast<std::size_t>(index)];
    if (node.left < 0) {
      ++count[static_cast<std::size_t>(std::min(depth, kMaxCodeLength))];
      continue;
    }
    stack.push_back({node.left, depth + 1});
    stack.push_back({node.right, depth + 1});
  }
  return count;
}

// Repairs a depth histogram that was clamped at kMaxCodeLength.
//
// A prefix code is decodable exactly when it satisfies the Kraft inequality:
//   sum over symbols of 2^-length <= 1
// Clamping long codes shorter can break that: it hands out more code space than exists.
// The fix is to lengthen codes until the inequality holds again -- repeatedly take a leaf
// from the deepest level that still has one below the limit and push it one level down,
// which frees code space. Each step strictly reduces the Kraft sum, so this terminates.
//
// The result can be an *incomplete* code (strictly less than the full code space used),
// which is still perfectly decodable, just fractionally larger than optimal. Since this
// path only triggers on adversarial inputs, reclaiming that slack is not worth the code.
void enforceLengthLimit(std::array<std::uint32_t, kMaxCodeLength + 1>& count) {
  // Work in units of 2^-kMaxCodeLength so the arithmetic stays in integers.
  std::uint64_t kraft = 0;
  for (int length = 1; length <= kMaxCodeLength; ++length) {
    kraft += static_cast<std::uint64_t>(count[static_cast<std::size_t>(length)])
             << (kMaxCodeLength - length);
  }
  const std::uint64_t limit = std::uint64_t{1} << kMaxCodeLength;

  while (kraft > limit) {
    // Find the deepest level short of the limit that still has a leaf to push down.
    // One always exists here: if every leaf were already at kMaxCodeLength the Kraft sum
    // would equal the number of symbols (at most 256), far under the limit.
    int length = kMaxCodeLength - 1;
    while (count[static_cast<std::size_t>(length)] == 0) --length;
    assert(length >= 1);

    --count[static_cast<std::size_t>(length)];
    ++count[static_cast<std::size_t>(length + 1)];
    kraft -= std::uint64_t{1} << (kMaxCodeLength - length - 1);
  }
}

}  // namespace

LengthTable buildLengths(const FreqTable& freq) {
  LengthTable lengths{};

  std::vector<int> present;
  for (int symbol = 0; symbol < kAlphabetSize; ++symbol) {
    if (freq[static_cast<std::size_t>(symbol)] != 0) present.push_back(symbol);
  }

  // Empty input: no symbols, no codes. The caller stores such input raw.
  if (present.empty()) return lengths;

  // A file of one repeated byte has a "tree" that is a single leaf, so the natural code
  // length is 0 bits -- and a decoder consuming 0 bits per symbol never advances. Real
  // implementations resolve this by forcing a 1-bit code, effectively inventing a second
  // symbol that never occurs; DEFLATE does the same thing. It costs one bit per byte,
  // which is still an 8x reduction, and keeps the decoder free of special cases.
  if (present.size() == 1) {
    lengths[static_cast<std::size_t>(present[0])] = 1;
    return lengths;
  }

  std::vector<Node> nodes;
  nodes.reserve(2 * present.size());
  for (int symbol : present) {
    nodes.push_back(Node{freq[static_cast<std::size_t>(symbol)], -1, -1});
  }

  // Min-heap on frequency. Ties break on node index so the tree -- and therefore the
  // compressed output -- is byte-for-byte reproducible across runs and platforms, which
  // matters both for the roundtrip tests and for comparing benchmark runs later.
  const auto cheaper = [&nodes](int a, int b) {
    const std::uint64_t fa = nodes[static_cast<std::size_t>(a)].freq;
    const std::uint64_t fb = nodes[static_cast<std::size_t>(b)].freq;
    if (fa != fb) return fa > fb;  // std::priority_queue is a max-heap; invert it
    return a > b;
  };
  std::priority_queue<int, std::vector<int>, decltype(cheaper)> heap(cheaper);
  for (std::size_t i = 0; i < present.size(); ++i) heap.push(static_cast<int>(i));

  while (heap.size() > 1) {
    const int a = heap.top();
    heap.pop();
    const int b = heap.top();
    heap.pop();
    const std::uint64_t combined = nodes[static_cast<std::size_t>(a)].freq +
                                   nodes[static_cast<std::size_t>(b)].freq;
    nodes.push_back(Node{combined, a, b});
    heap.push(static_cast<int>(nodes.size()) - 1);
  }
  const int root = heap.top();

  auto count = leafDepthHistogram(nodes, root);
  enforceLengthLimit(count);

  // The tree gave a multiset of code lengths; it did not have to say which symbol gets
  // which. Handing the shortest lengths to the most frequent symbols is optimal for any
  // valid multiset, and doing the assignment here (rather than reading depths straight
  // off the tree) means the limited and unlimited cases share one code path.
  std::sort(present.begin(), present.end(), [&freq](int a, int b) {
    const std::uint64_t fa = freq[static_cast<std::size_t>(a)];
    const std::uint64_t fb = freq[static_cast<std::size_t>(b)];
    if (fa != fb) return fa > fb;
    return a < b;
  });

  std::size_t index = 0;
  for (int length = 1; length <= kMaxCodeLength; ++length) {
    for (std::uint32_t k = 0; k < count[static_cast<std::size_t>(length)]; ++k) {
      lengths[static_cast<std::size_t>(present[index++])] = static_cast<std::uint8_t>(length);
    }
  }
  assert(index == present.size());
  return lengths;
}

CodeTable buildCanonicalCodes(const LengthTable& lengths) {
  std::array<std::uint32_t, kMaxCodeLength + 1> count{};
  for (std::uint8_t length : lengths) {
    if (length != 0) ++count[length];
  }

  // The canonical rule: the first code of length L is the first code of length L-1, plus
  // the number of codes of length L-1, shifted left one bit. Within a length, codes are
  // handed out in increasing symbol order. Both sides derive this identically, which is
  // why the file only needs to carry lengths.
  std::array<std::uint32_t, kMaxCodeLength + 1> nextCode{};
  std::uint32_t code = 0;
  for (int length = 1; length <= kMaxCodeLength; ++length) {
    code = (code + count[static_cast<std::size_t>(length - 1)]) << 1;
    nextCode[static_cast<std::size_t>(length)] = code;
  }

  CodeTable codes{};
  for (int symbol = 0; symbol < kAlphabetSize; ++symbol) {
    const std::uint8_t length = lengths[static_cast<std::size_t>(symbol)];
    if (length == 0) continue;
    codes[static_cast<std::size_t>(symbol)] = Code{nextCode[length]++, length};
  }
  return codes;
}

DecodeTree::DecodeTree(const LengthTable& lengths) {
  nodes_.push_back(Node{});  // root

  const CodeTable codes = buildCanonicalCodes(lengths);
  for (int symbol = 0; symbol < kAlphabetSize; ++symbol) {
    const Code& code = codes[static_cast<std::size_t>(symbol)];
    if (code.length == 0) continue;

    int node = kRoot;
    for (int i = code.length - 1; i >= 0; --i) {  // most significant bit of the code first
      const int bit = static_cast<int>((code.bits >> i) & 1);
      if (nodes_[static_cast<std::size_t>(node)].child[bit] < 0) {
        const int fresh = static_cast<int>(nodes_.size());
        nodes_.push_back(Node{});
        nodes_[static_cast<std::size_t>(node)].child[bit] = fresh;
      }
      node = nodes_[static_cast<std::size_t>(node)].child[bit];
    }
    nodes_[static_cast<std::size_t>(node)].symbol = symbol;
  }
}

}  // namespace huffman
}  // namespace cmpr
