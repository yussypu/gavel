string(TOUPPER "${CMAKE_BUILD_TYPE}" GAVEL_BENCH_BT)

add_executable(gavel-bench gavel_bench.cpp)
target_link_libraries(gavel-bench PRIVATE gavel_core)
target_include_directories(gavel-bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(gavel-bench PRIVATE -Wall -Wextra -Wconversion -Wshadow -Werror)
target_compile_definitions(gavel-bench PRIVATE
  GAVEL_BENCH_FLAGS="${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${GAVEL_BENCH_BT}}")
