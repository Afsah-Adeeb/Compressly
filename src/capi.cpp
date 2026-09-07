#define CMPR_BUILDING_SHARED
#include "capi.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#include "codec.h"

namespace {

// Thread-local so two concurrent requests cannot overwrite each other's message. The
// service runs a thread per request, so a shared buffer here would produce error text
// belonging to a different caller -- a bug that only appears under load.
thread_local std::string g_lastError;

void setError(const char* message) { g_lastError = message; }
void clearError() { g_lastError.clear(); }

// Copies a vector into a malloc'd buffer. The caller frees it with cmpr_free, which is
// free() -- keeping allocation and release on the same allocator matters across a DLL
// boundary, where the caller may not share this library's C++ runtime.
cmpr_status handOff(const std::vector<unsigned char>& source, unsigned char** out,
                    size_t* out_size) {
  // An empty result is legitimate (compressing nothing), but malloc(0) may return null,
  // which the caller cannot distinguish from failure. One byte costs nothing.
  auto* buffer = static_cast<unsigned char*>(std::malloc(source.empty() ? 1 : source.size()));
  if (buffer == nullptr) {
    setError("out of memory");
    return CMPR_ERR_OUT_OF_MEMORY;
  }
  if (!source.empty()) std::memcpy(buffer, source.data(), source.size());
  *out = buffer;
  *out_size = source.size();
  return CMPR_OK;
}

}  // namespace

extern "C" {

cmpr_status cmpr_compress(const unsigned char* data, size_t size, int threads,
                          size_t block_size, unsigned char** out, size_t* out_size) {
  if (out == nullptr || out_size == nullptr || (data == nullptr && size != 0)) {
    setError("null argument");
    return CMPR_ERR_INVALID_ARGUMENT;
  }
  *out = nullptr;
  *out_size = 0;
  clearError();

  try {
    cmpr::Options options;
    options.threads = threads;
    if (block_size != 0) options.blockSize = block_size;
    return handOff(cmpr::compress(data, size, options), out, out_size);
  } catch (const std::bad_alloc&) {
    setError("out of memory");
    return CMPR_ERR_OUT_OF_MEMORY;
  } catch (const std::exception& e) {
    // Exceptions must never cross the C boundary: unwinding into a caller that is not C++
    // (or is a different C++ runtime) is undefined behaviour, so every one is caught and
    // turned into a status code here.
    setError(e.what());
    return CMPR_ERR_INVALID_ARGUMENT;
  } catch (...) {
    setError("unknown error");
    return CMPR_ERR_INTERNAL;
  }
}

cmpr_status cmpr_decompress(const unsigned char* data, size_t size, unsigned char** out,
                            size_t* out_size) {
  if (out == nullptr || out_size == nullptr || (data == nullptr && size != 0)) {
    setError("null argument");
    return CMPR_ERR_INVALID_ARGUMENT;
  }
  *out = nullptr;
  *out_size = 0;
  clearError();

  try {
    return handOff(cmpr::decompress(data, size), out, out_size);
  } catch (const cmpr::CorruptInput& e) {
    setError(e.what());
    return CMPR_ERR_CORRUPT_INPUT;
  } catch (const std::bad_alloc&) {
    setError("out of memory");
    return CMPR_ERR_OUT_OF_MEMORY;
  } catch (const std::exception& e) {
    setError(e.what());
    return CMPR_ERR_INTERNAL;
  } catch (...) {
    setError("unknown error");
    return CMPR_ERR_INTERNAL;
  }
}

void cmpr_free(unsigned char* buffer) { std::free(buffer); }

const char* cmpr_last_error(void) { return g_lastError.c_str(); }

const char* cmpr_version(void) { return "cmpr 1.0 (LZ77 + canonical Huffman)"; }

}  // extern "C"
