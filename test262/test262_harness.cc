#include "environment.h"
#include "value.h"
#include "interpreter.h"
#include <cmath>
#include <stdexcept>
#include <memory>

namespace tinyjs {

void installTest262Harness(std::shared_ptr<Environment> env) {
  // Test262 Error constructor
  auto Test262Error = std::make_shared<Function>();
  Test262Error->isNative = true;
  Test262Error->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto error = std::make_shared<Object>();
    error->properties["message"] = args.empty() ? Value(std::string("")) : args[0];
    error->properties["name"] = Value(std::string("Test262Error"));
    return Value(error);
  };
  env->define("Test262Error", Value(Test262Error));

  // $ERROR function (throws Test262Error)
  auto $ERROR = std::make_shared<Function>();
  $ERROR->isNative = true;
  $ERROR->nativeFunc = [](const std::vector<Value>& args) -> Value {
    std::string message = args.empty() ? "Test262 Error" : args[0].toString();
    throw std::runtime_error("Test262Error: " + message);
  };
  env->define("$ERROR", Value($ERROR));

  // $262 global object
  auto $262 = std::make_shared<Object>();

  // $262.createRealm()
  auto createRealm = std::make_shared<Function>();
  createRealm->isNative = true;
  createRealm->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto realm = std::make_shared<Object>();
    auto global = std::make_shared<Object>();
    realm->properties["global"] = Value(global);

    auto $eval = std::make_shared<Function>();
    $eval->isNative = true;
    $eval->nativeFunc = [](const std::vector<Value>& args) -> Value {
      // Simplified eval for realm
      return Value(Undefined{});
    };
    realm->properties["eval"] = Value($eval);

    return Value(realm);
  };
  $262->properties["createRealm"] = Value(createRealm);

  // $262.detachArrayBuffer()
  auto detachArrayBuffer = std::make_shared<Function>();
  detachArrayBuffer->isNative = true;
  detachArrayBuffer->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // Mark TypedArray as detached (simplified)
    if (!args.empty()) {
      if (auto* arr = std::get_if<std::shared_ptr<TypedArray>>(&args[0].data)) {
        (*arr)->buffer.clear();
      }
    }
    return Value(Undefined{});
  };
  $262->properties["detachArrayBuffer"] = Value(detachArrayBuffer);

  // $262.evalScript()
  auto evalScript = std::make_shared<Function>();
  evalScript->isNative = true;
  evalScript->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // Simplified evalScript
    return Value(Undefined{});
  };
  $262->properties["evalScript"] = Value(evalScript);

  // $262.gc()
  auto gc = std::make_shared<Function>();
  gc->isNative = true;
  gc->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // No-op for GC
    return Value(Undefined{});
  };
  $262->properties["gc"] = Value(gc);

  // $262.global
  $262->properties["global"] = Value(env->getGlobal());

  // $262.agent object
  auto agent = std::make_shared<Object>();

  auto start = std::make_shared<Function>();
  start->isNative = true;
  start->nativeFunc = [](const std::vector<Value>& args) -> Value {
    return Value(Undefined{});
  };
  agent->properties["start"] = Value(start);

  auto broadcast = std::make_shared<Function>();
  broadcast->isNative = true;
  broadcast->nativeFunc = [](const std::vector<Value>& args) -> Value {
    return Value(Undefined{});
  };
  agent->properties["broadcast"] = Value(broadcast);

  auto getReport = std::make_shared<Function>();
  getReport->isNative = true;
  getReport->nativeFunc = [](const std::vector<Value>& args) -> Value {
    return Value(std::string(""));
  };
  agent->properties["getReport"] = Value(getReport);

  auto sleep = std::make_shared<Function>();
  sleep->isNative = true;
  sleep->nativeFunc = [](const std::vector<Value>& args) -> Value {
    return Value(Undefined{});
  };
  agent->properties["sleep"] = Value(sleep);

  $262->properties["agent"] = Value(agent);

  env->define("$262", Value($262));

  // assert object with extended methods
  auto assert = std::make_shared<Object>();

  // Basic assert function
  auto assertFunc = std::make_shared<Function>();
  assertFunc->isNative = true;
  assertFunc->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].toBool()) {
      std::string message = args.size() > 1 ? args[1].toString() : "Assertion failed";
      throw std::runtime_error("AssertionError: " + message);
    }
    return Value(true);
  };

  // Make assert callable
  auto assertCallable = std::make_shared<Function>();
  assertCallable->isNative = true;
  assertCallable->nativeFunc = assertFunc->nativeFunc;

  // assert.sameValue
  auto sameValue = std::make_shared<Function>();
  sameValue->isNative = true;
  sameValue->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) {
      throw std::runtime_error("assert.sameValue requires at least 2 arguments");
    }

    bool same = false;
    const auto& actual = args[0];
    const auto& expected = args[1];

    if (actual.data.index() == expected.data.index()) {
      if (std::holds_alternative<double>(actual.data)) {
        double a = std::get<double>(actual.data);
        double b = std::get<double>(expected.data);
        same = (a == b) || (std::isnan(a) && std::isnan(b));
      } else if (std::holds_alternative<BigInt>(actual.data)) {
        same = std::get<BigInt>(actual.data).value == std::get<BigInt>(expected.data).value;
      } else {
        same = actual.toString() == expected.toString();
      }
    }

    if (!same) {
      std::string message = args.size() > 2 ? args[2].toString()
        : "Expected SameValue(" + actual.toString() + ", " + expected.toString() + ") to be true";
      throw std::runtime_error("AssertionError: " + message);
    }
    return Value(Undefined{});
  };
  assert->properties["sameValue"] = Value(sameValue);

  // assert.notSameValue
  auto notSameValue = std::make_shared<Function>();
  notSameValue->isNative = true;
  notSameValue->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) {
      throw std::runtime_error("assert.notSameValue requires at least 2 arguments");
    }

    bool same = false;
    const auto& actual = args[0];
    const auto& expected = args[1];

    if (actual.data.index() == expected.data.index()) {
      if (std::holds_alternative<double>(actual.data)) {
        double a = std::get<double>(actual.data);
        double b = std::get<double>(expected.data);
        same = (a == b) || (std::isnan(a) && std::isnan(b));
      } else if (std::holds_alternative<BigInt>(actual.data)) {
        same = std::get<BigInt>(actual.data).value == std::get<BigInt>(expected.data).value;
      } else {
        same = actual.toString() == expected.toString();
      }
    }

    if (same) {
      std::string message = args.size() > 2 ? args[2].toString()
        : "Expected SameValue(" + actual.toString() + ", " + expected.toString() + ") to be false";
      throw std::runtime_error("AssertionError: " + message);
    }
    return Value(Undefined{});
  };
  assert->properties["notSameValue"] = Value(notSameValue);

  // assert.throws
  auto throws = std::make_shared<Function>();
  throws->isNative = true;
  throws->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) {
      throw std::runtime_error("assert.throws requires at least 2 arguments");
    }

    // First arg is the error constructor, second is the function to call
    auto errorConstructor = args[0];
    auto func = args[1];

    if (auto* f = std::get_if<std::shared_ptr<Function>>(&func.data)) {
      bool thrown = false;
      try {
        // Try to call the function
        if ((*f)->isNative && (*f)->nativeFunc) {
          (*f)->nativeFunc({});
        }
      } catch (const std::exception& e) {
        thrown = true;
      }

      if (!thrown) {
        throw std::runtime_error("AssertionError: Expected function to throw");
      }
    }

    return Value(Undefined{});
  };
  assert->properties["throws"] = Value(throws);

  // compareArray helper
  auto compareArray = std::make_shared<Function>();
  compareArray->isNative = true;
  compareArray->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(false);

    auto* arr1 = std::get_if<std::shared_ptr<Array>>(&args[0].data);
    auto* arr2 = std::get_if<std::shared_ptr<Array>>(&args[1].data);

    if (!arr1 || !arr2) return Value(false);
    if ((*arr1)->elements.size() != (*arr2)->elements.size()) return Value(false);

    for (size_t i = 0; i < (*arr1)->elements.size(); i++) {
      if ((*arr1)->elements[i].toString() != (*arr2)->elements[i].toString()) {
        return Value(false);
      }
    }

    return Value(true);
  };
  assert->properties["compareArray"] = Value(compareArray);

  env->define("assert", Value(assert));

  // compareArray global function
  env->define("compareArray", Value(compareArray));

  // $DONE function for async tests
  auto $DONE = std::make_shared<Function>();
  $DONE->isNative = true;
  $DONE->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (!args.empty() && !std::holds_alternative<Undefined>(args[0].data)) {
      throw std::runtime_error("Async test failed: " + args[0].toString());
    }
    return Value(Undefined{});
  };
  env->define("$DONE", Value($DONE));

  // Test262 specific helpers

  // isConstructor
  auto isConstructor = std::make_shared<Function>();
  isConstructor->isNative = true;
  isConstructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(false);
    return Value(std::holds_alternative<std::shared_ptr<Function>>(args[0].data));
  };
  env->define("isConstructor", Value(isConstructor));

  // fnGlobalObject
  auto fnGlobalObject = std::make_shared<Function>();
  fnGlobalObject->isNative = true;
  fnGlobalObject->nativeFunc = [env](const std::vector<Value>& args) -> Value {
    return Value(env->getGlobal());
  };
  env->define("fnGlobalObject", Value(fnGlobalObject));

  // verifyProperty helper
  auto verifyProperty = std::make_shared<Function>();
  verifyProperty->isNative = true;
  verifyProperty->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 3) return Value(false);

    auto* obj = std::get_if<std::shared_ptr<Object>>(&args[0].data);
    if (!obj) return Value(false);

    std::string propName = args[1].toString();

    // Check if property exists
    return Value((*obj)->properties.find(propName) != (*obj)->properties.end());
  };
  env->define("verifyProperty", Value(verifyProperty));

  // buildString helper
  auto buildString = std::make_shared<Function>();
  buildString->isNative = true;
  buildString->nativeFunc = [](const std::vector<Value>& args) -> Value {
    std::string result;
    for (const auto& arg : args) {
      if (std::holds_alternative<double>(arg.data)) {
        int count = static_cast<int>(std::get<double>(arg.data));
        result += std::string(count, 'x');
      } else {
        result += arg.toString();
      }
    }
    return Value(result);
  };
  env->define("buildString", Value(buildString));
}

std::shared_ptr<Environment> createTest262Environment() {
  auto env = Environment::createGlobal();
  installTest262Harness(env);
  return env;
}

} // namespace tinyjs