/**
 * @file simple_app.cc
 * @brief Example application using TinyJS as a library
 *
 * This demonstrates how to embed TinyJS in your C++ application.
 * Compile with:
 *   g++ -std=c++20 simple_app.cc -ltinyjs -o simple_app
 * Or use CMake (see CMakeLists.txt in this directory)
 */

#include <tinyjs.h>
#include <iostream>
#include <string>

/**
 * Helper function to evaluate JavaScript code and return the result
 */
tinyjs::Value evaluateJS(const std::string& code) {
  // Create lexer and tokenize
  tinyjs::Lexer lexer(code);
  auto tokens = lexer.tokenize();

  // Parse tokens into AST
  tinyjs::Parser parser(tokens);
  auto program = parser.parse();

  if (!program) {
    std::cerr << "Parse error!" << std::endl;
    return tinyjs::Value(tinyjs::Undefined{});
  }

  // Create global environment with all built-ins
  auto env = tinyjs::Environment::createGlobal();

  // Create interpreter
  tinyjs::Interpreter interpreter(env);

  // Evaluate the program using C++20 coroutines
  auto task = interpreter.evaluate(*program);
  while (!task.done()) {
    std::coroutine_handle<>::from_address(task.handle.address()).resume();
  }

  return task.result();
}

int main() {
  std::cout << "TinyJS Integration Example v" << tinyjs::version() << "\n";
  std::cout << "========================================\n\n";

  // Example 1: Simple arithmetic
  {
    std::cout << "Example 1: Arithmetic\n";
    auto result = evaluateJS("40 + 2");
    std::cout << "  40 + 2 = " << result.toString() << "\n\n";
  }

  // Example 2: Variables and functions
  {
    std::cout << "Example 2: Functions\n";
    auto result = evaluateJS(R"(
      function factorial(n) {
        if (n <= 1) return 1;
        return n * factorial(n - 1);
      }
      factorial(5);
    )");
    std::cout << "  factorial(5) = " << result.toString() << "\n\n";
  }

  // Example 3: Arrays
  {
    std::cout << "Example 3: Arrays\n";
    auto result = evaluateJS(R"(
      let arr = [1, 2, 3, 4, 5];
      let sum = 0;
      for (let i = 0; i < arr.length; i++) {
        sum = sum + arr[i];
      }
      sum;
    )");
    std::cout << "  sum([1,2,3,4,5]) = " << result.toString() << "\n\n";
  }

  // Example 4: Objects
  {
    std::cout << "Example 4: Objects\n";
    auto result = evaluateJS(R"(
      let person = {
        name: "Alice",
        age: 30,
        greet: function() {
          return "Hello, I'm " + this.name;
        }
      };
      person.greet();
    )");
    std::cout << "  person.greet() = " << result.toString() << "\n\n";
  }

  // Example 5: Generators
  {
    std::cout << "Example 5: Generators\n";
    auto result = evaluateJS(R"(
      function* fibonacci() {
        let a = 0, b = 1;
        yield a;
        yield b;
        for (let i = 0; i < 5; i++) {
          let temp = a + b;
          a = b;
          b = temp;
          yield temp;
        }
      }

      let sum = 0;
      for (let num of fibonacci()) {
        sum = sum + num;
      }
      sum;
    )");
    std::cout << "  sum(fibonacci()) = " << result.toString() << "\n\n";
  }

  // Example 6: Error handling with persistent environment
  {
    std::cout << "Example 6: Persistent Environment\n";

    auto env = tinyjs::Environment::createGlobal();
    tinyjs::Interpreter interpreter(env);

    // Define variable in first call
    {
      tinyjs::Lexer lexer("let counter = 0;");
      auto tokens = lexer.tokenize();
      tinyjs::Parser parser(tokens);
      auto program = parser.parse();

      auto task = interpreter.evaluate(*program);
      while (!task.done()) {
        std::coroutine_handle<>::from_address(task.handle.address()).resume();
      }
    }

    // Use variable in second call
    {
      tinyjs::Lexer lexer("counter = counter + 10; counter;");
      auto tokens = lexer.tokenize();
      tinyjs::Parser parser(tokens);
      auto program = parser.parse();

      auto task = interpreter.evaluate(*program);
      while (!task.done()) {
        std::coroutine_handle<>::from_address(task.handle.address()).resume();
      }

      auto result = task.result();
      std::cout << "  counter = " << result.toString() << "\n\n";
    }
  }

  std::cout << "All examples completed successfully!\n";
  return 0;
}
