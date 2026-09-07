cmake_minimum_required(VERSION 3.16)
project(compressor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to an optimised build: this project's whole point is measuring throughput,
# and benchmarking a -O0 binary would be meaningless.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_library(cmpr STATIC
  src/bitio.cpp
  src/huffman.cpp
  src/lz77.cpp
  src/deflate.cpp
  src/block.cpp
  src/threadpool.cpp
  src/codec.cpp
)
target_include_directories(cmpr PUBLIC src)
target_compile_options(cmpr PRIVATE -Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)
target_link_libraries(cmpr PUBLIC Threads::Threads)

add_executable(compressor tools/cli.cpp)
target_link_libraries(compressor PRIVATE cmpr)
if(WIN32)
  # For the peak working-set query used to report memory use.
  target_link_libraries(compressor PRIVATE psapi)
endif()

# Sweeps the LZ77 tuning knobs and prints the ratio/throughput table that goes in
# the writeup. Kept separate from the CLI: it is a measurement instrument, not a
# feature of the compressor.
add_executable(sweep tools/sweep.cpp)
target_link_libraries(sweep PRIVATE cmpr)

# Shared library with a flat C ABI, loaded by the HTTP service through ctypes. Without it
# the service would have to spawn the CLI per request, putting ~11 ms of process creation
# into every latency measurement.
add_library(cmpr_c SHARED src/capi.cpp)
target_link_libraries(cmpr_c PRIVATE cmpr)
set_target_properties(cmpr_c PROPERTIES PREFIX "" OUTPUT_NAME "cmpr")
if(MINGW)
  # Self-contained: the service loads this by path and should not also need the toolchain
  # runtime DLLs to be on PATH.
  # -static pulls libgcc, libstdc++ and libwinpthread into the DLL itself. Without it the
  # loader needs the toolchain runtime on PATH, and Python 3.8+ deliberately stopped
  # searching PATH for a DLL's dependencies -- so the service would fail to load it.
  target_link_options(cmpr_c PRIVATE -static)
endif()

add_executable(tests
  tests/testing.cpp
  tests/test_bitio.cpp
  tests/test_huffman.cpp
  tests/test_lz77.cpp
  tests/test_deflate.cpp
  tests/test_blocked.cpp
  tests/test_threadpool.cpp
  tests/test_roundtrip.cpp
)
target_link_libraries(tests PRIVATE cmpr)
target_include_directories(tests PRIVATE tests)

enable_testing()
add_test(NAME unit COMMAND tests)
