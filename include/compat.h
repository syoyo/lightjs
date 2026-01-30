#pragma once

/**
 * LightJS C++ Standard Compatibility Layer
 *
 * This header provides macros to detect the C++ standard version and enable
 * the appropriate code paths for C++17 vs C++20 builds.
 *
 * Key macros:
 *   LIGHTJS_HAS_CPP20     - True if compiling with C++20 or later
 *   LIGHTJS_HAS_COROUTINES - True if C++20 coroutines are available
 *
 * To force C++17 mode even on a C++20 compiler, define LIGHTJS_FORCE_CPP17_MODE
 */

// C++ standard version detection
#if __cplusplus >= 202002L
  #define LIGHTJS_HAS_CPP20 1
  #define LIGHTJS_HAS_COROUTINES 1
#elif __cplusplus >= 201703L
  #define LIGHTJS_HAS_CPP20 0
  #define LIGHTJS_HAS_COROUTINES 0
#else
  #error "LightJS requires at least C++17"
#endif

// Allow forcing C++17 mode even on C++20 compiler (for testing)
#ifdef LIGHTJS_FORCE_CPP17_MODE
  #undef LIGHTJS_HAS_COROUTINES
  #define LIGHTJS_HAS_COROUTINES 0
  #undef LIGHTJS_HAS_CPP20
  #define LIGHTJS_HAS_CPP20 0
#endif

// Feature detection for std::filesystem
// In C++17 mode, we use our own fs_compat layer
#if LIGHTJS_HAS_CPP20
  #define LIGHTJS_HAS_STD_FILESYSTEM 1
#else
  // Use our compatibility layer in C++17 mode
  #define LIGHTJS_HAS_STD_FILESYSTEM 0
#endif

// Optional: detect if running on specific platforms
#if defined(_WIN32) || defined(_WIN64)
  #define LIGHTJS_PLATFORM_WINDOWS 1
  #define LIGHTJS_PLATFORM_POSIX 0
#else
  #define LIGHTJS_PLATFORM_WINDOWS 0
  #define LIGHTJS_PLATFORM_POSIX 1
#endif
