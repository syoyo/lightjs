#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "environment.h"
#include <iostream>
#include <cassert>

using namespace lightjs;

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
  std::cout << "=== Simple Generator Tests ===" << std::endl << std::endl;

  // Test 1: Generator function creates object
  runTest("Generator function returns object", R"(
    function* gen() {
      return 42;
    }
    let g = gen();
    typeof g;
  )", "object");

  // Test 2: Simple generator with return
  runTest("Generator with return value", R"(
    function* gen() {
      return 100;
    }
    let g = gen();
    let result = g.next();
    result.value;
  )", "100");

  // Test 3: Generator done status
  runTest("Generator done status", R"(
    function* gen() {
      return 42;
    }
    let g = gen();
    let result = g.next();
    result.done;
  )", "true");

  // Test 4: Generator with yield (simple)
  runTest("Simple yield", R"(
    function* gen() {
      yield 1;
      return 2;
    }
    let g = gen();
    let result = g.next();
    result.value;
  )", "2");

  // Test 5: Generator without return
  runTest("Generator without explicit return", R"(
    function* gen() {
      let x = 10;
    }
    let g = gen();
    let result = g.next();
    result.done;
  )", "true");

  std::cout << "=== All Simple Generator Tests Completed ===" << std::endl;
  return 0;
}
