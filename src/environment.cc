#include "environment.h"
#include "crypto.h"
#include "http.h"
#include "gc.h"
#include "json.h"
#include "object_methods.h"
#include "array_methods.h"
#include "string_methods.h"
#include "math_object.h"
#include "date_object.h"
#include <iostream>
#include <thread>
#include <limits>
#include <cmath>

namespace tinyjs {

Environment::Environment(std::shared_ptr<Environment> parent)
  : parent_(parent) {}

void Environment::define(const std::string& name, const Value& value, bool isConst) {
  bindings_[name] = value;
  if (isConst) {
    constants_[name] = true;
  }
}

std::optional<Value> Environment::get(const std::string& name) const {
  auto it = bindings_.find(name);
  if (it != bindings_.end()) {
    return it->second;
  }
  if (parent_) {
    return parent_->get(name);
  }
  return std::nullopt;
}

bool Environment::set(const std::string& name, const Value& value) {
  auto it = bindings_.find(name);
  if (it != bindings_.end()) {
    if (constants_.find(name) != constants_.end()) {
      return false;
    }
    bindings_[name] = value;
    return true;
  }
  if (parent_) {
    return parent_->set(name, value);
  }
  return false;
}

bool Environment::has(const std::string& name) const {
  if (bindings_.find(name) != bindings_.end()) {
    return true;
  }
  if (parent_) {
    return parent_->has(name);
  }
  return false;
}

std::shared_ptr<Environment> Environment::createChild() {
  return std::make_shared<Environment>(shared_from_this());
}

std::shared_ptr<Environment> Environment::createGlobal() {
  auto env = std::make_shared<Environment>();

  auto consoleFn = std::make_shared<Function>();
  consoleFn->isNative = true;
  consoleFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    for (const auto& arg : args) {
      std::cout << arg.toString() << " ";
    }
    std::cout << std::endl;
    return Value(Undefined{});
  };

  auto consoleObj = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  consoleObj->properties["log"] = Value(consoleFn);

  env->define("console", Value(consoleObj));
  env->define("undefined", Value(Undefined{}));

  // Symbol constructor
  auto symbolFn = std::make_shared<Function>();
  symbolFn->isNative = true;
  symbolFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    std::string description = args.empty() ? "" : args[0].toString();
    return Value(Symbol(description));
  };
  env->define("Symbol", Value(symbolFn));

  auto createTypedArrayConstructor = [](TypedArrayType type) {
    auto func = std::make_shared<Function>();
    func->isNative = true;
    func->nativeFunc = [type](const std::vector<Value>& args) -> Value {
      if (args.empty()) {
        return Value(std::make_shared<TypedArray>(type, 0));
      }
      size_t length = static_cast<size_t>(args[0].toNumber());
      return Value(std::make_shared<TypedArray>(type, length));
    };
    return Value(func);
  };

  env->define("Int8Array", createTypedArrayConstructor(TypedArrayType::Int8));
  env->define("Uint8Array", createTypedArrayConstructor(TypedArrayType::Uint8));
  env->define("Uint8ClampedArray", createTypedArrayConstructor(TypedArrayType::Uint8Clamped));
  env->define("Int16Array", createTypedArrayConstructor(TypedArrayType::Int16));
  env->define("Uint16Array", createTypedArrayConstructor(TypedArrayType::Uint16));
  env->define("Float16Array", createTypedArrayConstructor(TypedArrayType::Float16));
  env->define("Int32Array", createTypedArrayConstructor(TypedArrayType::Int32));
  env->define("Uint32Array", createTypedArrayConstructor(TypedArrayType::Uint32));
  env->define("Float32Array", createTypedArrayConstructor(TypedArrayType::Float32));
  env->define("Float64Array", createTypedArrayConstructor(TypedArrayType::Float64));
  env->define("BigInt64Array", createTypedArrayConstructor(TypedArrayType::BigInt64));
  env->define("BigUint64Array", createTypedArrayConstructor(TypedArrayType::BigUint64));

  auto cryptoObj = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  auto sha256Fn = std::make_shared<Function>();
  sha256Fn->isNative = true;
  sha256Fn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::string(""));
    std::string input = args[0].toString();
    std::string result = crypto::SHA256::hashHex(
      reinterpret_cast<const uint8_t*>(input.c_str()), input.length()
    );
    return Value(result);
  };
  cryptoObj->properties["sha256"] = Value(sha256Fn);

  auto hmacFn = std::make_shared<Function>();
  hmacFn->isNative = true;
  hmacFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(std::string(""));
    std::string key = args[0].toString();
    std::string message = args[1].toString();
    std::string result = crypto::HMAC::computeHex(
      reinterpret_cast<const uint8_t*>(key.c_str()), key.length(),
      reinterpret_cast<const uint8_t*>(message.c_str()), message.length()
    );
    return Value(result);
  };
  cryptoObj->properties["hmac"] = Value(hmacFn);

  auto toHexFn = std::make_shared<Function>();
  toHexFn->isNative = true;
  toHexFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::string(""));
    std::string input = args[0].toString();
    std::string result = crypto::toHex(
      reinterpret_cast<const uint8_t*>(input.c_str()), input.length()
    );
    return Value(result);
  };
  cryptoObj->properties["toHex"] = Value(toHexFn);

  env->define("crypto", Value(cryptoObj));

  auto fetchFn = std::make_shared<Function>();
  fetchFn->isNative = true;
  fetchFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(Undefined{});
    }

    std::string url = args[0].toString();

    auto promise = std::make_shared<Promise>();
    http::HTTPClient client;

    try {
      http::Response httpResp = client.get(url);

      auto respObj = std::make_shared<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      respObj->properties["status"] = Value(static_cast<double>(httpResp.statusCode));
      respObj->properties["statusText"] = Value(httpResp.statusText);
      respObj->properties["ok"] = Value(httpResp.statusCode >= 200 && httpResp.statusCode < 300);

      auto textFn = std::make_shared<Function>();
      textFn->isNative = true;
      std::string bodyText = httpResp.bodyAsString();
      textFn->nativeFunc = [bodyText](const std::vector<Value>&) -> Value {
        return Value(bodyText);
      };
      respObj->properties["text"] = Value(textFn);

      auto headersObj = std::make_shared<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      for (const auto& [key, value] : httpResp.headers) {
        headersObj->properties[key] = Value(value);
      }
      respObj->properties["headers"] = Value(headersObj);

      promise->resolve(Value(respObj));
    } catch (...) {
      promise->reject(Value(std::string("Fetch failed")));
    }

    return Value(promise);
  };
  env->define("fetch", Value(fetchFn));

  auto regExpConstructor = std::make_shared<Function>();
  regExpConstructor->isNative = true;
  regExpConstructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(Undefined{});
    std::string pattern = args[0].toString();
    std::string flags = args.size() > 1 ? args[1].toString() : "";
    return Value(std::make_shared<Regex>(pattern, flags));
  };
  env->define("RegExp", Value(regExpConstructor));

  // Number object with static methods
  auto numberObj = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // Number.parseInt
  auto parseIntFn = std::make_shared<Function>();
  parseIntFn->isNative = true;
  parseIntFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    std::string str = args[0].toString();
    int radix = args.size() > 1 ? static_cast<int>(args[1].toNumber()) : 10;

    if (radix != 0 && (radix < 2 || radix > 36)) {
      return Value(std::numeric_limits<double>::quiet_NaN());
    }

    // Trim whitespace
    size_t start = str.find_first_not_of(" \t\n\r\f\v");
    if (start == std::string::npos) {
      return Value(std::numeric_limits<double>::quiet_NaN());
    }
    str = str.substr(start);

    // Handle sign
    bool negative = false;
    if (!str.empty() && (str[0] == '+' || str[0] == '-')) {
      negative = (str[0] == '-');
      str = str.substr(1);
    }

    // Auto-detect radix if 0
    if (radix == 0) {
      if (str.size() >= 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        radix = 16;
        str = str.substr(2);
      } else {
        radix = 10;
      }
    }

    try {
      size_t idx;
      long long result = std::stoll(str, &idx, radix);
      if (idx == 0) {
        return Value(std::numeric_limits<double>::quiet_NaN());
      }
      return Value(static_cast<double>(negative ? -result : result));
    } catch (...) {
      return Value(std::numeric_limits<double>::quiet_NaN());
    }
  };
  numberObj->properties["parseInt"] = Value(parseIntFn);

  // Number.parseFloat
  auto parseFloatFn = std::make_shared<Function>();
  parseFloatFn->isNative = true;
  parseFloatFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    std::string str = args[0].toString();

    // Trim whitespace
    size_t start = str.find_first_not_of(" \t\n\r\f\v");
    if (start == std::string::npos) {
      return Value(std::numeric_limits<double>::quiet_NaN());
    }
    str = str.substr(start);

    if (str.empty()) {
      return Value(std::numeric_limits<double>::quiet_NaN());
    }

    try {
      size_t idx;
      double result = std::stod(str, &idx);
      if (idx == 0) {
        return Value(std::numeric_limits<double>::quiet_NaN());
      }
      return Value(result);
    } catch (...) {
      return Value(std::numeric_limits<double>::quiet_NaN());
    }
  };
  numberObj->properties["parseFloat"] = Value(parseFloatFn);

  // Number.isNaN
  auto isNaNFn = std::make_shared<Function>();
  isNaNFn->isNative = true;
  isNaNFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isNumber()) return Value(false);
    double num = std::get<double>(args[0].data);
    return Value(std::isnan(num));
  };
  numberObj->properties["isNaN"] = Value(isNaNFn);

  // Number.isFinite
  auto isFiniteFn = std::make_shared<Function>();
  isFiniteFn->isNative = true;
  isFiniteFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isNumber()) return Value(false);
    double num = std::get<double>(args[0].data);
    return Value(std::isfinite(num));
  };
  numberObj->properties["isFinite"] = Value(isFiniteFn);

  // Number constants
  numberObj->properties["MAX_VALUE"] = Value(std::numeric_limits<double>::max());
  numberObj->properties["MIN_VALUE"] = Value(std::numeric_limits<double>::min());
  numberObj->properties["POSITIVE_INFINITY"] = Value(std::numeric_limits<double>::infinity());
  numberObj->properties["NEGATIVE_INFINITY"] = Value(-std::numeric_limits<double>::infinity());
  numberObj->properties["NaN"] = Value(std::numeric_limits<double>::quiet_NaN());

  env->define("Number", Value(numberObj));

  // Global parseInt and parseFloat (aliases)
  env->define("parseInt", Value(parseIntFn));
  env->define("parseFloat", Value(parseFloatFn));

  // Global isNaN (different from Number.isNaN - coerces to number first)
  auto globalIsNaNFn = std::make_shared<Function>();
  globalIsNaNFn->isNative = true;
  globalIsNaNFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(true);
    double num = args[0].toNumber();
    return Value(std::isnan(num));
  };
  env->define("isNaN", Value(globalIsNaNFn));

  // Global isFinite
  auto globalIsFiniteFn = std::make_shared<Function>();
  globalIsFiniteFn->isNative = true;
  globalIsFiniteFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(false);
    double num = args[0].toNumber();
    return Value(std::isfinite(num));
  };
  env->define("isFinite", Value(globalIsFiniteFn));

  // Array object with static methods
  auto arrayObj = std::make_shared<Object>();
  // GarbageCollector::instance().reportAllocation(sizeof(Object));

  // Array.isArray
  auto isArrayFn = std::make_shared<Function>();
  isArrayFn->isNative = true;
  isArrayFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(false);
    return Value(args[0].isArray());
  };
  arrayObj->properties["isArray"] = Value(isArrayFn);

  // Array.from - creates array from array-like or iterable object
  auto fromFn = std::make_shared<Function>();
  fromFn->isNative = true;
  fromFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto result = std::make_shared<Array>();

    if (args.empty()) {
      return Value(result);
    }

    const Value& arrayLike = args[0];

    // If it's already an array, copy it
    if (arrayLike.isArray()) {
      auto srcArray = std::get<std::shared_ptr<Array>>(arrayLike.data);
      for (const auto& elem : srcArray->elements) {
        result->elements.push_back(elem);
      }
      return Value(result);
    }

    // If it's a string, convert each character to array element
    if (arrayLike.isString()) {
      std::string str = std::get<std::string>(arrayLike.data);
      for (char c : str) {
        result->elements.push_back(Value(std::string(1, c)));
      }
      return Value(result);
    }

    // Otherwise return empty array
    return Value(result);
  };
  arrayObj->properties["from"] = Value(fromFn);

  // Array.of - creates array from arguments
  auto ofFn = std::make_shared<Function>();
  ofFn->isNative = true;
  ofFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto result = std::make_shared<Array>();
    result->elements = args;
    return Value(result);
  };
  arrayObj->properties["of"] = Value(ofFn);

  env->define("Array", Value(arrayObj));

  // Promise constructor
  auto promiseConstructor = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // Promise.resolve
  auto promiseResolve = std::make_shared<Function>();
  promiseResolve->isNative = true;
  promiseResolve->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto promise = std::make_shared<Promise>();
    if (!args.empty()) {
      promise->resolve(args[0]);
    } else {
      promise->resolve(Value(Undefined{}));
    }
    return Value(promise);
  };
  promiseConstructor->properties["resolve"] = Value(promiseResolve);

  // Promise.reject
  auto promiseReject = std::make_shared<Function>();
  promiseReject->isNative = true;
  promiseReject->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto promise = std::make_shared<Promise>();
    if (!args.empty()) {
      promise->reject(args[0]);
    } else {
      promise->reject(Value(Undefined{}));
    }
    return Value(promise);
  };
  promiseConstructor->properties["reject"] = Value(promiseReject);

  // Promise.all
  auto promiseAll = std::make_shared<Function>();
  promiseAll->isNative = true;
  promiseAll->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      auto promise = std::make_shared<Promise>();
      GarbageCollector::instance().reportAllocation(sizeof(Promise));
      promise->reject(Value(std::string("Promise.all expects an array")));
      return Value(promise);
    }

    auto arr = std::get<std::shared_ptr<Array>>(args[0].data);
    auto resultPromise = std::make_shared<Promise>();
    auto results = std::make_shared<Array>();

    bool hasRejection = false;
    for (const auto& elem : arr->elements) {
      if (elem.isPromise()) {
        auto p = std::get<std::shared_ptr<Promise>>(elem.data);
        if (p->state == PromiseState::Rejected) {
          hasRejection = true;
          resultPromise->reject(p->result);
          break;
        } else if (p->state == PromiseState::Fulfilled) {
          results->elements.push_back(p->result);
        }
      } else {
        results->elements.push_back(elem);
      }
    }

    if (!hasRejection) {
      resultPromise->resolve(Value(results));
    }

    return Value(resultPromise);
  };
  promiseConstructor->properties["all"] = Value(promiseAll);

  // Promise constructor function
  auto promiseFunc = std::make_shared<Function>();
  promiseFunc->isNative = true;
  promiseFunc->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto promise = std::make_shared<Promise>();

    if (!args.empty() && args[0].isFunction()) {
      auto executor = std::get<std::shared_ptr<Function>>(args[0].data);

      // Create resolve and reject functions
      auto resolveFunc = std::make_shared<Function>();
      resolveFunc->isNative = true;
      auto promisePtr = promise;
      resolveFunc->nativeFunc = [promisePtr](const std::vector<Value>& args) -> Value {
        if (!args.empty()) {
          promisePtr->resolve(args[0]);
        } else {
          promisePtr->resolve(Value(Undefined{}));
        }
        return Value(Undefined{});
      };

      auto rejectFunc = std::make_shared<Function>();
      rejectFunc->isNative = true;
      rejectFunc->nativeFunc = [promisePtr](const std::vector<Value>& args) -> Value {
        if (!args.empty()) {
          promisePtr->reject(args[0]);
        } else {
          promisePtr->reject(Value(Undefined{}));
        }
        return Value(Undefined{});
      };

      // Call executor with resolve and reject
      if (executor->isNative) {
        try {
          executor->nativeFunc({Value(resolveFunc), Value(rejectFunc)});
        } catch (const std::exception& e) {
          promise->reject(Value(std::string(e.what())));
        }
      }
    }

    return Value(promise);
  };

  // For now, define Promise as an object with static methods
  // In a full implementation, we'd need to make Function objects support properties
  env->define("Promise", Value(promiseConstructor));

  // JSON object
  auto jsonObj = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // JSON.parse
  auto jsonParse = std::make_shared<Function>();
  jsonParse->isNative = true;
  jsonParse->nativeFunc = JSON_parse;
  jsonObj->properties["parse"] = Value(jsonParse);

  // JSON.stringify
  auto jsonStringify = std::make_shared<Function>();
  jsonStringify->isNative = true;
  jsonStringify->nativeFunc = JSON_stringify;
  jsonObj->properties["stringify"] = Value(jsonStringify);

  env->define("JSON", Value(jsonObj));

  // Object static methods
  auto objectConstructor = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // Object.keys
  auto objectKeys = std::make_shared<Function>();
  objectKeys->isNative = true;
  objectKeys->nativeFunc = Object_keys;
  objectConstructor->properties["keys"] = Value(objectKeys);

  // Object.values
  auto objectValues = std::make_shared<Function>();
  objectValues->isNative = true;
  objectValues->nativeFunc = Object_values;
  objectConstructor->properties["values"] = Value(objectValues);

  // Object.entries
  auto objectEntries = std::make_shared<Function>();
  objectEntries->isNative = true;
  objectEntries->nativeFunc = Object_entries;
  objectConstructor->properties["entries"] = Value(objectEntries);

  // Object.assign
  auto objectAssign = std::make_shared<Function>();
  objectAssign->isNative = true;
  objectAssign->nativeFunc = Object_assign;
  objectConstructor->properties["assign"] = Value(objectAssign);

  // Object.hasOwnProperty (for prototypal access)
  auto objectHasOwnProperty = std::make_shared<Function>();
  objectHasOwnProperty->isNative = true;
  objectHasOwnProperty->nativeFunc = Object_hasOwnProperty;
  objectConstructor->properties["hasOwnProperty"] = Value(objectHasOwnProperty);

  // Object.getOwnPropertyNames
  auto objectGetOwnPropertyNames = std::make_shared<Function>();
  objectGetOwnPropertyNames->isNative = true;
  objectGetOwnPropertyNames->nativeFunc = Object_getOwnPropertyNames;
  objectConstructor->properties["getOwnPropertyNames"] = Value(objectGetOwnPropertyNames);

  // Object.create
  auto objectCreate = std::make_shared<Function>();
  objectCreate->isNative = true;
  objectCreate->nativeFunc = Object_create;
  objectConstructor->properties["create"] = Value(objectCreate);

  // Object.freeze - makes an object immutable
  auto objectFreeze = std::make_shared<Function>();
  objectFreeze->isNative = true;
  objectFreeze->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isObject()) {
      return args.empty() ? Value(Undefined{}) : args[0];
    }
    auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
    obj->frozen = true;
    obj->sealed = true;  // Frozen objects are also sealed
    return args[0];
  };
  objectConstructor->properties["freeze"] = Value(objectFreeze);

  // Object.seal - prevents adding or removing properties
  auto objectSeal = std::make_shared<Function>();
  objectSeal->isNative = true;
  objectSeal->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isObject()) {
      return args.empty() ? Value(Undefined{}) : args[0];
    }
    auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
    obj->sealed = true;
    return args[0];
  };
  objectConstructor->properties["seal"] = Value(objectSeal);

  // Object.isFrozen - check if object is frozen
  auto objectIsFrozen = std::make_shared<Function>();
  objectIsFrozen->isNative = true;
  objectIsFrozen->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isObject()) {
      return Value(true);  // Non-objects are considered frozen
    }
    auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
    return Value(obj->frozen);
  };
  objectConstructor->properties["isFrozen"] = Value(objectIsFrozen);

  // Object.isSealed - check if object is sealed
  auto objectIsSealed = std::make_shared<Function>();
  objectIsSealed->isNative = true;
  objectIsSealed->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isObject()) {
      return Value(true);  // Non-objects are considered sealed
    }
    auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
    return Value(obj->sealed);
  };
  objectConstructor->properties["isSealed"] = Value(objectIsSealed);

  env->define("Object", Value(objectConstructor));

  // Math object
  auto mathObj = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // Math constants
  mathObj->properties["PI"] = Value(3.141592653589793);
  mathObj->properties["E"] = Value(2.718281828459045);
  mathObj->properties["LN2"] = Value(0.6931471805599453);
  mathObj->properties["LN10"] = Value(2.302585092994046);
  mathObj->properties["LOG2E"] = Value(1.4426950408889634);
  mathObj->properties["LOG10E"] = Value(0.4342944819032518);
  mathObj->properties["SQRT1_2"] = Value(0.7071067811865476);
  mathObj->properties["SQRT2"] = Value(1.4142135623730951);

  // Math methods
  auto mathAbs = std::make_shared<Function>();
  mathAbs->isNative = true;
  mathAbs->nativeFunc = Math_abs;
  mathObj->properties["abs"] = Value(mathAbs);

  auto mathCeil = std::make_shared<Function>();
  mathCeil->isNative = true;
  mathCeil->nativeFunc = Math_ceil;
  mathObj->properties["ceil"] = Value(mathCeil);

  auto mathFloor = std::make_shared<Function>();
  mathFloor->isNative = true;
  mathFloor->nativeFunc = Math_floor;
  mathObj->properties["floor"] = Value(mathFloor);

  auto mathRound = std::make_shared<Function>();
  mathRound->isNative = true;
  mathRound->nativeFunc = Math_round;
  mathObj->properties["round"] = Value(mathRound);

  auto mathTrunc = std::make_shared<Function>();
  mathTrunc->isNative = true;
  mathTrunc->nativeFunc = Math_trunc;
  mathObj->properties["trunc"] = Value(mathTrunc);

  auto mathMax = std::make_shared<Function>();
  mathMax->isNative = true;
  mathMax->nativeFunc = Math_max;
  mathObj->properties["max"] = Value(mathMax);

  auto mathMin = std::make_shared<Function>();
  mathMin->isNative = true;
  mathMin->nativeFunc = Math_min;
  mathObj->properties["min"] = Value(mathMin);

  auto mathPow = std::make_shared<Function>();
  mathPow->isNative = true;
  mathPow->nativeFunc = Math_pow;
  mathObj->properties["pow"] = Value(mathPow);

  auto mathSqrt = std::make_shared<Function>();
  mathSqrt->isNative = true;
  mathSqrt->nativeFunc = Math_sqrt;
  mathObj->properties["sqrt"] = Value(mathSqrt);

  auto mathSin = std::make_shared<Function>();
  mathSin->isNative = true;
  mathSin->nativeFunc = Math_sin;
  mathObj->properties["sin"] = Value(mathSin);

  auto mathCos = std::make_shared<Function>();
  mathCos->isNative = true;
  mathCos->nativeFunc = Math_cos;
  mathObj->properties["cos"] = Value(mathCos);

  auto mathTan = std::make_shared<Function>();
  mathTan->isNative = true;
  mathTan->nativeFunc = Math_tan;
  mathObj->properties["tan"] = Value(mathTan);

  auto mathRandom = std::make_shared<Function>();
  mathRandom->isNative = true;
  mathRandom->nativeFunc = Math_random;
  mathObj->properties["random"] = Value(mathRandom);

  auto mathSign = std::make_shared<Function>();
  mathSign->isNative = true;
  mathSign->nativeFunc = Math_sign;
  mathObj->properties["sign"] = Value(mathSign);

  auto mathLog = std::make_shared<Function>();
  mathLog->isNative = true;
  mathLog->nativeFunc = Math_log;
  mathObj->properties["log"] = Value(mathLog);

  auto mathLog10 = std::make_shared<Function>();
  mathLog10->isNative = true;
  mathLog10->nativeFunc = Math_log10;
  mathObj->properties["log10"] = Value(mathLog10);

  auto mathExp = std::make_shared<Function>();
  mathExp->isNative = true;
  mathExp->nativeFunc = Math_exp;
  mathObj->properties["exp"] = Value(mathExp);

  env->define("Math", Value(mathObj));

  // Date constructor
  auto dateConstructor = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // Date as function
  auto dateFunc = std::make_shared<Function>();
  dateFunc->isNative = true;
  dateFunc->nativeFunc = Date_constructor;
  dateConstructor->properties["constructor"] = Value(dateFunc);

  // Date static methods
  auto dateNow = std::make_shared<Function>();
  dateNow->isNative = true;
  dateNow->nativeFunc = Date_now;
  dateConstructor->properties["now"] = Value(dateNow);

  auto dateParse = std::make_shared<Function>();
  dateParse->isNative = true;
  dateParse->nativeFunc = Date_parse;
  dateConstructor->properties["parse"] = Value(dateParse);

  env->define("Date", Value(dateConstructor));

  return env;
}

std::shared_ptr<Object> Environment::getGlobal() const {
  auto globalObj = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // Walk up to the root environment
  const Environment* current = this;
  while (current->parent_) {
    current = current->parent_.get();
  }

  // Add all global bindings to the object
  for (const auto& [name, value] : current->bindings_) {
    globalObj->properties[name] = value;
  }

  return globalObj;
}

}