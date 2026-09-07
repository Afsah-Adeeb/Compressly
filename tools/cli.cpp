//
// Command-line front end.
//
//   compressor c <in> <out>    compress
//   compressor d <in> <out>    decompress
//   compressor t <in>          roundtrip in memory, report ratio and throughput
//
// Flags: --deflate (default) / --lz77 / --huffman choose the algorithm; --block-size and
// --threads control blocking and parallelism; --stream uses the bounded-memory path.
// Decompression takes no algorithm flag -- the file records which method produced it.
//
// The `t` mode is the measurement that matters: it verifies correctness and produces the
// ratio and throughput numbers in one pass, with no file I/O inside the timed region.
//
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "codec.h"
#include "threadpool.h"

#ifdef _WIN32
#include <windows.h>
// psapi.h must follow windows.h.
#include <psapi.h>
#endif

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

std::uint64_t fileSize(const std::string& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) throw std::runtime_error("cannot open " + path + " for reading");
  return static_cast<std::uint64_t>(in.tellg());
}

// Peak working set: the high-water mark of physical memory this process has held. That is
// the number Phase 4 is about -- whether the compressor's footprint tracks the block size
// or the file size.
std::uint64_t peakMemoryBytes() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS counters{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
    return static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
  }
#endif
  return 0;
}

// Accepts a plain byte count or one with a K/M/G suffix, and "0" to disable blocking.
std::size_t parseSize(const std::string& text) {
  if (text.empty()) throw std::runtime_error("empty size");
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
  std::size_t scale = 1;
  if (end != nullptr && *end != 0) {
    switch (*end) {
      case 'k': case 'K': scale = 1024; break;
      case 'm': case 'M': scale = 1024 * 1024; break;
      case 'g': case 'G': scale = 1024 * 1024 * 1024; break;
      default: throw std::runtime_error("unrecognised size suffix in " + text);
    }
  }
  return static_cast<std::size_t>(value) * scale;
}

double megabytesPerSecond(std::uint64_t bytes, double seconds) {
  if (seconds <= 0.0) return 0.0;
  return static_cast<double>(bytes) / (1024.0 * 1024.0) / seconds;
}

double secondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

void reportMemory() {
  const std::uint64_t peak = peakMemoryBytes();
  if (peak != 0) {
    std::printf("peak memory  %.1f MiB\n", static_cast<double>(peak) / (1024.0 * 1024.0));
  }
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
               "  --huffman                 order-0 Huffman alone\n"
               "  --block-size <n>          block size, K/M/G suffixes, 0 to disable\n"
               "  --threads <n>             worker threads, 0 for one per core\n"
               "  --stream                  bounded-memory streaming path\n");
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
  std::printf("threads      %d\n", cmpr::resolveThreadCount(options.threads));
  std::printf("compress     %.2f MB/s (%.3f s)\n",
              megabytesPerSecond(input.size(), compressSeconds), compressSeconds);
  std::printf("decompress   %.2f MB/s (%.3f s)\n",
              megabytesPerSecond(input.size(), decompressSeconds), decompressSeconds);
  reportMemory();
  std::printf("roundtrip    %s\n", identical ? "OK (byte-identical)" : "FAILED");
  return identical ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  // Flags are pulled out first so they can appear anywhere on the line; what remains is
  // positional.
  cmpr::Options options;
  bool streaming = false;
  std::vector<std::string> args;

  try {
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      const auto next = [&]() -> std::string {
        if (i + 1 >= argc) throw std::runtime_error(arg + " needs a value");
        return argv[++i];
      };
      if (arg == "--lz77") {
        options.algorithm = cmpr::Algorithm::kLz77;
      } else if (arg == "--deflate") {
        options.algorithm = cmpr::Algorithm::kDeflate;
      } else if (arg == "--huffman") {
        options.algorithm = cmpr::Algorithm::kHuffman;
      } else if (arg == "--block-size") {
        options.blockSize = parseSize(next());
      } else if (arg == "--threads") {
        options.threads = std::atoi(next().c_str());
      } else if (arg == "--stream") {
        streaming = true;
      } else {
        args.push_back(arg);
      }
    }

    if (args.size() < 2) return usage();
    const std::string mode = args[0];
    const int positional = static_cast<int>(args.size());

    if ((mode == "c" || mode == "compress") && positional == 3) {
      const auto start = std::chrono::steady_clock::now();
      std::uint64_t size = 0;
      if (streaming) {
        size = fileSize(args[1]);
        std::ifstream in(args[1], std::ios::binary);
        std::ofstream out(args[2], std::ios::binary);
        if (!in) throw std::runtime_error("cannot open " + args[1] + " for reading");
        if (!out) throw std::runtime_error("cannot open " + args[2] + " for writing");
        cmpr::compressStream(in, out, size, options);
      } else {
        const Bytes input = readFile(args[1]);
        size = input.size();
        writeFile(args[2], cmpr::compress(input, options));
      }
      const double seconds = secondsSince(start);
      std::printf("compressed   %llu -> %llu bytes  (%.2f%% reduction)\n",
                  static_cast<unsigned long long>(size),
                  static_cast<unsigned long long>(fileSize(args[2])),
                  size == 0 ? 0.0
                            : 100.0 * (1.0 - static_cast<double>(fileSize(args[2])) /
                                                 static_cast<double>(size)));
      std::printf("throughput   %.2f MB/s (%.3f s, %s, %d thread%s)\n",
                  megabytesPerSecond(size, seconds), seconds, streaming ? "streaming" : "in-memory",
                  cmpr::resolveThreadCount(options.threads),
                  cmpr::resolveThreadCount(options.threads) == 1 ? "" : "s");
      reportMemory();
      return 0;
    }

    if ((mode == "d" || mode == "decompress") && positional == 3) {
      const auto start = std::chrono::steady_clock::now();
      if (streaming) {
        std::ifstream in(args[1], std::ios::binary);
        std::ofstream out(args[2], std::ios::binary);
        if (!in) throw std::runtime_error("cannot open " + args[1] + " for reading");
        if (!out) throw std::runtime_error("cannot open " + args[2] + " for writing");
        cmpr::decompressStream(in, out);
      } else {
        writeFile(args[2], cmpr::decompress(readFile(args[1])));
      }
      const double seconds = secondsSince(start);
      const std::uint64_t size = fileSize(args[2]);
      std::printf("decompressed %llu bytes\n", static_cast<unsigned long long>(size));
      std::printf("throughput   %.2f MB/s (%.3f s, %s)\n", megabytesPerSecond(size, seconds),
                  seconds, streaming ? "streaming" : "in-memory");
      reportMemory();
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
