# LightJS Future Tasks

This document tracks planned enhancements and future work for LightJS.

**Current Status:** ~24,000 LOC, CTest 12/12 passing, ES2020 support, WebAssembly 1.0, C++17/C++20 dual support

---

## Test262 ES2020 Repro Procedure and Status

### Reproduce

1. Download the Test262 suite:
   - `git clone --depth 1 https://github.com/tc39/test262.git test262-suite`
2. Configure and build:
   - `cmake -S . -B build -DLIGHTJS_BUILD_TESTS=ON`
   - `cmake --build build -j$(nproc)`
3. Run ES2020 shards:
   - `build/test262_runner ./test262-suite --no-temp-skips --test language/module-code/top-level-await`
   - `build/test262_runner ./test262-suite --no-temp-skips --test language/expressions/optional-chaining`
   - `build/test262_runner ./test262-suite --no-temp-skips --test language/expressions/coalesce`
   - `build/test262_runner ./test262-suite --no-temp-skips --test language/literals/bigint`
   - `build/test262_runner ./test262-suite --no-temp-skips --test language/expressions/dynamic-import`
   - `build/test262_runner ./test262-suite --no-temp-skips --test language --filter "tco"`

### Current Status (2026-02-14)

| Feature | Pass | Total | Rate |
|---|---|---|---|
| Top-Level Await | 251 | 251 | 100.0% |
| Optional Chaining | 38 | 38 | 100.0% |
| Nullish Coalescing | 24 | 24 | 100.0% |
| Dynamic Import | 939 | 939 | 100.0% |
| BigInt Literals | 59 | 59 | 100.0% |
| Tail Call Optimization (`--filter "tco"`) | 34 | 34 | 100.0% |

- Note: runner may print `Failed to read module: .../syntax/foo.js` because that file is intentionally absent in the suite.

### Latest Status (2026-03-21)

| Scope | Pass | Total | Rate |
|---|---|---|---|
| `language` | 23194 | 23629 | 98.2% (11 fail, 424 skipped) |
| `built-ins/Math` | 325 | 327 | 99.4% |
| `built-ins/Number` | 337 | 338 | 99.7% |
| `built-ins/Boolean` | 50 | 51 | 98.0% |
| `built-ins/JSON` | 126 | 165 | 76.4% |
| `built-ins/String` | 1164 | 1223 | 95.2% |
| `built-ins/Object` | 3215 | 3411 | 94.3% |
| `built-ins/eval` | 10 | 10 | 100.0% |
| `built-ins/parseInt` | 55 | 55 | 100.0% |
| `built-ins/parseFloat` | 54 | 54 | 100.0% |
| `built-ins/ArrayBuffer` | 196 | 196 | 100.0% |
| `built-ins/DataView` | 561 | 561 | 100.0% |
| `built-ins/BigInt` | 77 | 77 | 100.0% |
| `built-ins/Promise` | 631 | 652 | 96.8% |
| `built-ins/Set` | 349 | 383 | 91.1% |
| `built-ins/Map` | 186 | 204 | 91.2% |
| `built-ins/Symbol` | 88 | 98 | 89.8% |
| `built-ins/Error` | 53 | 58 | 91.4% |
| `built-ins/WeakSet` | 69 | 85 | 81.2% |
| `built-ins/Function` | 382 | 509 | 75.0% |
| `built-ins/WeakMap` | 96 | 141 | 68.1% |

Unit tests: 346/346 passing.

#### Changes (2026-03-21)

122 Object tests fixed (3070→3192/3411), 5 String tests fixed, 1 JSON test fixed, 0 regressions:

- **ToPropertyDescriptor getter invocation** (`src/environment.cc`): readDescriptorField now uses getPropertyLike() to properly invoke accessor getters on descriptor objects per ES spec 8.10.5. Fixes defineProperty/defineProperties tests where descriptor attributes were defined as getters.
- **defineProperties getter invocation** (`src/environment.cc`): getOwnPropertyLike() in iterateOwnEnumerable to invoke getters when reading property descriptors.
- **Object() constructor prototype** (`src/environment.cc`): Set __proto__ to Object.prototype on objects created by Object(undefined/null).
- **Primitive wrapper __proto__** (`src/interpreter.cc`): Set correct __proto__ (String.prototype, Number.prototype, etc.) on primitive wrapper objects created for `this` binding.
- **Array length ToPrimitive** (`src/environment.cc`): Handle object values for array length via valueOf()/toString().
- **Array length non-configurable blocking** (`src/environment.cc`): Check for non-configurable elements blocking length decrease per spec 15.4.5.1.
- **Array sparse length** (`src/environment.cc`, `src/interpreter.cc`): __array_length__ property for lengths >1M without materializing elements. Fixes 2^32-1/2^32-2 boundary tests.
- **Array index auto-extend length** (`src/environment.cc`): Update length when defining index >= current length.
- **Non-writable length rejection** (`src/environment.cc`): Reject defineProperty on indices >= length when non-writable.
- **Object.keys hole skipping** (`src/value.cc`): Skip __hole__/__deleted__ markers for sparse arrays.

#### Changes (2026-03-20 #4)

214+ String tests fixed (927→1141/1223), 8 Function tests fixed (366→374/509), 0 regressions:

- **String.raw** (`src/environment.cc`): spec-compliant ToObject validation, TypeError on null/undefined, JS toString() invocation via callForHarness, getter support, Symbol rejection. 30/30 passing.
- **String.prototype.slice** (`src/interpreter.cc`, `src/string_methods.cc`): NaN/Infinity handling, double→int clamping via toIntegerForStringBuiltinArg. 38/38 passing.
- **indexOf/lastIndexOf** (`src/string_methods.cc`): Symbol.toPrimitive support in toPrimitiveForStringBuiltin, BigInt TypeError. 47/47 passing.
- **fromCharCode/fromCodePoint** (`src/string_methods.cc`): proper ToPrimitive coercion via toNumberForStringBuiltinArg. 28/28 passing.
- **substring** (`src/interpreter.cc`, `src/string_methods.cc`): toIntegerForStringBuiltinArg for argument coercion. 46/46 passing.
- **isWellFormed/toWellFormed** (`src/interpreter.cc`, `src/environment.cc`): surrogate pair detection (adjacent high+low surrogates recognized as valid pairs).
- **includes** (`src/interpreter.cc`, `src/environment.cc`): empty string at position >= length returns true.
- **Symbol protocol** (`src/interpreter.cc`, `src/environment.cc`, `src/string_methods.cc`): Symbol.match/search/replace/split checks on search/match/replace/replaceAll/split. Installed @@match/@@search/@@replace/@@split on RegExp.prototype. Fixed Regex prototype chain traversal in getPropertyForPrimitive.
- **Date.prototype[@@toPrimitive]** (`src/environment.cc`): hint-based toPrimitive for Date objects.
- **Type coercion** (`src/string_methods.cc`): Symbol.toPrimitive support, BigInt rejection, proper OrdinaryToPrimitive ordering across all string methods.

#### Changes (2026-03-20 #3)

10 JSON tests fixed (115→125/165), 0 regressions:

- **JSON.parse `__proto__`** (`src/json.cc`): treat `__proto__` as regular data property via `__own_prop___proto__` marker, without modifying prototype chain.
- **JSON.parse prototype chain** (`src/json.cc`): objects get `Object.prototype`, arrays use `makeArrayWithPrototype()`, enabling `hasOwnProperty` and other inherited methods on parsed values.
- **JSON.parse reviver wrapper** (`src/json.cc`): wrapper object gets `Object.prototype`, proper `""` key data property.
- **JSON.parse reviver non-configurable** (`src/json.cc`): delete/create silently fail for non-configurable properties per OrdinaryDelete/CreateDataProperty spec.
- **JSON.parse reviver Get** (`src/json.cc`): use `getPropertyForExternal` for proper prototype chain lookup during reviver walk.
- **JSON.stringify toJSON abrupt** (`src/json.cc`): check `hasError()` after `getPropertyForExternal` to propagate getter errors.
- **JSON.stringify wrapper** (`src/json.cc`): wrapper object gets `Object.prototype`.
- **JSON.stringify error propagation** (`src/json.cc`): remove try/catch that swallowed non-TypeError exceptions.

#### Changes (2026-03-20 #2)

11 built-in tests fixed, 0 regressions:

- **JSON.parse whitespace** (`src/json.cc`): strict SP/HT/LF/CR validation per spec.
- **JSON.parse reviver** (`src/json.cc`): full reviver callback with correct call order (numeric indices ascending, then string keys, root last), property walking, wrapper object creation.
- **JSON.parse ToString** (`src/json.cc`): call JS toString() on non-string input.
- **JSON.stringify surrogates** (`src/json.cc`): proper lone surrogate escaping (\uXXXX), UTF-8 decoding for multi-byte chars.
- **JSON.stringify toJSON** (`src/json.cc`): use getPropertyForExternal for prototype chain lookup.
- **JSON.stringify getters** (`src/json.cc`): invoke getters during property serialization; handle deleted properties.
- **JSON.stringify regex** (`src/json.cc`): serialize Regex/Map/Set as `{}` instead of `null`.
- **Math.sumPrecise** (`src/math_object.cc`): Shewchuk exact summation with overflow prevention via pos/neg interleaving; full iterator protocol support (arrays, generators, custom iterables); iterator closing on TypeError.
- **Number.prototype.toFixed** (`src/environment.cc`): throw TypeError for BigInt argument.
- **Object.getOwnPropertyNames** (`src/value.cc`): include empty string keys.
- **Interpreter.generatorNext** (`src/interpreter.cc`): public API for calling generator.next() from native code.

#### Changes (2026-03-20)

18 language tests fixed, 0 regressions since 2026-03-10 baseline (29 fail → 11 fail):

- **Regex u-flag validation** (`src/lexer.cc`): identity escape, `\c` control char, legacy octal, `\u{...}` range check, lone `{`/`}`, lookbehind/lookahead quantifier, character class range.
- **Regex named group validation** (`src/lexer.cc`): empty/invalid/duplicate group names, `\k` backreference validation, pattern transformation for std::regex compatibility.
- **Arguments mapped nonconfigurable** (`src/environment.cc`): call parameter setter before erasing accessor markers in `Object.defineProperty`.
- **Logical assignment no-set-put** (`src/interpreter.cc`): `&&=`/`||=`/`??=` now check if setter is callable (not just if key exists).
- **Async function constructor** (`src/interpreter.cc`): plain async functions no longer get `isConstructor=true` or `.prototype`. Async generators unaffected.
- **Object.setPrototypeOf with Proxy** (`src/environment.cc`): added `isProxy()` to allowed prototype types.

#### OOM Safety (2026-03-20)

- Per-child 2GB memory limit via `setrlimit(RLIMIT_AS)` in test262_runner fork (configurable: `LIGHTJS_TEST262_MEM_LIMIT_MB`).
- `String.prototype.repeat` capped at 256MB output.
- Parser recursion depth limit of 500 (`parseDepth_` in `parser.h`).
- Loop iteration limit via `LIGHTJS_MAX_LOOP_ITERATIONS` env var (default unlimited).

#### Remaining 11 Language Failures

| Test | Category | Root Cause |
|---|---|---|
| `u-surrogate-pairs*.js` (5) | regex runtime | Runtime regex engine lacks Unicode surrogate pair support |
| `u-astral.js` | regex runtime | Runtime regex engine lacks Unicode astral plane support |
| `u-case-mapping.js` | regex runtime | Runtime regex engine lacks Unicode case folding |
| `u-unicode-esc.js` | regex runtime | Runtime regex engine doesn't support `\u{HHHH}` syntax |
| `forward-reference.js` | regex runtime | Runtime regex engine doesn't support named groups / `\k<name>` |
| `instanceof/prototype-getter-with-object.js` | prototype chain | Crash when getter installed on `Function.prototype.prototype` |
| `keyed-destructuring-...order...js` | eval order | Complex `with` + Proxy destructuring evaluation order |

All require deeper runtime changes (regex engine Unicode overhaul or prototype chain rework).

### Previous Status (2026-03-10)

| Scope | Pass | Total | Rate |
|---|---|---|---|
| `language/expressions` | 11022 | 11036 | 99.9% (0 fail, 14 skipped) |
| `language/async-generator` | 623 | 623 | 100.0% |
| `language` | 23126 | 23624 | 97.9% (81 fail, 417 skipped) |

- Added async-generator request queueing for `next/return/throw` in `src/interpreter.cc` to serialize concurrent async-generator operations.
- Fixed regex literal lexing around line terminators (`\n`, `\r`, `U+2028`, `U+2029`) and statement-list regex starts after `}` in `src/lexer.cc`.
- Updated Test262 isolated worker timeout defaults to 5 minutes in `test262/test262_runner.cc`:
  - `kPerTestTimeoutSeconds = 300`
  - `kTailCallPerTestTimeoutSeconds = 300`
  - `kResizableArrayBufferPerTestTimeoutSeconds = 300`

#### Hang/Timeout Investigation (2026-03-10)

- No true per-test timeout events were observed in the latest `language` run (`[timeout]` not present in output).
- The run can look stalled because a few tests are very slow but eventually pass:
  - `test/language/comments/S7.4_A5.js` (~48s)
  - `test/language/comments/S7.4_A6.js` (~17s)
  - `test/language/expressions/call/tco-call-args.js` (~14s)

### Recent Completions (2026-02-14)

- Fixed `return` comma-expression parsing in `src/parser.cc` (unblocks TCO comma tests).
- Implemented/optimized self-tail-call handling and enabled targeted TCO runs in Test262 runner.
- Fixed direct vs indirect `eval` semantics (`eval(...)` local, `eval?.(...)` indirect/global).
- Improved `with`-scope name resolution to reflect dynamic object property updates.
- Fixed generator `for...of` advancement (`ResumeMode::Next`) to avoid repeated-yield hangs.

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
**Status:** `language` 98.2%, expanding to `built-ins`

Next targets for `built-ins` scope (by priority):
- **JSON** (75.8%): 40 remaining failures — `JSON.rawJSON`/`JSON.isRawJSON` (14), Proxy-dependent (15), ES2024 reviver context/source (5), BigInt `this` boxing (2), array element deletion prototype chain (1), other edge cases (3)
- **Math** (99.4%): `Math.sumPrecise` rounding correction (1 test), `Math.f16round` value conversion (1 test)
- **Number** (99.7%): `proto-from-ctor-realm` (1 remaining failure, requires multi-realm support)
- **Boolean** (98.0%): `proto-from-ctor-realm` (1 remaining failure)
- **String** (93.3%): 77 remaining — replace (14), replaceAll (12), split (11), match (4), normalize (6), search (3), toLocale/Case (12), other (15). Remaining: $1-$9 capture groups, regex evaluation order, empty regex split, Unicode normalization.
- **Promise** (96.8%): 21 remaining — allSettledKeyed/allKeyed proposals (12), finally species (5), then self-resolution (4)
- **Set** (90.1%): 38 remaining — set methods proposals (24), Symbol.species (4), constructor iterator (10)
- **Map** (89.2%): 22 remaining — Symbol.species (4), constructor iterator protocol (14), proposals (4)
- **Function** (71.9%): 143 remaining — toString source text (73), bind (15), apply (7), other (48)
- **Object**, **Array**, **Function**, **Promise**, **RegExp**: not yet baselined

Next targets for remaining 11 `language` failures:
- Runtime regex Unicode overhaul (surrogate pairs, astral, `\u{...}`, `\k<name>`) — 9 failures
- `instanceof` prototype getter crash — 1 failure
- Destructuring evaluation order with `with`+Proxy — 1 failure

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

**Last Updated:** 2026-03-20 (updated)
