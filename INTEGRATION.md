# LightJS Library Integration Guide

This guide explains how to use LightJS as a library in your C++ applications.

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
# Build and install LightJS
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local
make -j
sudo make install
```

This installs:
- Headers to `/usr/local/include/lightjs/`
- Library to `/usr/local/lib/liblightjs.{a,so}`
- CMake config to `/usr/local/lib/cmake/LightJS/`
- pkg-config file to `/usr/local/lib/pkgconfig/lightjs.pc`
- REPL binary to `/usr/local/bin/lightjs`

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

# Find LightJS
find_package(LightJS REQUIRED)

add_executable(my_app main.cc)
target_link_libraries(my_app PRIVATE LightJS::lightjs)
```

**Benefits:**
- Automatic include path configuration
- Transitive dependency handling
- Version checking support
- Works with CMAKE_PREFIX_PATH

**Usage:**
```bash
cmake .. -DCMAKE_PREFIX_PATH=/path/to/lightjs/install
```

### 2. pkg-config

```bash
# Get compilation flags
pkg-config --cflags lightjs
# Output: -I/usr/local/include/lightjs -std=c++20

# Get linking flags
pkg-config --libs lightjs
# Output: -L/usr/local/lib -llightjs

# Compile application
g++ -std=c++20 $(pkg-config --cflags lightjs) main.cc $(pkg-config --libs lightjs) -o my_app
```

**Benefits:**
- Simple command-line integration
- Works with Makefiles and other build systems
- Standard tool on Unix-like systems

### 3. Manual Linking

```bash
g++ -std=c++20 -I/usr/local/include/lightjs main.cc -L/usr/local/lib -llightjs -o my_app
```

## CMake Build Options

When building LightJS from source, you can customize the build:

```bash
cmake .. \
  -DLIGHTJS_BUILD_TESTS=ON \      # Build test suite (default: ON)
  -DLIGHTJS_BUILD_REPL=ON \       # Build REPL executable (default: ON)
  -DLIGHTJS_BUILD_EXAMPLES=ON \   # Build examples (default: ON)
  -DLIGHTJS_INSTALL=ON \          # Generate install target (default: ON)
  -DUSE_SIMPLE_REGEX=OFF         # Use simple regex vs std::regex (default: OFF)
```

### Minimal Library Build

To build only the library without tests or REPL:

```bash
cmake .. \
  -DLIGHTJS_BUILD_TESTS=OFF \
  -DLIGHTJS_BUILD_REPL=OFF \
  -DLIGHTJS_BUILD_EXAMPLES=OFF
make
```

## API Overview

### Basic Usage Pattern

```cpp
#include <lightjs.h>

lightjs::Value evaluate(const std::string& code) {
  // 1. Tokenize source code
  lightjs::Lexer lexer(code);
  auto tokens = lexer.tokenize();

  // 2. Parse tokens into AST
  lightjs::Parser parser(tokens);
  auto program = parser.parse();

  if (!program) {
    throw std::runtime_error("Parse error");
  }

  // 3. Create environment with built-ins
  auto env = lightjs::Environment::createGlobal();

  // 4. Create interpreter
  lightjs::Interpreter interpreter(env);

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
lightjs::Value undef(lightjs::Undefined{});
lightjs::Value null(lightjs::Null{});
lightjs::Value boolean(true);
lightjs::Value number(42.0);
lightjs::Value string("hello");

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
#include <lightjs.h>
#include <iostream>

int main() {
  lightjs::Lexer lexer("40 + 2");
  auto tokens = lexer.tokenize();

  lightjs::Parser parser(tokens);
  auto program = parser.parse();

  auto env = lightjs::Environment::createGlobal();
  lightjs::Interpreter interpreter(env);

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
auto env = lightjs::Environment::createGlobal();
lightjs::Interpreter interpreter(env);

// Define function
{
  lightjs::Lexer lexer("function greet(name) { return 'Hello, ' + name; }");
  auto tokens = lexer.tokenize();
  lightjs::Parser parser(tokens);
  auto program = parser.parse();

  auto task = interpreter.evaluate(*program);
  while (!task.done()) {
    std::coroutine_handle<>::from_address(task.handle.address()).resume();
  }
}

// Call function later
{
  lightjs::Lexer lexer("greet('World')");
  auto tokens = lexer.tokenize();
  lightjs::Parser parser(tokens);
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
auto env = lightjs::Environment::createGlobal();

// Define custom native function
auto printFunc = std::make_shared<lightjs::Function>();
printFunc->isNative = true;
printFunc->nativeFunc = [](const std::vector<lightjs::Value>& args) -> lightjs::Value {
  for (const auto& arg : args) {
    std::cout << arg.toString() << " ";
  }
  std::cout << std::endl;
  return lightjs::Value(lightjs::Undefined{});
};

env->define("print", lightjs::Value(printFunc));

// Now 'print' is available in JavaScript
lightjs::Interpreter interpreter(env);
// ... evaluate code that calls print()
```

### Example 4: Error Handling

```cpp
try {
  lightjs::Lexer lexer(code);
  auto tokens = lexer.tokenize();

  lightjs::Parser parser(tokens);
  auto program = parser.parse();

  if (!program) {
    std::cerr << "Syntax error in JavaScript code" << std::endl;
    return;
  }

  auto env = lightjs::Environment::createGlobal();
  lightjs::Interpreter interpreter(env);

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
auto env = lightjs::Environment::createGlobal();
lightjs::Interpreter interpreter(env);

for (const auto& script : scripts) {
  // Evaluate multiple scripts with same environment
}

// Bad: Creating environment for each script
for (const auto& script : scripts) {
  auto env = lightjs::Environment::createGlobal(); // Expensive!
  lightjs::Interpreter interpreter(env);
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

LightJS uses automatic garbage collection. No manual memory management needed for JavaScript objects.

### 6. Thread Safety

LightJS is not thread-safe. Use separate `Environment` and `Interpreter` instances per thread.

## Library Structure

```
liblightjs.{a,so}          # Main library
include/lightjs/
  ├── lightjs.h            # Convenience header (include this)
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

**Issue:** `cannot find -llightjs`
- Ensure LightJS is installed
- Add library path: `-L/path/to/lib`
- Set `LD_LIBRARY_PATH` for shared library

**Issue:** `lightjs.h: No such file or directory`
- Add include path: `-I/path/to/include/lightjs`
- Or use `$(pkg-config --cflags lightjs)`

**Issue:** Coroutine errors
- Ensure C++20 is enabled: `-std=c++20`
- Check compiler supports coroutines

## Further Reading

- See `examples/integration/` for complete examples
- See `CLAUDE.md` for architecture details
- See `REPL_USAGE.md` for REPL documentation
- Run `lightjs --help` for REPL usage
