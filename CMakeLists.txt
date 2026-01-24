cmake_minimum_required(VERSION 3.20)
project(TinyJS
  VERSION 1.0.0
  DESCRIPTION "C++20 JavaScript ES2020 Interpreter"
  LANGUAGES CXX
)

# Project options
option(TINYJS_BUILD_TESTS "Build TinyJS tests" ON)
option(TINYJS_BUILD_REPL "Build TinyJS REPL" ON)
option(TINYJS_BUILD_EXAMPLES "Build TinyJS examples" ON)
option(TINYJS_INSTALL "Generate install target" ON)
option(USE_SIMPLE_REGEX "Use simple regex implementation instead of std::regex" OFF)

# C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")

# Add compile definitions
if(USE_SIMPLE_REGEX)
  add_compile_definitions(USE_SIMPLE_REGEX=1)
else()
  add_compile_definitions(USE_SIMPLE_REGEX=0)
endif()

# Include GNUInstallDirs for standard installation paths
include(GNUInstallDirs)

# ==============================================================================
# TinyJS Core Library
# ==============================================================================

set(TINYJS_CORE_SOURCES
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
  src/symbols.cc
)

set(TINYJS_PUBLIC_HEADERS
  include/tinyjs.h
  include/ast.h
  include/environment.h
  include/interpreter.h
  include/lexer.h
  include/parser.h
  include/token.h
  include/value.h
  include/gc.h
  include/module.h
  include/event_loop.h
  include/crypto.h
  include/http.h
  include/unicode.h
  include/json.h
  include/array_methods.h
  include/string_methods.h
  include/math_object.h
  include/date_object.h
  include/object_methods.h
  include/simple_regex.h
)

# Create main library target
add_library(tinyjs ${TINYJS_CORE_SOURCES})
add_library(TinyJS::tinyjs ALIAS tinyjs)

# Set library properties
set_target_properties(tinyjs PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION 1
  PUBLIC_HEADER "${TINYJS_PUBLIC_HEADERS}"
  EXPORT_NAME tinyjs
)

# Include directories
target_include_directories(tinyjs
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/tinyjs>
)

# ==============================================================================
# REPL Executable
# ==============================================================================

if(TINYJS_BUILD_REPL)
  add_executable(tinyjs_repl src/repl.cc)
  target_link_libraries(tinyjs_repl PRIVATE TinyJS::tinyjs)
  set_target_properties(tinyjs_repl PROPERTIES
    OUTPUT_NAME tinyjs
  )
endif()

# ==============================================================================
# Tests
# ==============================================================================

if(TINYJS_BUILD_TESTS)
  enable_testing()

  # Helper function to create test executables
  function(add_tinyjs_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME} PRIVATE TinyJS::tinyjs)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 60)
  endfunction()

  # Core functionality tests
  add_tinyjs_test(tinyjs_test tests/test.cc)
  add_tinyjs_test(gc_test tests/gc_test.cc)
  add_tinyjs_test(json_object_test tests/json_object_test.cc)
  add_tinyjs_test(methods_test tests/methods_test.cc)

  # Event loop tests
  add_tinyjs_test(event_loop_test tests/event_loop_test.cc)

  # Generator tests
  add_tinyjs_test(generator_test tests/generator_test.cc)
  add_tinyjs_test(generator_simple_test tests/generator_simple_test.cc)
  add_tinyjs_test(generator_multi_yield_test tests/generator_multi_yield_test.cc)
  add_tinyjs_test(generator_forof_test tests/generator_forof_test.cc)

  # Debug tests (not added to CTest suite)
  add_executable(debug_test tests/debug_test.cc)
  target_link_libraries(debug_test PRIVATE TinyJS::tinyjs)

  add_executable(debug_test2 tests/debug_test2.cc)
  target_link_libraries(debug_test2 PRIVATE TinyJS::tinyjs)

  add_executable(simple_object_test tests/simple_object_test.cc)
  target_link_libraries(simple_object_test PRIVATE TinyJS::tinyjs)

  # Test262 conformance runner
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test262/test262_runner.cc)
    add_executable(test262_runner
      test262/test262_runner.cc
      test262/test262_harness.cc
    )
    target_link_libraries(test262_runner PRIVATE TinyJS::tinyjs)
    target_include_directories(test262_runner PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/test262
    )
  endif()
endif()

# ==============================================================================
# Installation
# ==============================================================================

if(TINYJS_INSTALL)
  # Install library
  install(TARGETS tinyjs
    EXPORT TinyJSTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/tinyjs
  )

  # Install REPL
  if(TINYJS_BUILD_REPL)
    install(TARGETS tinyjs_repl
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
  endif()

  # Install CMake package configuration
  install(EXPORT TinyJSTargets
    FILE TinyJSTargets.cmake
    NAMESPACE TinyJS::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TinyJS
  )

  # Create and install package config file
  include(CMakePackageConfigHelpers)

  configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TinyJSConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/TinyJSConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TinyJS
  )

  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/TinyJSConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
  )

  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/TinyJSConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/TinyJSConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TinyJS
  )

  # Install pkg-config file
  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/tinyjs.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/tinyjs.pc
    @ONLY
  )

  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/tinyjs.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
  )

  # Install documentation
  install(FILES
    README.md
    CLAUDE.md
    REPL_USAGE.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
  )
endif()

# ==============================================================================
# Summary
# ==============================================================================

message(STATUS "")
message(STATUS "TinyJS Configuration Summary")
message(STATUS "============================")
message(STATUS "Version:           ${PROJECT_VERSION}")
message(STATUS "Build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ compiler:      ${CMAKE_CXX_COMPILER}")
message(STATUS "C++ standard:      ${CMAKE_CXX_STANDARD}")
message(STATUS "Install prefix:    ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")
message(STATUS "Options:")
message(STATUS "  Build tests:     ${TINYJS_BUILD_TESTS}")
message(STATUS "  Build REPL:      ${TINYJS_BUILD_REPL}")
message(STATUS "  Build examples:  ${TINYJS_BUILD_EXAMPLES}")
message(STATUS "  Install:         ${TINYJS_INSTALL}")
message(STATUS "  Simple regex:    ${USE_SIMPLE_REGEX}")
message(STATUS "")
