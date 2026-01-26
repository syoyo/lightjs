#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "environment.h"
#include <iostream>
#include <string>

using namespace lightjs;

void runAsyncTest(const std::string& name, const std::string& code) {
  std::cout << "=== " << name << " ===" << std::endl;

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

  } catch (const std::exception& e) {
    std::cout << "  Error: " << e.what() << std::endl;
  }

  std::cout << std::endl;
}

int main() {
  std::cout << "=== LightJS Async/Await Test Suite ===" << std::endl << std::endl;

  runAsyncTest("Basic async function", R"(
    async function getValue() {
      return 42;
    }
    getValue()
  )");

  runAsyncTest("Await expression", R"(
    async function test() {
      let value = await 100;
      return value + 1;
    }
    test()
  )");

  runAsyncTest("Promise.resolve", R"(
    Promise.resolve(123)
  )");

  runAsyncTest("Await Promise.resolve", R"(
    async function test() {
      let value = await Promise.resolve(100);
      return value * 2;
    }
    test()
  )");

  runAsyncTest("Multiple awaits", R"(
    async function sum() {
      let a = await 10;
      let b = await 20;
      let c = await 30;
      return a + b + c;
    }
    sum()
  )");

  runAsyncTest("Nested async calls", R"(
    async function innerAsync() {
      return 5;
    }

    async function outerAsync() {
      let val = await innerAsync();
      return val * 10;
    }

    outerAsync()
  )");

  runAsyncTest("Promise.all", R"(
    let p1 = Promise.resolve(1);
    let p2 = Promise.resolve(2);
    let p3 = Promise.resolve(3);
    Promise.all([p1, p2, p3])
  )");

  std::cout << "=== All async tests completed ===" << std::endl;

  return 0;
}