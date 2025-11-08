# LightJS Integration Example

This directory demonstrates how to use LightJS as a library in your C++ applications.

## Building the Example

### Method 1: Using Installed LightJS

If you have installed LightJS system-wide:

```bash
cd examples/integration
mkdir build && cd build
cmake ..
make
./simple_app
```

### Method 2: Using LightJS from Source

If building from the LightJS source tree:

```bash
cd examples/integration
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/path/to/lightjs/install
make
./simple_app
```

### Method 3: Manual Compilation

You can also compile directly:

```bash
# With installed LightJS
g++ -std=c++20 simple_app.cc $(pkg-config --cflags --libs lightjs) -o simple_app

# Or with explicit paths
g++ -std=c++20 simple_app.cc -I/path/to/lightjs/include -L/path/to/lightjs/lib -llightjs -o simple_app
```

## Integration Methods

### Using find_package (CMake)

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyApp CXX)

set(CMAKE_CXX_STANDARD 20)

find_package(LightJS REQUIRED)

add_executable(my_app main.cc)
target_link_libraries(my_app PRIVATE LightJS::lightjs)
```

### Using pkg-config

```bash
# Get compiler flags
pkg-config --cflags lightjs

# Get linker flags
pkg-config --libs lightjs

# Compile your application
g++ -std=c++20 $(pkg-config --cflags lightjs) my_app.cc $(pkg-config --libs lightjs)
```

### Direct Linking

```cpp
#include <lightjs.h>

int main() {
  auto env = lightjs::Environment::createGlobal();
  lightjs::Interpreter interpreter(env);

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

- `lightjs::Lexer` - Tokenize JavaScript source code
- `lightjs::Parser` - Parse tokens into AST
- `lightjs::Interpreter` - Execute AST using C++20 coroutines
- `lightjs::Environment` - Variable scoping and built-in objects
- `lightjs::Value` - JavaScript value representation

## Notes

- Requires C++20 compiler with coroutine support
- Link with `-llightjs`
- Include path: `include/lightjs/`
- All LightJS symbols are in the `lightjs` namespace
