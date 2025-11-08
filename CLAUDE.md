# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

LightJS is a C++20 JavaScript (ES2020) interpreter featuring:
- C++20 coroutine-based async execution model
- **Full async/await support** for asynchronous programming
- No C++ exceptions (`-fno-exceptions` removed, but try/catch is used)
- No RTTI (`-fno-rtti` enabled)
- Pure C/C++ crypto implementations (SHA-256, HMAC)
- Minimal fetch API with platform-specific sockets (Winsock2/POSIX)
- Comprehensive type support: BigInt, TypedArrays (12 types including Float16Array), Promises, RegExp
- **Dual regex implementations**: Choose between std::regex or pure C/C++ simple regex engine

## Build Commands

```bash
# Initial setup (from repository root)
mkdir -p build && cd build

# Build with std::regex (default)
cmake ..
make

# Build with simple regex implementation (pure C/C++, no STL regex)
cmake .. -DUSE_SIMPLE_REGEX=ON
make

# Run all tests (197 tests)
./lightjs_test

# Rebuild after changes
make
```

## High-Level Architecture

### Execution Pipeline

The interpreter follows a four-stage pipeline where each stage operates independently:

1. **Lexer** (`src/lexer.cc`) → Tokenizes source code
2. **Parser** (`src/parser.cc`) → Builds AST using recursive descent
3. **Interpreter** (`src/interpreter.cc`) → Evaluates AST using C++20 coroutines
4. **Environment** (`src/environment.cc`) → Manages variable scope and global objects

Each stage is decoupled and testable independently.

### C++20 Coroutine-Based Execution

**Critical Architecture Decision:** The interpreter uses C++20 coroutines (`co_await`, `co_return`) for evaluation, NOT traditional recursive calls.

- Every `evaluate*()` method returns a `Task` (coroutine handle)
- Tasks must be manually resumed using the coroutine handle
- Standard pattern for evaluation:
  ```cpp
  auto task = interpreter.evaluate(expr);
  while (!task.done()) {
    std::coroutine_handle<>::from_address(task.handle.address()).resume();
  }
  Value result = task.result();
  ```

**Why this matters:** When adding new AST node types or evaluation logic, you MUST use coroutine syntax (`co_await`, `co_return`) in `evaluate*()` methods, never plain `return`.

### Value System (include/value.h)

The `Value` struct uses `std::variant` to represent all JavaScript types:

```cpp
std::variant<
  Undefined, Null, bool, double, BigInt, std::string,
  std::shared_ptr<Function>,
  std::shared_ptr<Array>,
  std::shared_ptr<Object>,
  std::shared_ptr<TypedArray>,
  std::shared_ptr<Promise>
>
```

**Important ordering:** Promise is defined AFTER the complete Value struct to avoid circular dependency issues. If you modify the Value struct, maintain this ordering.

### AST Representation (include/ast.h)

AST nodes use `std::variant` for type-safe representation:
- `Expression` variant contains all expression node types
- `Statement` variant contains all statement node types
- Use `std::get_if<NodeType>()` for type checking in interpreter

**Adding new AST nodes:**
1. Define struct in `ast.h`
2. Add to appropriate variant (`Expression::node` or `Statement::node`)
3. Add token type in `token.h` if needed
4. Handle in `parser.cc` (recursive descent methods)
5. Handle in `interpreter.cc` (add `evaluate*()` coroutine method)

### Environment and Scoping (src/environment.cc)

Implements lexical scoping with parent chain:
- `Environment::createGlobal()` creates the root environment with built-ins
- `Environment::createChild()` creates a new scope linked to parent
- Supports `const` binding enforcement
- Variable lookup walks the parent chain until found or reaches root

**Global built-ins defined in createGlobal():**
- `console.log()` - Native function
- TypedArray constructors (Int8Array, Float16Array, etc.)
- ArrayBuffer, DataView constructors
- String constructor with static methods (fromCharCode, fromCodePoint)
- `crypto` object (sha256, hmac, toHex)
- `fetch()` - Returns Promise with Response object
- `globalThis` - Reference to the global object itself

### Unicode Support (`include/unicode.h`, `src/unicode.cc`)

LightJS provides full UTF-8 Unicode support:
- **UTF-8 aware string operations**: All string methods work with Unicode code points, not bytes
- **String length**: Returns code point count, not byte count (e.g., "👋".length === 1)
- **Character access**: `charAt()` returns full Unicode characters including emoji
- **Code point utilities**: `codePointAt()`, `charCodeAt()`, `fromCharCode()`, `fromCodePoint()`
- **Multi-byte sequences**: Proper handling of 1-4 byte UTF-8 sequences
- **Emoji support**: Full support for emoji including surrogate pairs
- **International scripts**: CJK characters, Arabic, Hebrew, Cyrillic, etc.

**Implementation details:**
- `unicode::utf8Length()` - Count code points in UTF-8 string
- `unicode::codePointAt()` - Get Unicode code point at index
- `unicode::charAt()` - Get character string at code point index
- `unicode::encodeUTF8()` / `decodeUTF8()` - Convert between code points and UTF-8
- Validation and proper error handling for invalid UTF-8 sequences

### ArrayBuffer and DataView Implementation

**ArrayBuffer** (`include/value.h`, `src/value.cc`):
- Fixed-length raw binary data buffer backed by `std::vector<uint8_t>`
- Constructor: `ArrayBuffer(byteLength)` - zero-initializes buffer
- Property: `byteLength` - read-only size of the buffer
- Shared across DataView and TypedArray instances for efficient memory management

**DataView** (`include/value.h`, `src/value.cc`):
- Low-level interface for reading/writing multiple numeric types in an ArrayBuffer
- Constructor: `DataView(buffer, byteOffset?, byteLength?)`
- Properties: `buffer`, `byteOffset`, `byteLength`
- **Get methods**: `getInt8()`, `getUint8()`, `getInt16()`, `getUint16()`, `getInt32()`, `getUint32()`, `getFloat32()`, `getFloat64()`, `getBigInt64()`, `getBigUint64()`
- **Set methods**: `setInt8()`, `setUint8()`, `setInt16()`, `setUint16()`, `setInt32()`, `setUint32()`, `setFloat32()`, `setFloat64()`, `setBigInt64()`, `setBigUint64()`
- **Endianness support**: All multi-byte get/set methods accept optional `littleEndian` parameter (default: false for big-endian)
- **Implementation**: Uses `std::memcpy` and template-based byte swapping for cross-platform endianness handling
- **Bounds checking**: All methods throw RangeError if accessing out of bounds

**Adding DataView methods to interpreter:**
- Methods are dynamically bound in `Interpreter::evaluateMember()` when accessing DataView properties
- Each method returns a native Function that captures the DataView pointer via lambda
- Pattern used: Check `obj.isDataView()`, create native function with captured `viewPtr`, bind to property name

### TypedArray Implementation

TypedArrays store binary data in `std::vector<uint8_t>` with typed accessors:
- `getElement(idx)` and `setElement(idx, val)` handle type conversion
- Float16 uses portable bit-manipulation conversion (no hardware intrinsics)
- BigInt64/BigUint64 arrays use separate `getBigIntElement()`/`setBigIntElement()` methods

**When adding new TypedArray types:** Update `TypedArrayType` enum, `elementSize()`, and both get/set methods in `value.cc`.

**Future enhancement:** TypedArrays can be updated to share ArrayBuffer backing store instead of maintaining separate buffers.

### Async/Await and Promise API

Full async/await support is implemented:
- **Async functions** return Promises automatically
- **Await expressions** properly unwrap Promise values
- **Promise constructor** with executor function
- **Promise.resolve()** and **Promise.reject()** static methods
- **Promise.all()** for parallel Promise handling

**Implementation details:**
- Async functions in `evaluateCall()` wrap their results in Promises
- Await expressions in `evaluateAwait()` extract values from Promises
- Uses C++20 coroutines throughout for async execution
- `fetch()` returns a Promise that resolves with Response object

**Important:** When accessing properties on a Promise (e.g., `fetch(url).status`), the interpreter automatically unwraps the fulfilled Promise result. This logic is in `Interpreter::evaluateMember()`.

**Top-level await:** The interpreter supports top-level await out of the box. Since the evaluation engine is coroutine-based, `await` can be used at the module/script level without requiring an async function wrapper. The `evaluateAwait()` method works anywhere in the execution context.

### HTTP/Fetch Implementation (src/http.cc)

Platform-specific socket code:
- **Windows:** Uses Winsock2 (`WSAStartup`/`WSACleanup`)
- **POSIX:** Uses BSD sockets (Linux, macOS)
- Compile guards: `#ifdef _WIN32` for platform detection

**Protocol handlers:**
- `file://` - Reads from local filesystem using `std::ifstream`
- `http://` - Custom HTTP/1.1 client with socket operations
- `https://` - Returns 501 (structured for future TLS integration)

### Crypto Implementation (src/crypto.cc)

Pure C/C++ implementations without external dependencies:
- **SHA-256:** NIST FIPS 180-4 compliant, processes 512-bit blocks
- **HMAC-SHA256:** RFC 2104 compliant with proper key padding
- **Utilities:** Hex encoding/decoding

**No external crypto libraries:** All cryptographic functions are implemented from scratch for portability.

## Code Conventions

### Error Handling
- Parser returns `std::optional<Program>` - `std::nullopt` indicates parse failure
- No C++ exceptions thrown in hot paths (interpreter loop)
- Use `try/catch` only for native function calls that may throw

### Type Checking Pattern
```cpp
// In interpreter, use std::get_if for safe type checking:
if (auto* node = std::get_if<BinaryExpr>(&expr.node)) {
  co_return co_await evaluateBinary(*node);
}
```

### Adding Native Functions

Global native functions in `environment.cc`:
```cpp
auto myFunc = std::make_shared<Function>();
myFunc->isNative = true;
myFunc->nativeFunc = [](const std::vector<Value>& args) -> Value {
  // Implementation
  return Value(result);
};
env->define("myFunc", Value(myFunc));
```

### File Organization
- `include/` - Public headers (API surface)
- `src/` - Implementation files
- `examples/` - Test files and usage demos
- NO separate tests directory - tests are in `examples/test.cc`

## Testing

Test structure in `examples/test.cc`:
```cpp
runTest("Test name", R"(
  let x = 42;
  x + 8
)", "50");
```

**When adding features:**
1. Add test cases to `test.cc` using `runTest()`
2. Build and verify: `make && ./lightjs_test`
3. All 46 tests must pass before committing

## Platform Considerations

- **Sockets:** Winsock2 on Windows, POSIX everywhere else
- **Compiler:** Requires C++20 for coroutines (`std::coroutine_handle`, `co_await`, `co_return`)
- **CMake minimum:** 3.20 for C++20 support
- **Build flags:** `-fno-rtti` (no RTTI), exceptions are enabled for std::filesystem and native functions