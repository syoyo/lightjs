cmake_minimum_required(VERSION 3.14)
project(LightJS
  VERSION 1.0.0
  DESCRIPTION "C++17/C++20 JavaScript ES2020 Interpreter"
  LANGUAGES CXX
)

# Project options
option(LIGHTJS_BUILD_TESTS "Build LightJS tests" ON)
option(LIGHTJS_BUILD_REPL "Build LightJS REPL" ON)
option(LIGHTJS_BUILD_EXAMPLES "Build LightJS examples" ON)
option(LIGHTJS_INSTALL "Generate install target" ON)
option(USE_SIMPLE_REGEX "Use simple regex implementation instead of std::regex" OFF)
option(USE_SIMD "Enable SIMD optimizations (SSE2/AVX2 on x86, NEON/SVE on ARM64)" OFF)
option(LIGHTJS_USE_CPP17 "Build with C++17 instead of C++20 (uses compatibility layer)" OFF)

# Cross-compilation options for ARM64
option(CROSS_COMPILE_ARM64 "Cross-compile for ARM64 with NEON" OFF)
option(CROSS_COMPILE_A64FX "Cross-compile for Fujitsu A64FX with SVE (512-bit)" OFF)

# ==============================================================================
# Cross-compilation configuration
# ==============================================================================

if(CROSS_COMPILE_A64FX)
  # A64FX implies ARM64 cross-compilation with SVE
  set(CROSS_COMPILE_ARM64 ON)
  set(CROSS_COMPILE_TARGET "a64fx")
  message(STATUS "Cross-compiling for Fujitsu A64FX (ARM64 + SVE 512-bit)")
elseif(CROSS_COMPILE_ARM64)
  set(CROSS_COMPILE_TARGET "arm64")
  message(STATUS "Cross-compiling for ARM64 with NEON")
endif()

if(CROSS_COMPILE_ARM64)
  # Set cross-compilation variables
  set(CMAKE_SYSTEM_NAME Linux)
  set(CMAKE_SYSTEM_PROCESSOR aarch64)

  # Try to find aarch64 cross-compiler if not already set
  if(NOT CMAKE_CXX_COMPILER)
    find_program(AARCH64_CXX_COMPILER
      NAMES aarch64-linux-gnu-g++ aarch64-unknown-linux-gnu-g++ aarch64-linux-g++
    )
    if(AARCH64_CXX_COMPILER)
      set(CMAKE_CXX_COMPILER ${AARCH64_CXX_COMPILER})
      message(STATUS "Found ARM64 cross-compiler: ${AARCH64_CXX_COMPILER}")
    else()
      message(WARNING "ARM64 cross-compiler not found. Set CMAKE_CXX_COMPILER manually.")
    endif()

    find_program(AARCH64_C_COMPILER
      NAMES aarch64-linux-gnu-gcc aarch64-unknown-linux-gnu-gcc aarch64-linux-gcc
    )
    if(AARCH64_C_COMPILER)
      set(CMAKE_C_COMPILER ${AARCH64_C_COMPILER})
    endif()
  endif()

  # Enable SIMD automatically for cross-compilation
  set(USE_SIMD ON)

  # Set architecture-specific flags
  if(CROSS_COMPILE_A64FX)
    # A64FX: ARMv8.2-A with SVE (512-bit vector length)
    # -mcpu=a64fx provides optimal tuning for Fujitsu A64FX
    set(CROSS_COMPILE_FLAGS "-mcpu=a64fx -march=armv8.2-a+sve")
    add_compile_definitions(TARGET_A64FX=1)
    add_compile_definitions(SVE_VECTOR_LENGTH=512)
  else()
    # Generic ARM64 with NEON (ARMv8-A baseline)
    set(CROSS_COMPILE_FLAGS "-march=armv8-a")
    add_compile_definitions(TARGET_ARM64_GENERIC=1)
  endif()
endif()

# C++ standard configuration
if(LIGHTJS_USE_CPP17)
  set(CMAKE_CXX_STANDARD 17)
  add_compile_definitions(LIGHTJS_FORCE_CPP17_MODE=1)
  message(STATUS "Building with C++17 compatibility mode")
else()
  set(CMAKE_CXX_STANDARD 20)
  message(STATUS "Building with C++20 (coroutines enabled)")
endif()
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

# SIMD configuration
if(USE_SIMD)
  add_compile_definitions(USE_SIMD=1)

  # Cross-compilation: use pre-determined settings
  if(CROSS_COMPILE_ARM64)
    set(SIMD_ARCH "arm64")
    add_compile_definitions(SIMD_ARCH_ARM64=1)

    if(CROSS_COMPILE_A64FX)
      # A64FX with SVE (512-bit)
      add_compile_definitions(SIMD_SVE=1)
      add_compile_definitions(SIMD_NEON=1)  # A64FX also supports NEON
      set(SIMD_FLAGS "${CROSS_COMPILE_FLAGS}")
      message(STATUS "SIMD: SVE+NEON enabled (A64FX cross-compile)")
    else()
      # Generic ARM64 with NEON
      add_compile_definitions(SIMD_NEON=1)
      set(SIMD_FLAGS "${CROSS_COMPILE_FLAGS}")
      message(STATUS "SIMD: NEON enabled (ARM64 cross-compile)")
    endif()

  # Native compilation: detect architecture
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64|amd64|i686|i386)")
    set(SIMD_ARCH "x86")
    add_compile_definitions(SIMD_ARCH_X86=1)

    # Check for AVX2 support (up to Zen2)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-mavx2" COMPILER_SUPPORTS_AVX2)
    check_cxx_compiler_flag("-mfma" COMPILER_SUPPORTS_FMA)
    check_cxx_compiler_flag("-msse4.2" COMPILER_SUPPORTS_SSE42)
    check_cxx_compiler_flag("-msse2" COMPILER_SUPPORTS_SSE2)

    if(COMPILER_SUPPORTS_AVX2 AND COMPILER_SUPPORTS_FMA)
      add_compile_definitions(SIMD_AVX2=1)
      set(SIMD_FLAGS "-mavx2 -mfma")
      message(STATUS "SIMD: AVX2+FMA enabled (x86)")
    elseif(COMPILER_SUPPORTS_SSE42)
      add_compile_definitions(SIMD_SSE42=1)
      set(SIMD_FLAGS "-msse4.2")
      message(STATUS "SIMD: SSE4.2 enabled (x86)")
    elseif(COMPILER_SUPPORTS_SSE2)
      add_compile_definitions(SIMD_SSE2=1)
      set(SIMD_FLAGS "-msse2")
      message(STATUS "SIMD: SSE2 enabled (x86)")
    else()
      message(WARNING "SIMD: No supported x86 SIMD instructions found")
      set(USE_SIMD OFF)
    endif()

  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "(aarch64|arm64|ARM64)")
    set(SIMD_ARCH "arm64")
    add_compile_definitions(SIMD_ARCH_ARM64=1)

    # Check for SVE support
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-march=armv8.2-a+sve" COMPILER_SUPPORTS_SVE)

    if(COMPILER_SUPPORTS_SVE)
      add_compile_definitions(SIMD_SVE=1)
      set(SIMD_FLAGS "-march=armv8.2-a+sve")
      message(STATUS "SIMD: SVE enabled (ARM64)")
    else()
      # NEON is always available on ARM64
      add_compile_definitions(SIMD_NEON=1)
      set(SIMD_FLAGS "")
      message(STATUS "SIMD: NEON enabled (ARM64)")
    endif()

  else()
    message(WARNING "SIMD: Unsupported architecture ${CMAKE_SYSTEM_PROCESSOR}")
    set(USE_SIMD OFF)
  endif()

  # Apply SIMD flags if set
  if(USE_SIMD AND SIMD_FLAGS)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SIMD_FLAGS}")
  endif()
else()
  add_compile_definitions(USE_SIMD=0)
endif()

# Include GNUInstallDirs for standard installation paths
include(GNUInstallDirs)

# ==============================================================================
# LightJS Core Library
# ==============================================================================

set(LIGHTJS_CORE_SOURCES
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
  src/string_table.cc
  src/error_formatter.cc
  src/object_shape.cc
  src/text_encoding.cc
  src/url.cc
  src/fs.cc
  src/fs_compat.cc
  src/tls_aes.cc
  src/tls_gcm.cc
  src/tls_x25519.cc
  src/tls_kdf.cc
  src/tls_x509.cc
  src/tls_connection.cc
  src/wasm/wasm_memory.cc
  src/wasm/wasm_decoder.cc
  src/wasm/wasm_interpreter.cc
  src/wasm_js.cc
  src/simd.cc
)

set(LIGHTJS_PUBLIC_HEADERS
  include/lightjs.h
  include/ast.h
  include/compat.h
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
  include/string_table.h
  include/error_formatter.h
  include/object_shape.h
  include/text_encoding.h
  include/url.h
  include/fs.h
  include/fs_compat.h
  include/tls.h
  include/simd.h
  include/wasm/wasm_types.h
  include/wasm/wasm_runtime.h
  include/wasm/wasm_decoder.h
  include/wasm_js.h
)

# Create main library target
add_library(lightjs ${LIGHTJS_CORE_SOURCES})
add_library(LightJS::lightjs ALIAS lightjs)

# Set library properties
set_target_properties(lightjs PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION 1
  PUBLIC_HEADER "${LIGHTJS_PUBLIC_HEADERS}"
  EXPORT_NAME lightjs
)

# Include directories
target_include_directories(lightjs
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/lightjs>
)

# ==============================================================================
# REPL Executable
# ==============================================================================

if(LIGHTJS_BUILD_REPL)
  add_executable(lightjs_repl src/repl.cc)
  target_link_libraries(lightjs_repl PRIVATE LightJS::lightjs)
  set_target_properties(lightjs_repl PROPERTIES
    OUTPUT_NAME lightjs
  )
endif()

# ==============================================================================
# Benchmark Executable
# ==============================================================================

option(LIGHTJS_BUILD_BENCHMARKS "Build LightJS benchmarks" ON)

if(LIGHTJS_BUILD_BENCHMARKS)
  add_executable(lightjs_bench benchmarks/bench_runner.cc)
  target_link_libraries(lightjs_bench PRIVATE LightJS::lightjs)
  set_target_properties(lightjs_bench PROPERTIES
    OUTPUT_NAME bench
  )
endif()

# ==============================================================================
# Tests
# ==============================================================================

if(LIGHTJS_BUILD_TESTS)
  enable_testing()

  # Helper function to create test executables
  function(add_lightjs_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME} PRIVATE LightJS::lightjs)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 60)
  endfunction()

  # Create test fixture file for fetch tests
  file(WRITE ${CMAKE_BINARY_DIR}/test.txt "Hello from file!\n")

  # Core functionality tests
  add_lightjs_test(lightjs_test tests/test.cc)
  add_lightjs_test(gc_test tests/gc_test.cc)
  add_lightjs_test(json_object_test tests/json_object_test.cc)
  add_lightjs_test(methods_test tests/methods_test.cc)

  # Event loop tests
  add_lightjs_test(event_loop_test tests/event_loop_test.cc)

  # Generator tests
  add_lightjs_test(generator_test tests/generator_test.cc)
  add_lightjs_test(generator_simple_test tests/generator_simple_test.cc)
  add_lightjs_test(generator_multi_yield_test tests/generator_multi_yield_test.cc)
  add_lightjs_test(generator_forof_test tests/generator_forof_test.cc)

  # String interning test
  add_lightjs_test(string_interning_test tests/string_interning_test.cc)

  # Error formatting test
  add_lightjs_test(error_formatting_test tests/error_formatting_test.cc)

  # Memory limit test
  add_lightjs_test(memory_limit_test tests/memory_limit_test.cc)

  # Debug tests (not added to CTest suite)
  add_executable(debug_test tests/debug_test.cc)
  target_link_libraries(debug_test PRIVATE LightJS::lightjs)

  add_executable(debug_test2 tests/debug_test2.cc)
  target_link_libraries(debug_test2 PRIVATE LightJS::lightjs)

  add_executable(simple_object_test tests/simple_object_test.cc)
  target_link_libraries(simple_object_test PRIVATE LightJS::lightjs)

  # Test262 conformance runner
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test262/test262_runner.cc)
    add_executable(test262_runner
      test262/test262_runner.cc
      test262/test262_harness.cc
    )
    target_link_libraries(test262_runner PRIVATE LightJS::lightjs)
    target_include_directories(test262_runner PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/test262
    )
  endif()
endif()

# ==============================================================================
# Installation
# ==============================================================================

if(LIGHTJS_INSTALL)
  # Install library
  install(TARGETS lightjs
    EXPORT LightJSTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lightjs
  )

  # Install REPL
  if(LIGHTJS_BUILD_REPL)
    install(TARGETS lightjs_repl
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
  endif()

  # Install CMake package configuration
  install(EXPORT LightJSTargets
    FILE LightJSTargets.cmake
    NAMESPACE LightJS::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LightJS
  )

  # Create and install package config file
  include(CMakePackageConfigHelpers)

  configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/LightJSConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/LightJSConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LightJS
  )

  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/LightJSConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
  )

  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/LightJSConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/LightJSConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LightJS
  )

  # Install pkg-config file
  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/lightjs.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/lightjs.pc
    @ONLY
  )

  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/lightjs.pc
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
message(STATUS "LightJS Configuration Summary")
message(STATUS "============================")
message(STATUS "Version:           ${PROJECT_VERSION}")
message(STATUS "Build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ compiler:      ${CMAKE_CXX_COMPILER}")
message(STATUS "C++ standard:      ${CMAKE_CXX_STANDARD}")
message(STATUS "Install prefix:    ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")
message(STATUS "Options:")
message(STATUS "  Build tests:     ${LIGHTJS_BUILD_TESTS}")
message(STATUS "  Build REPL:      ${LIGHTJS_BUILD_REPL}")
message(STATUS "  Build examples:  ${LIGHTJS_BUILD_EXAMPLES}")
message(STATUS "  Install:         ${LIGHTJS_INSTALL}")
message(STATUS "  Simple regex:    ${USE_SIMPLE_REGEX}")
message(STATUS "  SIMD:            ${USE_SIMD}")
message(STATUS "  C++17 mode:      ${LIGHTJS_USE_CPP17}")
if(USE_SIMD)
  message(STATUS "  SIMD arch:       ${SIMD_ARCH}")
  message(STATUS "  SIMD flags:      ${SIMD_FLAGS}")
endif()
if(CROSS_COMPILE_ARM64)
  message(STATUS "")
  message(STATUS "Cross-compilation:")
  message(STATUS "  Target:          ${CROSS_COMPILE_TARGET}")
  message(STATUS "  System:          ${CMAKE_SYSTEM_NAME}")
  message(STATUS "  Processor:       ${CMAKE_SYSTEM_PROCESSOR}")
  if(CROSS_COMPILE_A64FX)
    message(STATUS "  SVE vector len:  512 bits")
  endif()
endif()
message(STATUS "")
