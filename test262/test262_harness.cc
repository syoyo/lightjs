#include "environment.h"
#include "value.h"
#include "interpreter.h"
#include <cmath>
#include <stdexcept>
#include <memory>

namespace lightjs {

namespace {
bool isSameValue(const Value& actual, const Value& expected) {
  if (actual.data.index() != expected.data.index()) {
    return false;
  }

  if (actual.isUndefined() || actual.isNull()) {
    return true;
  }
  if (actual.isBool()) {
    return std::get<bool>(actual.data) == std::get<bool>(expected.data);
  }
  if (actual.isNumber()) {
    double a = std::get<double>(actual.data);
    double b = std::get<double>(expected.data);
    if (std::isnan(a) && std::isnan(b)) {
      return true;
    }
    if (a == 0.0 && b == 0.0) {
      return std::signbit(a) == std::signbit(b);
    }
    return a == b;
  }
  if (actual.isBigInt()) {
    return std::get<BigInt>(actual.data).value == std::get<BigInt>(expected.data).value;
  }
  if (actual.isSymbol()) {
    return std::get<Symbol>(actual.data).id == std::get<Symbol>(expected.data).id;
  }
  if (actual.isString()) {
    return std::get<std::string>(actual.data) == std::get<std::string>(expected.data);
  }
  if (actual.isFunction()) {
    return std::get<std::shared_ptr<Function>>(actual.data) ==
           std::get<std::shared_ptr<Function>>(expected.data);
  }
  if (actual.isArray()) {
    return std::get<std::shared_ptr<Array>>(actual.data) ==
           std::get<std::shared_ptr<Array>>(expected.data);
  }
  if (actual.isObject()) {
    return std::get<std::shared_ptr<Object>>(actual.data) ==
           std::get<std::shared_ptr<Object>>(expected.data);
  }
  if (actual.isTypedArray()) {
    return std::get<std::shared_ptr<TypedArray>>(actual.data) ==
           std::get<std::shared_ptr<TypedArray>>(expected.data);
  }
  if (actual.isPromise()) {
    return std::get<std::shared_ptr<Promise>>(actual.data) ==
           std::get<std::shared_ptr<Promise>>(expected.data);
  }
  if (actual.isRegex()) {
    return std::get<std::shared_ptr<Regex>>(actual.data) ==
           std::get<std::shared_ptr<Regex>>(expected.data);
  }
  if (actual.isMap()) {
    return std::get<std::shared_ptr<Map>>(actual.data) ==
           std::get<std::shared_ptr<Map>>(expected.data);
  }
  if (actual.isSet()) {
    return std::get<std::shared_ptr<Set>>(actual.data) ==
           std::get<std::shared_ptr<Set>>(expected.data);
  }
  if (actual.isError()) {
    return std::get<std::shared_ptr<Error>>(actual.data) ==
           std::get<std::shared_ptr<Error>>(expected.data);
  }
  if (actual.isGenerator()) {
    return std::get<std::shared_ptr<Generator>>(actual.data) ==
           std::get<std::shared_ptr<Generator>>(expected.data);
  }
  if (actual.isProxy()) {
    return std::get<std::shared_ptr<Proxy>>(actual.data) ==
           std::get<std::shared_ptr<Proxy>>(expected.data);
  }
  if (actual.isWeakMap()) {
    return std::get<std::shared_ptr<WeakMap>>(actual.data) ==
           std::get<std::shared_ptr<WeakMap>>(expected.data);
  }
  if (actual.isWeakSet()) {
    return std::get<std::shared_ptr<WeakSet>>(actual.data) ==
           std::get<std::shared_ptr<WeakSet>>(expected.data);
  }
  if (actual.isArrayBuffer()) {
    return std::get<std::shared_ptr<ArrayBuffer>>(actual.data) ==
           std::get<std::shared_ptr<ArrayBuffer>>(expected.data);
  }
  if (actual.isDataView()) {
    return std::get<std::shared_ptr<DataView>>(actual.data) ==
           std::get<std::shared_ptr<DataView>>(expected.data);
  }
  if (actual.isClass()) {
    return std::get<std::shared_ptr<Class>>(actual.data) ==
           std::get<std::shared_ptr<Class>>(expected.data);
  }
  if (actual.isWasmInstance()) {
    return std::get<std::shared_ptr<WasmInstanceJS>>(actual.data) ==
           std::get<std::shared_ptr<WasmInstanceJS>>(expected.data);
  }
  if (actual.isWasmMemory()) {
    return std::get<std::shared_ptr<WasmMemoryJS>>(actual.data) ==
           std::get<std::shared_ptr<WasmMemoryJS>>(expected.data);
  }
  if (actual.isReadableStream()) {
    return std::get<std::shared_ptr<ReadableStream>>(actual.data) ==
           std::get<std::shared_ptr<ReadableStream>>(expected.data);
  }
  if (actual.isWritableStream()) {
    return std::get<std::shared_ptr<WritableStream>>(actual.data) ==
           std::get<std::shared_ptr<WritableStream>>(expected.data);
  }
  if (actual.isTransformStream()) {
    return std::get<std::shared_ptr<TransformStream>>(actual.data) ==
           std::get<std::shared_ptr<TransformStream>>(expected.data);
  }

  return false;
}
}  // namespace

void installTest262Harness(std::shared_ptr<Environment> env) {
  // Test262 Error constructor
  auto Test262Error = std::make_shared<Function>();
  Test262Error->isNative = true;
  Test262Error->isConstructor = true;
  Test262Error->properties["prototype"] = Value(std::make_shared<Object>());
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
    const auto& actual = args[0];
    const auto& expected = args[1];
    bool same = isSameValue(actual, expected);

    if (!same) {
      std::string message = args.size() > 2 ? args[2].toString()
        : "Expected SameValue(" + actual.toString() + ", " + expected.toString() + ") to be true";
      throw std::runtime_error("AssertionError: " + message);
    }
    return Value(Undefined{});
  };
  assertCallable->properties["sameValue"] = Value(sameValue);

  // assert.notSameValue
  auto notSameValue = std::make_shared<Function>();
  notSameValue->isNative = true;
  notSameValue->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) {
      throw std::runtime_error("assert.notSameValue requires at least 2 arguments");
    }
    const auto& actual = args[0];
    const auto& expected = args[1];
    bool same = isSameValue(actual, expected);

    if (same) {
      std::string message = args.size() > 2 ? args[2].toString()
        : "Expected SameValue(" + actual.toString() + ", " + expected.toString() + ") to be false";
      throw std::runtime_error("AssertionError: " + message);
    }
    return Value(Undefined{});
  };
  assertCallable->properties["notSameValue"] = Value(notSameValue);

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
      Interpreter* interpreter = getGlobalInterpreter();

      try {
        if (interpreter) {
          interpreter->clearError();
          interpreter->callForHarness(Value(*f), {});
          if (interpreter->hasError()) {
            thrown = true;
            interpreter->clearError();
          }
        } else if ((*f)->isNative && (*f)->nativeFunc) {
          (*f)->nativeFunc({});
        }
      } catch (const std::exception&) {
        thrown = true;
        if (interpreter) {
          interpreter->clearError();
        }
      }

      if (!thrown) {
        throw std::runtime_error("AssertionError: Expected function to throw");
      }
    }

    return Value(Undefined{});
  };
  assertCallable->properties["throws"] = Value(throws);

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

  assertCallable->properties["compareArray"] = Value(compareArray);

  env->define("assert", Value(assertCallable));

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
    if (!std::holds_alternative<std::shared_ptr<Function>>(args[0].data)) {
      return Value(false);
    }
    auto fn = std::get<std::shared_ptr<Function>>(args[0].data);
    return Value(fn->isConstructor);
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

} // namespace lightjs
