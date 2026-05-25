cmake_minimum_required(VERSION 3.16)
project(matching_engine_benchmark CXX C)

# -----------------------------------------------------------------------------
# Matching Engine Benchmark Harness
# Published with "The World's Fastest Matching Engine Algorithm"
# (Flash One Technologies, 2026).
#
# `cmake` here builds the harness and the workload generator.
# The three baseline engine adapters need their upstream engine sources and are
# built separately by scripts/build_baselines.sh.
# -----------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3")
set(CMAKE_C_FLAGS_RELEASE "-O3")

# harness + generator land at the repo root so `./harness` works post-`make`.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})

find_package(Threads REQUIRED)
find_package(Boost REQUIRED)   # header-only boost::lockfree::spsc_queue

# --- workload generator -------------------------------------------------------
add_executable(generator workload/generator.cpp)
target_include_directories(generator PRIVATE api)

# --- harness ------------------------------------------------------------------
# Sources live in src/ (not harness/) so the `harness` binary can sit at the
# repo root without colliding with a same-named directory.
add_executable(harness
  src/harness.cpp
  src/transport.cpp
  src/correctness.cpp
  src/platform.cpp
  src/audit.cpp
  third_party/sha256.c
)
target_include_directories(harness PRIVATE api third_party ${Boost_INCLUDE_DIRS})
target_link_libraries(harness PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
