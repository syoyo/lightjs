#include "environment.h"
#include "interpreter.h"
#include "lexer.h"
#include "parser.h"

#include <cassert>
#include <iostream>

using namespace lightjs;

namespace {

Value runScript(const char* source, GCPtr<Environment>& env, Interpreter& interpreter) {
  Lexer lexer(source);
  auto tokens = lexer.tokenize();

  Parser parser(tokens);
  auto program = parser.parse();
  assert(program && "script should parse");

  auto task = interpreter.evaluate(*program);
  Value result;
  LIGHTJS_RUN_TASK(task, result);
  return result;
}

void testStrictPrefixIncrementThrowsAfterGlobalGetterDeletesBinding() {
  auto env = Environment::createGlobal();
  Interpreter interpreter(env);

  runScript(R"(
    var count = 0;
    Object.defineProperty(this, "x", {
      configurable: true,
      get: function() {
        delete this.x;
        return 2;
      }
    });

    (function() {
      "use strict";
      try {
        count++;
        ++x;
        count++;
      } catch (e) {
        globalThis.errorName = e.name;
      }
      count++;
    })();
  )", env, interpreter);

  assert(!interpreter.hasError());

  auto count = env->get("count");
  assert(count.has_value());
  assert(count->isNumber());
  assert(count->toNumber() == 2.0);

  auto errorName = env->get("errorName");
  assert(errorName.has_value());
  assert(errorName->isString());
  assert(errorName->toString() == "ReferenceError");

  auto globalObj = env->getGlobal();
  assert(globalObj);
  assert(globalObj->properties.count("x") == 0);
  assert(globalObj->properties.count("__get_x") == 0);
}

void testPrefixIncrementUsesGlobalAccessorSetter() {
  auto env = Environment::createGlobal();
  Interpreter interpreter(env);

  runScript(R"(
    var seen = 0;
    Object.defineProperty(this, "y", {
      configurable: true,
      get: function() {
        return 1;
      },
      set: function(value) {
        seen = value;
      }
    });

    ++y;
  )", env, interpreter);

  assert(!interpreter.hasError());

  auto seen = env->get("seen");
  assert(seen.has_value());
  assert(seen->isNumber());
  assert(seen->toNumber() == 2.0);
}

}  // namespace

int main() {
  testStrictPrefixIncrementThrowsAfterGlobalGetterDeletesBinding();
  testPrefixIncrementUsesGlobalAccessorSetter();
  std::cout << "global_accessor_update_test passed\n";
  return 0;
}
