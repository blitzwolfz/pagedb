cmake_minimum_required(VERSION 3.16)
project(pagedb CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

include_directories(include)

add_library(pagedb
  src/crc32.cpp
  src/disk_manager.cpp
  src/buffer_pool.cpp
)

enable_testing()

function(pagedb_test name)
  add_executable(${name} tests/${name}.cpp)
  target_link_libraries(${name} pagedb)
  target_include_directories(${name} PRIVATE tests)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

pagedb_test(disk_test)
pagedb_test(buffer_pool_test)
