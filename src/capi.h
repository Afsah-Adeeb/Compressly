#ifndef CMPR_CAPI_H
#define CMPR_CAPI_H
//
// A flat C interface over the codec, so it can be loaded as a shared library.
//
// This exists for the HTTP service. The obvious alternative -- have the service shell out
// to the command-line binary per request -- would put roughly 11 ms of process creation
// into every request, which on payloads of a few hundred kilobytes is most of the
// latency. The p99 figure would then be measuring Windows process creation rather than
// this compressor, which makes the whole load test pointless.
//
// Deliberately a C interface rather than C++: `extern "C"` gives a stable, name-mangling
// free ABI that ctypes, cgo, JNI or anything else can bind to without a C++ compiler.
//
// OWNERSHIP: every buffer returned through an out-parameter is allocated by this library
// and must be released with cmpr_free. Nothing else in the API allocates.
//
#include <stddef.h>

#if defined(_WIN32)
#if defined(CMPR_BUILDING_SHARED)
#define CMPR_API __declspec(dllexport)
#else
#define CMPR_API __declspec(dllimport)
#endif
#else
#define CMPR_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  CMPR_OK = 0,
  CMPR_ERR_INVALID_ARGUMENT = 1,
  CMPR_ERR_CORRUPT_INPUT = 2,
  CMPR_ERR_OUT_OF_MEMORY = 3,
  CMPR_ERR_INTERNAL = 4
} cmpr_status;

// Compresses `size` bytes from `data`.
//
// `threads`    worker threads; 0 means one per core, 1 is sequential.
// `block_size` bytes per independent block; 0 disables blocking. Pass 0 for the default.
//
// On CMPR_OK, *out points to a buffer of *out_size bytes owned by the caller, to be
// released with cmpr_free. On failure *out is left null and cmpr_last_error() describes
// what happened.
CMPR_API cmpr_status cmpr_compress(const unsigned char* data, size_t size, int threads,
                                   size_t block_size, unsigned char** out, size_t* out_size);

// Reverses cmpr_compress. The method is read from the input, so there are no options.
CMPR_API cmpr_status cmpr_decompress(const unsigned char* data, size_t size,
                                     unsigned char** out, size_t* out_size);

// Releases a buffer returned by this library. Null is accepted and ignored.
CMPR_API void cmpr_free(unsigned char* buffer);

// The most recent error on the calling thread, or "" if the last call succeeded. The
// storage is thread-local, so concurrent requests cannot overwrite each other's message.
CMPR_API const char* cmpr_last_error(void);

// Library version string, for the service health endpoint.
CMPR_API const char* cmpr_version(void);

#ifdef __cplusplus
}
#endif

#endif  // CMPR_CAPI_H
