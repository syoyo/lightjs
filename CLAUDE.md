# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

TinyJS is a C++20 JavaScript (ES2020) interpreter featuring:
- C++20 coroutine-based async execution model
- No C++ exceptions (`-fno-exceptions` removed, but try/catch is used)
- No RTTI (`-fno-rtti` enabled)
- Pure C/C++ crypto implementations (SHA-256, HMAC)
- Minimal fetch API with platform-specific sockets (Winsock2/POSIX)
- Comprehensive type support: BigInt, TypedArrays (12 types including Float16Array), Promises

## Build Commands

```bash
# Initial setup (from repository root)
mkdir -p build && cd build
cmake ..

# Build the project
make

# Run all tests (46 tests)
./tinyjs_test

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
- `crypto` object (sha256, hmac, toHex)
- `fetch()` - Returns Promise with Response object

### TypedArray Implementation

TypedArrays store binary data in `std::vector<uint8_t>` with typed accessors:
- `getElement(idx)` and `setElement(idx, val)` handle type conversion
- Float16 uses portable bit-manipulation conversion (no hardware intrinsics)
- BigInt64/BigUint64 arrays use separate `getBigIntElement()`/`setBigIntElement()` methods

**When adding new TypedArray types:** Update `TypedArrayType` enum, `elementSize()`, and both get/set methods in `value.cc`.

### Promise and Fetch API

Promises are currently synchronous (resolve immediately):
- `fetch()` in `environment.cc` executes HTTP/file requests synchronously
- Returns fulfilled Promise with Response object
- Promise property access is handled specially in `evaluateMember()` in `interpreter.cc`

**Important:** When accessing properties on a Promise (e.g., `fetch(url).status`), the interpreter automatically unwraps the fulfilled Promise result. This logic is in `Interpreter::evaluateMember()`.

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
2. Build and verify: `make && ./tinyjs_test`
3. All 46 tests must pass before committing

## Platform Considerations

- **Sockets:** Winsock2 on Windows, POSIX everywhere else
- **Compiler:** Requires C++20 for coroutines (`std::coroutine_handle`, `co_await`, `co_return`)
- **CMake minimum:** 3.20 for C++20 support
- **Build flags:** `-fno-rtti` (no RTTI), exceptions are enabled for std::filesystem and native functions