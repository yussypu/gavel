cmake_minimum_required(VERSION 3.20)
project(gavel CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(GAVEL_SANITIZE "Build with ASan and UBSan" OFF)
option(GAVEL_WERROR "Treat warnings as errors" ON)
set(GAVEL_HARDENING "1" CACHE STRING "Contract level: 0 off, 1 hot path preconditions")

add_library(gavel_core
  src/engine.cpp
  src/match.cpp
)
target_include_directories(gavel_core PUBLIC include)
target_compile_definitions(gavel_core PUBLIC GAVEL_HARDENING=${GAVEL_HARDENING})
target_compile_options(gavel_core PRIVATE -Wall -Wextra -Wconversion -Wshadow)
if(GAVEL_WERROR)
  target_compile_options(gavel_core PRIVATE -Werror)
endif()
if(GAVEL_SANITIZE)
  target_compile_options(gavel_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(gavel_core PUBLIC -fsanitize=address,undefined)
endif()

enable_testing()
add_subdirectory(tests)
add_subdirectory(tools)
# bench uses the arm64 cycle counter and Apple kperf, so it builds on macOS only
if(APPLE)
  add_subdirectory(bench)
endif()
