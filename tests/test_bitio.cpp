#include <random>

#include "bitio.h"
#include "testing.h"

using cmpr::BitReader;
using cmpr::BitWriter;
using cmpr::CorruptInput;

TEST(BitWriterPacksMsbFirst) {
  std::vector<std::uint8_t> out;
  BitWriter writer(out);
  writer.writeBits(0b101, 3);
  writer.writeBits(0b01, 2);
  writer.writeBits(0b110, 3);
  writer.flush();

  // 101 01 110 -> one full byte, no padding.
  CHECK_EQ(out.size(), std::size_t{1});
  CHECK_EQ(static_cast<int>(out[0]), 0b10101110);
}

TEST(FlushPadsWithZeros) {
  std::vector<std::uint8_t> out;
  BitWriter writer(out);
  writer.writeBits(0b1, 1);
  writer.flush();
  CHECK_EQ(out.size(), std::size_t{1});
  CHECK_EQ(static_cast<int>(out[0]), 0b10000000);
}

TEST(WriteBitsMasksStrayHighBits) {
  // A caller passing a value wider than `count` must not corrupt neighbouring bits.
  std::vector<std::uint8_t> out;
  BitWriter writer(out);
  writer.writeBits(0xFFFFFFFF, 4);
  writer.writeBits(0, 4);
  writer.flush();
  CHECK_EQ(static_cast<int>(out[0]), 0b11110000);
}

TEST(BitsWrittenExcludesPadding) {
  std::vector<std::uint8_t> out;
  BitWriter writer(out);
  writer.writeBits(0, 3);
  writer.flush();
  CHECK_EQ(writer.bitsWritten(), std::uint64_t{3});
  CHECK_EQ(out.size(), std::size_t{1});
}

TEST(RandomWidthRoundtrip) {
  // The interesting failures in bit I/O are at byte boundaries and across the 32-bit
  // accumulator edge, so widths are drawn randomly rather than swept in order.
  std::mt19937 rng(12345);
  std::uniform_int_distribution<int> widthDist(0, 32);
  std::uniform_int_distribution<std::uint32_t> valueDist(0, 0xFFFFFFFF);

  std::vector<std::pair<std::uint32_t, int>> written;
  std::vector<std::uint8_t> buffer;
  BitWriter writer(buffer);
  for (int i = 0; i < 20000; ++i) {
    const int width = widthDist(rng);
    const std::uint32_t masked =
        width == 0 ? 0u : (width >= 32 ? valueDist(rng) : (valueDist(rng) & ((1u << width) - 1)));
    writer.writeBits(masked, width);
    written.push_back({masked, width});
  }
  writer.flush();

  BitReader reader(buffer.data(), buffer.size());
  for (std::size_t i = 0; i < written.size(); ++i) {
    CHECK_EQ(reader.readBits(written[i].second), written[i].first);
  }
}

TEST(ReadingPastTheEndThrows) {
  std::vector<std::uint8_t> buffer{0xFF};
  BitReader reader(buffer.data(), buffer.size());
  for (int i = 0; i < 8; ++i) CHECK_EQ(reader.readBit(), 1);
  CHECK_THROWS(reader.readBit(), CorruptInput);
}

TEST(EmptyBufferThrowsImmediately) {
  BitReader reader(nullptr, 0);
  CHECK_THROWS(reader.readBit(), CorruptInput);
}
