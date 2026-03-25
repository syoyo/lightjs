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

### Latest Status (2026-03-22)

| Scope | Pass | Total | Rate |
|---|---|---|---|
| `language` | 23194 | 23629 | 98.2% (11 fail, 424 skipped) |
| `built-ins/Math` | 326 | 327 | 99.7% |
| `built-ins/Number` | 338 | 338 | 100.0% |
| `built-ins/Boolean` | 51 | 51 | 100.0% |
| `built-ins/JSON` | 165 | 165 | 100.0% |
| `built-ins/String` | 1168 | 1223 | 95.5% (50 fail, 5 skipped) |
| `built-ins/Object` | 3283 | 3411 | 96.2% |
| `built-ins/eval` | 10 | 10 | 100.0% |
| `built-ins/parseInt` | 55 | 55 | 100.0% |
| `built-ins/parseFloat` | 54 | 54 | 100.0% |
| `built-ins/ArrayBuffer` | 196 | 196 | 100.0% |
| `built-ins/DataView` | 561 | 561 | 100.0% |
| `built-ins/BigInt` | 77 | 77 | 100.0% |
| `built-ins/Promise` | 640 | 652 | 98.2% |
| `built-ins/Set` | 378 | 383 | 98.7% |
| `built-ins/Map` | 201 | 204 | 98.5% |
| `built-ins/Symbol` | 92 | 98 | 93.9% |
| `built-ins/Error` | 58 | 58 | 100.0% |
| `built-ins/WeakSet` | 85 | 85 | 100.0% |
| `built-ins/Reflect` | 153 | 153 | 100.0% |
| `built-ins/Function` | 392 | 509 | 77.0% |
| `built-ins/WeakMap` | 139 | 141 | 98.6% |
| `built-ins/Array` (partial) | ~2260 | ~2644 | ~85.5% |
Note: Array prototype methods at 2193/2549 (86.0%), plus non-proto tests.

Unit tests: 346/346 passing.

Note: Array total excludes reverse/lastIndexOf/from (timeout on sparse array tests).
Note: Broad URI legacy shards (`decodeURI` / `decodeURIComponent`) still contain timeout-heavy Sputnik loops; defer those while focusing on deterministic runtime failures in other built-ins.

#### Changes (2026-03-22)

**Batch 13:** Reflect 100.0% (153/153), Number 100.0% (338/338), Boolean 100.0% (51/51), Math +1 (`f16round`) to 326/327:

- **Reflect completion** (`src/environment.cc`, `src/interpreter.cc`): finished `Reflect.defineProperty`, `set`, `deleteProperty`, `setPrototypeOf`, `isExtensible`, and `construct` so they now validate targets correctly, preserve abrupt proxy/property-key/descriptor failures, return booleans on ordinary failure, follow receiver/prototype setter behavior, and accept callable-wrapper constructors such as `Array` as `newTarget`.
- **Math f16round** (`src/math_object.cc`): switched `Math.f16round` to the engine’s shared binary16 conversion helpers, fixing subnormal/tie-to-even behavior against the Float16 Test262 conversion table.
- **Small shard follow-up**: full reruns confirm `built-ins/Reflect`, `built-ins/Number`, and `built-ins/Boolean` are now green. `built-ins/Math` is down to a single remaining `Math.sumPrecise/sum.js` precision failure.
- **Next bounded item**: replace the current approximate `Math.sumPrecise` partial-sum finalization with an exact finite-double accumulator so the remaining large-magnitude cancellation case rounds to `9.565271205476347e+307` instead of `9.565271205476349e+307`.

**Batch 10:** Error 100.0% (58/58), WeakSet 100.0% (85/85), Symbol +4 (88->92/98), ArrayIteratorPrototype 100.0% (27/27):

- **Error `cause` abrupt propagation** (`src/environment.cc`): `new Error(message, options)` now uses proxy-aware `HasProperty`/`Get` semantics for `options.cause`, so Proxy `has` traps and abrupt getters propagate correctly.
- **Global built-in descriptors** (`src/environment.cc`): Added missing non-enumerable `WeakMap`/`WeakSet` globals during `globalThis` finalization and installed Array constructor `@@species` as a real accessor with the correct getter name.
- **Primitive write semantics and Symbol wrapper coercion** (`src/interpreter.cc`, `src/environment.cc`, `src/date_object.cc`): property assignment on primitive bases now keeps the primitive receiver for `[[Set]]`, `Object(Symbol())` no longer installs ad hoc own `valueOf`/`toString`, and single-argument `Date` construction / `getTime()` no longer treat invalid coerced strings as "now".
- **%ArrayIteratorPrototype% normalization** (`src/environment.cc`): moved `next` onto `%ArrayIteratorPrototype%`, stored iterator state in internal-slot markers, latched exhaustion, fixed descriptor/name/length metadata, and made detached typed-array iteration throw instead of silently exhausting.

**Batch 11:** Date constructor/parse/display follow-up:

- **Date construction and receiver validation** (`src/environment.cc`, `src/date_object.cc`): `new Date(dateObj)` now preserves `[[DateValue]]` without invoking user-defined coercion hooks, explicit `undefined` in multi-arg construction yields `NaN` instead of omitted-argument defaults, and prototype methods now reject incompatible receivers instead of silently accepting arbitrary objects.
- **Date local/UTC separation** (`src/date_object.cc`): multi-argument `new Date(y, m, ...)` now constructs local-time values, while `Date.UTC` keeps UTC arithmetic and correct omitted argument defaults.
- **Date parse/display round-tripping** (`src/environment.cc`, `src/date_object.cc`): `Date()` as a function now returns a current date string, `Date.parse` accepts year-only ISO strings, extended signed ISO years, and the engine’s own `toString`/`toUTCString` outputs, and `toISOString` no longer relies on platform `gmtime` for extreme-range dates.
- **Date completion** (`src/date_object.cc`, `src/environment.cc`, `include/date_object.h`): `built-ins/Date` is now `594/594` passing after implementing the mutator families, `toJSON`, `toTimeString`, arithmetic `toUTCString`, `toTemporalInstant`, extended-year rejection for `-000000`, and IEEE-754-ordered `Date.UTC` arithmetic.

**Batch 12:** Reflect runtime pass:

- **Reflect metadata and dispatch** (`src/environment.cc`): added proper `name` / `length` / non-enumerability / non-constructibility for the `Reflect` methods, plus `Reflect[Symbol.toStringTag] = "Reflect"` with the correct descriptor shape.
- **Reflect operation routing** (`src/environment.cc`): `Reflect.get`, `has`, `apply`, `getPrototypeOf`, `isExtensible`, `preventExtensions`, `ownKeys`, and `getOwnPropertyDescriptor` now route through the runtime’s property/prototype helpers or `Object.*` statics, including proxy trap abrupt propagation for the focused `getPrototypeOf` / `ownKeys` / `preventExtensions` / `getOwnPropertyDescriptor` cases.
- **Current Reflect status**: focused `Reflect` metadata/type-check/get/has/apply/ownKeys/getPrototypeOf/isExtensible/preventExtensions/getOwnPropertyDescriptor` cases are green, and a full `built-ins/Reflect` rerun is now `124/153` passing.
- **Remaining Reflect work**: `defineProperty`, `set`, `deleteProperty`, `setPrototypeOf`, `isExtensible` proxy abrupts, and `construct` array-like / `newTarget` handling are the next bounded runtime cluster before revisiting broader `built-ins/*`.

**Batch 9:** JSON 100.0% (165/165), +19 tests:

- **JSON.parse proxy-aware reviver walk** (`src/json.cc`): InternalizeJSONProperty now uses proxy-aware `[[Get]]`, `IsArray`, `LengthOfArrayLike`, enumerable own-key enumeration, `[[Delete]]`, and `CreateDataProperty`, including revoked proxy and trap abrupt propagation.
- **JSON.stringify proxy support** (`src/json.cc`): proxy arrays now serialize via `LengthOfArrayLike` + indexed `[[Get]]`, proxy objects via enumerable own property names, and proxy array replacers work as property filters with abrupt/revoked handling.
- **JSON.stringify BigInt `toJSON` receiver** (`src/json.cc`): BigInt primitive `toJSON` lookup now preserves the primitive receiver for accessor-defined methods on `BigInt.prototype`.

**Batch 8:** Splice generic path, frozen array checks: +11 tests:

- **splice generic object** (`src/environment.cc`): Generic splice path now properly shifts and inserts elements on plain objects, updates length.
- **Frozen array checks** (`src/environment.cc`): pop/push/shift check `__non_writable_length` and throw TypeError for frozen/sealed arrays. push validates null/undefined/string this.

**Batch 7:** ArraySpeciesCreate for map/filter/slice/concat: +33 tests:

- **ArraySpeciesCreate** (`src/environment.cc`): Per spec 9.4.2.3, map/filter/slice/concat check `originalArray.constructor[@@species]` to determine which constructor to use for result arrays. Validates species is constructor, handles null species fallback.

**Batch 6:** Route Array methods through prototype for accessor/hole support: +60 tests:

- **Bypass interpreter fast path** (`src/interpreter.cc`): Array methods now use prototype versions that properly handle accessor properties (getters/setters), sparse array holes, and array-like objects. Interpreter fast path skipped for all methods that have proper Array.prototype implementations.
- **Array prototype chain fallback** (`src/interpreter.cc`): When array has no `__proto__` set, falls back to looking up `Array.prototype` from environment.
- **new Array(n) holes** (`src/environment.cc`): `new Array(10)` creates hole-marked elements (never-assigned indices) so `in` operator and `[[HasProperty]]` work correctly.
- **structuredClone __proto__** (`src/environment.cc`): Cloned arrays preserve `__proto__` for prototype chain.
- **concat Symbol.isConcatSpreadable** (`src/environment.cc`): Fixed prototype concat to use `WellKnownSymbols::isConcatSpreadableKey()`.

**Batch 5:** Array iterator properties + generic methods: +40+ tests:

- **Array.prototype.keys/entries/values** (`src/environment.cc`): Defined as proper Function objects on Array.prototype with correct name/length properties. values uses mutable length re-read, entries returns [index, value] pairs, keys returns indices. All use [[Get]] for element access.
- **Array.prototype.sort generic** (`src/environment.cc`): Sort accepts array-like this, validates comparefn before reading length, collects elements via [[Get]], writes back after sorting. Handles holes properly.
- **Generic flat/flatMap/at/with/toReversed/toSorted/toSpliced** (`src/environment.cc`): All methods now use toObjectChecked + getArrayLikeLength + getArrayLikeElement for array-like support.

**Batch 4:** Array prototype methods rewrite: +150+ tests across reduce/map/filter/every/some/forEach/concat etc.:

- **ToObject primitive boxing** (`src/environment.cc`): `toObjectChecked` now boxes primitives (Boolean/Number/String/Symbol/BigInt) to proper wrapper objects with correct prototypes, enabling `Array.prototype.method.call(primitive, ...)` to work per spec.
- **Generic [[Get]]/[[HasProperty]]** (`src/environment.cc`): `getArrayLikeLength` and `getArrayLikeElement` now use `getPropertyForExternal` for ALL types (including arrays), enabling getter invocation, prototype chain lookups, and array-like object support.
- **Symbol.isConcatSpreadable** (`src/interpreter.cc`): `concat` checks `Symbol.isConcatSpreadable` per spec, supporting spreadable non-Array objects and non-spreadable arrays.
- **indexOf primitive boxing** (`src/environment.cc`): `toArrayLikeObject` properly boxes primitives for `indexOf`/`lastIndexOf`.

**Batch 3:** +10 tests: Function +10 (382→392), 0 regressions:

- **Function constructor ToString coercion** (`src/environment.cc`): Function constructor now calls ToPrimitive (string hint) on arguments via `toPrimitiveFromNative()` before parsing, enabling objects with custom toString() to work as expected per spec.
- **Function.prototype.bind Object callables** (`src/environment.cc`): Extended bind to accept Object-based callables (those with `__callable_object__` marker, e.g. String constructor). Handles name, length, and call dispatch for bound Object targets.

**Batch 2:** +13 tests: Promise +9 (631→640), Object +4 (3268→3272), 0 regressions:

- **Promise.prototype.then PromiseReactionJob** (`src/environment.cc`): Changed then handler to call capResolve (Promise Resolve Function) with callback result instead of directly calling C++ Promise::then(). This enables self-resolution detection, custom .then support, and proper thenable resolution per spec 25.4.2.1 step 8.
- **Object.groupBy iterator protocol** (`src/environment.cc`): Rewrote to use full iterator protocol (Symbol.iterator/.next) instead of only handling Arrays/Strings. Fixes invalid-iterable, iterator-next-throws, null-prototype tests.
- **Object.getPrototypeOf no-args TypeError** (`src/environment.cc`): Throw TypeError when called with no arguments instead of returning null.
- **Null-prototype hasOwnProperty** (`src/interpreter.cc`): Skip hardcoded Object.prototype method dispatch (hasOwnProperty, propertyIsEnumerable) for objects with null __proto__.

**Batch 1:** +73 tests: Map +14 (187→201), Set +20 (358→378), WeakMap +42 (97→139), WeakSet +13 (70→83), 0 regressions:

- **Collection constructor iterator protocol** (`src/environment.cc`): Rewrote Map/Set/WeakMap/WeakSet constructors to use full ES spec iterator protocol: get adder method (set/add) from instance, verify callable, iterate via Symbol.iterator/.next(), call adder via JS dispatch (not internal insert). Set __proto__ early in constructor so method lookup works before constructValue sets it.
- **Symbol key support in WeakMap/WeakSet** (`include/value.h`, `src/gc_value.cc`): Added `symbolEntries`/`symbolValues` parallel storage keyed by Symbol::id. All 4 methods (set/get/has/delete for WeakMap, add/has/delete for WeakSet) now handle Symbol keys. Extracted `extractGCObject()` helper to handle all 15+ GC types.
- **canBeHeldWeakly validation** (`src/environment.cc`): Shared `canBeHeldWeakly` lambda validates keys before WeakMap.set and values before WeakSet.add, throwing TypeError for non-weakly-holdable values (primitives, registered symbols).
- **WeakMap getOrInsert/getOrInsertComputed** (`src/environment.cc`): Implemented ES2025 upsert methods with proper key validation and callback invocation.
- **Map getOrInsertComputed validation fix** (`src/environment.cc`): Validate callback is callable BEFORE checking key existence per spec.
- **Set-like protocol in set methods** (`src/environment.cc`): Fixed `setRecordKeys` to get `next` method once before the loop (not on each iteration), added generator iterator support via `generatorNext()`, proper `getPropertyForExternal` for iterator step access.
- **Lazy iterator with early close** (`src/environment.cc`): Added `iterateSetRecordKeys` helper for `isSupersetOf` and `isDisjointFrom` that closes the iterator early (calls `.return()`) when result is determined. Added `isSupersetOf` early return when `this.size < other.size`.
- **WeakMap/WeakSet non-enumerable globals** (`src/environment.cc`): Marked WeakMap/WeakSet bindings as non-enumerable on globalThis.
- **WeakMap iterator close on entry access error** (`src/environment.cc`): Close iterator when Get(nextItem, "0") or Get(nextItem, "1") throws.

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

## Test Status
- `./build/test262_runner ./test262_suite --no-temp-skips` (2026-03-26) → 23,205 passed, 0 failed, 424 skipped (skips are known unsupported bundles such as `import-defer` and `regexp-modifiers`).
- Targeted `with`/`Symbol.unscopables` probes (`language/statements/with/{unscopables-inc-dec,set-mutable-binding-idref-with-proxy-env,set-mutable-binding-idref-compound-assign-with-proxy-env}.js`) and the related `function/arrow` unscopables suites now pass on the updated runtime.

---

**Last Updated:** 2026-03-26 (updated)
