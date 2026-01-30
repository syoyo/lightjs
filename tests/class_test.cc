#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include <iostream>
#include <sstream>

using namespace lightjs;

bool runTest(const std::string& name, const std::string& code, const std::string& expected) {
  std::cout << "Test: " << name << std::endl;

  Lexer lexer(code);
  auto tokens = lexer.tokenize();

  Parser parser(tokens);
  auto program = parser.parse();

  if (!program) {
    std::cout << "  Parse error!" << std::endl;
    return false;
  }

  auto env = Environment::createGlobal();
  Interpreter interp(env);

  auto task = interp.evaluate(*program);
  LIGHTJS_RUN_TASK_VOID(task);

  // Check for errors
  if (interp.hasError()) {
    Value err = interp.getError();
    if (auto* errPtr = std::get_if<std::shared_ptr<Error>>(&err.data)) {
      std::cout << "  Error: " << (*errPtr)->message << std::endl;
    } else {
      std::cout << "  Thrown: " << err.toString() << std::endl;
    }
    return false;
  }

  auto result = task.result();
  std::string resultStr = result.toString();

  if (resultStr == expected) {
    std::cout << "  Result: " << resultStr << std::endl;
    std::cout << "  PASSED" << std::endl << std::endl;
    return true;
  } else {
    std::cout << "  Expected: " << expected << std::endl;
    std::cout << "  Got: " << resultStr << std::endl;
    std::cout << "  FAILED" << std::endl << std::endl;
    return false;
  }
}

int main() {
  int passed = 0, failed = 0;

  std::cout << "=== Class and Constructor Tests ===" << std::endl << std::endl;

  // Test 1: Basic class with constructor
  if (runTest("Basic class with constructor", R"(
    class Person {
      constructor(name) {
        this.name = name;
      }
    }
    let p = new Person("Alice");
    p.name
  )", "Alice")) passed++; else failed++;

  // Test 2: Class with method
  if (runTest("Class with method", R"(
    class Counter {
      constructor(start) {
        this.count = start;
      }
      increment() {
        this.count = this.count + 1;
        return this.count;
      }
    }
    let c = new Counter(5);
    c.increment()
  )", "6")) passed++; else failed++;

  // Test 3: Multiple instances
  if (runTest("Multiple instances", R"(
    class Box {
      constructor(value) {
        this.value = value;
      }
    }
    let a = new Box(10);
    let b = new Box(20);
    a.value + b.value
  )", "30")) passed++; else failed++;

  // Test 4: Constructor function (old style)
  if (runTest("Constructor function", R"(
    function Animal(type) {
      this.type = type;
    }
    let dog = new Animal("dog");
    dog.type
  )", "dog")) passed++; else failed++;

  // Test 5: this in method - simplified
  if (runTest("this in method - single call", R"(
    class Calculator {
      constructor() {
        this.result = 0;
      }
      add(x) {
        this.result = this.result + x;
        return this.result;
      }
    }
    let calc = new Calculator();
    calc.add(5)
  )", "5")) passed++; else failed++;

  // Test 6: Class with multiple methods
  if (runTest("Class with multiple methods", R"(
    class Point {
      constructor(x, y) {
        this.x = x;
        this.y = y;
      }
      getX() {
        return this.x;
      }
      getY() {
        return this.y;
      }
      sum() {
        return this.x + this.y;
      }
    }
    let p = new Point(3, 4);
    p.sum()
  )", "7")) passed++; else failed++;

  std::cout << "=== Results ===" << std::endl;
  std::cout << "Passed: " << passed << std::endl;
  std::cout << "Failed: " << failed << std::endl;

  return failed > 0 ? 1 : 0;
}
