#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "environment.h"
#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <vector>
#include <algorithm>
#include <cstdlib>

using namespace lightjs;

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

// History management
class History {
public:
  History(const std::string& filename) : filename_(filename) {
    loadHistory();
  }

  ~History() {
    saveHistory();
  }

  void add(const std::string& entry) {
    if (!entry.empty() && (entries_.empty() || entries_.back() != entry)) {
      entries_.push_back(entry);
    }
  }

  const std::vector<std::string>& getEntries() const {
    return entries_;
  }

  void clear() {
    entries_.clear();
  }

private:
  std::string filename_;
  std::vector<std::string> entries_;

  void loadHistory() {
    std::ifstream file(filename_);
    if (!file) return;

    std::string line;
    while (std::getline(file, line)) {
      if (!line.empty()) {
        entries_.push_back(line);
      }
    }
  }

  void saveHistory() {
    std::ofstream file(filename_);
    if (!file) return;

    // Save last 1000 entries
    size_t start = entries_.size() > 1000 ? entries_.size() - 1000 : 0;
    for (size_t i = start; i < entries_.size(); i++) {
      file << entries_[i] << "\n";
    }
  }
};

std::string getHistoryPath() {
  const char* home = std::getenv("HOME");
  if (!home) {
    home = std::getenv("USERPROFILE"); // Windows
  }
  if (home) {
    return std::string(home) + "/.lightjs_history";
  }
  return ".lightjs_history";
}

void printHelp() {
  std::cout << "\nLightJS REPL - Interactive JavaScript Shell\n";
  std::cout << "============================================\n";
  std::cout << "\nCommands:\n";
  std::cout << "  .help           - Show this help message\n";
  std::cout << "  .exit, .quit    - Exit the REPL\n";
  std::cout << "  .clear          - Clear the environment\n";
  std::cout << "  .load <file>    - Load and execute a JavaScript file\n";
  std::cout << "  .save <file>    - Save session history to a file\n";
  std::cout << "  .history        - Show command history\n";
  std::cout << "  .version        - Show version information\n";
  std::cout << "\nFeatures:\n";
  std::cout << "  - Multi-line input (auto-continue if braces/parens unbalanced)\n";
  std::cout << "  - Expression results auto-printed\n";
  std::cout << "  - Persistent environment across evaluations\n";
  std::cout << "  - Command history saved to ~/.lightjs_history\n";
  std::cout << "  - Full ES2020 support (async/await, generators, classes, etc.)\n";
  std::cout << "\nGlobal APIs:\n";
  std::cout << "  - Console: console.log()\n";
  std::cout << "  - Timers: setTimeout, setInterval, queueMicrotask\n";
  std::cout << "  - Web: TextEncoder, TextDecoder, URL, URLSearchParams\n";
  std::cout << "  - File System: fs.readFileSync, fs.writeFileSync, etc.\n";
  std::cout << "  - Crypto: crypto.sha256, crypto.hmac\n";
  std::cout << "  - WebAssembly: WebAssembly.instantiate, compile, validate\n";
  std::cout << "\nExamples:\n";
  std::cout << "  > let x = 42\n";
  std::cout << "  > x + 8\n";
  std::cout << "  50\n\n";
  std::cout << "  > function* fib() {\n";
  std::cout << "  ...   let [a, b] = [0, 1];\n";
  std::cout << "  ...   while (true) { yield a; [a, b] = [b, a + b]; }\n";
  std::cout << "  ... }\n";
  std::cout << "  > const gen = fib()\n";
  std::cout << "  > gen.next().value\n";
  std::cout << "  0\n\n";
}

void printVersion() {
  std::cout << "\nLightJS REPL v1.0.0\n";
  std::cout << "JavaScript Engine: ES2020\n";
#if LIGHTJS_HAS_COROUTINES
  std::cout << "Build: C++20 coroutine-based interpreter\n";
#else
  std::cout << "Build: C++17 compatibility mode\n";
#endif
  std::cout << "Features: Async/await, Generators, WebAssembly, TLS 1.3\n\n";
}

int main(int argc, char* argv[]) {
  // Create persistent global environment
  auto env = Environment::createGlobal();
  Interpreter interpreter(env);

  // Initialize history
  History history(getHistoryPath());

  std::cout << "LightJS REPL v1.0.0\n";
  std::cout << "Type '.help' for help, '.exit' to quit\n\n";

  // Load file if provided as argument
  if (argc > 1) {
    std::string filename = argv[1];
    std::ifstream file(filename);
    if (!file) {
      std::cerr << "Error: Cannot open file '" << filename << "'\n";
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
        std::cerr << "Parse error in file '" << filename << "'\n";
        return 1;
      }

      auto task = interpreter.evaluate(*program);
      LIGHTJS_RUN_TASK_VOID(task);
      // File mode: exit after execution
      return 0;
    } catch (const std::exception& e) {
      std::cerr << "Error in file '" << filename << "': " << e.what() << "\n";
      return 1;
    }
  }

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
      if (input == ".version") {
        printVersion();
        accumulatedInput.clear();
        continue;
      }
      if (input == ".clear") {
        env = Environment::createGlobal();
        interpreter = Interpreter(env);
        std::cout << "Environment cleared\n";
        accumulatedInput.clear();
        continue;
      }
      if (input == ".history") {
        const auto& entries = history.getEntries();
        std::cout << "\nCommand history (" << entries.size() << " entries):\n";
        size_t histStart = entries.size() > 20 ? entries.size() - 20 : 0;
        for (size_t i = histStart; i < entries.size(); i++) {
          std::cout << "  " << (i + 1) << ": " << entries[i] << "\n";
        }
        std::cout << "\n";
        accumulatedInput.clear();
        continue;
      }
      if (input.rfind(".load ", 0) == 0) {
        std::string filename = input.substr(6);
        // Trim quotes if present
        if (!filename.empty() && (filename.front() == '"' || filename.front() == '\'')) {
          filename = filename.substr(1, filename.length() - 2);
        }

        std::ifstream file(filename);
        if (!file) {
          std::cout << "Error: Cannot open file '" << filename << "'\n";
          accumulatedInput.clear();
          continue;
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
            std::cout << "Parse error in file '" << filename << "'\n";
            accumulatedInput.clear();
            continue;
          }

          auto task = interpreter.evaluate(*program);
          LIGHTJS_RUN_TASK_VOID(task);
          std::cout << "Loaded and executed '" << filename << "'\n";
        } catch (const std::exception& e) {
          std::cout << "Error loading file: " << e.what() << "\n";
        }

        accumulatedInput.clear();
        continue;
      }
      if (input.rfind(".save ", 0) == 0) {
        std::string filename = input.substr(6);
        // Trim quotes if present
        if (!filename.empty() && (filename.front() == '"' || filename.front() == '\'')) {
          filename = filename.substr(1, filename.length() - 2);
        }

        std::ofstream file(filename);
        if (!file) {
          std::cout << "Error: Cannot write to file '" << filename << "'\n";
          accumulatedInput.clear();
          continue;
        }

        const auto& entries = history.getEntries();
        for (const auto& entry : entries) {
          file << entry << "\n";
        }
        std::cout << "Saved " << entries.size() << " entries to '" << filename << "'\n";
        accumulatedInput.clear();
        continue;
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
      Value result;
      LIGHTJS_RUN_TASK(task, result);

      // Auto-print result if it's not undefined (expression result)
      if (!result.isUndefined()) {
        std::cout << result.toString() << "\n";
      }

      // Add to history (only completed commands, not multi-line parts)
      if (!continuingInput) {
        history.add(accumulatedInput);
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
