# LightJS Future Tasks

This document tracks planned enhancements and future work for LightJS.

**Current Status:** ~24,000 LOC, 204+ tests passing, ES2020 support, WebAssembly 1.0, C++17/C++20 dual support

---

## High Priority

### Performance

#### Bytecode Compilation
**Impact:** 10-50x performance improvement
**Complexity:** High
**Status:** Not started

Replace direct AST interpretation with bytecode compilation:
- Design stack-based bytecode instruction set
- Implement AST to bytecode compiler
- Create bytecode virtual machine
- Add bytecode serialization/deserialization
- Implement optimization passes (constant folding, dead code elimination)

#### Inline Caching for Property Access
**Impact:** 3-10x faster property access
**Complexity:** Medium
**Status:** Object shapes infrastructure exists

- Add inline cache to property access nodes
- Implement monomorphic cache (1 shape)
- Extend to polymorphic cache (2-4 shapes)
- Track cache hit/miss statistics

#### Memory Pooling
**Impact:** Reduced allocation overhead
**Complexity:** Medium
**Status:** Not started

- Implement pool allocator for frequently allocated types
- Create pools for Object, Array, Function
- Add pool statistics tracking

### Language Features

#### Proxy and Reflect API
**Impact:** Metaprogramming support
**Complexity:** High
**Status:** Not started

Implement all 13 proxy traps:
- get, set, has, deleteProperty
- apply, construct
- getPrototypeOf, setPrototypeOf
- isExtensible, preventExtensions
- getOwnPropertyDescriptor, defineProperty
- ownKeys

#### Private Class Fields
**Impact:** ES2022 compliance
**Complexity:** Medium
**Status:** Not started

- Parse private field syntax (#field)
- Store private fields separately from public
- Implement static private fields
- Enforce access restrictions

#### Numeric Separators
**Impact:** ES2021 compliance
**Complexity:** Low
**Status:** Not started

- Parse numeric literals with underscores (1_000_000)

---

## Medium Priority

### Standard Library

#### Intl (Internationalization)
**Impact:** I18n support
**Complexity:** High
**Status:** Not started

Consider using ICU library or implement subset:
- Intl.DateTimeFormat
- Intl.NumberFormat
- Intl.Collator

#### Streams API
**Impact:** Async I/O
**Complexity:** High
**Status:** Not started

- Implement ReadableStream
- Implement WritableStream
- Implement TransformStream
- Add backpressure handling

### WebAssembly Enhancements

#### WASM SIMD
**Impact:** 4-8x speedup for numeric workloads
**Complexity:** High
**Status:** Not started

- Add v128 type and instructions
- Implement vector loads/stores
- Implement i8x16, i16x8, i32x4, i64x2 operations
- Leverage existing SIMD infrastructure

#### WASM Threads and Atomics
**Impact:** Parallel execution
**Complexity:** High
**Status:** Not started

- Add SharedArrayBuffer type
- Implement atomic operations
- Add memory.atomic.* instructions
- Implement wait/notify

#### WASM Exception Handling
**Impact:** Better error handling
**Complexity:** Medium
**Status:** Not started

- Add try/catch/throw instructions
- Implement exception tags
- Integrate with JS exceptions

#### WASM Bulk Memory Operations
**Impact:** Performance
**Complexity:** Low
**Status:** Not started

- Implement memory.copy
- Implement memory.fill
- Implement table.copy, table.init

### Developer Experience

#### Interactive Debugger
**Impact:** Better debugging
**Complexity:** High
**Status:** Not started

REPL debugging commands:
- .break - Set breakpoint
- .continue - Resume execution
- .step/.next/.out - Step control
- .inspect - Variable inspection
- .backtrace - Show stack

#### Profiler
**Impact:** Performance analysis
**Complexity:** Medium
**Status:** Not started

- Track function entry/exit times
- Build call tree with timing
- Add memory allocation tracking
- Generate flame graphs
- Export to Chrome DevTools format

#### Source Maps Support
**Impact:** Debug transpiled code
**Complexity:** Medium
**Status:** Not started

- Parse source map JSON
- Map generated locations to original
- Support inline source maps

---

## Low Priority

### Build System

#### Package Manager Support
**Impact:** Easier adoption
**Complexity:** Low
**Status:** Not started

- Create Conan recipe
- Add to vcpkg registry
- Create Homebrew formula
- Create Debian/RPM packages

#### WebAssembly Build Target
**Impact:** Run in browser
**Complexity:** Medium
**Status:** Not started

- Create Emscripten build configuration
- Add JavaScript API wrapper
- Create browser demo

#### Python Bindings
**Impact:** Python interop
**Complexity:** Medium
**Status:** Not started

- Use pybind11 for bindings
- Wrap Lexer, Parser, Interpreter
- Build wheel packages

### Testing

#### Fuzzing Integration
**Impact:** Find crashes and edge cases
**Complexity:** Medium
**Status:** Not started

- Integrate libFuzzer
- Add AFL++ support
- Create fuzzing harness
- Add to CI

#### Expand Test262 Coverage
**Impact:** Standards compliance
**Complexity:** Ongoing
**Status:** Infrastructure exists

- Run full Test262 suite
- Generate compliance report
- Fix failing tests systematically
- Target: 50%+ compliance

### Documentation

#### API Reference
**Impact:** Better docs
**Complexity:** Low
**Status:** Not started

- Add Doxygen comments to all public APIs
- Generate HTML documentation
- Host on GitHub Pages

#### Language Compatibility Matrix
**Impact:** Clear feature status
**Complexity:** Low
**Status:** Not started

Create detailed ES5/ES6/ES2020 feature comparison table

### TypeScript Support

#### Type Stripping (Phase 1)
**Impact:** TS compatibility
**Complexity:** High
**Status:** Not started

Parse and ignore TypeScript syntax:
- Type annotations
- Interfaces/type aliases
- Generics syntax
- Enum to const object

---

## Completed Features

### Recently Completed
- C++17 compatibility layer
- String interning (integrated into lexer)
- Error formatting with stack traces
- Hidden classes/object shapes infrastructure
- Benchmark suite
- TextEncoder/TextDecoder
- URL/URLSearchParams
- File System API (fs module)
- WeakMap/WeakSet
- Enhanced REPL with commands
- Timer APIs (setTimeout, setInterval)
- Generators and iterators
- Memory tracking and heap limits
- ARM64 NEON SIMD support
- WebAssembly 1.0 implementation

### Core Features
- Full ES2020 language support
- Async/await with top-level await
- ES6 modules
- All 12 TypedArray types
- BigInt
- Regular expressions (dual implementation)
- Map/Set collections
- Symbols
- Classes with inheritance
- Destructuring (arrays and objects)
- Spread/rest operators
- Template literals
- Optional chaining (?.)
- Nullish coalescing (??)
- Pure C++ crypto (SHA-256, HMAC)
- Fetch API
- JSON object

---

## Contributing

When working on tasks:
1. Check this file for priority and dependencies
2. Update task status when starting work
3. Add tests for new features
4. Update documentation as needed
5. Run full test suite before committing

---

**Last Updated:** 2026-01-31
