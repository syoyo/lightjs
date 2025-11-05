# TinyJS Library Integration Guide

This guide explains how to use TinyJS as a library in your C++ applications.

## Table of Contents

- [Installation](#installation)
- [Integration Methods](#integration-methods)
- [CMake Build Options](#cmake-build-options)
- [API Overview](#api-overview)
- [Examples](#examples)
- [Best Practices](#best-practices)

## Installation

### System-Wide Installation

```bash
# Build and install TinyJS
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local
make -j
sudo make install
```

This installs:
- Headers to `/usr/local/include/tinyjs/`
- Library to `/usr/local/lib/libtinyjs.{a,so}`
- CMake config to `/usr/local/lib/cmake/TinyJS/`
- pkg-config file to `/usr/local/lib/pkgconfig/tinyjs.pc`
- REPL binary to `/usr/local/bin/tinyjs`

### Custom Installation Prefix

```bash
cmake .. -DCMAKE_INSTALL_PREFIX=$HOME/.local
make install
```

## Integration Methods

### 1. CMake find_package (Recommended)

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyApp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find TinyJS
find_package(TinyJS REQUIRED)

add_executable(my_app main.cc)
target_link_libraries(my_app PRIVATE TinyJS::tinyjs)
```

**Benefits:**
- Automatic include path configuration
- Transitive dependency handling
- Version checking support
- Works with CMAKE_PREFIX_PATH

**Usage:**
```bash
cmake .. -DCMAKE_PREFIX_PATH=/path/to/tinyjs/install
```

### 2. pkg-config

```bash
# Get compilation flags
pkg-config --cflags tinyjs
# Output: -I/usr/local/include/tinyjs -std=c++20

# Get linking flags
pkg-config --libs tinyjs
# Output: -L/usr/local/lib -ltinyjs

# Compile application
g++ -std=c++20 $(pkg-config --cflags tinyjs) main.cc $(pkg-config --libs tinyjs) -o my_app
```

**Benefits:**
- Simple command-line integration
- Works with Makefiles and other build systems
- Standard tool on Unix-like systems

### 3. Manual Linking

```bash
g++ -std=c++20 -I/usr/local/include/tinyjs main.cc -L/usr/local/lib -ltinyjs -o my_app
```

## CMake Build Options

When building TinyJS from source, you can customize the build:

```bash
cmake .. \
  -DTINYJS_BUILD_TESTS=ON \      # Build test suite (default: ON)
  -DTINYJS_BUILD_REPL=ON \       # Build REPL executable (default: ON)
  -DTINYJS_BUILD_EXAMPLES=ON \   # Build examples (default: ON)
  -DTINYJS_INSTALL=ON \          # Generate install target (default: ON)
  -DUSE_SIMPLE_REGEX=OFF         # Use simple regex vs std::regex (default: OFF)
```

### Minimal Library Build

To build only the library without tests or REPL:

```bash
cmake .. \
  -DTINYJS_BUILD_TESTS=OFF \
  -DTINYJS_BUILD_REPL=OFF \
  -DTINYJS_BUILD_EXAMPLES=OFF
make
```

## API Overview

### Basic Usage Pattern

```cpp
#include <tinyjs.h>

tinyjs::Value evaluate(const std::string& code) {
  // 1. Tokenize source code
  tinyjs::Lexer lexer(code);
  auto tokens = lexer.tokenize();

  // 2. Parse tokens into AST
  tinyjs::Parser parser(tokens);
  auto program = parser.parse();

  if (!program) {
    throw std::runtime_error("Parse error");
  }

  // 3. Create environment with built-ins
  auto env = tinyjs::Environment::createGlobal();

  // 4. Create interpreter
  tinyjs::Interpreter interpreter(env);

  // 5. Evaluate using C++20 coroutines
  auto task = interpreter.evaluate(*program);
  while (!task.done()) {
    std::coroutine_handle<>::from_address(task.handle.address()).resume();
  }

  return task.result();
}
```

### Core Classes

**Lexer** (`lexer.h`)
- Tokenizes JavaScript source code
- `auto tokens = lexer.tokenize()`

**Parser** (`parser.h`)
- Parses tokens into Abstract Syntax Tree (AST)
- `auto program = parser.parse()`
- Returns `std::optional<Program>`

**Interpreter** (`interpreter.h`)
- Evaluates AST using C++20 coroutines
- `auto task = interpreter.evaluate(ast)`
- Requires manual coroutine resumption

**Environment** (`environment.h`)
- Variable scoping and binding
- `Environment::createGlobal()` - Global scope with built-ins
- `env->createChild()` - Child scope

**Value** (`value.h`)
- JavaScript value representation using `std::variant`
- Supports all JS types: undefined, null, boolean, number, string, object, array, function, etc.
- `value.toString()` - Convert to string
- `value.toBool()` - Convert to boolean
- `value.toNumber()` - Convert to number

### Value Type System

```cpp
// Creating values
tinyjs::Value undef(tinyjs::Undefined{});
tinyjs::Value null(tinyjs::Null{});
tinyjs::Value boolean(true);
tinyjs::Value number(42.0);
tinyjs::Value string("hello");

// Type checking
value.isUndefined()
value.isNull()
value.isNumber()
value.isString()
value.isArray()
value.isObject()
value.isFunction()
value.isPromise()
value.isGenerator()
```

## Examples

### Example 1: Simple Evaluation

```cpp
#include <tinyjs.h>
#include <iostream>

int main() {
  tinyjs::Lexer lexer("40 + 2");
  auto tokens = lexer.tokenize();

  tinyjs::Parser parser(tokens);
  auto program = parser.parse();

  auto env = tinyjs::Environment::createGlobal();
  tinyjs::Interpreter interpreter(env);

  auto task = interpreter.evaluate(*program);
  while (!task.done()) {
    std::coroutine_handle<>::from_address(task.handle.address()).resume();
  }

  std::cout << task.result().toString() << std::endl; // Output: 42
}
```

### Example 2: Persistent Environment

```cpp
// Create shared environment
auto env = tinyjs::Environment::createGlobal();
tinyjs::Interpreter interpreter(env);

// Define function
{
  tinyjs::Lexer lexer("function greet(name) { return 'Hello, ' + name; }");
  auto tokens = lexer.tokenize();
  tinyjs::Parser parser(tokens);
  auto program = parser.parse();

  auto task = interpreter.evaluate(*program);
  while (!task.done()) {
    std::coroutine_handle<>::from_address(task.handle.address()).resume();
  }
}

// Call function later
{
  tinyjs::Lexer lexer("greet('World')");
  auto tokens = lexer.tokenize();
  tinyjs::Parser parser(tokens);
  auto program = parser.parse();

  auto task = interpreter.evaluate(*program);
  while (!task.done()) {
    std::coroutine_handle<>::from_address(task.handle.address()).resume();
  }

  std::cout << task.result().toString() << std::endl; // Output: Hello, World
}
```

### Example 3: Custom Native Functions

```cpp
auto env = tinyjs::Environment::createGlobal();

// Define custom native function
auto printFunc = std::make_shared<tinyjs::Function>();
printFunc->isNative = true;
printFunc->nativeFunc = [](const std::vector<tinyjs::Value>& args) -> tinyjs::Value {
  for (const auto& arg : args) {
    std::cout << arg.toString() << " ";
  }
  std::cout << std::endl;
  return tinyjs::Value(tinyjs::Undefined{});
};

env->define("print", tinyjs::Value(printFunc));

// Now 'print' is available in JavaScript
tinyjs::Interpreter interpreter(env);
// ... evaluate code that calls print()
```

### Example 4: Error Handling

```cpp
try {
  tinyjs::Lexer lexer(code);
  auto tokens = lexer.tokenize();

  tinyjs::Parser parser(tokens);
  auto program = parser.parse();

  if (!program) {
    std::cerr << "Syntax error in JavaScript code" << std::endl;
    return;
  }

  auto env = tinyjs::Environment::createGlobal();
  tinyjs::Interpreter interpreter(env);

  auto task = interpreter.evaluate(*program);
  while (!task.done()) {
    std::coroutine_handle<>::from_address(task.handle.address()).resume();
  }

  auto result = task.result();
  std::cout << "Result: " << result.toString() << std::endl;

} catch (const std::exception& e) {
  std::cerr << "Runtime error: " << e.what() << std::endl;
}
```

## Best Practices

### 1. Reuse Environment for Performance

Creating a global environment is expensive. Reuse it when evaluating multiple scripts:

```cpp
// Good: Create once, reuse
auto env = tinyjs::Environment::createGlobal();
tinyjs::Interpreter interpreter(env);

for (const auto& script : scripts) {
  // Evaluate multiple scripts with same environment
}

// Bad: Creating environment for each script
for (const auto& script : scripts) {
  auto env = tinyjs::Environment::createGlobal(); // Expensive!
  tinyjs::Interpreter interpreter(env);
}
```

### 2. Handle Parse Errors

Always check if parsing succeeded:

```cpp
auto program = parser.parse();
if (!program) {
  // Handle parse error
  return;
}
```

### 3. Resume Coroutines Properly

The interpreter uses C++20 coroutines. Always resume until done:

```cpp
auto task = interpreter.evaluate(*program);
while (!task.done()) {
  std::coroutine_handle<>::from_address(task.handle.address()).resume();
}
auto result = task.result();
```

### 4. Use Type Checking Before Conversion

```cpp
if (value.isNumber()) {
  double num = value.toNumber();
} else if (value.isString()) {
  std::string str = value.toString();
}
```

### 5. Memory Management

TinyJS uses automatic garbage collection. No manual memory management needed for JavaScript objects.

### 6. Thread Safety

TinyJS is not thread-safe. Use separate `Environment` and `Interpreter` instances per thread.

## Library Structure

```
libtinyjs.{a,so}          # Main library
include/tinyjs/
  ├── tinyjs.h            # Convenience header (include this)
  ├── lexer.h             # Tokenizer
  ├── parser.h            # Parser
  ├── interpreter.h       # Interpreter
  ├── environment.h       # Scoping
  ├── value.h             # Value types
  ├── ast.h               # AST nodes
  ├── gc.h                # Garbage collector
  ├── event_loop.h        # Event loop (setTimeout, etc.)
  ├── module.h            # ES6 modules
  └── ...                 # Other headers
```

## Compiler Requirements

- **C++20 or later**
- Coroutine support required
- Tested with:
  - GCC 11+
  - Clang 14+
  - MSVC 2022+

## Troubleshooting

**Issue:** `cannot find -ltinyjs`
- Ensure TinyJS is installed
- Add library path: `-L/path/to/lib`
- Set `LD_LIBRARY_PATH` for shared library

**Issue:** `tinyjs.h: No such file or directory`
- Add include path: `-I/path/to/include/tinyjs`
- Or use `$(pkg-config --cflags tinyjs)`

**Issue:** Coroutine errors
- Ensure C++20 is enabled: `-std=c++20`
- Check compiler supports coroutines

## Further Reading

- See `examples/integration/` for complete examples
- See `CLAUDE.md` for architecture details
- See `REPL_USAGE.md` for REPL documentation
- Run `tinyjs --help` for REPL usage
