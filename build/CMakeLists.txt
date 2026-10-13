cmake_minimum_required(VERSION 3.16)
project(Grace LANGUAGES CXX)

# Layout: sources in ../src, this file in build/, binary in build/bin/.
# Configure with:  cmake -S build -B build/cmake && cmake --build build/cmake

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  message(FATAL_ERROR "Grace requires GCC or Clang (uses __builtin_popcount).")
endif()

get_filename_component(GRACE_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src" ABSOLUTE)

find_package(Threads REQUIRED)

add_executable(grace
  ${GRACE_SRC_DIR}/main.cpp
  ${GRACE_SRC_DIR}/game.cpp
)
target_include_directories(grace PRIVATE ${GRACE_SRC_DIR})
target_compile_options(grace PRIVATE -Wall -Wextra)
target_link_libraries(grace PRIVATE Threads::Threads)
set_target_properties(grace PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)

# std::filesystem lives in a separate library on GCC < 9.1
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.1)
  target_link_libraries(grace PRIVATE stdc++fs)
endif()