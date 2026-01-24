#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "environment.h"
#include <iostream>
#include <string>

using namespace tinyjs;

void runTest(const std::string& name, const std::string& code, const std::string& expected = "") {
  std::cout << "Test: " << name << std::endl;

  try {
    Lexer lexer(code);
    auto tokens = lexer.tokenize();

    Parser parser(tokens);
    auto program = parser.parse();

    if (!program) {
      std::cout << "  Parse error!" << std::endl;
      return;
    }

    auto env = Environment::createGlobal();
    Interpreter interpreter(env);

    auto task = interpreter.evaluate(*program);

    while (!task.done()) {
      std::coroutine_handle<>::from_address(task.handle.address()).resume();
    }

    Value result = task.result();
    std::cout << "  Result: " << result.toString() << std::endl;

    if (!expected.empty() && result.toString() != expected) {
      std::cout << "  FAILED! Expected: " << expected << std::endl;
    } else {
      std::cout << "  PASSED" << std::endl;
    }
  } catch (const std::exception& e) {
    std::cout << "  Error: " << e.what() << std::endl;
  }

  std::cout << std::endl;
}

int main() {
  std::cout << "=== TinyJS C++20 Test Suite ===" << std::endl << std::endl;

  runTest("Basic arithmetic", "2 + 3 * 4", "14");

  runTest("Variable declaration", R"(
    let x = 10;
    let y = 20;
    x + y
  )", "30");

  runTest("Function declaration", R"(
    function add(a, b) {
      return a + b;
    }
    add(5, 7)
  )", "12");

  runTest("If statement", R"(
    let num = 15;
    if (num > 10) {
      num * 2
    } else {
      num / 2
    }
  )", "30");

  runTest("While loop", R"(
    let sum = 0;
    let i = 1;
    while (i <= 5) {
      sum = sum + i;
      i = i + 1;
    }
    sum
  )", "15");

  runTest("For loop", R"(
    let total = 0;
    for (let i = 0; i < 10; i = i + 1) {
      total = total + i;
    }
    total
  )", "45");

  runTest("Array creation", R"(
    let arr = [1, 2, 3, 4, 5];
    arr
  )", "[Array]");

  runTest("Object creation", R"(
    let obj = { x: 10, y: 20 };
    obj
  )", "[Object]");

  runTest("Function closure", R"(
    function makeCounter() {
      let count = 0;
      function increment() {
        count = count + 1;
        return count;
      }
      return increment;
    }
    let counter = makeCounter();
    counter();
    counter();
    counter()
  )", "3");

  runTest("Recursive factorial", R"(
    function factorial(n) {
      if (n <= 1) {
        return 1;
      }
      return n * factorial(n - 1);
    }
    factorial(5)
  )", "120");

  runTest("Conditional expression", R"(
    let age = 25;
    age >= 18 ? "adult" : "minor"
  )", "adult");

  runTest("String concatenation", R"(
    let greeting = "Hello, ";
    let name = "TinyJS";
    greeting + name
  )", "Hello, TinyJS");

  runTest("BigInt literal", R"(
    let big = 9007199254740991n;
    big
  )", "9007199254740991n");

  runTest("BigInt arithmetic addition", R"(
    let a = 100n;
    let b = 200n;
    a + b
  )", "300n");

  runTest("BigInt arithmetic subtraction", R"(
    let a = 500n;
    let b = 200n;
    a - b
  )", "300n");

  runTest("BigInt arithmetic multiplication", R"(
    let a = 123456789n;
    let b = 987654321n;
    a * b
  )", "121932631112635269n");

  runTest("BigInt arithmetic division", R"(
    let a = 1000n;
    let b = 3n;
    a / b
  )", "333n");

  runTest("BigInt arithmetic modulo", R"(
    let a = 1000n;
    let b = 7n;
    a % b
  )", "6n");

  runTest("BigInt comparison", R"(
    let a = 100n;
    let b = 200n;
    a < b
  )", "true");

  runTest("BigInt equality", R"(
    let a = 12345n;
    let b = 12345n;
    a === b
  )", "true");

  runTest("BigInt typeof", R"(
    let big = 999n;
    typeof big
  )", "bigint");

  runTest("BigInt negation", R"(
    let big = 42n;
    -big
  )", "-42n");

  runTest("Uint8Array creation", R"(
    let arr = Uint8Array(10);
    arr
  )", "[TypedArray]");

  runTest("Uint8Array length", R"(
    let arr = Uint8Array(5);
    arr.length
  )", "5");

  runTest("Uint8Array set and get", R"(
    let arr = Uint8Array(3);
    arr[0] = 100;
    arr[1] = 200;
    arr[2] = 50;
    arr[1]
  )", "200");

  runTest("Int8Array negative values", R"(
    let arr = Int8Array(2);
    arr[0] = -10;
    arr[1] = 120;
    arr[0]
  )", "-10");

  runTest("Uint8ClampedArray clamping", R"(
    let arr = Uint8ClampedArray(3);
    arr[0] = 300;
    arr[1] = -50;
    arr[2] = 128;
    arr[0]
  )", "255");

  runTest("Float32Array", R"(
    let arr = Float32Array(2);
    arr[0] = 3.14;
    arr[1] = 2.71;
    arr[0]
  )", "3.14");

  runTest("Int32Array", R"(
    let arr = Int32Array(2);
    arr[0] = 1000000;
    arr[1] = -999999;
    arr[0]
  )", "1e+06");

  runTest("Uint16Array", R"(
    let arr = Uint16Array(3);
    arr[0] = 65535;
    arr[1] = 32768;
    arr[2] = 100;
    arr[1]
  )", "32768");

  runTest("TypedArray byteLength", R"(
    let arr = Uint32Array(10);
    arr.byteLength
  )", "40");

  runTest("Float16Array creation", R"(
    let arr = Float16Array(5);
    arr
  )", "[TypedArray]");

  runTest("Float16Array length", R"(
    let arr = Float16Array(8);
    arr.length
  )", "8");

  runTest("Float16Array set and get", R"(
    let arr = Float16Array(3);
    arr[0] = 1.5;
    arr[1] = 2.75;
    arr[2] = 3.25;
    arr[1]
  )", "2.75");

  runTest("Float16Array byteLength", R"(
    let arr = Float16Array(10);
    arr.byteLength
  )", "20");

  runTest("Async function declaration", R"(
    async function test() {
      return 42;
    }
    test()
  )", "[Promise]");

  runTest("Async function expression", R"(
    let fn = async function() {
      return "hello";
    };
    fn()
  )", "[Promise]");

  runTest("SHA-256 hash", R"(
    crypto.sha256("hello")
  )", "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");

  runTest("SHA-256 empty string", R"(
    crypto.sha256("")
  )", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  runTest("HMAC-SHA256", R"(
    crypto.hmac("key", "message")
  )", "6e9ef29b75fffc5b7abae527d58fdadb2fe42e7219011976917343065f58ed4a");

  runTest("Hex encoding", R"(
    crypto.toHex("hello")
  )", "68656c6c6f");

  runTest("Fetch returns promise", R"(
    let result = fetch("file:///test.txt");
    typeof result
  )", "object");

  runTest("Fetch file protocol", R"(
    let resp = fetch("file:///home/syoyo/work/tinyjs/build/test.txt");
    resp.status
  )", "200");

  runTest("Fetch file protocol - ok property", R"(
    let resp = fetch("file:///home/syoyo/work/tinyjs/build/test.txt");
    resp.ok
  )", "true");

  runTest("Fetch file protocol - text method", R"(
    let resp = fetch("file:///home/syoyo/work/tinyjs/build/test.txt");
    resp.text()
  )", "Hello from file!\n");

  runTest("Fetch file not found", R"(
    let resp = fetch("file:///nonexistent.txt");
    resp.status
  )", "404");

  runTest("Regex literal", R"(
    let re = /hello/;
    re
  )", "/hello/");

  runTest("Regex literal with flags", R"(
    let re = /hello/i;
    re
  )", "/hello/i");

  runTest("Regex test - match", R"(
    let re = /world/;
    re.test("hello world")
  )", "true");

  runTest("Regex test - no match", R"(
    let re = /xyz/;
    re.test("hello world")
  )", "false");

  runTest("Regex test - case insensitive", R"(
    let re = /HELLO/i;
    re.test("hello world")
  )", "true");

  runTest("Regex exec - match", R"(
    let re = /world/;
    let result = re.exec("hello world");
    result
  )", "[Array]");

  runTest("Regex exec - no match", R"(
    let re = /xyz/;
    let result = re.exec("hello world");
    result
  )", "null");

  runTest("String match method", R"(
    let str = "hello world";
    let result = str.match(/world/);
    result
  )", "[Array]");

  runTest("String replace with regex", R"(
    let str = "hello world";
    str.replace(/world/, "universe")
  )", "hello universe");

  runTest("String replace with string", R"(
    let str = "hello world";
    str.replace("world", "there")
  )", "hello there");

  runTest("RegExp constructor", R"(
    let re = RegExp("test", "i");
    re.test("TEST")
  )", "true");

  runTest("Regex source property", R"(
    let re = /hello/;
    re.source
  )", "hello");

  runTest("Regex flags property", R"(
    let re = /hello/gi;
    re.flags
  )", "gi");

  runTest("Arrow function - single parameter", R"(
    let square = x => x * x;
    square(5)
  )", "25");

  runTest("Arrow function - multiple parameters", R"(
    let add = (a, b) => a + b;
    add(10, 20)
  )", "30");

  runTest("Arrow function - no parameters", R"(
    let getNum = () => 42;
    getNum()
  )", "42");

  runTest("Arrow function - block body", R"(
    let multiply = (x, y) => {
      let result = x * y;
      return result;
    };
    multiply(6, 7)
  )", "42");

  runTest("Arrow function - with rest parameters", R"(
    let sum = (...nums) => {
      let total = 0;
      for (let i = 0; i < nums.length; i = i + 1) {
        total = total + nums[i];
      }
      return total;
    };
    sum(1, 2, 3, 4, 5)
  )", "15");

  runTest("Arrow function - in array method", R"(
    let nums = [1, 2, 3, 4, 5];
    let doubled = nums.map(n => n * 2);
    doubled
  )", "[Array]");

  runTest("Optional chaining - with value", R"(
    let obj = {a: {b: {c: 42}}};
    obj?.a?.b?.c
  )", "42");

  runTest("Optional chaining - with null", R"(
    let obj = null;
    obj?.a?.b?.c
  )", "undefined");

  runTest("Optional chaining - with undefined", R"(
    let obj = {a: null};
    obj?.a?.b?.c
  )", "undefined");

  runTest("Nullish coalescing - with null", R"(
    let x = null;
    x ?? 42
  )", "42");

  runTest("Nullish coalescing - with undefined", R"(
    let x;
    x ?? 100
  )", "100");

  runTest("Nullish coalescing - with value", R"(
    let x = 0;
    x ?? 42
  )", "0");

  runTest("Nullish coalescing - with false", R"(
    let x = false;
    x ?? true
  )", "false");

  runTest("Number.toFixed", R"(
    let num = 3.14159;
    num.toFixed(2)
  )", "3.14");

  runTest("Number.toPrecision", R"(
    let num = 123.456;
    num.toPrecision(4)
  )", "123.5");

  runTest("Number.toString with radix", R"(
    let num = 255;
    num.toString(16)
  )", "ff");

  runTest("Number.parseInt - decimal", R"(
    Number.parseInt("42")
  )", "42");

  runTest("Number.parseInt - hexadecimal", R"(
    Number.parseInt("0xFF", 16)
  )", "255");

  runTest("Number.parseFloat", R"(
    Number.parseFloat("3.14")
  )", "3.14");

  runTest("Global parseInt", R"(
    parseInt("123")
  )", "123");

  runTest("Global parseFloat", R"(
    parseFloat("2.71828")
  )", "2.71828");

  runTest("Number.isNaN", R"(
    Number.isNaN(0 / 0)
  )", "true");

  runTest("Number.isFinite", R"(
    Number.isFinite(42)
  )", "true");

  runTest("Logical AND assignment (&&=) - truthy", R"(
    let x = 5;
    x &&= 10;
    x
  )", "10");

  runTest("Logical AND assignment (&&=) - falsy", R"(
    let x = 0;
    x &&= 10;
    x
  )", "0");

  runTest("Logical OR assignment (||=) - truthy", R"(
    let x = 5;
    x ||= 10;
    x
  )", "5");

  runTest("Logical OR assignment (||=) - falsy", R"(
    let x = 0;
    x ||= 10;
    x
  )", "10");

  runTest("Nullish assignment (??=) - nullish", R"(
    let x = null;
    x ??= 42;
    x
  )", "42");

  runTest("Nullish assignment (??=) - zero", R"(
    let x = 0;
    x ??= 42;
    x
  )", "0");

  runTest("String.includes - found", R"(
    let str = "hello world";
    str.includes("world")
  )", "true");

  runTest("String.includes - not found", R"(
    let str = "hello world";
    str.includes("xyz")
  )", "false");

  runTest("String.includes - with position", R"(
    let str = "hello world";
    str.includes("hello", 1)
  )", "false");

  runTest("String.repeat", R"(
    let str = "abc";
    str.repeat(3)
  )", "abcabcabc");

  runTest("String.padStart", R"(
    let str = "5";
    str.padStart(3, "0")
  )", "005");

  runTest("String.padEnd", R"(
    let str = "5";
    str.padEnd(3, "0")
  )", "500");

  runTest("Array.isArray - array", R"(
    let arr = [1, 2, 3];
    Array.isArray(arr)
  )", "true");

  runTest("Array.isArray - not array", R"(
    let obj = {a: 1};
    Array.isArray(obj)
  )", "false");

  runTest("Array.from - array", R"(
    let arr = [1, 2, 3];
    let copy = Array.from(arr);
    copy
  )", "[Array]");

  runTest("Array.from - string", R"(
    let str = "abc";
    let arr = Array.from(str);
    arr
  )", "[Array]");

  runTest("Array.of", R"(
    let arr = Array.of(1, 2, 3, 4);
    arr
  )", "[Array]");

  runTest("Object.freeze", R"(
    let obj = { x: 10, y: 20 };
    Object.freeze(obj);
    obj.x = 100;
    obj.z = 30;
    obj.x
  )", "10");

  runTest("Object.isFrozen", R"(
    let obj = { a: 1 };
    let frozen = Object.isFrozen(obj);
    Object.freeze(obj);
    let frozenAfter = Object.isFrozen(obj);
    frozen + "," + frozenAfter
  )", "false,true");

  runTest("Object.seal", R"(
    let obj = { x: 10 };
    Object.seal(obj);
    obj.x = 20;
    obj.y = 30;
    obj.x + "," + obj.y
  )", "20,undefined");

  runTest("Object.isSealed", R"(
    let obj = { a: 1 };
    let sealed = Object.isSealed(obj);
    Object.seal(obj);
    let sealedAfter = Object.isSealed(obj);
    sealed + "," + sealedAfter
  )", "false,true");

  runTest("Object.keys", R"(
    let obj = { a: 1, b: 2, c: 3 };
    Object.keys(obj)
  )", "[Array]");

  runTest("Object.values", R"(
    let obj = { a: 1, b: 2, c: 3 };
    Object.values(obj)
  )", "[Array]");

  runTest("Object.entries", R"(
    let obj = { a: 1, b: 2 };
    Object.entries(obj)
  )", "[Array]");

  runTest("Template literal - basic", R"(
    let name = "World";
    `Hello, ${name}!`
  )", "Hello, World!");

  runTest("Template literal - expression", R"(
    let a = 10;
    let b = 20;
    `The sum of ${a} and ${b} is ${a + b}`
  )", "The sum of 10 and 20 is 30");

  runTest("Template literal - nested", R"(
    let x = 5;
    `Result: ${x * 2} (doubled from ${x})`
  )", "Result: 10 (doubled from 5)");

  runTest("Object spread - basic", R"(
    let obj1 = { a: 1, b: 2 };
    let obj2 = { ...obj1, c: 3 };
    obj2.a + "," + obj2.b + "," + obj2.c
  )", "1,2,3");

  runTest("Object spread - override", R"(
    let obj1 = { a: 1, b: 2 };
    let obj2 = { ...obj1, b: 3, c: 4 };
    obj2.a + "," + obj2.b + "," + obj2.c
  )", "1,3,4");

  runTest("Object spread - multiple", R"(
    let obj1 = { a: 1 };
    let obj2 = { b: 2 };
    let obj3 = { ...obj1, ...obj2, c: 3 };
    obj3.a + "," + obj3.b + "," + obj3.c
  )", "1,2,3");

  runTest("Array spread in function call", R"(
    function sum(a, b, c) {
      return a + b + c;
    }
    let nums = [1, 2, 3];
    sum(...nums)
  )", "6");

  runTest("Object shorthand property", R"(
    let x = 10;
    let y = 20;
    let obj = { x, y };
    obj.x + "," + obj.y
  )", "10,20");

  runTest("Object shorthand with spread", R"(
    let a = 1;
    let b = 2;
    let obj1 = { a };
    let obj2 = { ...obj1, b };
    obj2.a + "," + obj2.b
  )", "1,2");

  // Default function parameters tests
  runTest("Default parameter - basic", R"(
    function greet(name = "World") {
      return "Hello, " + name;
    }
    greet()
  )", "Hello, World");

  runTest("Default parameter - with argument", R"(
    function greet(name = "World") {
      return "Hello, " + name;
    }
    greet("Alice")
  )", "Hello, Alice");

  runTest("Default parameter - multiple", R"(
    function add(a = 0, b = 0) {
      return a + b;
    }
    add() + "," + add(5) + "," + add(5, 3)
  )", "0,5,8");

  runTest("Default parameter - expression", R"(
    function multiply(a, b = a * 2) {
      return a * b;
    }
    multiply(3) + "," + multiply(3, 4)
  )", "18,12");

  runTest("Default parameter - arrow function", R"(
    const greet = (name = "World") => "Hello, " + name;
    greet() + "," + greet("Bob")
  )", "Hello, World,Hello, Bob");

  runTest("Default parameter - with rest", R"(
    function test(a = 1, b = 2, ...rest) {
      return a + "," + b + "," + rest.length;
    }
    test() + "|" + test(10) + "|" + test(10, 20, 30, 40)
  )", "1,2,0|10,2,0|10,20,2");

  // Array destructuring tests
  runTest("Array destructuring - basic", R"(
    const [a, b] = [1, 2];
    a + "," + b
  )", "1,2");

  runTest("Array destructuring - extra elements", R"(
    const [x, y] = [10, 20, 30, 40];
    x + "," + y
  )", "10,20");

  runTest("Array destructuring - missing elements", R"(
    const [m, n, o] = [100, 200];
    m + "," + n + "," + o
  )", "100,200,undefined");

  runTest("Array destructuring - with holes", R"(
    const [first, , third] = [1, 2, 3];
    first + "," + third
  )", "1,3");

  runTest("Array destructuring - let declaration", R"(
    let [p, q] = [5, 6];
    p = 10;
    q = 20;
    p + "," + q
  )", "10,20");

  // Object destructuring tests
  runTest("Object destructuring - basic", R"(
    const {x, y} = {x: 10, y: 20};
    x + "," + y
  )", "10,20");

  runTest("Object destructuring - renamed", R"(
    const {x: a, y: b} = {x: 1, y: 2};
    a + "," + b
  )", "1,2");

  runTest("Object destructuring - missing properties", R"(
    const {name, age} = {name: "Alice"};
    name + "," + age
  )", "Alice,undefined");

  runTest("Object destructuring - shorthand", R"(
    const obj = {foo: 100, bar: 200};
    const {foo, bar} = obj;
    foo + "," + bar
  )", "100,200");

  // Exponentiation operator tests
  runTest("Exponentiation - basic", R"(
    2 ** 3
  )", "8");

  runTest("Exponentiation - right associative", R"(
    2 ** 3 ** 2
  )", "512");

  runTest("Exponentiation - with negatives", R"(
    (-2) ** 3
  )", "-8");

  runTest("Exponentiation - fractional", R"(
    4 ** 0.5
  )", "2");

  runTest("Exponentiation - zero exponent", R"(
    10 ** 0
  )", "1");

  runTest("Exponentiation - with precedence", R"(
    2 + 3 ** 2
  )", "11");

  runTest("Exponentiation - multiple", R"(
    const a = 2 ** 4;
    const b = 3 ** 3;
    a + "," + b
  )", "16,27");

  // Rest/spread in destructuring tests
  runTest("Array destructuring - rest element", R"(
    const [first, ...rest] = [1, 2, 3, 4, 5];
    first + "," + rest.length + "," + rest[0] + "," + rest[3]
  )", "1,4,2,5");

  runTest("Array destructuring - rest with holes", R"(
    const [a, , ...rest] = [10, 20, 30, 40];
    a + "," + rest.length + "," + rest[0] + "," + rest[1]
  )", "10,2,30,40");

  runTest("Array destructuring - empty rest", R"(
    const [x, y, ...rest] = [1, 2];
    x + "," + y + "," + rest.length
  )", "1,2,0");

  runTest("Object destructuring - rest properties", R"(
    const {a, ...rest} = {a: 1, b: 2, c: 3, d: 4};
    a + "," + rest.b + "," + rest.c + "," + rest.d
  )", "1,2,3,4");

  runTest("Object destructuring - rest with renamed", R"(
    const {x: foo, ...rest} = {x: 10, y: 20, z: 30};
    foo + "," + rest.y + "," + rest.z
  )", "10,20,30");

  // Computed property names tests
  runTest("Computed property name - basic", R"(
    const key = "foo";
    const obj = {[key]: 42};
    obj.foo
  )", "42");

  runTest("Computed property name - expression", R"(
    const prefix = "prop";
    const num = 3;
    const obj = {[prefix + num]: "value"};
    obj.prop3
  )", "value");

  runTest("Computed property name - with regular props", R"(
    const key1 = "dynamic";
    const obj = {
      normal: "static",
      [key1]: "computed",
      another: "regular"
    };
    obj.normal + "," + obj.dynamic + "," + obj.another
  )", "static,computed,regular");

  runTest("Computed property name - multiple", R"(
    const a = "x";
    const b = "y";
    const obj = {[a]: 1, [b]: 2, z: 3};
    obj.x + "," + obj.y + "," + obj.z
  )", "1,2,3");

  // Symbol tests
  runTest("Symbol - basic creation", R"(
    const sym = Symbol();
    typeof sym
  )", "symbol");

  runTest("Symbol - with description", R"(
    const sym = Symbol("mySymbol");
    "" + sym  // This will use toString() internally
  )", "Symbol(mySymbol)");

  runTest("Symbol - unique identity", R"(
    const sym1 = Symbol("test");
    const sym2 = Symbol("test");
    sym1 === sym2
  )", "false");

  runTest("Symbol - as object key", R"(
    const sym = Symbol("prop");
    const obj = {};
    obj[sym] = 42;
    obj[sym]
  )", "42");

  runTest("Symbol.iterator exists", R"(
    typeof Symbol.iterator
  )", "symbol");

  runTest("Array Symbol.iterator returns function", R"(
    const iterFn = [1, 2, 3][Symbol.iterator];
    typeof iterFn;
  )", "function");

  runTest("Array Symbol.iterator produces iterator", R"(
    const iter = [10, 20][Symbol.iterator]();
    const first = iter.next();
    first.value;
  )", "10");

  runTest("String Symbol.iterator produces iterator", R"(
    const iter = "ok"[Symbol.iterator]();
    const step = iter.next();
    step.value;
  )", "o");

  runTest("String Symbol.iterator chained value", R"(
    const iter = "ok"[Symbol.iterator]();
    iter.next().value;
  )", "o");

  runTest("Custom object generator iterator", R"(
    const obj = {
      *[Symbol.iterator]() {
        yield 1;
        yield 2;
      }
    };
    let sum = 0;
    for (const n of obj) {
      sum = sum + n;
    }
    sum;
  )", "3");

  runTest("Iterator object without this", R"(
    const iter = {
      [Symbol.iterator]() {
        const data = [0, 1, 2];
        let index = 0;
        return {
          next() {
            if (index < data.length) {
              return { value: data[index++], done: false };
            }
            return { value: undefined, done: true };
          }
        };
      }
    };
    let total = 0;
    for (const val of iter) {
      total = total + val;
    }
    total;
  )", "3");

  // Error types
  runTest("Error - basic constructor", R"(
    const err = Error("Something went wrong");
    err.toString()
  )", "Error: Something went wrong");

  runTest("TypeError - basic constructor", R"(
    const err = TypeError("Type mismatch");
    err.toString()
  )", "TypeError: Type mismatch");

  runTest("ReferenceError - basic constructor", R"(
    const err = ReferenceError("Variable not found");
    err.toString()
  )", "ReferenceError: Variable not found");

  runTest("Error - without message", R"(
    const err = Error();
    err.toString()
  )", "Error");

  runTest("RangeError - basic constructor", R"(
    const err = RangeError("Index out of bounds");
    err.toString()
  )", "RangeError: Index out of bounds");

  // Dynamic import tests
  runTest("Dynamic import - returns Promise", R"(
    const p = import("./module.js");
    p.toString()
  )", "[Promise]");

  runTest("Dynamic import - module namespace properties", R"(
    const modulePromise = import("./test-module.js");
    modulePromise.toString()
  )", "[Promise]");

  runTest("Dynamic import - without argument returns Promise", R"(
    const p = import();
    p.toString()
  )", "[Promise]");

  runTest("Dynamic import - can be called multiple times", R"(
    const m1 = import("./module1.js");
    const m2 = import("./module2.js");
    m1.toString() + "," + m2.toString()
  )", "[Promise],[Promise]");

  // WeakMap/WeakSet infrastructure is implemented but needs method binding
  // TODO: Add tests once .set(), .get(), .has(), .delete() are bound

  // Proxy and Reflect API infrastructure implemented
  // TODO: Add tests once non-native function trap handlers are supported

  // ArrayBuffer tests
  runTest("ArrayBuffer - basic construction", R"(
    const buffer = ArrayBuffer(16);
    buffer.byteLength
  )", "16");

  runTest("ArrayBuffer - zero length", R"(
    const buffer = ArrayBuffer(0);
    buffer.byteLength
  )", "0");

  runTest("ArrayBuffer - type check", R"(
    const buffer = ArrayBuffer(8);
    "" + buffer
  )", "[ArrayBuffer]");

  // DataView tests - basic properties
  runTest("DataView - basic construction", R"(
    const buffer = ArrayBuffer(16);
    const view = DataView(buffer);
    view.byteLength
  )", "16");

  runTest("DataView - with offset", R"(
    const buffer = ArrayBuffer(16);
    const view = DataView(buffer, 4);
    view.byteOffset
  )", "4");

  runTest("DataView - with offset and length", R"(
    const buffer = ArrayBuffer(16);
    const view = DataView(buffer, 4, 8);
    view.byteLength
  )", "8");

  runTest("DataView - buffer property", R"(
    const buffer = ArrayBuffer(16);
    const view = DataView(buffer);
    view.buffer.byteLength
  )", "16");

  // DataView - Int8/Uint8 operations
  runTest("DataView - setInt8 and getInt8", R"(
    const buffer = ArrayBuffer(4);
    const view = DataView(buffer);
    view.setInt8(0, -42);
    view.getInt8(0)
  )", "-42");

  runTest("DataView - setUint8 and getUint8", R"(
    const buffer = ArrayBuffer(4);
    const view = DataView(buffer);
    view.setUint8(0, 200);
    view.getUint8(0)
  )", "200");

  // DataView - Int16/Uint16 operations
  runTest("DataView - setInt16 and getInt16 (big-endian)", R"(
    const buffer = ArrayBuffer(4);
    const view = DataView(buffer);
    view.setInt16(0, -1234, false);
    view.getInt16(0, false)
  )", "-1234");

  runTest("DataView - setUint16 and getUint16 (little-endian)", R"(
    const buffer = ArrayBuffer(4);
    const view = DataView(buffer);
    view.setUint16(0, 5678, true);
    view.getUint16(0, true)
  )", "5678");

  // DataView - Int32/Uint32 operations
  runTest("DataView - setInt32 and getInt32", R"(
    const buffer = ArrayBuffer(8);
    const view = DataView(buffer);
    view.setInt32(0, -123456, false);
    view.getInt32(0, false)
  )", "-123456");

  runTest("DataView - setUint32 and getUint32", R"(
    const buffer = ArrayBuffer(8);
    const view = DataView(buffer);
    view.setUint32(0, 987654, true);
    view.getUint32(0, true)
  )", "987654");

  // DataView - Float32/Float64 operations
  runTest("DataView - setFloat32 and getFloat32", R"(
    const buffer = ArrayBuffer(8);
    const view = DataView(buffer);
    view.setFloat32(0, 3.14, false);
    view.getFloat32(0, false)
  )", "3.14");

  runTest("DataView - setFloat64 and getFloat64", R"(
    const buffer = ArrayBuffer(16);
    const view = DataView(buffer);
    view.setFloat64(0, 2.718281828, true);
    view.getFloat64(0, true)
  )", "2.71828");

  // DataView - BigInt operations
  runTest("DataView - setBigInt64 and getBigInt64", R"(
    const buffer = ArrayBuffer(16);
    const view = DataView(buffer);
    view.setBigInt64(0, 9007199254740991n, false);
    view.getBigInt64(0, false)
  )", "9007199254740991n");

  runTest("DataView - setBigUint64 and getBigUint64", R"(
    const buffer = ArrayBuffer(16);
    const view = DataView(buffer);
    view.setBigUint64(0, 18446744073709551n, true);
    view.getBigUint64(0, true)
  )", "18446744073709551n");

  // DataView - multiple values in same buffer
  runTest("DataView - multiple values", R"(
    const buffer = ArrayBuffer(16);
    const view = DataView(buffer);
    view.setInt8(0, 42);
    view.setInt16(2, 1000, false);
    view.setInt32(4, 100000, false);
    view.getInt8(0) + "," + view.getInt16(2, false) + "," + view.getInt32(4, false)
  )", "42,1000,100000");

  // DataView - endianness test
  runTest("DataView - endianness matters", R"(
    const buffer = ArrayBuffer(4);
    const view = DataView(buffer);
    view.setUint16(0, 258, true);
    view.getUint8(0) + "," + view.getUint8(1)
  )", "2,1");

  // globalThis tests
  runTest("globalThis - exists", R"(
    typeof globalThis
  )", "object");

  runTest("globalThis - has console", R"(
    typeof globalThis.console
  )", "object");

  runTest("globalThis - can define and access variables", R"(
    globalThis.myVar = 42;
    globalThis.myVar
  )", "42");

  runTest("globalThis - references itself", R"(
    typeof globalThis.globalThis
  )", "object");

  runTest("globalThis - has built-in constructors", R"(
    typeof globalThis.ArrayBuffer
  )", "function");

  // Top-level await tests
  runTest("Top-level await - with Promise.resolve", R"(
    const result = await Promise.resolve(42);
    result
  )", "42");

  runTest("Top-level await - with async expression", R"(
    const value = await Promise.resolve("hello");
    value
  )", "hello");

  runTest("Top-level await - multiple awaits", R"(
    const a = await Promise.resolve(10);
    const b = await Promise.resolve(20);
    a + b
  )", "30");

  runTest("Top-level await - with computation", R"(
    const num = await Promise.resolve(5);
    num * num
  )", "25");

  // Unicode tests
  runTest("Unicode - emoji length", R"(
    const str = "Hello 👋 World 🌍";
    str.length
  )", "15");

  runTest("Unicode - CJK characters", R"(
    const str = "你好世界";
    str.length
  )", "4");

  runTest("Unicode - charAt with emoji", R"(
    const str = "A👋B";
    str.charAt(1)
  )", "👋");

  runTest("Unicode - codePointAt", R"(
    const str = "👋";
    str.codePointAt(0)
  )", "128075");

  runTest("Unicode - String.fromCodePoint", R"(
    String.fromCodePoint(128075)
  )", "👋");

  runTest("Unicode - String.fromCodePoint multiple", R"(
    String.fromCodePoint(72, 101, 108, 108, 111)
  )", "Hello");

  runTest("Unicode - String.fromCharCode", R"(
    String.fromCharCode(72, 101, 108, 108, 111)
  )", "Hello");

  runTest("Unicode - Arabic characters", R"(
    const str = "مرحبا";
    str.length
  )", "5");

  runTest("Unicode - mixed scripts", R"(
    const str = "Hello世界🌍";
    str.length
  )", "8");

  runTest("Unicode - surrogate pair emoji", R"(
    const str = "🎉🎊🎈";
    str.length
  )", "3");

  std::cout << "=== All tests completed ===" << std::endl;

  return 0;
}
