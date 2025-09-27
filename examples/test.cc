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

  std::cout << "=== All tests completed ===" << std::endl;

  return 0;
}