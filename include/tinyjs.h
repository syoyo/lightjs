#pragma once

/**
 * @file tinyjs.h
 * @brief TinyJS - C++20 JavaScript ES2020 Interpreter
 *
 * This is the main convenience header that includes all public TinyJS APIs.
 * Include this single header to use TinyJS in your application.
 *
 * @example
 * ```cpp
 * #include <tinyjs.h>
 *
 * int main() {
 *   // Create global environment
 *   auto env = tinyjs::Environment::createGlobal();
 *
 *   // Create interpreter
 *   tinyjs::Interpreter interpreter(env);
 *
 *   // Parse JavaScript code
 *   tinyjs::Lexer lexer("let x = 40 + 2; x;");
 *   auto tokens = lexer.tokenize();
 *
 *   tinyjs::Parser parser(tokens);
 *   auto program = parser.parse();
 *
 *   // Evaluate
 *   auto task = interpreter.evaluate(*program);
 *   while (!task.done()) {
 *     std::coroutine_handle<>::from_address(task.handle.address()).resume();
 *   }
 *
 *   // Get result
 *   tinyjs::Value result = task.result();
 *   std::cout << result.toString() << std::endl; // Output: 42
 *
 *   return 0;
 * }
 * ```
 */

// Core interpreter components
#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "environment.h"
#include "value.h"
#include "ast.h"
#include "token.h"

// Runtime support
#include "gc.h"
#include "event_loop.h"
#include "module.h"

// Built-in objects and methods
#include "array_methods.h"
#include "string_methods.h"
#include "math_object.h"
#include "date_object.h"
#include "object_methods.h"
#include "json.h"

// System utilities
#include "crypto.h"
#include "http.h"
#include "unicode.h"
#include "simple_regex.h"

/**
 * @namespace tinyjs
 * @brief Main namespace for TinyJS interpreter
 *
 * All TinyJS classes and functions are in this namespace.
 */
namespace tinyjs {

/**
 * @brief Get TinyJS version string
 * @return Version string in format "MAJOR.MINOR.PATCH"
 */
inline const char* version() {
  return "1.0.0";
}

/**
 * @brief Get TinyJS version major number
 */
inline int version_major() {
  return 1;
}

/**
 * @brief Get TinyJS version minor number
 */
inline int version_minor() {
  return 0;
}

/**
 * @brief Get TinyJS version patch number
 */
inline int version_patch() {
  return 0;
}

} // namespace tinyjs
