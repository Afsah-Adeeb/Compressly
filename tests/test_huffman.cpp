#include <algorithm>
#include <string>

#include "huffman.h"
#include "testing.h"

using namespace cmpr::huffman;

namespace {

FreqTable freqFromString(const std::string& text) {
  return countFrequencies(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

// Kraft sum in units of 2^-kMaxCodeLength, so it stays integral. A valid prefix code
// never exceeds 2^kMaxCodeLength.
std::uint64_t kraftSum(const LengthTable& lengths) {
  std::uint64_t total = 0;
  for (std::uint8_t length : lengths) {
    if (length != 0) total += std::uint64_t{1} << (kMaxCodeLength - length);
  }
  return total;
}

// Independently confirms the codes are prefix-free and self-consistent: walking each
// code's bits down the decode tree must land on exactly that symbol's leaf.
void checkDecodesToItself(const LengthTable& lengths) {
  const CodeTable codes = buildCanonicalCodes(lengths);
  const DecodeTree tree(lengths);
  for (int symbol = 0; symbol < kByteAlphabetSize; ++symbol) {
    const Code& code = codes[static_cast<std::size_t>(symbol)];
    if (code.length == 0) continue;
    int node = DecodeTree::kRoot;
    for (int i = code.length - 1; i >= 0; --i) {
      const int bit = static_cast<int>((code.bits >> i) & 1);
      node = tree.step(node, bit);
      CHECK(node >= 0);
      // No proper prefix of a code may itself be a code -- that is the prefix-free
      // property, and it is what makes the stream decodable without separators.
      const bool atEnd = (i == 0);
      CHECK(atEnd || !tree.isLeaf(node));
    }
    CHECK(tree.isLeaf(node));
    CHECK_EQ(tree.symbol(node), symbol);
  }
}

}  // namespace

TEST(EmptyInputProducesNoCodes) {
  const LengthTable lengths = buildLengths(FreqTable(kByteAlphabetSize, 0));
  for (std::uint8_t length : lengths) CHECK_EQ(static_cast<int>(length), 0);
}

TEST(SingleDistinctSymbolGetsAOneBitCode) {
  // The degenerate case: a zero-bit code would leave the decoder looping forever.
  const LengthTable lengths = buildLengths(freqFromString(std::string(1000, 'x')));
  CHECK_EQ(static_cast<int>(lengths['x']), 1);
  for (int symbol = 0; symbol < kByteAlphabetSize; ++symbol) {
    if (symbol != 'x') CHECK_EQ(static_cast<int>(lengths[static_cast<std::size_t>(symbol)]), 0);
  }
  checkDecodesToItself(lengths);
}

TEST(MoreFrequentSymbolsGetShorterCodes) {
  FreqTable freq(kByteAlphabetSize, 0);
  freq['a'] = 100;
  freq['b'] = 10;
  freq['c'] = 1;
  const LengthTable lengths = buildLengths(freq);
  CHECK(lengths['a'] < lengths['b']);
  CHECK(lengths['b'] <= lengths['c']);
  checkDecodesToItself(lengths);
}

TEST(BalancedFrequenciesGiveBalancedCodes) {
  // Four equally likely symbols: information-theoretically 2 bits each, and Huffman
  // should find exactly that.
  FreqTable freq(kByteAlphabetSize, 0);
  for (int symbol = 0; symbol < 4; ++symbol) freq[static_cast<std::size_t>(symbol)] = 25;
  const LengthTable lengths = buildLengths(freq);
  for (int symbol = 0; symbol < 4; ++symbol) {
    CHECK_EQ(static_cast<int>(lengths[static_cast<std::size_t>(symbol)]), 2);
  }
  CHECK_EQ(kraftSum(lengths), std::uint64_t{1} << kMaxCodeLength);  // a complete code
}

TEST(CanonicalCodesAreOrderedByLengthThenSymbol) {
  const LengthTable lengths = buildLengths(freqFromString("the quick brown fox jumps over the lazy dog"));
  const CodeTable codes = buildCanonicalCodes(lengths);

  // The canonical property that makes the tree reconstructible from lengths alone:
  // reading symbols in (length, symbol) order gives strictly increasing codes, once
  // shorter codes are left-aligned against longer ones for comparison.
  std::uint64_t previous = 0;
  bool first = true;
  for (int length = 1; length <= kMaxCodeLength; ++length) {
    for (int symbol = 0; symbol < kByteAlphabetSize; ++symbol) {
      const Code& code = codes[static_cast<std::size_t>(symbol)];
      if (code.length != length) continue;
      const std::uint64_t aligned = static_cast<std::uint64_t>(code.bits)
                                    << (kMaxCodeLength - code.length);
      CHECK(first || aligned > previous);
      previous = aligned;
      first = false;
    }
  }
  checkDecodesToItself(lengths);
}

TEST(AllTwoFiftySixSymbolsAreCodeable) {
  FreqTable freq(kByteAlphabetSize, 0);
  for (int symbol = 0; symbol < kByteAlphabetSize; ++symbol) {
    freq[static_cast<std::size_t>(symbol)] = static_cast<std::uint64_t>(symbol + 1);
  }
  const LengthTable lengths = buildLengths(freq);
  for (std::uint8_t length : lengths) {
    CHECK(length >= 1 && length <= kMaxCodeLength);
  }
  checkDecodesToItself(lengths);
}

TEST(FibonacciFrequenciesForceTheLengthLimit) {
  // Fibonacci frequencies are the worst case for Huffman depth: every merge produces
  // exactly the next term, so the tree degenerates into a chain and the rarest symbol
  // would get a code as long as the alphabet. This is the input that exercises the Kraft
  // repair path, and it is the reason the limit exists at all.
  FreqTable freq(kByteAlphabetSize, 0);
  std::uint64_t a = 1;
  std::uint64_t b = 1;
  const int symbols = 40;
  for (int symbol = 0; symbol < symbols; ++symbol) {
    freq[static_cast<std::size_t>(symbol)] = a;
    const std::uint64_t next = a + b;
    a = b;
    b = next;
  }

  const LengthTable lengths = buildLengths(freq);

  int longest = 0;
  int present = 0;
  for (std::uint8_t length : lengths) {
    if (length == 0) continue;
    ++present;
    longest = std::max(longest, static_cast<int>(length));
  }
  CHECK_EQ(present, symbols);
  // Without the limiter this would be roughly `symbols` bits deep.
  CHECK(longest > 12);              // confirms the test really is hitting the limit
  CHECK(longest <= kMaxCodeLength); // ... and that the limiter caught it
  CHECK(kraftSum(lengths) <= (std::uint64_t{1} << kMaxCodeLength));
  checkDecodesToItself(lengths);
}

TEST(LengthsAreDeterministicAcrossRuns) {
  // Ties in frequency are broken deterministically so the same input always produces the
  // same bytes -- otherwise benchmark runs would not be comparable.
  FreqTable freq(kByteAlphabetSize, 0);
  for (int symbol = 0; symbol < 16; ++symbol) freq[static_cast<std::size_t>(symbol)] = 7;
  const LengthTable first = buildLengths(freq);
  const LengthTable second = buildLengths(freq);
  for (int symbol = 0; symbol < kByteAlphabetSize; ++symbol) {
    CHECK_EQ(static_cast<int>(first[static_cast<std::size_t>(symbol)]),
             static_cast<int>(second[static_cast<std::size_t>(symbol)]));
  }
}
