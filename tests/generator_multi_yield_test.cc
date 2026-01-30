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
  std::cout << "=== Multi-Yield Generator Tests ===" << std::endl << std::endl;

  // Test 1: Generator with multiple yields - first value
  runTest("Multiple yields - first next()", R"(
    function* gen() {
      yield 1;
      yield 2;
      yield 3;
      return 4;
    }
    let g = gen();
    let result = g.next();
    result.value;
  )", "1");

  // Test 2: Multiple yields - second value
  runTest("Multiple yields - second next()", R"(
    function* gen() {
      yield 10;
      yield 20;
      yield 30;
    }
    let g = gen();
    g.next();
    let result = g.next();
    result.value;
  )", "20");

  // Test 3: Multiple yields - third value
  runTest("Multiple yields - third next()", R"(
    function* gen() {
      yield 100;
      yield 200;
      yield 300;
    }
    let g = gen();
    g.next();
    g.next();
    let result = g.next();
    result.value;
  )", "300");

  // Test 4: Multiple yields - done after last yield
  runTest("Done status after exhausting yields", R"(
    function* gen() {
      yield 1;
      yield 2;
    }
    let g = gen();
    g.next();
    g.next();
    let result = g.next();
    result.done;
  )", "true");

  // Test 5: Generator with expression yields
  runTest("Yield with expressions", R"(
    function* gen() {
      yield 5 + 5;
      yield 10 * 2;
      return 15 + 15;
    }
    let g = gen();
    g.next();
    let result = g.next();
    result.value;
  )", "20");

  // Test 6: Yield in loop (simple case)
  runTest("Yield in simple loop", R"(
    function* gen() {
      let i = 0;
      yield i;
      i = i + 1;
      yield i;
      i = i + 1;
      yield i;
    }
    let g = gen();
    g.next();
    g.next();
    let result = g.next();
    result.value;
  )", "2");

  // Test 7: Check done=false for active generator
  runTest("Done=false while yielding", R"(
    function* gen() {
      yield 1;
      yield 2;
    }
    let g = gen();
    let result = g.next();
    result.done;
  )", "false");

  // Test 8: Generator with return after yields
  runTest("Return after yields", R"(
    function* gen() {
      yield 1;
      yield 2;
      return 99;
    }
    let g = gen();
    g.next();
    g.next();
    let result = g.next();
    result.value;
  )", "99");

  std::cout << "=== All Multi-Yield Tests Completed ===" << std::endl;
  return 0;
}
