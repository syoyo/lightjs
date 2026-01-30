#include "../include/lightjs.h"
#include <iostream>

using namespace lightjs;

int main() {
  // Simple test: access undefined variable
  const char* script = "undefinedVariable;";

  Lexer lexer(script);
  auto tokens = lexer.tokenize();

  Parser parser(tokens);
  auto program = parser.parse();

  if (!program) {
    std::cerr << "Parse error\n";
    return 1;
  }

  auto env = Environment::createGlobal();
  Interpreter interpreter(env);

  auto task = interpreter.evaluate(*program);
  LIGHTJS_RUN_TASK_VOID(task);

  std::cout << "hasError: " << interpreter.hasError() << "\n";

  if (interpreter.hasError()) {
    auto errorValue = interpreter.getError();
    std::cout << "Got error!\n";

    if (auto errorPtr = std::get_if<std::shared_ptr<Error>>(&errorValue.data)) {
      auto error = *errorPtr;
      std::cout << "Error Type: " << error->getName() << "\n";
      std::cout << "Error Message: " << error->message << "\n";
      std::cout << "Stack Trace:\n" << error->stack << "\n";
    }
    return 0;
  } else {
    std::cerr << "No error detected!\n";
    return 1;
  }
}
