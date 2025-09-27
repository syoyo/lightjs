cmake_minimum_required(VERSION 3.20)
project(TinyJS CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")

add_library(tinyjs
  src/lexer.cc
  src/parser.cc
  src/value.cc
  src/environment.cc
  src/interpreter.cc
)

target_include_directories(tinyjs PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(tinyjs_test
  examples/test.cc
)

target_link_libraries(tinyjs_test tinyjs)

enable_testing()
add_test(NAME tinyjs_test COMMAND tinyjs_test)