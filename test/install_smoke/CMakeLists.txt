cmake_minimum_required(VERSION 3.24)

project(oil_install_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(oil REQUIRED)

add_executable(oil_static_smoke liboil_smoke.cpp)
target_link_libraries(oil_static_smoke PRIVATE oil::oil_static)

add_executable(oil_shared_smoke liboil_smoke.cpp)
target_link_libraries(oil_shared_smoke PRIVATE oil::oil_shared)
