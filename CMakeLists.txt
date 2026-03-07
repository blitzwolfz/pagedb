cmake_minimum_required(VERSION 3.16)
project(pagedb CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra)

include_directories(include)

add_library(pagedb src/crc32.cpp)
