#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include <iostream>
#include <sstream>

using namespace lightjs;

void testError(const std::string& name, const std::string& code) {
  std::cout << "Test: " << name << std::endl;
  std::cout << "  Code:" << std::endl;
  // Print code with line numbers
  std::istringstream iss(code);
  std::string line;
  int lineNum = 1;
  while (std::getline(iss, line)) {
    std::cout << "    " << lineNum++ << ": " << line << std::endl;
  }

  Lexer lexer(code);
  auto tokens = lexer.tokenize();

  Parser parser(tokens);
  auto program = parser.parse();

  if (!program) {
    std::cout << "  Parse error!" << std::endl;
    return;
  }

  auto env = Environment::createGlobal();
  Interpreter interp(env);

  auto task = interp.evaluate(*program);
  while (!task.done()) {
    std::coroutine_handle<>::from_address(task.handle.address()).resume();
  }
  auto result = task.result();

  // Check if interpreter has a thrown error
  if (interp.hasError()) {
    Value err = interp.getError();
    if (auto* errPtr = std::get_if<std::shared_ptr<Error>>(&err.data)) {
      std::cout << "  Error: " << (*errPtr)->message << std::endl;
    } else {
      std::cout << "  Thrown: " << err.toString() << std::endl;
    }
  } else {
    std::cout << "  Result: " << result.toString() << std::endl;
  }
  std::cout << std::endl;
}

int main() {
  std::cout << "=== Error Message Line Number Tests ===" << std::endl << std::endl;

  testError("Undefined variable on line 3",
    "let x = 1;\n"
    "let y = 2;\n"
    "undefinedVar"
  );

  testError("Undefined variable on line 1",
    "unknownFunc()"
  );

  testError("Undefined nested on line 5",
    "let a = 1;\n"
    "let b = 2;\n"
    "let c = 3;\n"
    "let d = 4;\n"
    "let result = missingVar + 10"
  );

  return 0;
}
