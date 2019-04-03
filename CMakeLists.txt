cmake_minimum_required(VERSION 3.0)
project(benchmark)

enable_testing()

set(CMAKE_CXX_STANDARD 14)
add_executable(benchmark main.cpp)
add_test(benchmark benchmark)
