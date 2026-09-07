//
// Command-line front end.
//
//   compressor c <in> <out>    compress
//   compressor d <in> <out>    decompress
//   compressor t <in>          roundtrip in memory and report ratio and throughput
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
               "  compressor t <in>         roundtrip test, report ratio and throughput\n");
  return 2;
}

int runTest(const std::string& path) {
  const Bytes input = readFile(path);

  auto start = std::chrono::steady_clock::now();
  const Bytes compressed = cmpr::compress(input);
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
  std::printf("method       %s\n",
              cmpr::methodOf(compressed) == cmpr::Method::kStored ? "stored" : "huffman");
  std::printf("compress     %.2f MB/s (%.3f s)\n",
              megabytesPerSecond(input.size(), compressSeconds), compressSeconds);
  std::printf("decompress   %.2f MB/s (%.3f s)\n",
              megabytesPerSecond(input.size(), decompressSeconds), decompressSeconds);
  std::printf("roundtrip    %s\n", identical ? "OK (byte-identical)" : "FAILED");
  return identical ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return usage();
  const std::string mode = argv[1];

  try {
    if ((mode == "c" || mode == "compress") && argc == 4) {
      const Bytes input = readFile(argv[2]);
      writeFile(argv[3], cmpr::compress(input));
      return 0;
    }
    if ((mode == "d" || mode == "decompress") && argc == 4) {
      const Bytes input = readFile(argv[2]);
      writeFile(argv[3], cmpr::decompress(input));
      return 0;
    }
    if ((mode == "t" || mode == "test") && argc == 3) {
      return runTest(argv[2]);
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return usage();
}
