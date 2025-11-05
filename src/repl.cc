#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "environment.h"
#include <iostream>
#include <string>
#include <sstream>
#include <coroutine>

using namespace tinyjs;

// Check if input looks incomplete (unbalanced braces/parens)
bool isInputComplete(const std::string& input) {
  int braceCount = 0;
  int parenCount = 0;
  int bracketCount = 0;
  bool inString = false;
  bool inSingleQuote = false;
  char prevChar = '\0';

  for (char ch : input) {
    // Track string state to ignore braces inside strings
    if (ch == '"' && prevChar != '\\' && !inSingleQuote) {
      inString = !inString;
    } else if (ch == '\'' && prevChar != '\\' && !inString) {
      inSingleQuote = !inSingleQuote;
    }

    if (!inString && !inSingleQuote) {
      if (ch == '{') braceCount++;
      else if (ch == '}') braceCount--;
      else if (ch == '(') parenCount++;
      else if (ch == ')') parenCount--;
      else if (ch == '[') bracketCount++;
      else if (ch == ']') bracketCount--;
    }

    prevChar = ch;
  }

  return braceCount == 0 && parenCount == 0 && bracketCount == 0;
}

void printHelp() {
  std::cout << "\nTinyJS REPL - Interactive JavaScript Shell\n";
  std::cout << "==========================================\n";
  std::cout << "Commands:\n";
  std::cout << "  .help    - Show this help message\n";
  std::cout << "  .exit    - Exit the REPL\n";
  std::cout << "  .quit    - Exit the REPL\n";
  std::cout << "\nFeatures:\n";
  std::cout << "  - Multi-line input (continue on next line if braces/parens unbalanced)\n";
  std::cout << "  - Expression results auto-printed\n";
  std::cout << "  - Persistent environment across evaluations\n";
  std::cout << "  - Full ES2020 feature support\n";
  std::cout << "\nExamples:\n";
  std::cout << "  > let x = 42\n";
  std::cout << "  > x + 8\n";
  std::cout << "  50\n";
  std::cout << "  > function factorial(n) {\n";
  std::cout << "  ...   return n <= 1 ? 1 : n * factorial(n - 1);\n";
  std::cout << "  ... }\n";
  std::cout << "  > factorial(5)\n";
  std::cout << "  120\n\n";
}

int main() {
  // Create persistent global environment
  auto env = Environment::createGlobal();
  Interpreter interpreter(env);

  std::cout << "TinyJS REPL v1.0.0\n";
  std::cout << "Type '.help' for help, '.exit' to quit\n\n";

  std::string input;
  std::string accumulatedInput;
  bool continuingInput = false;

  while (true) {
    // Display prompt
    if (continuingInput) {
      std::cout << "... ";
    } else {
      std::cout << "> ";
    }
    std::cout.flush();

    // Read line
    if (!std::getline(std::cin, input)) {
      // EOF (Ctrl+D)
      std::cout << "\nGoodbye!\n";
      break;
    }

    // Trim whitespace
    size_t start = input.find_first_not_of(" \t\n\r");
    size_t end = input.find_last_not_of(" \t\n\r");
    if (start != std::string::npos && end != std::string::npos) {
      input = input.substr(start, end - start + 1);
    } else {
      input = "";
    }

    // Handle empty input
    if (input.empty() && !continuingInput) {
      continue;
    }

    // Accumulate input for multi-line
    if (!accumulatedInput.empty()) {
      accumulatedInput += "\n" + input;
    } else {
      accumulatedInput = input;
    }

    // Check for special commands (only if not continuing)
    if (!continuingInput) {
      if (input == ".help") {
        printHelp();
        accumulatedInput.clear();
        continue;
      }
      if (input == ".exit" || input == ".quit") {
        std::cout << "Goodbye!\n";
        break;
      }
    }

    // Check if input is complete
    if (!isInputComplete(accumulatedInput)) {
      continuingInput = true;
      continue;
    }

    // Reset continuation state
    continuingInput = false;

    // Evaluate the input
    try {
      Lexer lexer(accumulatedInput);
      auto tokens = lexer.tokenize();

      Parser parser(tokens);
      auto program = parser.parse();

      if (!program) {
        std::cout << "Parse error: Invalid syntax\n";
        accumulatedInput.clear();
        continue;
      }

      // Execute the program
      auto task = interpreter.evaluate(*program);
      while (!task.done()) {
        std::coroutine_handle<>::from_address(task.handle.address()).resume();
      }

      Value result = task.result();

      // Auto-print result if it's not undefined (expression result)
      if (!result.isUndefined()) {
        std::cout << result.toString() << "\n";
      }

    } catch (const std::exception& e) {
      std::cout << "Error: " << e.what() << "\n";
    } catch (...) {
      std::cout << "Unknown error occurred\n";
    }

    accumulatedInput.clear();
  }

  return 0;
}
