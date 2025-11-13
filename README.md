# LightJS

W.I.P.

A modern, compact JavaScript (ES2020) interpreter written in C++20
(TypeScript support planned.)

[![GitHub Sponsors](https://img.shields.io/badge/Sponsor-❤️-pink?logo=github)](https://github.com/sponsors/syoyo)

## Features

### Core Architecture
- **C++20 coroutine-based execution** - Efficient async execution model
- **No C++ exceptions** in hot paths (compile with `-fno-exceptions` where possible)
- **No RTTI** (`-fno-rtti` enabled) - Reduced binary size
- **CMake build system** - Cross-platform support
- **Security-focused** - Safe evaluation with bounded execution
- **Garbage Collection** - Automatic memory management with reference counting and cycle detection

### Implemented JavaScript Features

#### Core Language (ES2020)
- ✅ **Variables** - `let`, `const`, `var` with proper scoping
- ✅ **Data Types**
  - Primitives: `undefined`, `null`, `boolean`, `number`, `string`, `bigint`
  - Objects and Arrays
  - Functions (regular and arrow functions)
  - Template literals with interpolation (`` `Hello, ${name}` ``)
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
  - `do...while` loops
  - `for` loops
  - `for...in` loops (iterate over object keys)
  - `for...of` loops (iterate over iterable values)
  - `switch` statements with case/default and fall-through
  - `break` and `continue`
  - `return` statements
  - `try`/`catch`/`finally` with error handling
  - `throw` statements
- ✅ **Functions**
  - Function declarations and expressions
  - Arrow functions with expression and block bodies
  - Single parameter and multi-parameter arrow functions
  - Arrow functions with rest parameters
  - Default parameters for all function types
  - Closures and lexical scoping
  - Recursion support
- ✅ **Objects**
  - Object literals
  - Property access (dot and bracket notation)
  - Method calls
  - `this` binding
  - `new` operator for constructors
- ✅ **Classes**
  - ES6 class syntax
  - Constructor methods
  - Instance and static methods
  - Class inheritance with `extends`
  - `super` keyword
  - Getters and setters

#### Advanced Features
- ✅ **ES2020/ES2021 Features**
  - Arrow functions (`=>`) with all variations
  - Template literals with interpolation (`` `Hello ${name}` ``)
  - Optional chaining (`?.`) for safe property access
  - Nullish coalescing (`??`) operator
  - Logical assignment operators (`&&=`, `||=`, `??=`)
  - Object spread syntax (`{...obj}`)
  - Object shorthand property notation (`{x}` for `{x: x}`)
  - Array spread in function calls (`func(...args)`)
  - Default function parameters (`function fn(a = 0, b = 1)`)
  - Destructuring assignment for arrays (`const [a, b] = [1, 2]`)
  - Destructuring assignment for objects (`const {x, y} = obj`)
  - Rest/spread in destructuring (`const [first, ...rest] = arr`, `const {a, ...rest} = obj`)
  - Exponentiation operator (`**`) with right associativity
  - Computed property names (`{[key]: value}`)
  - Symbol primitive type with unique identity
- ✅ **Async/Await**
  - Full async function support
  - Await expressions with Promise unwrapping
  - **Top-level await** - Use await at module scope without async function wrapper
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
- ✅ **Map/Set Collections**
  - Map with key-value pairs
  - Set with unique values
  - Insertion order preservation
  - has(), get(), set(), delete() methods

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
  - `Promise.all()`, `Promise.race()`
  - `.then()`, `.catch()`, `.finally()`
  - Promise chaining support
- ✅ **JSON object** (Pure C++ implementation)
  - JSON.parse() for parsing JSON strings
  - JSON.stringify() for serializing objects to JSON
  - Support for all JSON types (string, number, boolean, null, array, object)
  - Proper escape sequence handling
- ✅ **Object methods**
  - Object.keys() - get property names
  - Object.values() - get property values
  - Object.entries() - get key-value pairs
  - Object.assign() - copy properties between objects
  - Object.hasOwnProperty() - check property existence
  - Object.getOwnPropertyNames() - get all property names
  - Object.create() - create new objects
  - Object.freeze() - make objects immutable
  - Object.seal() - prevent adding/removing properties
  - Object.isFrozen() - check if object is frozen
  - Object.isSealed() - check if object is sealed
- ✅ **Array methods**
  - Array.isArray() - check if value is array
  - Array.from() - create array from iterable/array-like
  - Array.of() - create array from arguments
  - Array.prototype.push() - add elements to end
  - Array.prototype.pop() - remove element from end
  - Array.prototype.shift() - remove element from beginning
  - Array.prototype.unshift() - add elements to beginning
  - Array.prototype.slice() - extract section of array
  - Array.prototype.splice() - change array contents
  - Array.prototype.join() - join elements into string
  - Array.prototype.indexOf() - find index of element
  - Array.prototype.includes() - check if element exists
  - Array.prototype.reverse() - reverse array in place
  - Array.prototype.concat() - concatenate arrays
  - Array.prototype.map() - transform elements (functional)
  - Array.prototype.filter() - select elements (functional)
  - Array.prototype.reduce() - aggregate values (functional)
  - Array.prototype.forEach() - iterate over elements
- ✅ **Unicode Support**
  - Full UTF-8 string handling
  - Unicode-aware string length (code points, not bytes)
  - Unicode-aware charAt() and string indexing
  - String.fromCharCode() and String.fromCodePoint()
  - String.prototype.charCodeAt() and codePointAt()
  - Support for emoji, CJK characters, Arabic, and all Unicode scripts
  - Proper handling of multi-byte UTF-8 sequences
- ✅ **String methods**
  - String.prototype.charAt() - character at index
  - String.prototype.charCodeAt() - character code at index
  - String.prototype.indexOf() - find substring index
  - String.prototype.lastIndexOf() - find last substring index
  - String.prototype.substring() - extract substring
  - String.prototype.substr() - extract substring (legacy)
  - String.prototype.slice() - extract portion of string
  - String.prototype.split() - split string into array
  - String.prototype.replace() - replace substring
  - String.prototype.toLowerCase() - convert to lowercase
  - String.prototype.toUpperCase() - convert to uppercase
  - String.prototype.trim() - remove whitespace
  - String.prototype.includes() - check if contains substring
  - String.prototype.repeat() - repeat string n times
  - String.prototype.padStart() - pad string at start
  - String.prototype.padEnd() - pad string at end
- ✅ **Number methods**
  - Number.parseInt() - parse string to integer with radix
  - Number.parseFloat() - parse string to floating point
  - Number.isNaN() - check if value is NaN
  - Number.isFinite() - check if value is finite
  - Number constants (MAX_VALUE, MIN_VALUE, NaN, POSITIVE_INFINITY, NEGATIVE_INFINITY)
  - Number.prototype.toFixed() - format with fixed decimals
  - Number.prototype.toPrecision() - format with precision
  - Number.prototype.toExponential() - format in exponential notation
  - Number.prototype.toString() - convert to string with optional radix
- ✅ **Math object** (Pure C++ implementation)
  - Math constants: PI, E, LN2, LN10, LOG2E, LOG10E, SQRT1_2, SQRT2
  - Math.abs() - absolute value
  - Math.ceil() - round up
  - Math.floor() - round down
  - Math.round() - round to nearest integer
  - Math.trunc() - truncate decimal part
  - Math.max() - maximum value
  - Math.min() - minimum value
  - Math.pow() - exponentiation
  - Math.sqrt() - square root
  - Math.sin(), Math.cos(), Math.tan() - trigonometric functions
  - Math.random() - random number generation
  - Math.sign() - sign of number
  - Math.log(), Math.log10() - logarithmic functions
  - Math.exp() - exponential function
- ✅ **Date object** (Chrono-based implementation)
  - Date constructor with multiple signatures
  - Date.now() - current timestamp
  - Date.parse() - parse date string (simplified)
  - Date.prototype.getTime() - get timestamp
  - Date.prototype.getFullYear() - get year
  - Date.prototype.getMonth() - get month
  - Date.prototype.getDate() - get day of month
  - Date.prototype.getDay() - get day of week
  - Date.prototype.getHours() - get hours
  - Date.prototype.getMinutes() - get minutes
  - Date.prototype.getSeconds() - get seconds
  - Date.prototype.toString() - string representation
  - Date.prototype.toISOString() - ISO 8601 string
- ✅ **ArrayBuffer** - Fixed-length binary data buffer
  - ArrayBuffer constructor
  - byteLength property
  - Zero-initialized memory allocation
- ✅ **DataView** - Low-level interface for reading/writing binary data
  - DataView constructor with buffer, byteOffset, byteLength
  - buffer, byteOffset, byteLength properties
  - getInt8(), getUint8() - 8-bit integer access
  - getInt16(), getUint16() - 16-bit integer access
  - getInt32(), getUint32() - 32-bit integer access
  - getFloat32(), getFloat64() - floating point access
  - getBigInt64(), getBigUint64() - 64-bit BigInt access
  - setInt8(), setUint8(), setInt16(), setUint16() - set integer values
  - setInt32(), setUint32(), setFloat32(), setFloat64() - set numeric values
  - setBigInt64(), setBigUint64() - set BigInt values
  - **Full endianness support** (little-endian and big-endian)
- ✅ **globalThis** - Universal global object reference
  - Access to all global variables and functions
  - Consistent across all JavaScript environments
  - Self-referential (globalThis.globalThis === globalThis)
  - Provides access to console, constructors, and all built-ins

### Testing Infrastructure
- ✅ **Test262 Support**
  - Conformance test runner
  - Harness function implementation
  - Metadata parsing (YAML frontmatter)
  - Negative test support
  - Async test handling
  - Module test support

## Recent Updates

### Latest Changes (2025-10-01) - Session 2
- ✅ **Implemented for...in loops** - Iterate over object property keys
  - Syntax: `for (let key in obj) { ... }`
  - Works with object literals and Object instances

- ✅ **Implemented for...of loops** - Iterate over iterable values
  - Syntax: `for (let value of iterable) { ... }`
  - Supports arrays and strings
  - Proper iterator protocol support

- ✅ **Implemented do...while loops** - Execute body at least once
  - Syntax: `do { ... } while (condition);`
  - Condition evaluated after each iteration

- ✅ **Implemented switch statements** - Multi-way conditional branching
  - Syntax: `switch (expr) { case value: ...; break; default: ...; }`
  - Full support for `case`, `default`, and `break` statements
  - Proper fall-through behavior when break is omitted
  - Uses strict equality (===) for case matching

- 🔧 **Fixed object property access bug** - Object literals now work correctly
  - Object literal keys (`{a: 1}`) are now properly treated as property names
  - Previously, keys were incorrectly evaluated as variable lookups
  - Fix enables proper for...in iteration and Object.keys() functionality

- ✅ **Implemented template literals** - Modern string interpolation
  - Syntax: `` `Hello, ${name}!` ``
  - Full support for `${expression}` interpolation
  - Expressions are evaluated and converted to strings
  - Supports nested expressions and function calls
  - Escape sequences: `\n`, `\t`, `\r`, `\\`, `` \` ``, `\$`

- ✅ **Implemented array higher-order methods** - Functional programming support
  - `Array.prototype.map()` - Transform array elements
  - `Array.prototype.filter()` - Select array elements
  - `Array.prototype.reduce()` - Aggregate array values
  - `Array.prototype.forEach()` - Iterate over array elements
  - Currently supports native callback functions
  - Note: Full JavaScript callback support in progress

## Build Instructions

### Prerequisites
- C++20 compatible compiler (GCC 10+, Clang 10+, MSVC 2019+)
- CMake 3.20 or higher

### Building

```bash
# Clone the repository
git clone https://github.com/syoyo/lightjs.git
cd lightjs

# Create build directory
mkdir build && cd build

# Configure (default: with std::regex)
cmake ..

# Or configure with simple regex implementation (pure C++)
cmake .. -DUSE_SIMPLE_REGEX=ON

# Build
make -j$(nproc)

# Run tests
./lightjs_test
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

using namespace lightjs;

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

### Control Flow Examples

```javascript
// for...in loop - iterate over object keys
let person = {name: 'Alice', age: 30, city: 'NYC'};
for (let key in person) {
    console.log(key + ':', person[key]);
}

// for...of loop - iterate over array values
let numbers = [10, 20, 30, 40];
let sum = 0;
for (let num of numbers) {
    sum = sum + num;
}
console.log('Sum:', sum);  // Sum: 100

// for...of with strings
let word = 'hello';
for (let char of word) {
    console.log(char);
}

// do...while loop
let count = 0;
do {
    count = count + 1;
} while (count < 5);
console.log('Count:', count);  // Count: 5

// switch statement with break
function getGrade(score) {
    switch (score) {
        case 90:
            return 'A';
        case 80:
            return 'B';
        case 70:
            return 'C';
        default:
            return 'F';
    }
}

// switch with fall-through
function getDaysInMonth(month) {
    let days = 0;
    switch (month) {
        case 'January':
        case 'March':
        case 'May':
            days = 31;
            break;
        case 'February':
            days = 28;
            break;
        default:
            days = 30;
    }
    return days;
}
```

### Template Literals

```javascript
// Basic interpolation
let name = 'Alice';
let age = 30;
console.log(`Name: ${name}, Age: ${age}`);  // Name: Alice, Age: 30

// Expression interpolation
let a = 10;
let b = 20;
console.log(`Sum: ${a + b}`);  // Sum: 30
console.log(`Product: ${a * b}`);  // Product: 200

// Function calls in interpolation
function greet(name) {
    return `Hello, ${name}!`;
}
console.log(`Message: ${greet('World')}`);  // Message: Hello, World!

// Multi-line strings
let poem = `Roses are red,
Violets are blue,
JavaScript is great,
And so are you!`;
console.log(poem);
```

### Arrow Functions

```javascript
// Single parameter (no parentheses needed)
let square = x => x * x;
console.log(square(5));  // 25

// Multiple parameters
let add = (a, b) => a + b;
console.log(add(10, 20));  // 30

// No parameters
let getFortyTwo = () => 42;
console.log(getFortyTwo());  // 42

// Block body with explicit return
let multiply = (x, y) => {
    let result = x * y;
    return result;
};
console.log(multiply(6, 7));  // 42

// With rest parameters
let sum = (...nums) => {
    let total = 0;
    for (let i = 0; i < nums.length; i++) {
        total = total + nums[i];
    }
    return total;
};
console.log(sum(1, 2, 3, 4, 5));  // 15

// Used with array methods
let numbers = [1, 2, 3, 4, 5];
let doubled = numbers.map(n => n * 2);
let evens = numbers.filter(n => n % 2 === 0);
```

### Optional Chaining and Nullish Coalescing

```javascript
// Optional chaining - safe property access
let user = {
    name: 'Alice',
    address: {
        city: 'NYC',
        zip: '10001'
    }
};

console.log(user?.address?.city);  // 'NYC'
console.log(user?.contact?.phone);  // undefined (no error!)

let nullUser = null;
console.log(nullUser?.name);  // undefined (no error!)

// Nullish coalescing - default values for null/undefined only
let x = null;
console.log(x ?? 'default');  // 'default'

let y = 0;
console.log(y ?? 42);  // 0 (not 42, because 0 is not null/undefined)

let z = false;
console.log(z ?? true);  // false (not true, because false is not null/undefined)

let config = {
    timeout: 0  // explicitly set to 0
};
let timeout = config.timeout ?? 5000;  // Uses 0, not 5000

// Combining optional chaining with nullish coalescing
let settings = null;
let theme = settings?.theme ?? 'light';  // 'light'
```

- ✅ **Modern Operators**
  - Optional chaining (`?.`) - Safe property access on null/undefined
  - Nullish coalescing (`??`) - Default values for null/undefined only
  - Logical assignment (`&&=`, `||=`, `??=`) - Short-circuit assignment operators
  - Spread operator for arrays and function calls
  - Rest parameters for functions

- ✅ **Number Object**
  - Number.parseInt(), Number.parseFloat()
  - Number.isNaN(), Number.isFinite()
  - Number.MAX_VALUE, Number.MIN_VALUE, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY, Number.NaN
  - Global parseInt(), parseFloat(), isNaN(), isFinite()
  - Instance methods: toFixed(), toPrecision(), toExponential(), toString(radix)

## TODO - Unimplemented Features

### Language Features
- [ ] **Generators** - Generator functions and iterators
- [ ] **Destructuring** - Array and object destructuring
- [x] **Spread/Rest** operators (`...`) (✅ Implemented for objects, arrays, and function calls)
- [ ] **Symbol** type
- [ ] **WeakMap/WeakSet**
- [ ] **Proxy/Reflect** APIs
- [ ] **with** statement (deprecated but in spec)
- [ ] **Labels** and labeled statements
- [ ] **Computed property names**
- [ ] **Property descriptors** and defineProperty
- [ ] **Static class members**
- [ ] **Private class fields**
- [x] **Optional chaining** (`?.`) (✅ Implemented)
- [x] **Nullish coalescing** (`??`) (✅ Implemented)
- [x] **Logical assignment** operators (`&&=`, `||=`, `??=`) (✅ Implemented)
- [x] **Arrow functions** (✅ Implemented)
- [x] **Template literals** (✅ Implemented)
- [ ] **Numeric separators** (`1_000_000`)

### Built-in Objects & APIs
- [x] **Number methods** - toFixed, toPrecision, parseInt, parseFloat, isNaN, isFinite (✅ Implemented)
- [x] **Error types** - TypeError, ReferenceError, SyntaxError, etc. (✅ Implemented)
- [x] **ArrayBuffer** and **DataView** (✅ Implemented with full endianness support)
- [ ] **Intl** - Internationalization API
- [ ] **URL** and URLSearchParams
- [ ] **TextEncoder/TextDecoder**
- [ ] **setTimeout/setInterval** - Timer functions
- [ ] **queueMicrotask**
- [x] **globalThis** (✅ Implemented)

### Module System Enhancements
- [x] **Dynamic imports** - `import()` function (✅ Implemented)
- [ ] **Import.meta**
- [x] **Top-level await** (✅ Implemented)
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
lightjs/
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

MIT license.

## Acknowledgments

- Test262 conformance suite by TC39
- C++20 coroutines for efficient async implementation
- Community contributions and feedback
