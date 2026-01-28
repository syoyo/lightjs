#include "../include/lightjs.h"
#include <iostream>
#include <cassert>

using namespace lightjs;

void testStackTrace() {
  std::cout << "\n=== Error Stack Trace Test ===\n";

  // Test script with nested function calls that triggers an error
  const char* script = R"(
    function outer() {
      return middle();
    }

    function middle() {
      return inner();
    }

    function inner() {
      return undefinedVariable;  // This will throw ReferenceError
    }

    outer();
  )";

  Lexer lexer(script);
  auto tokens = lexer.tokenize();

  Parser parser(tokens);
  auto program = parser.parse();

  if (!program) {
    std::cerr << "Parse error\n";
    return;
  }

  auto env = Environment::createGlobal();
  Interpreter interpreter(env);

  auto task = interpreter.evaluate(*program);
  while (!task.done()) {
    std::coroutine_handle<>::from_address(task.handle.address()).resume();
  }

  // Check if error was thrown
  if (interpreter.hasError()) {
    auto errorValue = interpreter.getError();

    if (auto errorPtr = std::get_if<std::shared_ptr<Error>>(&errorValue.data)) {
      auto error = *errorPtr;

      std::cout << "Error Type: " << error->getName() << "\n";
      std::cout << "Error Message: " << error->message << "\n\n";
      std::cout << "Stack Trace:\n" << error->stack << "\n";

      // Verify stack trace contains expected information
      bool hasReferenceError = error->stack.find("ReferenceError") != std::string::npos;
      bool hasUndefinedVariable = error->stack.find("undefinedVariable") != std::string::npos ||
                                   error->stack.find("is not defined") != std::string::npos;

      std::cout << "\nVerification:\n";
      std::cout << "  Has ReferenceError: " << (hasReferenceError ? "YES" : "NO") << "\n";
      std::cout << "  Mentions undefined variable: " << (hasUndefinedVariable ? "YES" : "NO") << "\n";

      assert(hasReferenceError && "Stack trace should contain ReferenceError");
      assert(hasUndefinedVariable && "Stack trace should mention the undefined variable");

      std::cout << "\n✅ Stack trace is working correctly!\n";
    } else {
      std::cerr << "Error value is not an Error object\n";
      assert(false);
    }
  } else {
    std::cerr << "Expected error but none was thrown\n";
    assert(false);
  }
}

void testStackOverflowError() {
  std::cout << "\n=== Stack Overflow Error Test ===\n";

  // Test infinite recursion that triggers stack overflow
  const char* script = R"(
    function recursive() {
      return recursive();
    }

    recursive();
  )";

  Lexer lexer(script);
  auto tokens = lexer.tokenize();

  Parser parser(tokens);
  auto program = parser.parse();

  if (!program) {
    std::cerr << "Parse error\n";
    return;
  }

  auto env = Environment::createGlobal();
  Interpreter interpreter(env);

  auto task = interpreter.evaluate(*program);
  while (!task.done()) {
    std::coroutine_handle<>::from_address(task.handle.address()).resume();
  }

  // Check if error was thrown
  if (interpreter.hasError()) {
    auto errorValue = interpreter.getError();

    if (auto errorPtr = std::get_if<std::shared_ptr<Error>>(&errorValue.data)) {
      auto error = *errorPtr;

      std::cout << "Error Type: " << error->getName() << "\n";
      std::cout << "Error Message: " << error->message << "\n\n";
      std::cout << "Stack Trace:\n" << error->stack << "\n";

      // Verify error type and message
      bool isRangeError = error->type == ErrorType::RangeError;
      bool hasStackMessage = error->stack.find("Maximum call stack size exceeded") != std::string::npos;

      std::cout << "\nVerification:\n";
      std::cout << "  Is RangeError: " << (isRangeError ? "YES" : "NO") << "\n";
      std::cout << "  Has stack overflow message: " << (hasStackMessage ? "YES" : "NO") << "\n";

      assert(isRangeError && "Error should be RangeError");
      assert(hasStackMessage && "Stack trace should mention stack overflow");

      std::cout << "\n✅ Stack overflow error is formatted correctly!\n";
    } else {
      std::cerr << "Error value is not an Error object\n";
      assert(false);
    }
  } else {
    std::cerr << "Expected error but none was thrown\n";
    assert(false);
  }
}

void testTypeError() {
  std::cout << "\n=== Type Error Test ===\n";

  // Test that triggers a type error (calling non-function)
  const char* script = R"(
    let x = 42;
    x();  // Try to call a number
  )";

  Lexer lexer(script);
  auto tokens = lexer.tokenize();

  Parser parser(tokens);
  auto program = parser.parse();

  if (!program) {
    std::cerr << "Parse error\n";
    return;
  }

  auto env = Environment::createGlobal();
  Interpreter interpreter(env);

  auto task = interpreter.evaluate(*program);
  while (!task.done()) {
    std::coroutine_handle<>::from_address(task.handle.address()).resume();
  }

  // Check if error was thrown
  if (interpreter.hasError()) {
    auto errorValue = interpreter.getError();

    if (auto errorPtr = std::get_if<std::shared_ptr<Error>>(&errorValue.data)) {
      auto error = *errorPtr;

      std::cout << "Error Type: " << error->getName() << "\n";
      std::cout << "Error Message: " << error->message << "\n";

      if (!error->stack.empty()) {
        std::cout << "\nStack Trace:\n" << error->stack << "\n";
      }

      // Verify error type
      bool isTypeError = error->type == ErrorType::TypeError;

      std::cout << "\nVerification:\n";
      std::cout << "  Is TypeError: " << (isTypeError ? "YES" : "NO") << "\n";

      assert(isTypeError && "Error should be TypeError");

      std::cout << "\n✅ Type error is handled correctly!\n";
    } else {
      std::cerr << "Error value is not an Error object\n";
      assert(false);
    }
  } else {
    std::cerr << "Expected error but none was thrown\n";
    assert(false);
  }
}

int main() {
  try {
    testStackTrace();
    testStackOverflowError();
    testTypeError();

    std::cout << "\n🎉 All error formatting tests passed!\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Unexpected exception: " << e.what() << "\n";
    return 1;
  }
}
