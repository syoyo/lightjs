#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "environment.h"
#include <iostream>
#include <cassert>

using namespace lightjs;

void runGeneratorTest(const std::string& testName, const std::string& code, const std::string& expected) {
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
    Value result;
  LIGHTJS_RUN_TASK(task, result);
  std::string resultStr = result.toString();

  std::cout << "  Result: " << resultStr << std::endl;

  if (resultStr == expected) {
    std::cout << "  PASSED\n" << std::endl;
  } else {
    std::cout << "  FAILED - Expected: " << expected << "\n" << std::endl;
  }
}

int main() {
  std::cout << "=== Generator Tests ===" << std::endl << std::endl;

  // Test 1: Basic generator function creation
  runGeneratorTest("Generator function creates Generator object", R"(
    function* gen() {
      return 42;
    }
    let g = gen();
    typeof g;
  )", "object");

  // Test 2: Generator has next method
  runGeneratorTest("Generator has next method", R"(
    function* gen() {
      return 42;
    }
    let g = gen();
    typeof g.next;
  )", "function");

  // Test 3: Generator.next() returns object
  runGeneratorTest("Generator.next() returns object", R"(
    function* gen() {
      return 42;
    }
    let g = gen();
    let result = g.next();
    typeof result;
  )", "object");

  // Test 4: Generator.return() method exists
  runGeneratorTest("Generator has return method", R"(
    function* gen() {
      return 42;
    }
    let g = gen();
    typeof g.return;
  )", "function");

  // Test 5: Generator.throw() method exists
  runGeneratorTest("Generator has throw method", R"(
    function* gen() {
      return 42;
    }
    let g = gen();
    typeof g.throw;
  )", "function");

  // Test 6: Generator function expression
  runGeneratorTest("Generator function expression", R"(
    let gen = function*() {
      return 100;
    };
    let g = gen();
    typeof g;
  )", "object");

  // Test 7: Generator with parameters
  runGeneratorTest("Generator with parameters", R"(
    function* gen(x) {
      return x * 2;
    }
    let g = gen(21);
    typeof g;
  )", "object");

  std::cout << "=== Generator Tests Completed ===" << std::endl;
  return 0;
}
