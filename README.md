# TinyJS

A modern, compact JavaScript (ES2020) interpreter written in C++20 with TypeScript support planned.

## Features

### Core Architecture
- **C++20 coroutine-based execution** - Efficient async execution model
- **No C++ exceptions** in hot paths (compile with `-fno-exceptions` where possible)
- **No RTTI** (`-fno-rtti` enabled) - Reduced binary size
- **CMake build system** - Cross-platform support
- **Security-focused** - Safe evaluation with bounded execution

### Implemented JavaScript Features

#### Core Language (ES2020)
- ✅ **Variables** - `let`, `const`, `var` with proper scoping
- ✅ **Data Types**
  - Primitives: `undefined`, `null`, `boolean`, `number`, `string`, `bigint`
  - Objects and Arrays
  - Functions (regular and arrow functions)
  - Regular Expressions (dual implementation: std::regex or pure C++)
- ✅ **Operators**
  - Arithmetic: `+`, `-`, `*`, `/`, `%`
  - Comparison: `==`, `!=`, `===`, `!==`, `<`, `>`, `<=`, `>=`
  - Logical: `&&`, `||`, `!`
  - Assignment: `=`, `+=`, `-=`, `*=`, `/=`
  - Increment/Decrement: `++`, `--`
  - Conditional: `? :`
  - Type: `typeof`
- ✅ **Control Flow**
  - `if`/`else` statements
  - `while` loops
  - `for` loops
  - `break` and `continue`
  - `return` statements
  - `try`/`catch`/`finally` with error handling
  - `throw` statements
- ✅ **Functions**
  - Function declarations and expressions
  - Arrow functions (parsing ready)
  - Closures and lexical scoping
  - Recursion support
- ✅ **Objects**
  - Object literals
  - Property access (dot and bracket notation)
  - Method calls
  - `this` binding
  - `new` operator for constructors

#### Advanced Features
- ✅ **Async/Await**
  - Full async function support
  - Await expressions with Promise unwrapping
  - Promise API (resolve, reject, all)
  - Automatic Promise wrapping for async functions
- ✅ **ES6 Modules**
  - `import`/`export` statements (named, default, namespace)
  - Module loading and resolution
  - Dependency management and caching
  - Re-exports (`export * from`)
  - Dynamic imports (planned)
- ✅ **TypedArrays** (All 12 types)
  - Int8Array, Uint8Array, Uint8ClampedArray
  - Int16Array, Uint16Array, Float16Array
  - Int32Array, Uint32Array, Float32Array
  - Float64Array, BigInt64Array, BigUint64Array
- ✅ **BigInt**
  - 64-bit integer support
  - BigInt literals (e.g., `123n`)
  - BigInt operations
- ✅ **Regular Expressions**
  - Dual implementation support
  - Standard regex with flags (i, g, m)
  - Pure C++ simple regex engine option

#### Built-in APIs
- ✅ **Console**
  - `console.log()` for output
- ✅ **Crypto** (Pure C++ implementation)
  - SHA-256 hashing
  - HMAC-SHA256
  - Hex encoding/decoding
- ✅ **Fetch API** (Minimal)
  - HTTP/HTTPS support
  - File protocol support
  - Platform-specific sockets (Winsock2/POSIX)
  - Returns Promises
- ✅ **Promise API**
  - Promise constructor
  - `Promise.resolve()`, `Promise.reject()`
  - `Promise.all()`
  - `.then()`, `.catch()` (planned)

### Testing Infrastructure
- ✅ **Test262 Support**
  - Conformance test runner
  - Harness function implementation
  - Metadata parsing (YAML frontmatter)
  - Negative test support
  - Async test handling
  - Module test support

## Build Instructions

### Prerequisites
- C++20 compatible compiler (GCC 10+, Clang 10+, MSVC 2019+)
- CMake 3.20 or higher

### Building

```bash
# Clone the repository
git clone https://github.com/yourusername/tinyjs.git
cd tinyjs

# Create build directory
mkdir build && cd build

# Configure (default: with std::regex)
cmake ..

# Or configure with simple regex implementation (pure C++)
cmake .. -DUSE_SIMPLE_REGEX=ON

# Build
make -j$(nproc)

# Run tests
./tinyjs_test
```

### Running Test262 Conformance Tests

```bash
# Download test262 suite
../scripts/download_test262.sh

# Run conformance tests
./test262_runner ../test262 --test language/expressions
```

## Usage Examples

### Basic Script Execution

```cpp
#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "environment.h"

using namespace tinyjs;

int main() {
    std::string code = R"(
        async function fetchData() {
            let response = await fetch('https://api.example.com/data');
            return response;
        }

        let result = fetchData();
        console.log('Fetching:', result);
    )";

    Lexer lexer(code);
    auto tokens = lexer.tokenize();

    Parser parser(tokens);
    auto program = parser.parse();

    if (!program) {
        std::cerr << "Parse error!" << std::endl;
        return 1;
    }

    auto env = Environment::createGlobal();
    Interpreter interpreter(env);

    auto task = interpreter.evaluate(*program);
    while (!task.done()) {
        std::coroutine_handle<>::from_address(task.handle.address()).resume();
    }

    return 0;
}
```

### Module Usage

```javascript
// math.js
export function add(a, b) {
    return a + b;
}

export const PI = 3.14159;

// main.js
import { add, PI } from './math.js';

console.log('2 + 3 =', add(2, 3));
console.log('PI =', PI);
```

## TODO - Unimplemented Features

### Language Features
- [ ] **Classes** - ES6 class syntax
- [ ] **Generators** - Generator functions and iterators
- [ ] **Destructuring** - Array and object destructuring
- [ ] **Spread/Rest** operators (`...`)
- [ ] **Template literals** - Backtick strings with interpolation
- [ ] **Symbol** type
- [ ] **Map/Set** collections
- [ ] **WeakMap/WeakSet**
- [ ] **Proxy/Reflect** APIs
- [ ] **for...of** loops
- [ ] **for...in** loops
- [ ] **switch** statements
- [ ] **do...while** loops
- [ ] **with** statement (deprecated but in spec)
- [ ] **Labels** and labeled statements
- [ ] **Computed property names**
- [ ] **Property descriptors** and defineProperty
- [ ] **Getters/Setters**
- [ ] **Static class members**
- [ ] **Private class fields**
- [ ] **Optional chaining** (`?.`)
- [ ] **Nullish coalescing** (`??`)
- [ ] **Logical assignment** operators (`&&=`, `||=`, `??=`)
- [ ] **Numeric separators** (`1_000_000`)

### Built-in Objects & APIs
- [ ] **Array methods** - map, filter, reduce, forEach, etc.
- [ ] **String methods** - split, join, substring, replace, etc.
- [ ] **Object methods** - keys, values, entries, assign, etc.
- [ ] **Number methods** - toFixed, toPrecision, parseInt, parseFloat
- [ ] **Math object** - Math.random, Math.floor, Math.ceil, etc.
- [ ] **Date object** - Date manipulation
- [ ] **JSON object** - JSON.parse, JSON.stringify
- [ ] **Error types** - TypeError, ReferenceError, SyntaxError, etc.
- [ ] **Promise methods** - .then(), .catch(), .finally(), race()
- [ ] **ArrayBuffer** and DataView
- [ ] **Intl** - Internationalization API
- [ ] **URL** and URLSearchParams
- [ ] **TextEncoder/TextDecoder**
- [ ] **setTimeout/setInterval** - Timer functions
- [ ] **queueMicrotask**
- [ ] **globalThis**

### Module System Enhancements
- [ ] **Dynamic imports** - `import()` function
- [ ] **Import.meta**
- [ ] **Top-level await**
- [ ] **Module namespace objects**
- [ ] **Circular dependency handling**
- [ ] **CommonJS interop**

### TypeScript Support
- [ ] **Type annotations**
- [ ] **Interfaces**
- [ ] **Type inference**
- [ ] **Generics**
- [ ] **Enums**
- [ ] **Decorators**
- [ ] **Namespaces**
- [ ] **Type guards**
- [ ] **Union/Intersection types**

### Performance & Optimization
- [ ] **JIT compilation**
- [ ] **Bytecode generation**
- [ ] **Optimization passes**
- [ ] **Garbage collection**
- [ ] **Memory pooling**
- [ ] **String interning**

### Developer Tools
- [ ] **Source maps**
- [ ] **Debugger interface**
- [ ] **Profiling hooks**
- [ ] **REPL** (Read-Eval-Print Loop)
- [ ] **AST visitor pattern**
- [ ] **Code formatter**
- [ ] **Linter integration**

### Platform Integration
- [ ] **Node.js compatibility layer**
- [ ] **Browser API compatibility**
- [ ] **WebAssembly support**
- [ ] **Worker threads**
- [ ] **File system API**
- [ ] **Network API enhancements**
- [ ] **Process management**

## Project Structure

```
tinyjs/
├── include/          # Public headers
│   ├── ast.h        # AST node definitions
│   ├── lexer.h      # Tokenization
│   ├── parser.h     # Parsing logic
│   ├── interpreter.h # Execution engine
│   ├── value.h      # Value types
│   ├── environment.h # Scope management
│   └── module.h     # Module system
├── src/             # Implementation files
├── examples/        # Example scripts and tests
├── test262/         # Test262 runner
├── scripts/         # Utility scripts
└── CMakeLists.txt   # Build configuration
```

## Contributing

Contributions are welcome! Please focus on:
1. Implementing TODO features
2. Improving test coverage
3. Performance optimizations
4. Bug fixes
5. Documentation improvements

## License

[Specify your license here]

## Acknowledgments

- Test262 conformance suite by TC39
- C++20 coroutines for efficient async implementation
- Community contributions and feedback