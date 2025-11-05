cmake_minimum_required(VERSION 3.20)
project(TinyJS CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")

option(USE_SIMPLE_REGEX "Use simple regex implementation instead of std::regex" OFF)

if(USE_SIMPLE_REGEX)
  add_compile_definitions(USE_SIMPLE_REGEX=1)
else()
  add_compile_definitions(USE_SIMPLE_REGEX=0)
endif()

add_library(tinyjs
  src/lexer.cc
  src/parser.cc
  src/value.cc
  src/environment.cc
  src/interpreter.cc
  src/crypto.cc
  src/http.cc
  src/simple_regex.cc
  src/module.cc
  src/gc.cc
  src/gc_value.cc
  src/json.cc
  src/array_methods.cc
  src/string_methods.cc
  src/math_object.cc
  src/date_object.cc
  src/unicode.cc
  src/event_loop.cc
)

target_include_directories(tinyjs PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(tinyjs_test
  tests/test.cc
)

target_link_libraries(tinyjs_test tinyjs)

enable_testing()
add_test(NAME tinyjs_test COMMAND tinyjs_test)

# Garbage collection test
add_executable(gc_test
  tests/gc_test.cc
)

target_link_libraries(gc_test tinyjs)

# JSON and Object methods test
add_executable(json_object_test
  tests/json_object_test.cc
)

target_link_libraries(json_object_test tinyjs)

# Debug test
add_executable(debug_test
  tests/debug_test.cc
)

target_link_libraries(debug_test tinyjs)

# Debug test 2
add_executable(debug_test2
  tests/debug_test2.cc
)

target_link_libraries(debug_test2 tinyjs)

# Simple Object test
add_executable(simple_object_test
  tests/simple_object_test.cc
)

target_link_libraries(simple_object_test tinyjs)

# Array/String/Math/Date methods test
add_executable(methods_test
  tests/methods_test.cc
)

target_link_libraries(methods_test tinyjs)

# Event loop test
add_executable(event_loop_test
  tests/event_loop_test.cc
)

target_link_libraries(event_loop_test tinyjs)

# Generator test
add_executable(generator_test
  tests/generator_test.cc
)

target_link_libraries(generator_test tinyjs)

# Simple generator test
add_executable(generator_simple_test
  tests/generator_simple_test.cc
)

target_link_libraries(generator_simple_test tinyjs)

# Multi-yield generator test
add_executable(generator_multi_yield_test
  tests/generator_multi_yield_test.cc
)

target_link_libraries(generator_multi_yield_test tinyjs)

# Generator for...of test
add_executable(generator_forof_test
  tests/generator_forof_test.cc
)

target_link_libraries(generator_forof_test tinyjs)

# REPL (Read-Eval-Print Loop)
add_executable(tinyjs_repl
  src/repl.cc
)

target_link_libraries(tinyjs_repl tinyjs)

# Test262 conformance runner
add_executable(test262_runner
  test262/test262_runner.cc
  test262/test262_harness.cc
)

target_link_libraries(test262_runner tinyjs)

target_include_directories(test262_runner PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/test262
)