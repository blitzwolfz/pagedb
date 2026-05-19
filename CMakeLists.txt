cmake_minimum_required(VERSION 3.16)
project(pagedb CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

option(PAGEDB_ASAN "build with address and undefined behaviour sanitizer" OFF)
option(PAGEDB_TSAN "build with thread sanitizer" OFF)

if(PAGEDB_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

if(PAGEDB_TSAN)
  add_compile_options(-fsanitize=thread -g)
  add_link_options(-fsanitize=thread)
endif()

include_directories(include)

add_library(pagedb
  src/crc32.cpp
  src/disk_manager.cpp
  src/buffer_pool.cpp
  src/btree.cpp
  src/database.cpp
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
pagedb_test(btree_test)
pagedb_test(database_test)
