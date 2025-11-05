#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "environment.h"
#include <iostream>
#include <cassert>

using namespace tinyjs;

void runTest(const std::string& testName, const std::string& code, const std::string& expected) {
  std::cout << "Test: " << testName << std::endl;

  Lexer lexer(code);
  auto tokens = lexer.tokenize();

  Parser parser(tokens);
  auto program = parser.parse();

  if (!program) {
    std::cout << "  FAILED - Parse error" << std::endl;
    return;
  }

  auto env = Environment::createGlobal();
  Interpreter interpreter(env);

  auto task = interpreter.evaluate(*program);
  while (!task.done()) {
    std::coroutine_handle<>::from_address(task.handle.address()).resume();
  }

  Value result = task.result();
  std::string resultStr = result.toString();

  std::cout << "  Result: " << resultStr << std::endl;

  if (resultStr == expected) {
    std::cout << "  PASSED\n" << std::endl;
  } else {
    std::cout << "  FAILED - Expected: " << expected << "\n" << std::endl;
  }
}

int main() {
  std::cout << "=== Generator for...of Loop Tests ===\n" << std::endl;

  // Test 1: Simple for...of with generator
  runTest("for...of with simple generator", R"(
    function* gen() {
      yield 1;
      yield 2;
      yield 3;
    }
    let sum = 0;
    for (let x of gen()) {
      sum = sum + x;
    }
    sum;
  )", "6");

  // Test 2: for...of counting iterations
  runTest("for...of counting iterations", R"(
    function* gen() {
      yield 10;
      yield 20;
      yield 30;
    }
    let count = 0;
    for (let val of gen()) {
      count = count + 1;
    }
    count;
  )", "3");

  // Test 3: for...of with break
  runTest("for...of with break", R"(
    function* gen() {
      yield 1;
      yield 2;
      yield 3;
      yield 4;
      yield 5;
    }
    let sum = 0;
    for (let x of gen()) {
      if (x > 3) {
        break;
      }
      sum = sum + x;
    }
    sum;
  )", "6");

  // Test 4: for...of with continue
  runTest("for...of with continue", R"(
    function* gen() {
      yield 1;
      yield 2;
      yield 3;
      yield 4;
    }
    let sum = 0;
    for (let x of gen()) {
      if (x == 2) {
        continue;
      }
      sum = sum + x;
    }
    sum;
  )", "8");

  // Test 5: for...of with expression yields
  runTest("for...of with expression yields", R"(
    function* gen() {
      yield 5 + 5;
      yield 10 * 2;
      yield 15 - 5;
    }
    let product = 1;
    for (let x of gen()) {
      product = product * x;
    }
    product;
  )", "2000");

  // Test 6: for...of with empty generator
  runTest("for...of with empty generator", R"(
    function* gen() {
      return 42;
    }
    let count = 0;
    for (let x of gen()) {
      count = count + 1;
    }
    count;
  )", "0");

  // Test 7: for...of accumulating strings
  runTest("for...of accumulating strings", R"(
    function* gen() {
      yield "Hello";
      yield " ";
      yield "World";
    }
    let str = "";
    for (let s of gen()) {
      str = str + s;
    }
    str;
  )", "Hello World");

  // Test 8: Nested for...of with generators
  runTest("Nested for...of loops", R"(
    function* gen1() {
      yield 1;
      yield 2;
    }
    function* gen2() {
      yield 10;
      yield 20;
    }
    let sum = 0;
    for (let x of gen1()) {
      for (let y of gen2()) {
        sum = sum + x + y;
      }
    }
    sum;
  )", "66");

  std::cout << "=== All for...of Generator Tests Completed ===" << std::endl;
  return 0;
}
