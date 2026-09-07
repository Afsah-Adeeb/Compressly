//
// Command-line front end.
//
//   compressor c <in> <out>    compress
//   compressor d <in> <out>    decompress
//   compressor t <in>          roundtrip in memory and report ratio and throughput
//
// Add --deflate (default), --lz77 or --huffman to pick the algorithm when compressing.
// Decompression takes no flag: the file records which method produced it.
//
// The `t` mode exists because it is the measurement that matters: it verifies
// correctness and produces the ratio/throughput numbers in one pass, without file I/O
// timing contaminating the result. It is the seed of the Phase 6 benchmark suite.
//
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "codec.h"

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes readFile(const std::string& path) {
  // Binary mode matters on Windows: a text-mode read would silently translate CRLF and
  // the roundtrip would fail on any file containing 0x0D.
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open " + path + " for reading");
  return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeFile(const std::string& path, const Bytes& data) {
  std::ofstream out(path, std::ios::binary);
  if (!out) throw std::runtime_error("cannot open " + path + " for writing");
  if (!data.empty()) {
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  }
  if (!out) throw std::runtime_error("write failed for " + path);
}

double megabytesPerSecond(std::size_t bytes, double seconds) {
  if (seconds <= 0.0) return 0.0;
  return static_cast<double>(bytes) / (1024.0 * 1024.0) / seconds;
}

double secondsSince(std::chrono::steady_clock::time_point start) {
  const auto elapsed = std::chrono::steady_clock::now() - start;
  return std::chrono::duration<double>(elapsed).count();
}

int usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  compressor c <in> <out>   compress\n"
               "  compressor d <in> <out>   decompress\n"
               "  compressor t <in>         roundtrip test, report ratio and throughput\n"
               "\n"
               "  --deflate                 LZ77 then Huffman (default)\n"
               "  --lz77                    LZ77 alone, byte-aligned framing\n"
               "  --huffman                 order-0 Huffman alone\n");
  return 2;
}

int runTest(const std::string& path, const cmpr::Options& options) {
  const Bytes input = readFile(path);

  auto start = std::chrono::steady_clock::now();
  const Bytes compressed = cmpr::compress(input, options);
  const double compressSeconds = secondsSince(start);

  start = std::chrono::steady_clock::now();
  const Bytes restored = cmpr::decompress(compressed);
  const double decompressSeconds = secondsSince(start);

  const bool identical = (restored == input);
  const double ratio =
      input.empty() ? 0.0 : 100.0 * (1.0 - static_cast<double>(compressed.size()) /
                                               static_cast<double>(input.size()));

  std::printf("file         %s\n", path.c_str());
  std::printf("original     %zu bytes\n", input.size());
  std::printf("compressed   %zu bytes\n", compressed.size());
  std::printf("reduction    %.2f%%\n", ratio);
  std::printf("method       %s\n", cmpr::methodName(cmpr::methodOf(compressed)));
  std::printf("compress     %.2f MB/s (%.3f s)\n",
              megabytesPerSecond(input.size(), compressSeconds), compressSeconds);
  std::printf("decompress   %.2f MB/s (%.3f s)\n",
              megabytesPerSecond(input.size(), decompressSeconds), decompressSeconds);
  std::printf("roundtrip    %s\n", identical ? "OK (byte-identical)" : "FAILED");
  return identical ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  // Flags are pulled out first so they can appear anywhere on the line; what remains is
  // positional.
  cmpr::Options options;
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--lz77") {
      options.algorithm = cmpr::Algorithm::kLz77;
    } else if (arg == "--deflate") {
      options.algorithm = cmpr::Algorithm::kDeflate;
    } else if (arg == "--huffman") {
      options.algorithm = cmpr::Algorithm::kHuffman;
    } else {
      args.push_back(arg);
    }
  }

  if (args.size() < 2) return usage();
  const std::string mode = args[0];
  const int positional = static_cast<int>(args.size());

  try {
    if ((mode == "c" || mode == "compress") && positional == 3) {
      const Bytes input = readFile(args[1]);
      writeFile(args[2], cmpr::compress(input, options));
      return 0;
    }
    if ((mode == "d" || mode == "decompress") && positional == 3) {
      const Bytes input = readFile(args[1]);
      writeFile(args[2], cmpr::decompress(input));
      return 0;
    }
    if ((mode == "t" || mode == "test") && positional == 2) {
      return runTest(args[1], options);
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return usage();
}
