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

  std::cout << "Task created, running...\n";

  LIGHTJS_RUN_TASK_VOID(task);

  std::cout << "Task done\n";
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
