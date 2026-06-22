cmake_minimum_required(VERSION 3.16)
project(caliper_bridge CXX)

# The bridge links gavel's matching engine to produce a caliper feed from a
# gavel input stream. gavel needs C++23; caliper proper does not, so this lives
# in its own build and is the only place the two projects are compiled together.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

set(GAVEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../gavel" CACHE PATH "path to the gavel checkout")
set(CALIPER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src" CACHE PATH "path to caliper src")

add_executable(gavel_to_feed
  gavel_to_feed.cpp
  ${GAVEL_DIR}/src/engine.cpp
  ${GAVEL_DIR}/src/match.cpp
)
target_include_directories(gavel_to_feed PRIVATE ${GAVEL_DIR}/include ${CALIPER_SRC})
