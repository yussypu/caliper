cmake_minimum_required(VERSION 3.16)
project(caliper CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -fno-omit-frame-pointer")

include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-march=native" HAS_MARCH_NATIVE)
if(HAS_MARCH_NATIVE)
  add_compile_options(-march=native)
endif()

add_compile_options(-Wall -Wextra)

# Prefetch the next frame in the rx backend ahead of decode. Off by default:
# the hunt writeup measured no benefit under paced load, so it stays a
# reproducible experiment rather than a shipped default. Build with
# -DCALIPER_DECODE_PREFETCH=ON to rerun the after case.
option(CALIPER_DECODE_PREFETCH "prefetch the next rx frame ahead of decode" OFF)
if(CALIPER_DECODE_PREFETCH)
  add_compile_definitions(CALIPER_DECODE_PREFETCH=1)
else()
  add_compile_definitions(CALIPER_DECODE_PREFETCH=0)
endif()

# Replace the book type switch with a branchless cmov dispatch. Off by default;
# build with -DCALIPER_BOOK_BRANCHLESS=ON to measure the before and after the
# hunt writeup records for the branch mispredict finding on real flow.
option(CALIPER_BOOK_BRANCHLESS "branchless book type dispatch" OFF)
if(CALIPER_BOOK_BRANCHLESS)
  add_compile_definitions(CALIPER_BOOK_BRANCHLESS=1)
else()
  add_compile_definitions(CALIPER_BOOK_BRANCHLESS=0)
endif()

# Revert the book to the O(n) full ladder rescan for the best bid and ask. Off
# by default; the book maintains the best incrementally. Build with
# -DCALIPER_BOOK_FULLSCAN=ON to measure the before the hunt writeup records.
option(CALIPER_BOOK_FULLSCAN "full ladder rescan for best bid and ask" OFF)
if(CALIPER_BOOK_FULLSCAN)
  add_compile_definitions(CALIPER_BOOK_FULLSCAN=1)
else()
  add_compile_definitions(CALIPER_BOOK_FULLSCAN=0)
endif()

set(CALIPER_SOURCES
  src/timing/tsc.cpp
  src/hist/hist.cpp
  src/book/book.cpp
  src/rx/sim_transport.cpp
  src/rx/af_xdp.cpp
  src/pmu/pmu_null.cpp
  src/pmu/pmu_perf.cpp
  src/harness/main.cpp
)

add_executable(caliper ${CALIPER_SOURCES})
target_include_directories(caliper PRIVATE src)

find_package(Threads REQUIRED)
target_link_libraries(caliper PRIVATE Threads::Threads)

# The native AF_XDP backend needs libxdp and libbpf. Only x86_64 Linux builds
# the native path, so the portable build on other hosts does not require them.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBXDP REQUIRED libxdp)
  pkg_check_modules(LIBBPF REQUIRED libbpf)
  target_include_directories(caliper PRIVATE ${LIBXDP_INCLUDE_DIRS}
                             ${LIBBPF_INCLUDE_DIRS})
  target_link_libraries(caliper PRIVATE ${LIBXDP_LIBRARIES}
                        ${LIBBPF_LIBRARIES})
endif()

add_custom_target(bench
  COMMAND $<TARGET_FILE:caliper>
  DEPENDS caliper
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  USES_TERMINAL
)

# Unit tests for the histogram and the coordinated omission correction, the
# code every published number depends on. Run with ctest --test-dir build.
enable_testing()
add_executable(hist_test src/hist/hist.cpp src/hist/hist_test.cpp)
target_include_directories(hist_test PRIVATE src)
add_test(NAME hist COMMAND hist_test)

add_executable(book_test src/book/book.cpp src/book/book_test.cpp)
target_include_directories(book_test PRIVATE src)
add_test(NAME book COMMAND book_test)
