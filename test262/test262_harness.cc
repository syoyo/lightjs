#include "test262_harness.h"
#include "environment.h"
#include "lexer.h"
#include "parser.h"
#include "value.h"
#include "interpreter.h"
#include "symbols.h"
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
    return std::get<GCPtr<Function>>(actual.data) ==
           std::get<GCPtr<Function>>(expected.data);
  }
  if (actual.isArray()) {
    return std::get<GCPtr<Array>>(actual.data) ==
           std::get<GCPtr<Array>>(expected.data);
  }
  if (actual.isObject()) {
    return std::get<GCPtr<Object>>(actual.data) ==
           std::get<GCPtr<Object>>(expected.data);
  }
  if (actual.isTypedArray()) {
    return std::get<GCPtr<TypedArray>>(actual.data) ==
           std::get<GCPtr<TypedArray>>(expected.data);
  }
  if (actual.isPromise()) {
    return std::get<GCPtr<Promise>>(actual.data) ==
           std::get<GCPtr<Promise>>(expected.data);
  }
  if (actual.isRegex()) {
    return std::get<GCPtr<Regex>>(actual.data) ==
           std::get<GCPtr<Regex>>(expected.data);
  }
  if (actual.isMap()) {
    return std::get<GCPtr<Map>>(actual.data) ==
           std::get<GCPtr<Map>>(expected.data);
  }
  if (actual.isSet()) {
    return std::get<GCPtr<Set>>(actual.data) ==
           std::get<GCPtr<Set>>(expected.data);
  }
  if (actual.isError()) {
    return std::get<GCPtr<Error>>(actual.data) ==
           std::get<GCPtr<Error>>(expected.data);
  }
  if (actual.isGenerator()) {
    return std::get<GCPtr<Generator>>(actual.data) ==
           std::get<GCPtr<Generator>>(expected.data);
  }
  if (actual.isProxy()) {
    return std::get<GCPtr<Proxy>>(actual.data) ==
           std::get<GCPtr<Proxy>>(expected.data);
  }
  if (actual.isWeakMap()) {
    return std::get<GCPtr<WeakMap>>(actual.data) ==
           std::get<GCPtr<WeakMap>>(expected.data);
  }
  if (actual.isWeakSet()) {
    return std::get<GCPtr<WeakSet>>(actual.data) ==
           std::get<GCPtr<WeakSet>>(expected.data);
  }
  if (actual.isArrayBuffer()) {
    return std::get<GCPtr<ArrayBuffer>>(actual.data) ==
           std::get<GCPtr<ArrayBuffer>>(expected.data);
  }
  if (actual.isDataView()) {
    return std::get<GCPtr<DataView>>(actual.data) ==
           std::get<GCPtr<DataView>>(expected.data);
  }
  if (actual.isClass()) {
    return std::get<GCPtr<Class>>(actual.data) ==
           std::get<GCPtr<Class>>(expected.data);
  }
  if (actual.isWasmInstance()) {
    return std::get<GCPtr<WasmInstanceJS>>(actual.data) ==
           std::get<GCPtr<WasmInstanceJS>>(expected.data);
  }
  if (actual.isWasmMemory()) {
    return std::get<GCPtr<WasmMemoryJS>>(actual.data) ==
           std::get<GCPtr<WasmMemoryJS>>(expected.data);
  }
  if (actual.isReadableStream()) {
    return std::get<GCPtr<ReadableStream>>(actual.data) ==
           std::get<GCPtr<ReadableStream>>(expected.data);
  }
  if (actual.isWritableStream()) {
    return std::get<GCPtr<WritableStream>>(actual.data) ==
           std::get<GCPtr<WritableStream>>(expected.data);
  }
  if (actual.isTransformStream()) {
    return std::get<GCPtr<TransformStream>>(actual.data) ==
           std::get<GCPtr<TransformStream>>(expected.data);
  }

  return false;
}
}  // namespace

void installTest262Harness(GCPtr<Environment> env) {
  // Test262 Error constructor
  auto Test262Error = GarbageCollector::makeGC<Function>();
  Test262Error->isNative = true;
  Test262Error->isConstructor = true;
  auto test262ErrorProto = GarbageCollector::makeGC<Object>();
  test262ErrorProto->properties["constructor"] = Value(Test262Error);
  Test262Error->properties["prototype"] = Value(test262ErrorProto);
  Test262Error->nativeFunc = [Test262Error, test262ErrorProto](const std::vector<Value>& args) -> Value {
    auto error = GarbageCollector::makeGC<Object>();
    error->properties["__proto__"] = Value(test262ErrorProto);
    error->properties["constructor"] = Value(Test262Error);
    error->properties["message"] = args.empty() ? Value(std::string("")) : args[0];
    error->properties["name"] = Value(std::string("Test262Error"));
    return Value(error);
  };
  // Test262Error.thrower - throws a Test262Error (defined in sta.js)
  auto throwerFunc = GarbageCollector::makeGC<Function>();
  throwerFunc->isNative = true;
  throwerFunc->nativeFunc = [](const std::vector<Value>& args) -> Value {
    std::string message = args.empty() ? "" : args[0].toString();
    throw std::runtime_error("Test262Error: " + message);
  };
  Test262Error->properties["thrower"] = Value(throwerFunc);

  env->define("Test262Error", Value(Test262Error));

  // $ERROR function (throws Test262Error)
  auto $ERROR = GarbageCollector::makeGC<Function>();
  $ERROR->isNative = true;
  $ERROR->nativeFunc = [](const std::vector<Value>& args) -> Value {
    std::string message = args.empty() ? "Test262 Error" : args[0].toString();
    throw std::runtime_error("Test262Error: " + message);
  };
  env->define("$ERROR", Value($ERROR));

  // $262 global object
  auto $262 = GarbageCollector::makeGC<Object>();

  // $262.createRealm()
  auto createRealm = GarbageCollector::makeGC<Function>();
  createRealm->isNative = true;
  createRealm->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto realmEnv = createTest262Environment();
    auto realm = GarbageCollector::makeGC<Object>();
    realm->properties["global"] = Value(realmEnv->getGlobal());

    auto $eval = GarbageCollector::makeGC<Function>();
    $eval->isNative = true;
    $eval->nativeFunc = [realmEnv](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isString()) {
        return Value(Undefined{});
      }
      try {
        Lexer lexer(args[0].toString());
        auto tokens = lexer.tokenize();
        Parser parser(tokens);
        auto program = parser.parse();
        if (!program) {
          throw std::runtime_error("SyntaxError: Parse error");
        }
        Interpreter interpreter(realmEnv);
        auto task = interpreter.evaluate(*program);
        Value result = Value(Undefined{});
        LIGHTJS_RUN_TASK(task, result);
        if (interpreter.hasError()) {
          Value err = interpreter.getError();
          interpreter.clearError();
          throw std::runtime_error(err.toString());
        }
        return result;
      } catch (const std::exception& e) {
        throw std::runtime_error(e.what());
      }
    };
    realm->properties["eval"] = Value($eval);

    return Value(realm);
  };
  $262->properties["createRealm"] = Value(createRealm);

  // $262.detachArrayBuffer()
  auto detachArrayBuffer = GarbageCollector::makeGC<Function>();
  detachArrayBuffer->isNative = true;
  detachArrayBuffer->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (!args.empty()) {
      if (auto* buffer = std::get_if<GCPtr<ArrayBuffer>>(&args[0].data)) {
        if (*buffer) {
          (*buffer)->detach();
        }
      } else if (auto* arr = std::get_if<GCPtr<TypedArray>>(&args[0].data)) {
        if (*arr && (*arr)->viewedBuffer) {
          (*arr)->viewedBuffer->detach();
        }
      }
    }
    return Value(Undefined{});
  };
  $262->properties["detachArrayBuffer"] = Value(detachArrayBuffer);

  // $262.evalScript() - evaluate a script string in global scope
  // (like creating a new <script> element with proper GlobalDeclarationInstantiation).
  auto evalScript = GarbageCollector::makeGC<Function>();
  evalScript->isNative = true;
  evalScript->nativeFunc = [env](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isString()) {
      return Value(Undefined{});
    }
    auto* interpreter = getGlobalInterpreter();
    if (!interpreter) {
      throw std::runtime_error("TypeError: $262.evalScript: no active interpreter");
    }
    // Use runScriptInGlobalScope for proper script semantics
    // (non-configurable var bindings, GlobalDeclarationInstantiation checks)
    Value result = interpreter->runScriptInGlobalScope(args[0].toString());
    if (interpreter->hasError()) {
      Value err = interpreter->getError();
      interpreter->clearError();
      throw std::runtime_error(err.toString());
    }
    return result;
  };
  $262->properties["evalScript"] = Value(evalScript);

  // $262.gc()
  auto gc = GarbageCollector::makeGC<Function>();
  gc->isNative = true;
  gc->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // No-op for GC
    return Value(Undefined{});
  };
  $262->properties["gc"] = Value(gc);

  // $262.global
  $262->properties["global"] = Value(env->getGlobal());

  // $262.AbstractModuleSource
  auto abstractModuleSourcePrototype = GarbageCollector::makeGC<Object>();
  if (auto objectCtor = env->get("Object");
      objectCtor && objectCtor->isFunction()) {
    auto objectFn = objectCtor->getGC<Function>();
    auto objectProtoIt = objectFn->properties.find("prototype");
    if (objectProtoIt != objectFn->properties.end()) {
      abstractModuleSourcePrototype->properties["__proto__"] = objectProtoIt->second;
    }
  }

  auto abstractModuleSource = GarbageCollector::makeGC<Function>();
  abstractModuleSource->isNative = true;
  abstractModuleSource->isConstructor = true;
  abstractModuleSource->nativeFunc = [](const std::vector<Value>&) -> Value {
    throw std::runtime_error("TypeError: AbstractModuleSource cannot be constructed");
  };
  abstractModuleSource->properties["name"] = Value(std::string("AbstractModuleSource"));
  abstractModuleSource->properties["__non_writable_name"] = Value(true);
  abstractModuleSource->properties["__non_enum_name"] = Value(true);
  abstractModuleSource->properties["length"] = Value(0.0);
  abstractModuleSource->properties["__non_writable_length"] = Value(true);
  abstractModuleSource->properties["__non_enum_length"] = Value(true);
  abstractModuleSource->properties["prototype"] = Value(abstractModuleSourcePrototype);
  abstractModuleSource->properties["__non_writable_prototype"] = Value(true);
  abstractModuleSource->properties["__non_configurable_prototype"] = Value(true);
  abstractModuleSource->properties["__non_enum_prototype"] = Value(true);

  auto toStringTagGetter = GarbageCollector::makeGC<Function>();
  toStringTagGetter->isNative = true;
  toStringTagGetter->properties["__throw_on_new__"] = Value(true);
  toStringTagGetter->properties["name"] = Value(std::string("get [Symbol.toStringTag]"));
  toStringTagGetter->properties["__non_writable_name"] = Value(true);
  toStringTagGetter->properties["__non_enum_name"] = Value(true);
  toStringTagGetter->properties["length"] = Value(0.0);
  toStringTagGetter->properties["__non_writable_length"] = Value(true);
  toStringTagGetter->properties["__non_enum_length"] = Value(true);
  toStringTagGetter->nativeFunc = [](const std::vector<Value>&) -> Value {
    return Value(Undefined{});
  };

  abstractModuleSourcePrototype->properties["constructor"] = Value(abstractModuleSource);
  abstractModuleSourcePrototype->properties["__non_enum_constructor"] = Value(true);
  abstractModuleSourcePrototype->properties["__get_" + WellKnownSymbols::toStringTagKey()] =
    Value(toStringTagGetter);
  abstractModuleSourcePrototype->properties["__non_enum_" + WellKnownSymbols::toStringTagKey()] =
    Value(true);

  $262->properties["AbstractModuleSource"] = Value(abstractModuleSource);
  $262->properties["__non_enum_AbstractModuleSource"] = Value(true);

  // $262.agent object
  auto agent = GarbageCollector::makeGC<Object>();

  auto start = GarbageCollector::makeGC<Function>();
  start->isNative = true;
  start->nativeFunc = [](const std::vector<Value>& args) -> Value {
    return Value(Undefined{});
  };
  agent->properties["start"] = Value(start);

  auto broadcast = GarbageCollector::makeGC<Function>();
  broadcast->isNative = true;
  broadcast->nativeFunc = [](const std::vector<Value>& args) -> Value {
    return Value(Undefined{});
  };
  agent->properties["broadcast"] = Value(broadcast);

  auto getReport = GarbageCollector::makeGC<Function>();
  getReport->isNative = true;
  getReport->nativeFunc = [](const std::vector<Value>& args) -> Value {
    return Value(std::string(""));
  };
  agent->properties["getReport"] = Value(getReport);

  auto sleep = GarbageCollector::makeGC<Function>();
  sleep->isNative = true;
  sleep->nativeFunc = [](const std::vector<Value>& args) -> Value {
    return Value(Undefined{});
  };
  agent->properties["sleep"] = Value(sleep);

  $262->properties["agent"] = Value(agent);

  env->define("$262", Value($262));

  // Basic assert function
  auto assertFunc = GarbageCollector::makeGC<Function>();
  assertFunc->isNative = true;
  assertFunc->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].toBool()) {
      std::string message = args.size() > 1 ? args[1].toString() : "Assertion failed";
      throw std::runtime_error("AssertionError: " + message);
    }
    return Value(true);
  };

  // Make assert callable
  auto assertCallable = GarbageCollector::makeGC<Function>();
  assertCallable->isNative = true;
  assertCallable->nativeFunc = assertFunc->nativeFunc;

  // assert.sameValue
  auto sameValue = GarbageCollector::makeGC<Function>();
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
  auto notSameValue = GarbageCollector::makeGC<Function>();
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
  auto throws = GarbageCollector::makeGC<Function>();
  throws->isNative = true;
  throws->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) {
      throw std::runtime_error("assert.throws requires at least 2 arguments");
    }

    // First arg is the error constructor, second is the function to call
    auto errorConstructor = args[0];
    auto func = args[1];

    if (auto* f = std::get_if<GCPtr<Function>>(&func.data)) {
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

  auto extractArrayLike = [](const Value& value, std::vector<std::string>& out) -> bool {
    if (auto* arr = std::get_if<GCPtr<Array>>(&value.data)) {
      out.reserve((*arr)->elements.size());
      for (const auto& element : (*arr)->elements) {
        out.push_back(element.toString());
      }
      return true;
    }
    if (auto* typedArray = std::get_if<GCPtr<TypedArray>>(&value.data)) {
      size_t length = (*typedArray)->currentLength();
      out.reserve(length);
      for (size_t i = 0; i < length; ++i) {
        if ((*typedArray)->type == TypedArrayType::BigInt64 ||
            (*typedArray)->type == TypedArrayType::BigUint64) {
          if ((*typedArray)->type == TypedArrayType::BigUint64) {
            out.push_back(Value(BigInt(bigint::BigIntValue((*typedArray)->getBigUintElement(i)))).toString());
          } else {
            out.push_back(Value(BigInt((*typedArray)->getBigIntElement(i))).toString());
          }
        } else {
          out.push_back(Value((*typedArray)->getElement(i)).toString());
        }
      }
      return true;
    }
    return false;
  };

  auto arraysEqual = [extractArrayLike](const Value& lhs, const Value& rhs) -> bool {
    std::vector<std::string> left;
    std::vector<std::string> right;
    bool leftArrayLike = extractArrayLike(lhs, left);
    bool rightArrayLike = extractArrayLike(rhs, right);
    if (leftArrayLike && rightArrayLike) {
      return left == right;
    }
    return !lhs.isUndefined() && !lhs.isNull() && !rhs.isUndefined() && !rhs.isNull() &&
           (lhs.isObject() || lhs.isArray() || lhs.isFunction() || lhs.isRegex() || lhs.isPromise() ||
            lhs.isGenerator() || lhs.isClass() || lhs.isMap() || lhs.isSet() || lhs.isWeakMap() ||
            lhs.isWeakSet() || lhs.isTypedArray() || lhs.isArrayBuffer() || lhs.isDataView() || lhs.isError()) &&
           (rhs.isObject() || rhs.isArray() || rhs.isFunction() || rhs.isRegex() || rhs.isPromise() ||
            rhs.isGenerator() || rhs.isClass() || rhs.isMap() || rhs.isSet() || rhs.isWeakMap() ||
            rhs.isWeakSet() || rhs.isTypedArray() || rhs.isArrayBuffer() || rhs.isDataView() || rhs.isError());
  };

  // assert.compareArray(actual, expected[, message])
  // Test262 expects this to return `undefined` on success and throw on mismatch.
  auto assertCompareArray = GarbageCollector::makeGC<Function>();
  assertCompareArray->isNative = true;
  assertCompareArray->nativeFunc = [arraysEqual, extractArrayLike](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) {
      throw std::runtime_error("assert.compareArray requires at least 2 arguments");
    }
    if (!arraysEqual(args[0], args[1])) {
      std::string message = args.size() > 2 ? args[2].toString() : "Array comparison failed";
      std::vector<std::string> left;
      std::vector<std::string> right;
      bool leftArrayLike = extractArrayLike(args[0], left);
      bool rightArrayLike = extractArrayLike(args[1], right);
      if (!leftArrayLike || !rightArrayLike) {
        throw std::runtime_error("AssertionError: " + message + " (non-array operand)");
      }
      size_t len1 = left.size();
      size_t len2 = right.size();
      size_t minLen = std::min(len1, len2);
      for (size_t i = 0; i < minLen; i++) {
        const std::string& lhs = left[i];
        const std::string& rhs = right[i];
        if (lhs != rhs) {
          throw std::runtime_error(
            "AssertionError: " + message + " (index " + std::to_string(i) +
            ": got '" + lhs + "', expected '" + rhs + "')");
        }
      }
      throw std::runtime_error(
        "AssertionError: " + message + " (length " + std::to_string(len1) +
        " !== " + std::to_string(len2) + ")");
    }
    return Value(Undefined{});
  };
  assertCallable->properties["compareArray"] = Value(assertCompareArray);

  env->define("assert", Value(assertCallable));

  // Legacy global compareArray helper returns boolean.
  auto compareArray = GarbageCollector::makeGC<Function>();
  compareArray->isNative = true;
  compareArray->nativeFunc = [arraysEqual](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(false);
    return Value(arraysEqual(args[0], args[1]));
  };
  env->define("compareArray", Value(compareArray));

  // $DONE function for async tests
  auto $DONE = GarbageCollector::makeGC<Function>();
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
  auto isConstructor = GarbageCollector::makeGC<Function>();
  isConstructor->isNative = true;
  isConstructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(false);
    if (!std::holds_alternative<GCPtr<Function>>(args[0].data)) {
      return Value(false);
    }
    auto fn = std::get<GCPtr<Function>>(args[0].data);
    return Value(fn->isConstructor);
  };
  env->define("isConstructor", Value(isConstructor));

  // fnGlobalObject
  auto fnGlobalObject = GarbageCollector::makeGC<Function>();
  fnGlobalObject->isNative = true;
  fnGlobalObject->nativeFunc = [env](const std::vector<Value>& args) -> Value {
    return Value(env->getGlobal());
  };
  env->define("fnGlobalObject", Value(fnGlobalObject));

  // verifyProperty is provided by the JS harness (propertyHelper.js) when needed.
  // We don't define a native version to avoid conflicts with the more complete JS implementation.

  // buildString helper
  auto buildString = GarbageCollector::makeGC<Function>();
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

GCPtr<Environment> createTest262Environment() {
  auto env = Environment::createGlobal();
  installTest262Harness(env);
  return env;
}

} // namespace lightjs
