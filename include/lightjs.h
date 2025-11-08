#pragma once

/**
 * @file lightjs.h
 * @brief LightJS - C++20 JavaScript ES2020 Interpreter
 *
 * This is the main convenience header that includes all public LightJS APIs.
 * Include this single header to use LightJS in your application.
 *
 * @example
 * ```cpp
 * #include <lightjs.h>
 *
 * int main() {
 *   // Create global environment
 *   auto env = lightjs::Environment::createGlobal();
 *
 *   // Create interpreter
 *   lightjs::Interpreter interpreter(env);
 *
 *   // Parse JavaScript code
 *   lightjs::Lexer lexer("let x = 40 + 2; x;");
 *   auto tokens = lexer.tokenize();
 *
 *   lightjs::Parser parser(tokens);
 *   auto program = parser.parse();
 *
 *   // Evaluate
 *   auto task = interpreter.evaluate(*program);
 *   while (!task.done()) {
 *     std::coroutine_handle<>::from_address(task.handle.address()).resume();
 *   }
 *
 *   // Get result
 *   lightjs::Value result = task.result();
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
 * @namespace lightjs
 * @brief Main namespace for LightJS interpreter
 *
 * All LightJS classes and functions are in this namespace.
 */
namespace lightjs {

/**
 * @brief Get LightJS version string
 * @return Version string in format "MAJOR.MINOR.PATCH"
 */
inline const char* version() {
  return "1.0.0";
}

/**
 * @brief Get LightJS version major number
 */
inline int version_major() {
  return 1;
}

/**
 * @brief Get LightJS version minor number
 */
inline int version_minor() {
  return 0;
}

/**
 * @brief Get LightJS version patch number
 */
inline int version_patch() {
  return 0;
}

} // namespace lightjs
