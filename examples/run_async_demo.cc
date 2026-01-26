#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "environment.h"
#include <iostream>
#include <fstream>
#include <sstream>

using namespace lightjs;

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cout << "Usage: " << argv[0] << " <script.js>" << std::endl;
    return 1;
  }

  std::ifstream file(argv[1]);
  if (!file.is_open()) {
    std::cout << "Could not open file: " << argv[1] << std::endl;
    return 1;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string code = buffer.str();

  try {
    Lexer lexer(code);
    auto tokens = lexer.tokenize();

    Parser parser(tokens);
    auto program = parser.parse();

    if (!program) {
      std::cout << "Parse error!" << std::endl;
      return 1;
    }

    auto env = Environment::createGlobal();
    Interpreter interpreter(env);

    auto task = interpreter.evaluate(*program);

    while (!task.done()) {
      std::coroutine_handle<>::from_address(task.handle.address()).resume();
    }

    Value result = task.result();

  } catch (const std::exception& e) {
    std::cout << "Error: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}