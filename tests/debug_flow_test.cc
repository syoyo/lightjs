#include "../include/lightjs.h"
#include <iostream>

using namespace lightjs;

int main() {
  // Simplest possible test
  const char* script = R"(
    function test() {
      undefinedVar;
    }
    test();
  )";

  Lexer lexer(script);
  auto tokens = lexer.tokenize();

  std::cout << "Tokens: " << tokens.size() << "\n";

  Parser parser(tokens);
  auto program = parser.parse();

  if (!program) {
    std::cerr << "Parse error\n";
    return 1;
  }

  std::cout << "Program statements: " << program->body.size() << "\n";

  auto env = Environment::createGlobal();
  Interpreter interpreter(env);

  std::cout << "Starting evaluation...\n";

  auto task = interpreter.evaluate(*program);

  std::cout << "Task created, entering loop...\n";

  int iterations = 0;
  while (!task.done()) {
    std::coroutine_handle<>::from_address(task.handle.address()).resume();
    iterations++;
    if (iterations > 1000) {
      std::cerr << "Too many iterations!\n";
      break;
    }
  }

  std::cout << "Loop done after " << iterations << " iterations\n";
  std::cout << "Has error: " << interpreter.hasError() << "\n";

  if (interpreter.hasError()) {
    std::cout << "SUCCESS: Error detected!\n";
    auto err = interpreter.getError();
    if (auto* errPtr = std::get_if<std::shared_ptr<Error>>(&err.data)) {
      std::cout << "Error: " << (*errPtr)->getName() << ": " << (*errPtr)->message << "\n";
    }
    return 0;
  } else {
    std::cout << "FAILURE: No error detected\n";
    return 1;
  }
}
