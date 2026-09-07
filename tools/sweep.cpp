//
// Sweeps the LZ77 tuning knobs over a file and prints ratio and throughput for each
// setting. This is a measurement instrument, not part of the compressor.
//
//   sweep <file> [file...]
//
// One knob moves at a time, everything else held at the default, because the interesting
// question is what each parameter costs and buys on its own. Timing repeats small inputs
// until enough work has happened to out-measure the clock's resolution -- a single pass
// over a 50 KB file finishes faster than the timer can see.
//
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "codec.h"
#include "lz77.h"

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open " + path);
  return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

constexpr double kMinSampleSeconds = 0.20;

struct Result {
  std::size_t compressedSize = 0;
  double compressMbps = 0.0;
  double decompressMbps = 0.0;
  double averageMatchLength = 0.0;
  double matchedFraction = 0.0;
  const char* method = "";
};

double megabytesPerSecond(std::size_t bytes, std::size_t repeats, double seconds) {
  if (seconds <= 0.0) return 0.0;
  return static_cast<double>(bytes) * static_cast<double>(repeats) / (1024.0 * 1024.0) / seconds;
}

// Repeats the call until kMinSampleSeconds have elapsed, then reports the rate over the
// whole batch. Returns via `out` so the compressed bytes are available to the caller.
template <typename Fn>
double timeRepeatedly(Fn&& fn, std::size_t bytes) {
  auto start = std::chrono::steady_clock::now();
  std::size_t repeats = 0;
  double elapsed = 0.0;
  do {
    fn();
    ++repeats;
    elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  } while (elapsed < kMinSampleSeconds && repeats < 1000);
  return megabytesPerSecond(bytes, repeats, elapsed);
}

Result measure(const Bytes& input, const cmpr::Options& options) {
  Result result;

  cmpr::lz77::Stats stats;
  if (options.algorithm == cmpr::Algorithm::kLz77) {
    cmpr::lz77::tokenize(input.data(), input.size(), options.lz77, &stats);
    result.averageMatchLength = stats.averageMatchLength();
    result.matchedFraction = input.empty() ? 0.0
                                           : static_cast<double>(stats.matchedBytes) /
                                                 static_cast<double>(input.size());
  }

  Bytes compressed;
  result.compressMbps = timeRepeatedly([&] { compressed = cmpr::compress(input, options); },
                                       input.size());
  result.compressedSize = compressed.size();
  result.method = cmpr::methodName(cmpr::methodOf(compressed));

  Bytes restored;
  result.decompressMbps = timeRepeatedly([&] { restored = cmpr::decompress(compressed); },
                                         input.size());
  if (restored != input) throw std::runtime_error("ROUNDTRIP FAILED -- results are meaningless");

  return result;
}

double reductionPercent(std::size_t original, std::size_t compressed) {
  if (original == 0) return 0.0;
  return 100.0 * (1.0 - static_cast<double>(compressed) / static_cast<double>(original));
}

void printRow(const std::string& label, std::size_t original, const Result& result) {
  std::printf("  %-22s %10zu  %7.2f%%  %8.1f  %8.1f  %7.1f  %6.1f%%  %s\n", label.c_str(),
              result.compressedSize, reductionPercent(original, result.compressedSize),
              result.compressMbps, result.decompressMbps, result.averageMatchLength,
              100.0 * result.matchedFraction, result.method);
}

void printHeader() {
  std::printf("  %-22s %10s  %8s  %8s  %8s  %7s  %7s  %s\n", "setting", "bytes", "reduce",
              "comp MB/s", "dec MB/s", "avg len", "matched", "method");
  std::printf("  %s\n", std::string(92, '-').c_str());
}

void sweepFile(const std::string& path) {
  const Bytes input = readFile(path);
  std::printf("\n%s  (%zu bytes)\n", path.c_str(), input.size());
  printHeader();

  cmpr::Options huffman;
  huffman.algorithm = cmpr::Algorithm::kHuffman;
  printRow("huffman (phase 1)", input.size(), measure(input, huffman));

  const cmpr::lz77::Config defaults;
  const auto lz77With = [&](const cmpr::lz77::Config& config) {
    cmpr::Options options;
    options.algorithm = cmpr::Algorithm::kLz77;
    options.lz77 = config;
    return options;
  };

  printRow("lz77 (defaults)", input.size(), measure(input, lz77With(defaults)));

  std::printf("\n  -- window size --------------------------------------------------------\n");
  for (int windowBits = 8; windowBits <= 16; ++windowBits) {
    cmpr::lz77::Config config = defaults;
    config.windowBits = windowBits;
    const std::size_t kib = (std::size_t{1} << windowBits) / 1024;
    const std::string label =
        "window " + std::to_string(windowBits) + " (" +
        (kib == 0 ? std::to_string(std::size_t{1} << windowBits) + " B" : std::to_string(kib) + " KiB") + ")";
    printRow(label, input.size(), measure(input, lz77With(config)));
  }

  std::printf("\n  -- minimum match length -----------------------------------------------\n");
  for (int minMatch = 3; minMatch <= 8; ++minMatch) {
    cmpr::lz77::Config config = defaults;
    config.minMatch = minMatch;
    printRow("minMatch " + std::to_string(minMatch), input.size(), measure(input, lz77With(config)));
  }

  std::printf("\n  -- search depth (chain length) ----------------------------------------\n");
  for (int chain : {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 4096}) {
    cmpr::lz77::Config config = defaults;
    config.maxChainLength = chain;
    printRow("chain " + std::to_string(chain), input.size(), measure(input, lz77With(config)));
  }

  std::printf("\n  -- lazy matching ------------------------------------------------------\n");
  for (bool lazy : {false, true}) {
    cmpr::lz77::Config config = defaults;
    config.lazyMatching = lazy;
    printRow(lazy ? "lazy on" : "lazy off", input.size(), measure(input, lz77With(config)));
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: sweep <file> [file...]\n");
    return 2;
  }
  try {
    for (int i = 1; i < argc; ++i) sweepFile(argv[i]);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return 0;
}
