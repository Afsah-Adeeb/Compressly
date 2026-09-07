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
  src/codec.cpp
)
target_include_directories(cmpr PUBLIC src)
target_compile_options(cmpr PRIVATE -Wall -Wextra -Wpedantic)

add_executable(compressor tools/cli.cpp)
target_link_libraries(compressor PRIVATE cmpr)

# Sweeps the LZ77 tuning knobs and prints the ratio/throughput table that goes in
# the writeup. Kept separate from the CLI: it is a measurement instrument, not a
# feature of the compressor.
add_executable(sweep tools/sweep.cpp)
target_link_libraries(sweep PRIVATE cmpr)

add_executable(tests
  tests/testing.cpp
  tests/test_bitio.cpp
  tests/test_huffman.cpp
  tests/test_lz77.cpp
  tests/test_roundtrip.cpp
)
target_link_libraries(tests PRIVATE cmpr)
target_include_directories(tests PRIVATE tests)

enable_testing()
add_test(NAME unit COMMAND tests)
