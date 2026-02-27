#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "environment.h"
#include <iostream>
#include <string>

using namespace lightjs;

namespace {
int gTotalTests = 0;
int gFailedTests = 0;
}

void runTest(const std::string& name, const std::string& code, const std::string& expected = "",
             bool isModule = false) {
  gTotalTests++;
  std::cout << "Test: " << name << std::endl;

  try {
    Lexer lexer(code);
    auto tokens = lexer.tokenize();

    Parser parser(tokens, isModule);
    auto program = parser.parse();

    if (!program) {
      std::cout << "  Parse error!" << std::endl;
      gFailedTests++;
      return;
    }

    auto env = Environment::createGlobal();
    Interpreter interpreter(env);

    auto task = interpreter.evaluate(*program);

    Value result;
    LIGHTJS_RUN_TASK(task, result);
    std::cout << "  Result: " << result.toDisplayString() << std::endl;

    if (!expected.empty() && result.toDisplayString() != expected) {
      std::cout << "  FAILED! Expected: " << expected << std::endl;
      gFailedTests++;
    } else {
      std::cout << "  PASSED" << std::endl;
    }
  } catch (const std::exception& e) {
    std::cout << "  Error: " << e.what() << std::endl;
    gFailedTests++;
  }

  std::cout << std::endl;
}

int main() {
  std::cout << "=== LightJS C++20 Test Suite ===" << std::endl << std::endl;

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
    let name = "LightJS";
    greeting + name
  )", "Hello, LightJS");

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
    let resp = fetch("file://./test.txt");
    resp.status
  )", "200");

  runTest("Fetch file protocol - ok property", R"(
    let resp = fetch("file://./test.txt");
    resp.ok
  )", "true");

  runTest("Fetch file protocol - text method", R"(
    let resp = fetch("file://./test.txt");
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

  runTest("Dynamic import - undefined specifier returns Promise", R"(
    const p = import(undefined);
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

  // Top-level await tests (require module mode for spec-compliant parsing)
  runTest("Top-level await - with Promise.resolve", R"(
    const result = await Promise.resolve(42);
    result
  )", "42", true);

  runTest("Top-level await - with async expression", R"(
    const value = await Promise.resolve("hello");
    value
  )", "hello", true);

  runTest("Top-level await - multiple awaits", R"(
    const a = await Promise.resolve(10);
    const b = await Promise.resolve(20);
    a + b
  )", "30", true);

  runTest("Top-level await - with computation", R"(
    const num = await Promise.resolve(5);
    num * num
  )", "25", true);

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

  // Delete operator tests
  runTest("Delete operator - object property", R"(
    let obj = {x: 1, y: 2};
    delete obj.x;
    obj.x
  )", "undefined");

  runTest("Delete operator - returns true", R"(
    let obj = {a: 1};
    delete obj.a ? "yes" : "no"
  )", "yes");

  // In operator tests
  runTest("In operator - existing property", R"(
    let obj = {x: 10, y: 20};
    "x" in obj ? "yes" : "no"
  )", "yes");

  runTest("In operator - missing property", R"(
    let obj = {x: 10};
    "z" in obj ? "yes" : "no"
  )", "no");

  runTest("In operator - array index", R"(
    let arr = [10, 20, 30];
    1 in arr ? "yes" : "no"
  )", "yes");

  // Reflect API tests
  runTest("Reflect.has", R"(
    let obj = {name: "test"};
    Reflect.has(obj, "name") ? "yes" : "no"
  )", "yes");

  runTest("Reflect.get", R"(
    let obj = {value: 42};
    Reflect.get(obj, "value")
  )", "42");

  runTest("Reflect.set", R"(
    let obj = {};
    Reflect.set(obj, "x", 100);
    obj.x
  )", "100");

  runTest("Reflect.deleteProperty", R"(
    let obj = {a: 1, b: 2};
    Reflect.deleteProperty(obj, "a");
    obj.a
  )", "undefined");

  runTest("Reflect.ownKeys", R"(
    let obj = {x: 1, y: 2};
    let keys = Reflect.ownKeys(obj);
    keys.length
  )", "2");

  // Proxy tests
  runTest("Proxy - basic passthrough", R"(
    let target = {x: 100};
    let proxy = new Proxy(target, {});
    proxy.x
  )", "100");

  runTest("Proxy - set through proxy", R"(
    let target = {};
    let proxy = new Proxy(target, {});
    proxy.y = 50;
    target.y
  )", "50");

  runTest("Proxy get trap", R"(
    let target = {message: "hello"};
    let handler = {
      get: function(obj, prop) {
        return "intercepted:" + prop;
      }
    };
    let proxy = new Proxy(target, handler);
    proxy.message
  )", "intercepted:message");

  runTest("Proxy set trap", R"(
    let target = {};
    let handler = {
      set: function(obj, prop, value) {
        obj[prop] = value * 2;
        return true;
      }
    };
    let proxy = new Proxy(target, handler);
    proxy.x = 5;
    target.x
  )", "10");

  runTest("Proxy has trap", R"(
    let target = {a: 1};
    let handler = {
      has: function(obj, prop) {
        return prop === "secret" ? false : prop in obj;
      }
    };
    let proxy = new Proxy(target, handler);
    ("a" in proxy) + "," + ("secret" in proxy)
  )", "true,false");

  // Symbol tests
  runTest("Symbol.asyncIterator exists", R"(
    typeof Symbol.asyncIterator
  )", "symbol");

  runTest("Symbol.toStringTag exists", R"(
    typeof Symbol.toStringTag
  )", "symbol");

  // ReadableStream tests
  runTest("ReadableStream - creation", R"(
    let stream = new ReadableStream();
    stream.locked ? "locked" : "unlocked"
  )", "unlocked");

  runTest("WritableStream - creation", R"(
    let stream = new WritableStream();
    stream.locked ? "locked" : "unlocked"
  )", "unlocked");

  runTest("TransformStream - creation", R"(
    let ts = new TransformStream();
    ts.readable && ts.writable ? "has both" : "missing"
  )", "has both");

  // Getter/Setter syntax tests
  runTest("Object getter syntax", R"(
    let obj = {
      _value: 42,
      get value() { return this._value; }
    };
    obj.value
  )", "42");

  runTest("Object setter syntax", R"(
    let obj = {
      _value: 0,
      get value() { return this._value; },
      set value(v) { this._value = v * 2; }
    };
    obj.value = 21;
    obj.value
  )", "42");

  runTest("Object getter with computation", R"(
    let obj = {
      firstName: "John",
      lastName: "Doe",
      get fullName() { return this.firstName + " " + this.lastName; }
    };
    obj.fullName
  )", "John Doe");

  runTest("Object property named get", R"(
    let obj = { get: 42 };
    obj.get
  )", "42");

  runTest("Object property named set", R"(
    let obj = { set: 100 };
    obj.set
  )", "100");

  // Console methods tests
  runTest("console.error exists", R"(
    typeof console.error
  )", "function");

  runTest("console.warn exists", R"(
    typeof console.warn
  )", "function");

  runTest("console.info exists", R"(
    typeof console.info
  )", "function");

  runTest("console.debug exists", R"(
    typeof console.debug
  )", "function");

  runTest("console.time exists", R"(
    typeof console.time
  )", "function");

  runTest("console.timeEnd exists", R"(
    typeof console.timeEnd
  )", "function");

  runTest("console.assert exists", R"(
    typeof console.assert
  )", "function");

  // performance.now tests
  runTest("performance.now exists", R"(
    typeof performance.now
  )", "function");

  runTest("performance.now returns number", R"(
    typeof performance.now()
  )", "number");

  runTest("performance.now increases", R"(
    let t1 = performance.now();
    let sum = 0;
    for (let i = 0; i < 1000; i++) sum += i;
    let t2 = performance.now();
    t2 >= t1 ? "ok" : "fail"
  )", "ok");

  // structuredClone tests
  runTest("structuredClone - primitive", R"(
    let x = structuredClone(42);
    x
  )", "42");

  runTest("structuredClone - array", R"(
    let arr = [1, 2, 3];
    let clone = structuredClone(arr);
    clone.push(4);
    arr.length + "," + clone.length
  )", "3,4");

  runTest("structuredClone - object", R"(
    let obj = { a: 1, b: 2 };
    let clone = structuredClone(obj);
    clone.c = 3;
    Object.keys(obj).length + "," + Object.keys(clone).length
  )", "2,3");

  runTest("structuredClone - nested", R"(
    let obj = { arr: [1, 2], nested: { x: 10 } };
    let clone = structuredClone(obj);
    clone.nested.x = 20;
    obj.nested.x + "," + clone.nested.x
  )", "10,20");

  // Base64 encoding/decoding tests
  runTest("btoa - simple string", R"(
    btoa("Hello")
  )", "SGVsbG8=");

  runTest("btoa - hello world", R"(
    btoa("Hello, World!")
  )", "SGVsbG8sIFdvcmxkIQ==");

  runTest("atob - simple decode", R"(
    atob("SGVsbG8=")
  )", "Hello");

  runTest("atob - hello world", R"(
    atob("SGVsbG8sIFdvcmxkIQ==")
  )", "Hello, World!");

  runTest("btoa/atob roundtrip", R"(
    let original = "Test123!@#";
    let encoded = btoa(original);
    let decoded = atob(encoded);
    decoded === original ? "ok" : "fail"
  )", "ok");

  // URI encoding/decoding tests
  runTest("encodeURIComponent - space", R"(
    encodeURIComponent("hello world")
  )", "hello%20world");

  runTest("encodeURIComponent - special chars", R"(
    encodeURIComponent("a=b&c=d")
  )", "a%3Db%26c%3Dd");

  runTest("decodeURIComponent - space", R"(
    decodeURIComponent("hello%20world")
  )", "hello world");

  runTest("encodeURIComponent/decodeURIComponent roundtrip", R"(
    let original = "key=value&other=test!@#";
    let encoded = encodeURIComponent(original);
    let decoded = decodeURIComponent(encoded);
    decoded === original ? "ok" : "fail"
  )", "ok");

  runTest("encodeURI - preserves URL chars", R"(
    encodeURI("https://example.com/path?q=hello world")
  )", "https://example.com/path?q=hello%20world");

  runTest("decodeURI - decodes URL", R"(
    decodeURI("https://example.com/path?q=hello%20world")
  )", "https://example.com/path?q=hello world");

  // Global Infinity and NaN tests
  runTest("Global Infinity", R"(
    Infinity > 1e308 ? "ok" : "fail"
  )", "ok");

  runTest("Global NaN is NaN", R"(
    Number.isNaN(NaN) ? "ok" : "fail"
  )", "ok");

  runTest("Infinity arithmetic", R"(
    (1 / Infinity === 0) ? "ok" : "fail"
  )", "ok");

  // crypto.randomUUID tests
  runTest("crypto.randomUUID format", R"(
    let uuid = crypto.randomUUID();
    uuid.length === 36 && uuid.charAt(8) === '-' && uuid.charAt(13) === '-' ? "ok" : "fail"
  )", "ok");

  runTest("crypto.randomUUID uniqueness", R"(
    let uuid1 = crypto.randomUUID();
    let uuid2 = crypto.randomUUID();
    uuid1 !== uuid2 ? "ok" : "fail"
  )", "ok");

  runTest("crypto.getRandomValues exists", R"(
    typeof crypto.getRandomValues
  )", "function");

  // AbortController tests
  runTest("AbortController - creation", R"(
    let controller = new AbortController();
    controller.signal.aborted ? "aborted" : "not aborted"
  )", "not aborted");

  runTest("AbortController - abort", R"(
    let controller = new AbortController();
    controller.abort();
    controller.signal.aborted ? "aborted" : "not aborted"
  )", "aborted");

  runTest("AbortController - abort reason", R"(
    let controller = new AbortController();
    controller.abort("custom reason");
    controller.signal.reason
  )", "custom reason");

  runTest("AbortSignal.abort static method", R"(
    let signal = AbortSignal.abort();
    signal.aborted ? "aborted" : "not aborted"
  )", "aborted");

  // String bracket indexing tests
  runTest("String bracket indexing - first char", R"(
    let s = "hello";
    s[0]
  )", "h");

  runTest("String bracket indexing - middle char", R"(
    let s = "hello";
    s[2]
  )", "l");

  runTest("String bracket indexing - out of bounds", R"(
    let s = "hello";
    s[10] === undefined ? "undefined" : "defined"
  )", "undefined");

  runTest("String bracket indexing - unicode", R"(
    let s = "日本語";
    s[1]
  )", "本");

  // Object.getOwnPropertyDescriptor tests
  runTest("Object.getOwnPropertyDescriptor - basic", R"(
    let obj = { x: 42 };
    let desc = Object.getOwnPropertyDescriptor(obj, "x");
    desc.value
  )", "42");

  runTest("Object.getOwnPropertyDescriptor - writable", R"(
    let obj = { x: 42 };
    let desc = Object.getOwnPropertyDescriptor(obj, "x");
    desc.writable ? "writable" : "not writable"
  )", "writable");

  runTest("Object.defineProperty - basic", R"(
    let obj = {};
    Object.defineProperty(obj, "x", { value: 100 });
    obj.x
  )", "100");

  runTest("Object.defineProperties - multiple", R"(
    let obj = {};
    Object.defineProperties(obj, {
      a: { value: 1 },
      b: { value: 2 }
    });
    obj.a + obj.b
  )", "3");

  // String.prototype.matchAll - ES2020
  runTest("String.matchAll exists", R"(
    typeof "test".matchAll
  )", "function");

  // import.meta - ES2020
  runTest("import.meta exists", R"(
    typeof import.meta
  )", "object", true);

  runTest("import.meta.url exists", R"(
    typeof import.meta.url
  )", "string", true);

  runTest("import.meta.resolve exists", R"(
    typeof import.meta.resolve
  )", "function", true);

  // Generator methods: parameter destructuring errors occur at call time.
  runTest("Generator Param Destructure Throws On Call", R"(
    function* boom() { throw 1; }
    class C { *g([, ...x]) {} }
    try {
      new C().g(boom());
      "bad";
    } catch (e) {
      e === 1 ? "ok" : "bad";
    }
  )", "ok");

  // Array destructuring must use Array.prototype[Symbol.iterator] (including overrides).
  runTest("Array Destructure Uses Overridden Iterator", R"(
    Array.prototype[Symbol.iterator] = function* () {
      if (this.length > 0) yield this[0];
      if (this.length > 1) yield this[1];
      if (this.length > 2) yield 42;
    };
    class C {
      m([x, y, z] = [1, 2, 3]) { return z; }
    }
    new C().m()
  )", "42");

  // === Class features ===

  runTest("Class basic instantiation", R"(
    class Animal {
      constructor(name) { this.name = name; }
      speak() { return this.name + " speaks"; }
    }
    let a = new Animal("Dog");
    a.speak()
  )", "Dog speaks");

  runTest("Class inheritance", R"(
    class Base {
      constructor(x) { this.x = x; }
    }
    class Child extends Base {
      constructor(x, y) { super(x); this.y = y; }
    }
    let c = new Child(10, 20);
    c.x + c.y
  )", "30");

  runTest("Static methods", R"(
    class MathHelper {
      static add(a, b) { return a + b; }
    }
    MathHelper.add(3, 4)
  )", "7");

  runTest("Static fields", R"(
    class Config {
      static version = 42;
    }
    Config.version
  )", "42");

  runTest("Private instance fields", R"(
    class Counter {
      #count = 0;
      increment() { this.#count++; return this.#count; }
    }
    let c = new Counter();
    c.increment();
    c.increment();
    c.increment()
  )", "3");

  runTest("Private static fields", R"(
    class IdGen {
      static #nextId = 1;
      static generate() { return IdGen.#nextId++; }
    }
    let a = IdGen.generate();
    let b = IdGen.generate();
    "" + a + "," + b
  )", "1,2");

  runTest("Getter and setter", R"(
    class Temp {
      #celsius = 0;
      get fahrenheit() { return this.#celsius * 9 / 5 + 32; }
      set fahrenheit(f) { this.#celsius = (f - 32) * 5 / 9; }
    }
    let t = new Temp();
    t.fahrenheit = 212;
    t.fahrenheit
  )", "212");

  runTest("Class valueOf", R"(
    class Money {
      constructor(amount) { this.amount = amount; }
      valueOf() { return this.amount; }
    }
    let m = new Money(100);
    m + 50
  )", "150");

  runTest("instanceof operator", R"(
    class A {}
    class B extends A {}
    let b = new B();
    "" + (b instanceof B) + "," + (b instanceof A)
  )", "true,true");

  runTest("Private static methods", R"(
    class C {
      static #x(value) { return value / 2; }
      static x() { return this.#x(84); }
    }
    C.x()
  )", "42");

  // === Math trig/hyperbolic functions ===
  runTest("Math.asin", R"(Math.asin(1) === Math.PI / 2)", "true");
  runTest("Math.acos", R"(Math.acos(1) === 0)", "true");
  runTest("Math.atan", R"(Math.atan(0) === 0)", "true");
  runTest("Math.atan2", R"(Math.atan2(1, 1) === Math.PI / 4)", "true");
  runTest("Math.sinh", R"(Math.sinh(0) === 0)", "true");
  runTest("Math.cosh", R"(Math.cosh(0) === 1)", "true");
  runTest("Math.tanh", R"(Math.tanh(0) === 0)", "true");
  runTest("Math.asinh", R"(Math.asinh(0) === 0)", "true");
  runTest("Math.acosh", R"(Math.acosh(1) === 0)", "true");
  runTest("Math.atanh", R"(Math.atanh(0) === 0)", "true");

  // === Symbol.for / Symbol.keyFor ===
  runTest("Symbol.for returns same symbol", R"(
    Symbol.for("test") === Symbol.for("test")
  )", "true");
  runTest("Symbol.keyFor", R"(
    let s = Symbol.for("hello");
    Symbol.keyFor(s)
  )", "hello");
  runTest("Symbol.keyFor returns undefined for non-registered", R"(
    let s = Symbol("local");
    typeof Symbol.keyFor(s)
  )", "undefined");

  // === Well-known symbols exist ===
  runTest("Symbol.hasInstance exists", R"(typeof Symbol.hasInstance)", "symbol");
  runTest("Symbol.species exists", R"(typeof Symbol.species)", "symbol");
  runTest("Symbol.isConcatSpreadable exists", R"(typeof Symbol.isConcatSpreadable)", "symbol");
  runTest("Symbol.match exists", R"(typeof Symbol.match)", "symbol");
  runTest("Symbol.replace exists", R"(typeof Symbol.replace)", "symbol");
  runTest("Symbol.search exists", R"(typeof Symbol.search)", "symbol");
  runTest("Symbol.split exists", R"(typeof Symbol.split)", "symbol");

  // === Object.getOwnPropertyDescriptors ===
  runTest("Object.getOwnPropertyDescriptors", R"(
    let obj = { a: 1, b: 2 };
    let descs = Object.getOwnPropertyDescriptors(obj);
    descs.a.value + descs.b.value
  )", "3");

  // === String.raw ===
  runTest("String.raw basic", R"(
    String.raw({raw: ["a", "b", "c"]}, 1, 2)
  )", "a1b2c");

  std::cout << "=== All tests completed ===" << std::endl;
  std::cout << "Summary: " << (gTotalTests - gFailedTests) << "/" << gTotalTests
            << " passed, " << gFailedTests << " failed" << std::endl;

  return gFailedTests == 0 ? 0 : 1;
}
