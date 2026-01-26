# LightJS Enhancement TODO

This document outlines prioritized enhancements and recommendations for LightJS based on comprehensive codebase analysis (January 2026).

**Current State:** ~22,400 LOC, 204 passing tests, full ES2020 async/await, WebAssembly 1.0, HTTPS/TLS 1.3

---

## Priority Legend
- 🔴 **Critical** - Essential for production readiness
- 🟠 **High** - Major feature gaps or performance wins
- 🟡 **Medium** - Nice to have, moderate impact
- 🟢 **Low** - Future considerations

**Impact:** 🚀 High | ⚡ Medium | 💡 Low
**Complexity:** 🔥 High | 🌶️ Medium | 🍃 Low

---

## Q1 2026: Performance Foundation

### 🔴 1. Bytecode Compilation Pipeline
**Impact:** 🚀 (10-50x performance improvement)
**Complexity:** 🔥
**Effort:** ~2,000 LOC

**Description:** Replace direct AST interpretation with bytecode compilation.

**Current Flow:**
```
Source → Lexer → Parser → AST → Interpreter (direct execution)
```

**Target Flow:**
```
Source → Lexer → Parser → AST → Bytecode Compiler → VM Executor
```

**Implementation Tasks:**
- [ ] Design bytecode instruction set (`include/bytecode.h`)
  - Stack-based instructions (PUSH, POP, ADD, SUB, etc.)
  - Local/global variable access (LOAD_LOCAL, STORE_GLOBAL)
  - Control flow (JUMP, JUMP_IF_FALSE, CALL, RETURN)
  - Object operations (GET_PROPERTY, SET_PROPERTY)
- [ ] Implement `Compiler` class to translate AST → bytecode
- [ ] Create `BytecodeVM` class for execution
- [ ] Add bytecode serialization/deserialization
- [ ] Keep AST interpreter as fallback mode (cmake option)
- [ ] Add bytecode optimization passes
  - Constant folding
  - Dead code elimination
  - Peephole optimization
- [ ] Benchmark against AST interpreter

**Files to Create:**
- `include/bytecode.h` - Instruction definitions
- `include/compiler.h` - AST → bytecode compiler
- `include/vm.h` - Bytecode virtual machine
- `src/bytecode.cc`, `src/compiler.cc`, `src/vm.cc`

---

### 🟠 2. String Interning
**Impact:** ⚡ (20-40% memory reduction, faster comparisons)
**Complexity:** 🌶️
**Effort:** ~500 LOC

**Description:** Global string table for deduplication of property names, identifiers, and string literals.

**Benefits:**
- Property names stored once globally
- O(1) string equality via pointer comparison
- Reduced memory for duplicate strings
- Cache-friendly property lookup

**Implementation:**
```cpp
// Add to include/string_table.h
class StringTable {
  std::unordered_map<std::string_view, std::shared_ptr<std::string>> table;
  std::mutex mutex; // Thread safety

public:
  std::shared_ptr<std::string> intern(std::string_view str);
  void clear(); // For testing
  size_t size() const;
  size_t memoryUsage() const;
};
```

**Tasks:**
- [ ] Create `StringTable` class with thread-safe interning
- [ ] Integrate into `Lexer` for identifiers/string literals
- [ ] Use interned strings for object property names
- [ ] Modify `Value::toString()` to return interned strings
- [ ] Add statistics tracking (hit rate, memory saved)
- [ ] Benchmark memory usage improvement

**Files to Create:**
- `include/string_table.h`
- `src/string_table.cc`

**Files to Modify:**
- `include/value.h` - Use interned string pointers
- `src/lexer.cc` - Intern identifiers and literals
- `src/interpreter.cc` - Use interned strings for property access

---

### 🟠 3. Hidden Classes / Object Shapes
**Impact:** 🚀 (2-5x faster property access)
**Complexity:** 🔥
**Effort:** ~1,500 LOC

**Description:** Track object structure (shape) to enable optimized property storage and access.

**Concept:**
Objects with the same property names (in same order) share a "shape". Properties stored in flat array indexed by shape offset.

```javascript
// These share the same shape:
let obj1 = {x: 1, y: 2};
let obj2 = {x: 10, y: 20};
```

**Implementation:**
```cpp
class ObjectShape {
  std::vector<std::string_view> properties; // Interned strings
  std::unordered_map<std::string_view, size_t> propertyMap;
  std::shared_ptr<ObjectShape> parent; // Prototype chain
  std::unordered_map<std::string_view, std::shared_ptr<ObjectShape>> transitions;

public:
  size_t getPropertyOffset(std::string_view name) const;
  std::shared_ptr<ObjectShape> addProperty(std::string_view name);
};

// Modify Object in value.h
struct Object {
  std::shared_ptr<ObjectShape> shape;
  std::vector<Value> slots; // Dense array indexed by shape
  // Fallback for dynamic properties
  std::unordered_map<std::string, Value> dynamicProperties;
};
```

**Tasks:**
- [ ] Design ObjectShape class with transitions
- [ ] Implement shape-based Object storage
- [ ] Add inline caching for property access (see section 5)
- [ ] Handle shape transitions (adding/deleting properties)
- [ ] Optimize common patterns (constructor initialization)
- [ ] Add shape statistics and debugging

**Files to Create:**
- `include/object_shape.h`
- `src/object_shape.cc`

**Files to Modify:**
- `include/value.h` - Add shape to Object
- `src/interpreter.cc` - Use shapes for property access

---

### 🟡 4. Inline Caching for Property Access
**Impact:** ⚡ (3-10x faster property access)
**Complexity:** 🌶️
**Effort:** ~800 LOC

**Description:** Cache property offsets at call sites for polymorphic property access.

**Depends On:** Hidden Classes (section 3)

**Implementation:**
```cpp
// Add to interpreter.h
struct InlineCache {
  std::shared_ptr<ObjectShape> shape; // Expected shape
  size_t offset;                       // Property offset
  size_t hitCount = 0;
  size_t missCount = 0;
};

// In bytecode instructions or AST nodes
struct PropertyAccess {
  std::string propertyName;
  InlineCache cache[4]; // Polymorphic inline cache (PIC)
};
```

**Tasks:**
- [ ] Add InlineCache to property access nodes/instructions
- [ ] Implement monomorphic cache (1 shape)
- [ ] Extend to polymorphic cache (2-4 shapes)
- [ ] Add cache invalidation on shape changes
- [ ] Track cache hit/miss statistics
- [ ] Benchmark improvement on property-heavy code

---

### 🟡 5. Memory Pooling
**Impact:** ⚡ (Reduced allocation overhead, better cache locality)
**Complexity:** 🌶️
**Effort:** ~600 LOC

**Description:** Pool allocator for frequently allocated types (Value, Object, Array).

**Implementation:**
```cpp
template<typename T, size_t ChunkSize = 256>
class PoolAllocator {
  struct Chunk {
    std::array<T, ChunkSize> storage;
    std::bitset<ChunkSize> used;
  };

  std::vector<std::unique_ptr<Chunk>> chunks;
  std::vector<T*> freeList;

public:
  T* allocate();
  void deallocate(T* ptr);
  void clear();
};
```

**Tasks:**
- [ ] Implement PoolAllocator template
- [ ] Create pools for Object, Array, Function
- [ ] Integrate with Value creation
- [ ] Add pool statistics (utilization, fragmentation)
- [ ] Benchmark allocation performance

**Files to Create:**
- `include/pool_allocator.h`
- `src/pool_allocator.cc`

---

### 🟢 6. Fast Path Optimizations
**Impact:** ⚡
**Complexity:** 🌶️
**Effort:** ~400 LOC

**Description:** Specialized fast paths for common operations.

**Tasks:**
- [ ] Small integer (SMI) optimization
  - Tag low bit to distinguish int32 from double
  - Avoid double conversion for integer arithmetic
- [ ] Fast array index access
  - Recognize numeric strings "0", "1", etc.
  - Direct array access without property lookup
- [ ] Function call fast path
  - Inline small functions (< 50 bytecode instructions)
  - Skip argument marshaling for known arity
- [ ] Binary operation fast path
  - Type-specialized add/subtract/multiply

---

## Q2 2026: Language Completeness

### 🔴 7. Generators and Iterators
**Impact:** 🚀 (ES6 compliance, enables advanced patterns)
**Complexity:** 🔥
**Effort:** ~1,200 LOC

**Description:** Implement generator functions and iterator protocol.

**Target Code:**
```javascript
function* fibonacci() {
  let [a, b] = [0, 1];
  while (true) {
    yield a;
    [a, b] = [b, a + b];
  }
}

const gen = fibonacci();
console.log(gen.next().value); // 0
console.log(gen.next().value); // 1
console.log(gen.next().value); // 1
```

**Implementation:**
```cpp
// Add to value.h
struct Generator {
  std::coroutine_handle<> handle;
  Value currentValue;
  bool done = false;
};

struct GeneratorFunction {
  std::shared_ptr<Function> func;
};
```

**Tasks:**
- [ ] Add `yield` keyword to lexer/parser
- [ ] Add YieldExpr AST node
- [ ] Implement GeneratorFunction type
- [ ] Implement Generator type with next() method
- [ ] Use C++20 coroutines for generator state
- [ ] Add Symbol.iterator support
- [ ] Implement for...of with custom iterables
- [ ] Add generator delegation (yield*)
- [ ] Add tests for generators

**Files to Modify:**
- `include/token.h` - Add YIELD token
- `include/ast.h` - Add YieldExpr
- `include/value.h` - Add Generator types
- `src/parser.cc` - Parse function* and yield
- `src/interpreter.cc` - Evaluate generators

---

### 🟠 8. Complete Destructuring Assignment
**Impact:** ⚡ (ES6 feature parity)
**Complexity:** 🌶️
**Effort:** ~800 LOC

**Current:** Basic array destructuring implemented
**Missing:** Nested destructuring, object destructuring, defaults, rest

**Target Code:**
```javascript
// Object destructuring with defaults and rest
const {name, age = 18, ...rest} = user;

// Array destructuring with rest
const [first, second, ...others] = array;

// Nested destructuring
const {address: {city, zip}} = person;

// Parameter destructuring
function greet({name, greeting = "Hello"}) {
  return `${greeting}, ${name}!`;
}
```

**Tasks:**
- [ ] Extend parser for full destructuring patterns
- [ ] Add DestructuringPattern AST node
- [ ] Implement default value evaluation
- [ ] Implement rest pattern (...rest)
- [ ] Support nested destructuring
- [ ] Function parameter destructuring
- [ ] Add comprehensive tests

**Files to Modify:**
- `include/ast.h` - Extend destructuring nodes
- `src/parser.cc` - Parse all destructuring forms
- `src/interpreter.cc` - Evaluate destructuring

---

### 🟠 9. Proxy and Reflect API
**Impact:** ⚡ (Metaprogramming, observability frameworks)
**Complexity:** 🔥
**Effort:** ~1,500 LOC

**Description:** Intercept and customize object operations.

**Target Code:**
```javascript
const handler = {
  get(target, prop, receiver) {
    console.log(`Getting ${prop}`);
    return target[prop];
  },
  set(target, prop, value, receiver) {
    console.log(`Setting ${prop} = ${value}`);
    target[prop] = value;
    return true;
  }
};

const proxy = new Proxy(obj, handler);
proxy.name; // Logs: Getting name
```

**Implementation:**
```cpp
// Add to value.h
struct ProxyHandler {
  std::shared_ptr<Object> handler;
};

struct Proxy {
  Value target;
  std::shared_ptr<ProxyHandler> handler;
};
```

**Tasks:**
- [ ] Add Proxy and ProxyHandler types
- [ ] Implement all 13 proxy traps:
  - get, set, has, deleteProperty
  - apply, construct
  - getPrototypeOf, setPrototypeOf
  - isExtensible, preventExtensions
  - getOwnPropertyDescriptor, defineProperty
  - ownKeys
- [ ] Intercept operations in interpreter
- [ ] Implement Reflect API as static methods
- [ ] Add revocable proxies (Proxy.revocable)
- [ ] Handle proxy invariants and validation
- [ ] Add comprehensive tests

**Files to Modify:**
- `include/value.h` - Add Proxy types
- `src/environment.cc` - Add Proxy/Reflect globals
- `src/interpreter.cc` - Intercept all object ops

---

### 🟠 10. WeakMap and WeakSet
**Impact:** ⚡ (Better memory management, enable caches)
**Complexity:** 🌶️
**Effort:** ~600 LOC

**Description:** Collections with weak references that don't prevent garbage collection.

**Implementation:**
```cpp
// Requires integration with GC
struct WeakMap {
  std::map<Object*, Value, std::owner_less<>> map;
  // Register with GC for cleanup
};
```

**Tasks:**
- [ ] Add WeakMap type with weak key references
- [ ] Add WeakSet type
- [ ] Integrate with garbage collector
- [ ] Implement has(), get(), set(), delete()
- [ ] Add GC cleanup callbacks
- [ ] Prevent enumeration/iteration (per spec)
- [ ] Add tests with GC interaction

**Files to Create:**
- `include/weak_collections.h`
- `src/weak_collections.cc`

**Files to Modify:**
- `src/gc.cc` - Add weak reference handling

---

### 🟡 11. Symbol Type (Complete Implementation)
**Impact:** ⚡ (ES6 primitives, enable well-known symbols)
**Complexity:** 🌶️
**Effort:** ~500 LOC

**Current:** Basic Symbol implementation exists
**Missing:** Well-known symbols, Symbol registry

**Target Code:**
```javascript
const sym1 = Symbol('description');
const sym2 = Symbol.for('key'); // Global registry
const sym3 = Symbol.for('key'); // Same as sym2

// Well-known symbols
Symbol.iterator
Symbol.toStringTag
Symbol.toPrimitive
```

**Tasks:**
- [ ] Implement global symbol registry
- [ ] Add Symbol.for() and Symbol.keyFor()
- [ ] Implement well-known symbols:
  - Symbol.iterator
  - Symbol.toStringTag
  - Symbol.toPrimitive
  - Symbol.hasInstance
  - Symbol.asyncIterator
- [ ] Support symbols as property keys
- [ ] Add Symbol.description property

**Files to Modify:**
- `include/symbols.h` - Add well-known symbols
- `src/symbols.cc` - Implement registry
- `src/environment.cc` - Add Symbol methods

---

### 🟡 12. Private Fields and Static Class Members
**Impact:** ⚡
**Complexity:** 🌶️
**Effort:** ~700 LOC

**Target Code:**
```javascript
class Counter {
  #count = 0;
  static instances = 0;
  static #privateStatic = 'hidden';

  constructor() {
    Counter.instances++;
  }

  get count() { return this.#count; }
  increment() { this.#count++; }
}
```

**Tasks:**
- [ ] Parse private field syntax (#field)
- [ ] Add PrivateField to AST
- [ ] Store private fields separately from public
- [ ] Implement static members on class object
- [ ] Add private static fields
- [ ] Enforce private field access restrictions
- [ ] Add tests for private fields

---

## Q3 2026: Developer Experience

### 🔴 13. Enhanced Error Messages with Stack Traces
**Impact:** 🚀 (Massive DX improvement)
**Complexity:** 🌶️
**Effort:** ~1,000 LOC

**Current:** Basic error messages
**Target:** JavaScript-quality error reporting

**Example Output:**
```
ReferenceError: foo is not defined
  at myFunction (script.js:15:5)
  at calculateTotal (script.js:28:12)
  at <module> (script.js:35:1)

  13 | function myFunction() {
  14 |   let x = 10;
> 15 |   return foo + x;
     |          ^^^
  16 | }
```

**Implementation:**
```cpp
// Add to all AST nodes
struct SourceLocation {
  std::string filename;
  size_t line;
  size_t column;
  size_t offset;
};

// Maintain call stack
struct StackFrame {
  std::string functionName;
  SourceLocation location;
};

std::vector<StackFrame> callStack;
```

**Tasks:**
- [ ] Add SourceLocation to all AST nodes
- [ ] Maintain call stack during execution
- [ ] Store original source code for display
- [ ] Implement Error.captureStackTrace()
- [ ] Format errors with context (3 lines before/after)
- [ ] Add syntax highlighting (optional)
- [ ] Show column markers (^^^)
- [ ] Add error cause chain (.cause property)

**Files to Modify:**
- `include/ast.h` - Add SourceLocation to nodes
- `include/interpreter.h` - Add call stack
- `src/parser.cc` - Populate source locations
- `src/interpreter.cc` - Maintain stack trace

---

### 🟠 14. Interactive Debugger
**Impact:** ⚡
**Complexity:** 🔥
**Effort:** ~1,500 LOC

**Description:** Extend REPL with debugging capabilities.

**Target Commands:**
```javascript
.break myFunction:15  // Set breakpoint
.continue            // Resume execution
.step                // Step into
.next                // Step over
.out                 // Step out
.inspect x           // Inspect variable
.backtrace           // Show stack
.list                // Show source around current line
```

**Implementation:**
- [ ] Add breakpoint support to interpreter
- [ ] Implement step/continue/next/out
- [ ] Add variable inspection
- [ ] Show call stack and locals
- [ ] Source code display with current line marker
- [ ] Watch expressions
- [ ] Conditional breakpoints

**Files to Modify:**
- `src/repl.cc` - Add debug commands
- `src/interpreter.cc` - Add debugging hooks

---

### 🟠 15. Profiler
**Impact:** ⚡ (Performance analysis)
**Complexity:** 🌶️
**Effort:** ~800 LOC

**Description:** Built-in CPU and memory profiling.

**Target API:**
```javascript
console.profile('myOperation');
// ... code ...
console.profileEnd('myOperation');

// Output:
// myOperation: 245ms
//   calculateTotal: 180ms (73%)
//   renderUI: 65ms (27%)
```

**Implementation:**
- [ ] Track function entry/exit times
- [ ] Build call tree with timing
- [ ] Add memory allocation tracking
- [ ] Generate flame graphs
- [ ] Add console.profile/profileEnd
- [ ] Export profiles (Chrome DevTools format)

**Files to Create:**
- `include/profiler.h`
- `src/profiler.cc`

---

### 🟡 16. Enhanced REPL
**Impact:** ⚡
**Complexity:** 🍃
**Effort:** ~400 LOC

**Features:**
- [ ] Multi-line input support (detect incomplete expressions)
- [ ] History persistence (~/.lightjs_history)
- [ ] Tab completion for globals
- [ ] Syntax highlighting (ANSI colors)
- [ ] .help command with all available commands
- [ ] .load <file> - Load and execute file
- [ ] .save <file> - Save session
- [ ] .clear - Clear context
- [ ] .exit or Ctrl+D to quit
- [ ] Pretty-print objects/arrays

**Dependencies:** linenoise or readline

**Files to Modify:**
- `src/repl.cc` - Add features

---

### 🟡 17. Source Maps Support
**Impact:** 💡 (Debug transpiled code)
**Complexity:** 🌶️
**Effort:** ~600 LOC

**Description:** Support source maps for TypeScript/Babel debugging.

**Tasks:**
- [ ] Parse source map JSON (.map files)
- [ ] Map generated locations to original
- [ ] Use original locations in stack traces
- [ ] Support inline source maps
- [ ] Add //# sourceMappingURL parsing

---

## Q4 2026: Standard Library & APIs

### 🔴 18. Timer APIs
**Impact:** 🚀 (Essential browser/Node.js compatibility)
**Complexity:** 🍃
**Effort:** ~400 LOC

**Target API:**
```javascript
setTimeout(() => console.log('delayed'), 1000);
const id = setInterval(() => console.log('repeated'), 500);
clearInterval(id);
setImmediate(() => console.log('next tick'));
queueMicrotask(() => console.log('microtask'));
```

**Implementation:** Integrate with existing event loop

**Tasks:**
- [ ] Implement setTimeout/clearTimeout
- [ ] Implement setInterval/clearInterval
- [ ] Implement setImmediate/clearImmediate
- [ ] Implement queueMicrotask
- [ ] Integrate with event loop (src/event_loop.cc)
- [ ] Add timer tests

**Files to Modify:**
- `include/event_loop.h` - Add timer queue
- `src/event_loop.cc` - Implement timers
- `src/environment.cc` - Add global timer functions

---

### 🟠 19. File System API
**Impact:** ⚡ (Node.js compatibility)
**Complexity:** 🌶️
**Effort:** ~1,200 LOC

**Target API:**
```javascript
// Synchronous
const data = fs.readFileSync('file.txt', 'utf8');
fs.writeFileSync('output.txt', data);

// Asynchronous
const data = await fs.promises.readFile('file.txt', 'utf8');
await fs.promises.writeFile('output.txt', data);
```

**Tasks:**
- [ ] Use std::filesystem for cross-platform support
- [ ] Implement readFileSync/writeFileSync
- [ ] Implement async versions with Promises
- [ ] Add appendFile, unlink, mkdir, rmdir
- [ ] Add stat, exists, readdir
- [ ] Support binary and text modes
- [ ] Add error handling (ENOENT, etc.)

**Files to Create:**
- `include/fs.h`
- `src/fs.cc`

---

### 🟠 20. URL and URLSearchParams
**Impact:** ⚡ (Web API compatibility)
**Complexity:** 🍃
**Effort:** ~600 LOC

**Target API:**
```javascript
const url = new URL('https://example.com/path?q=search#hash');
console.log(url.protocol); // 'https:'
console.log(url.hostname); // 'example.com'
console.log(url.searchParams.get('q')); // 'search'

url.searchParams.set('page', '2');
console.log(url.href); // 'https://example.com/path?q=search&page=2#hash'
```

**Tasks:**
- [ ] Implement URL class with parsing
- [ ] Implement URLSearchParams
- [ ] Support percent encoding/decoding
- [ ] Add all URL properties (protocol, host, pathname, etc.)
- [ ] Add tests for edge cases

**Files to Create:**
- `include/url.h`
- `src/url.cc`

---

### 🟡 21. TextEncoder/TextDecoder
**Impact:** ⚡
**Complexity:** 🍃
**Effort:** ~300 LOC

**Target API:**
```javascript
const encoder = new TextEncoder();
const bytes = encoder.encode('Hello 👋');

const decoder = new TextDecoder();
const text = decoder.decode(bytes);
```

**Tasks:**
- [ ] Implement TextEncoder (UTF-8 encoding)
- [ ] Implement TextDecoder (UTF-8 decoding)
- [ ] Support other encodings (UTF-16, ISO-8859-1)
- [ ] Use existing Unicode utilities (src/unicode.cc)

**Files to Create:**
- `include/text_encoding.h`
- `src/text_encoding.cc`

---

### 🟡 22. Streams API
**Impact:** ⚡ (Async I/O)
**Complexity:** 🔥
**Effort:** ~2,000 LOC

**Target API:**
```javascript
const readable = new ReadableStream({
  start(controller) {
    controller.enqueue('chunk1');
    controller.enqueue('chunk2');
    controller.close();
  }
});

const reader = readable.getReader();
while (true) {
  const {done, value} = await reader.read();
  if (done) break;
  console.log(value);
}
```

**Tasks:**
- [ ] Implement ReadableStream
- [ ] Implement WritableStream
- [ ] Implement TransformStream
- [ ] Add backpressure handling
- [ ] Integrate with fetch() response bodies
- [ ] Add pipe() and pipeTo()

---

### 🟢 23. Intl (Internationalization API)
**Impact:** 💡
**Complexity:** 🔥
**Effort:** ~3,000+ LOC or use ICU library

**Note:** Consider using ICU library instead of pure implementation.

---

## WebAssembly Enhancements

### 🟠 24. WebAssembly SIMD
**Impact:** 🚀 (4-8x speedup for numeric workloads)
**Complexity:** 🔥
**Effort:** ~2,000 LOC

**Description:** Add WebAssembly SIMD proposal (v128 operations).

**Tasks:**
- [ ] Add v128 type and instructions
- [ ] Implement vector loads/stores
- [ ] Implement i8x16, i16x8, i32x4, i64x2 operations
- [ ] Implement f32x4, f64x2 operations
- [ ] Add shuffle, swizzle operations
- [ ] Add lane extract/replace
- [ ] Leverage existing SIMD infrastructure (src/simd.cc)

**Files to Modify:**
- `include/wasm/wasm_types.h` - Add v128 type
- `src/wasm/wasm_decoder.cc` - Decode SIMD instructions
- `src/wasm/wasm_interpreter.cc` - Execute SIMD ops

---

### 🟡 25. WebAssembly Threads and Atomics
**Impact:** ⚡
**Complexity:** 🔥
**Effort:** ~1,500 LOC

**Tasks:**
- [ ] Add SharedArrayBuffer type
- [ ] Implement atomic operations
- [ ] Add memory.atomic.* instructions
- [ ] Implement wait/notify
- [ ] Add threading support

---

### 🟡 26. WebAssembly Exception Handling
**Impact:** ⚡
**Complexity:** 🌶️
**Effort:** ~800 LOC

**Tasks:**
- [ ] Add try/catch/throw instructions
- [ ] Implement exception tags
- [ ] Integrate with JS exceptions

---

### 🟡 27. WebAssembly Reference Types
**Impact:** ⚡
**Complexity:** 🌶️
**Effort:** ~600 LOC

**Tasks:**
- [ ] Add anyref, funcref, externref types
- [ ] Better GC integration
- [ ] Table operations with references

---

### 🟡 28. Bulk Memory Operations
**Impact:** ⚡
**Complexity:** 🍃
**Effort:** ~300 LOC

**Tasks:**
- [ ] Implement memory.copy
- [ ] Implement memory.fill
- [ ] Implement table.copy, table.init

---

## Testing & Quality

### 🔴 29. Expand Test262 Coverage
**Impact:** 🚀 (Standards compliance)
**Complexity:** 🌶️
**Effort:** Ongoing

**Current:** Infrastructure exists, limited coverage
**Target:** 50%+ compliance in 2026

**Tasks:**
- [ ] Run full Test262 suite
- [ ] Generate compliance report
- [ ] Fix failing tests systematically
- [ ] Track progress over time
- [ ] Add CI job for Test262 regression

**Files to Modify:**
- `test262/test262_runner.cc` - Add reporting

---

### 🟠 30. Fuzzing Integration
**Impact:** 🚀 (Find crashes and edge cases)
**Complexity:** 🌶️
**Effort:** ~400 LOC

**Tasks:**
- [ ] Integrate libFuzzer
- [ ] Add AFL++ support
- [ ] Create fuzzing harness
- [ ] Add to CI (run 1hr per commit)
- [ ] Track corpus and crashes

**Files to Create:**
- `fuzz/fuzz_lexer.cc`
- `fuzz/fuzz_parser.cc`
- `fuzz/fuzz_interpreter.cc`

---

### 🟠 31. Benchmark Suite
**Impact:** ⚡ (Track performance regressions)
**Complexity:** 🍃
**Effort:** ~500 LOC

**Benchmarks to Add:**
- [ ] Richards (object/method dispatch)
- [ ] DeltaBlue (constraint solver)
- [ ] Crypto (arithmetic)
- [ ] RayTrace (floating point)
- [ ] NavierStokes (arrays)
- [ ] Octane subset
- [ ] SunSpider subset

**Tasks:**
- [ ] Implement benchmark runner
- [ ] Add baseline recording
- [ ] Add regression detection
- [ ] Integrate with CI
- [ ] Generate performance reports

**Files to Create:**
- `benchmarks/bench.cc`
- `benchmarks/*.js` - Benchmark scripts

---

### 🟡 32. Memory Leak Detection
**Impact:** ⚡
**Complexity:** 🍃
**Effort:** Configuration only

**Tasks:**
- [ ] Add Valgrind CI job
- [ ] Add AddressSanitizer build
- [ ] Add LeakSanitizer build
- [ ] Add tests for GC edge cases
- [ ] Document memory testing

---

### 🟡 33. Code Coverage Tracking
**Impact:** 💡
**Complexity:** 🍃
**Effort:** Configuration only

**Tasks:**
- [ ] Add coverage build (--coverage)
- [ ] Integrate with Codecov
- [ ] Set coverage targets (>80%)
- [ ] Add coverage badge to README

---

## Build System & Packaging

### 🟠 34. Package Manager Support
**Impact:** ⚡ (Easier adoption)
**Complexity:** 🍃
**Effort:** ~2-4 hours per package manager

**Tasks:**
- [ ] Create Conan recipe
- [ ] Add to vcpkg registry
- [ ] Create Homebrew formula
- [ ] Create Debian package
- [ ] Create RPM package
- [ ] Document installation methods

---

### 🟡 35. WebAssembly Build Target
**Impact:** 💡 (Run LightJS in browser/Node.js)
**Complexity:** 🌶️
**Effort:** ~600 LOC

**Tasks:**
- [ ] Create Emscripten build configuration
- [ ] Remove platform-specific code (filesystem, sockets)
- [ ] Add JavaScript API wrapper
- [ ] Build lightjs.wasm and lightjs.js
- [ ] Create browser demo
- [ ] Add to CI

---

### 🟡 36. Python Bindings
**Impact:** 💡
**Complexity:** 🌶️
**Effort:** ~800 LOC

**Target API:**
```python
import pylightjs

js = pylightjs.Runtime()
result = js.eval("2 + 2")
print(result)  # 4

func = js.compile("(a, b) => a + b")
print(func(10, 20))  # 30
```

**Tasks:**
- [ ] Use pybind11 for bindings
- [ ] Wrap Lexer, Parser, Interpreter
- [ ] Add Python exception mapping
- [ ] Build wheel packages
- [ ] Publish to PyPI

**Files to Create:**
- `python/bindings.cc`
- `python/setup.py`

---

### 🟠 37. Continuous Integration
**Impact:** 🚀
**Complexity:** 🍃
**Effort:** ~4 hours

**Tasks:**
- [ ] Add GitHub Actions workflow
- [ ] Build on Linux (GCC, Clang)
- [ ] Build on macOS (AppleClang)
- [ ] Build on Windows (MSVC)
- [ ] Run tests on all platforms
- [ ] Add coverage job
- [ ] Add fuzzing job
- [ ] Add benchmark job (track regressions)
- [ ] Add static analysis (clang-tidy, cppcheck)

**Files to Create:**
- `.github/workflows/ci.yml`
- `.github/workflows/benchmarks.yml`
- `.github/workflows/fuzzing.yml`

---

## Documentation

### 🟠 38. API Reference
**Impact:** ⚡
**Complexity:** 🍃
**Effort:** ~8 hours

**Tasks:**
- [ ] Add Doxygen comments to all public APIs
- [ ] Generate HTML documentation
- [ ] Host on GitHub Pages
- [ ] Add usage examples
- [ ] Document all built-in objects/methods

---

### 🟡 39. Language Compatibility Matrix
**Impact:** 💡
**Complexity:** 🍃
**Effort:** ~2 hours

**Create Table:**
| Feature | ES5 | ES6 | ES2020 | LightJS |
|---------|-----|-----|--------|---------|
| let/const | ❌ | ✅ | ✅ | ✅ |
| Arrow functions | ❌ | ✅ | ✅ | ✅ |
| Classes | ❌ | ✅ | ✅ | ✅ |
| Async/await | ❌ | ❌ | ✅ | ✅ |
| Generators | ❌ | ✅ | ✅ | ❌ |
| ... | ... | ... | ... | ... |

---

### 🟡 40. Performance Guide
**Impact:** 💡
**Complexity:** 🍃
**Effort:** ~4 hours

**Topics:**
- Best practices for embedding LightJS
- Performance characteristics
- Memory management tips
- Optimization techniques

---

### 🟡 41. Architecture Deep Dive
**Impact:** 💡
**Complexity:** 🍃
**Effort:** ~8 hours

**Topics:**
- Coroutine-based execution model
- Garbage collection algorithm
- WebAssembly integration
- Value representation
- Memory layout

---

## TypeScript Support

### 🟢 42. TypeScript Type Stripping (Phase 1)
**Impact:** ⚡
**Complexity:** 🔥
**Effort:** ~2,500 LOC

**Description:** Parse and ignore TypeScript syntax to execute JS code.

**Target:**
```typescript
function add(a: number, b: number): number {
  return a + b;
}
// Execute as: function add(a, b) { return a + b; }
```

**Tasks:**
- [ ] Extend lexer for TypeScript tokens (`:`, `<>`, `interface`, etc.)
- [ ] Parse type annotations and discard
- [ ] Parse interfaces/type aliases and discard
- [ ] Handle generics syntax
- [ ] Strip enum to const object
- [ ] Add .ts file support
- [ ] Add tests for type stripping

**Files to Create:**
- `include/typescript_stripper.h`
- `src/typescript_stripper.cc`

---

### 🟢 43. TypeScript Type Checking (Phase 2)
**Impact:** 🚀
**Complexity:** 🔥🔥🔥 (Massive undertaking)
**Effort:** ~15,000+ LOC

**Note:** This is essentially building a TypeScript compiler. Consider using official TypeScript compiler (tsc) as preprocessor instead.

**Alternative Approach:**
- Bundle LightJS with tsc
- Transpile .ts → .js before execution
- Much less effort, full TypeScript support

---

## Advanced Performance

### 🟢 44. JIT Compilation (LLVM Backend)
**Impact:** 🚀 (10-100x performance for hot code)
**Complexity:** 🔥🔥🔥
**Effort:** ~10,000+ LOC

**Description:** Compile hot functions to native code.

**Implementation:**
- [ ] Integrate LLVM
- [ ] Profile code to detect hot functions
- [ ] Translate bytecode → LLVM IR
- [ ] Optimize IR and generate native code
- [ ] Add deoptimization support
- [ ] Add inline caching integration

**Note:** This is a long-term project. Focus on bytecode first.

---

## Quick Wins (Low Effort, High Impact)

### 🔴 String Interning
**Effort:** ~500 LOC | **Impact:** 🚀
Memory reduction and faster comparisons.

### 🔴 Timer APIs
**Effort:** ~400 LOC | **Impact:** 🚀
Essential for any real-world application.

### 🔴 Enhanced Error Messages
**Effort:** ~1,000 LOC | **Impact:** 🚀
Massive DX improvement.

### 🟠 Generators
**Effort:** ~1,200 LOC | **Impact:** 🚀
High-demand ES6 feature, aligns with coroutine architecture.

### 🟠 Complete Destructuring
**Effort:** ~800 LOC | **Impact:** ⚡
Fill ES6 feature gap.

---

## Estimated Timeline

**Q1 2026:** Bytecode, string interning, hidden classes, benchmarks (Performance foundation)
**Q2 2026:** Generators, destructuring, WeakMap, timers (Language completeness)
**Q3 2026:** Error messages, debugger, profiler, Test262 (Developer experience)
**Q4 2026:** Proxy/Reflect, WASM SIMD, TypeScript stripping (Advanced features)

**Total Estimated LOC for 2026:** ~25,000 additional lines
**Total Estimated Effort:** ~400-600 hours

---

## Maintenance Tasks

- [ ] Update dependencies regularly
- [ ] Monitor security advisories
- [ ] Keep up with ECMAScript proposals
- [ ] Respond to community issues
- [ ] Review and merge PRs
- [ ] Update documentation
- [ ] Release versioning (semantic versioning)

---

## Long-Term Vision (2027+)

- Full ES2023+ support
- Production-grade JIT compiler
- Complete Node.js API compatibility
- Browser embedding
- Embedded systems optimization
- Real-time garbage collection
- Multi-threading support
- First-class TypeScript support

---

**Document Version:** 1.0
**Last Updated:** 2026-01-27
**Review Date:** 2026-04-27 (quarterly review)
