#include "../include/lightjs.h"
#include <iostream>

using namespace lightjs;

int main() {
  // Test with function call
  const char* script = R"(
    function test() {
      return undefinedVariable;
    }
    test();
  )";

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

  auto result = task.result();
  std::cout << "Result: " << result.toString() << "\n";
  std::cout << "Has error: " << interpreter.hasError() << "\n";

  if (interpreter.hasError()) {
    auto errorValue = interpreter.getError();

    if (auto errorPtr = std::get_if<std::shared_ptr<Error>>(&errorValue.data)) {
      auto error = *errorPtr;
      std::cout << "Error Type: " << error->getName() << "\n";
      std::cout << "Error Message: " << error->message << "\n\n";
      std::cout << "Stack Trace:\n" << error->stack << "\n";
      return 0;
    }
  }

  std::cerr << "No error detected!\n";
  return 1;
}
