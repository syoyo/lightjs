# TinyJS Integration Example

This directory demonstrates how to use TinyJS as a library in your C++ applications.

## Building the Example

### Method 1: Using Installed TinyJS

If you have installed TinyJS system-wide:

```bash
cd examples/integration
mkdir build && cd build
cmake ..
make
./simple_app
```

### Method 2: Using TinyJS from Source

If building from the TinyJS source tree:

```bash
cd examples/integration
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/path/to/tinyjs/install
make
./simple_app
```

### Method 3: Manual Compilation

You can also compile directly:

```bash
# With installed TinyJS
g++ -std=c++20 simple_app.cc $(pkg-config --cflags --libs tinyjs) -o simple_app

# Or with explicit paths
g++ -std=c++20 simple_app.cc -I/path/to/tinyjs/include -L/path/to/tinyjs/lib -ltinyjs -o simple_app
```

## Integration Methods

### Using find_package (CMake)

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyApp CXX)

set(CMAKE_CXX_STANDARD 20)

find_package(TinyJS REQUIRED)

add_executable(my_app main.cc)
target_link_libraries(my_app PRIVATE TinyJS::tinyjs)
```

### Using pkg-config

```bash
# Get compiler flags
pkg-config --cflags tinyjs

# Get linker flags
pkg-config --libs tinyjs

# Compile your application
g++ -std=c++20 $(pkg-config --cflags tinyjs) my_app.cc $(pkg-config --libs tinyjs)
```

### Direct Linking

```cpp
#include <tinyjs.h>

int main() {
  auto env = tinyjs::Environment::createGlobal();
  tinyjs::Interpreter interpreter(env);

  // Your code here...
}
```

## API Usage

See `simple_app.cc` for complete examples of:

1. **Simple Evaluation** - Execute JavaScript code and get results
2. **Functions** - Define and call JavaScript functions
3. **Arrays** - Create and manipulate arrays
4. **Objects** - Work with JavaScript objects
5. **Generators** - Use ES2020 generator functions
6. **Persistent Environment** - Maintain state across evaluations

## Key Classes

- `tinyjs::Lexer` - Tokenize JavaScript source code
- `tinyjs::Parser` - Parse tokens into AST
- `tinyjs::Interpreter` - Execute AST using C++20 coroutines
- `tinyjs::Environment` - Variable scoping and built-in objects
- `tinyjs::Value` - JavaScript value representation

## Notes

- Requires C++20 compiler with coroutine support
- Link with `-ltinyjs`
- Include path: `include/tinyjs/`
- All TinyJS symbols are in the `tinyjs` namespace
