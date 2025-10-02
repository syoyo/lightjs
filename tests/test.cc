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

  std::cout << "=== All tests completed ===" << std::endl;

  return 0;
}