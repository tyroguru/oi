cmake_minimum_required(VERSION 3.24)
project(oil_jit_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(oil REQUIRED CONFIG)

add_executable(oil_jit_smoke main.cpp)

target_compile_options(oil_jit_smoke
  PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-g>
)

target_link_libraries(oil_jit_smoke
  PRIVATE
    oil::oil_jit_shared
)
