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
#include "event_loop.h"
#include "symbols.h"
#include "module.h"
#include "interpreter.h"
#include "wasm_js.h"
#include "text_encoding.h"
#include "url.h"
#include "fs.h"
#include "streams.h"
#include <iostream>
#include <thread>
#include <limits>
#include <cmath>
#include <random>

namespace lightjs {

// Global module loader and interpreter for dynamic imports
static std::shared_ptr<ModuleLoader> g_moduleLoader;
static Interpreter* g_interpreter = nullptr;

void setGlobalModuleLoader(std::shared_ptr<ModuleLoader> loader) {
  g_moduleLoader = loader;
}

void setGlobalInterpreter(Interpreter* interpreter) {
  g_interpreter = interpreter;
}

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

  // console.error - writes to stderr
  auto consoleErrorFn = std::make_shared<Function>();
  consoleErrorFn->isNative = true;
  consoleErrorFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    for (const auto& arg : args) {
      std::cerr << arg.toString() << " ";
    }
    std::cerr << std::endl;
    return Value(Undefined{});
  };

  // console.warn - writes to stderr
  auto consoleWarnFn = std::make_shared<Function>();
  consoleWarnFn->isNative = true;
  consoleWarnFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    for (const auto& arg : args) {
      std::cerr << arg.toString() << " ";
    }
    std::cerr << std::endl;
    return Value(Undefined{});
  };

  // console.info - same as log
  auto consoleInfoFn = std::make_shared<Function>();
  consoleInfoFn->isNative = true;
  consoleInfoFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    for (const auto& arg : args) {
      std::cout << arg.toString() << " ";
    }
    std::cout << std::endl;
    return Value(Undefined{});
  };

  // console.debug - same as log
  auto consoleDebugFn = std::make_shared<Function>();
  consoleDebugFn->isNative = true;
  consoleDebugFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    for (const auto& arg : args) {
      std::cout << arg.toString() << " ";
    }
    std::cout << std::endl;
    return Value(Undefined{});
  };

  // console.time/timeEnd - simple timing
  static std::unordered_map<std::string, std::chrono::steady_clock::time_point> consoleTimers;

  auto consoleTimeFn = std::make_shared<Function>();
  consoleTimeFn->isNative = true;
  consoleTimeFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    std::string label = args.empty() ? "default" : args[0].toString();
    consoleTimers[label] = std::chrono::steady_clock::now();
    return Value(Undefined{});
  };

  auto consoleTimeEndFn = std::make_shared<Function>();
  consoleTimeEndFn->isNative = true;
  consoleTimeEndFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    std::string label = args.empty() ? "default" : args[0].toString();
    auto it = consoleTimers.find(label);
    if (it != consoleTimers.end()) {
      auto elapsed = std::chrono::steady_clock::now() - it->second;
      auto ms = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() / 1000.0;
      std::cout << label << ": " << ms << "ms" << std::endl;
      consoleTimers.erase(it);
    }
    return Value(Undefined{});
  };

  // console.assert
  auto consoleAssertFn = std::make_shared<Function>();
  consoleAssertFn->isNative = true;
  consoleAssertFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    bool condition = args.empty() ? false : args[0].toBool();
    if (!condition) {
      std::cerr << "Assertion failed:";
      for (size_t i = 1; i < args.size(); ++i) {
        std::cerr << " " << args[i].toString();
      }
      std::cerr << std::endl;
    }
    return Value(Undefined{});
  };

  auto consoleObj = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  consoleObj->properties["log"] = Value(consoleFn);
  consoleObj->properties["error"] = Value(consoleErrorFn);
  consoleObj->properties["warn"] = Value(consoleWarnFn);
  consoleObj->properties["info"] = Value(consoleInfoFn);
  consoleObj->properties["debug"] = Value(consoleDebugFn);
  consoleObj->properties["time"] = Value(consoleTimeFn);
  consoleObj->properties["timeEnd"] = Value(consoleTimeEndFn);
  consoleObj->properties["assert"] = Value(consoleAssertFn);

  env->define("console", Value(consoleObj));
  env->define("undefined", Value(Undefined{}));
  env->define("Infinity", Value(std::numeric_limits<double>::infinity()));
  env->define("NaN", Value(std::numeric_limits<double>::quiet_NaN()));

  // Symbol constructor
  auto symbolFn = std::make_shared<Function>();
  symbolFn->isNative = true;
  symbolFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    std::string description = args.empty() ? "" : args[0].toString();
    return Value(Symbol(description));
  };
  symbolFn->properties["iterator"] = WellKnownSymbols::iterator();
  symbolFn->properties["asyncIterator"] = WellKnownSymbols::asyncIterator();
  symbolFn->properties["toStringTag"] = WellKnownSymbols::toStringTag();
  env->define("Symbol", Value(symbolFn));

  auto createTypedArrayConstructor = [](TypedArrayType type) {
    auto func = std::make_shared<Function>();
    func->isNative = true;
    func->nativeFunc = [type](const std::vector<Value>& args) -> Value {
      if (args.empty()) {
        return Value(std::make_shared<TypedArray>(type, 0));
      }

      // Check if first argument is an array
      if (std::holds_alternative<std::shared_ptr<Array>>(args[0].data)) {
        auto arr = std::get<std::shared_ptr<Array>>(args[0].data);
        auto typedArray = std::make_shared<TypedArray>(type, arr->elements.size());

        // Fill the typed array with values from the regular array
        for (size_t i = 0; i < arr->elements.size(); ++i) {
          double val = arr->elements[i].toNumber();
          typedArray->setElement(i, val);
        }

        return Value(typedArray);
      }

      // Otherwise treat as length
      double lengthNum = args[0].toNumber();
      if (std::isnan(lengthNum) || std::isinf(lengthNum) || lengthNum < 0) {
        // Invalid length, return empty array
        return Value(std::make_shared<TypedArray>(type, 0));
      }

      size_t length = static_cast<size_t>(lengthNum);
      // Sanity check: prevent allocating huge arrays
      if (length > 1000000000) { // 1GB limit
        return Value(std::make_shared<TypedArray>(type, 0));
      }

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

  // ArrayBuffer constructor
  auto arrayBufferConstructor = std::make_shared<Function>();
  arrayBufferConstructor->isNative = true;
  arrayBufferConstructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    size_t length = 0;
    if (!args.empty()) {
      length = static_cast<size_t>(args[0].toNumber());
    }
    auto buffer = std::make_shared<ArrayBuffer>(length);
    GarbageCollector::instance().reportAllocation(length);
    return Value(buffer);
  };
  env->define("ArrayBuffer", Value(arrayBufferConstructor));

  // DataView constructor
  auto dataViewConstructor = std::make_shared<Function>();
  dataViewConstructor->isNative = true;
  dataViewConstructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArrayBuffer()) {
      throw std::runtime_error("TypeError: DataView requires an ArrayBuffer");
    }

    auto buffer = std::get<std::shared_ptr<ArrayBuffer>>(args[0].data);
    size_t byteOffset = 0;
    size_t byteLength = 0;

    if (args.size() > 1) {
      byteOffset = static_cast<size_t>(args[1].toNumber());
    }
    if (args.size() > 2) {
      byteLength = static_cast<size_t>(args[2].toNumber());
    }

    auto dataView = std::make_shared<DataView>(buffer, byteOffset, byteLength);
    GarbageCollector::instance().reportAllocation(sizeof(DataView));
    return Value(dataView);
  };
  env->define("DataView", Value(dataViewConstructor));

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

  // crypto.randomUUID - generate RFC 4122 version 4 UUID
  auto randomUUIDFn = std::make_shared<Function>();
  randomUUIDFn->isNative = true;
  randomUUIDFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // Generate 16 random bytes
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis(0, 255);

    uint8_t bytes[16];
    for (int i = 0; i < 16; ++i) {
      bytes[i] = static_cast<uint8_t>(dis(gen));
    }

    // Set version (4) and variant (RFC 4122)
    bytes[6] = (bytes[6] & 0x0f) | 0x40;  // Version 4
    bytes[8] = (bytes[8] & 0x3f) | 0x80;  // Variant RFC 4122

    // Format as UUID string
    char uuid[37];
    snprintf(uuid, sizeof(uuid),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11],
             bytes[12], bytes[13], bytes[14], bytes[15]);
    return Value(std::string(uuid));
  };
  cryptoObj->properties["randomUUID"] = Value(randomUUIDFn);

  // crypto.getRandomValues - fill typed array with random values
  auto getRandomValuesFn = std::make_shared<Function>();
  getRandomValuesFn->isNative = true;
  getRandomValuesFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isTypedArray()) {
      return Value(std::make_shared<Error>(ErrorType::TypeError, "getRandomValues requires a TypedArray"));
    }

    auto typedArray = std::get<std::shared_ptr<TypedArray>>(args[0].data);
    std::random_device rd;
    std::mt19937 gen(rd());

    // Fill buffer with random bytes
    std::uniform_int_distribution<uint32_t> dis(0, 255);
    for (size_t i = 0; i < typedArray->buffer.size(); ++i) {
      typedArray->buffer[i] = static_cast<uint8_t>(dis(gen));
    }

    return args[0];  // Return the same array
  };
  cryptoObj->properties["getRandomValues"] = Value(getRandomValuesFn);

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

  // Dynamic import() function - returns a Promise
  auto importFn = std::make_shared<Function>();
  importFn->isNative = true;
  importFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      auto promise = std::make_shared<Promise>();
      auto err = std::make_shared<Error>(ErrorType::TypeError, "import() requires a module specifier");
      promise->reject(Value(err));
      return Value(promise);
    }

    std::string specifier = args[0].toString();
    auto promise = std::make_shared<Promise>();

    try {
      // Use global module loader if available
      if (g_moduleLoader && g_interpreter) {
        // Resolve the module path
        std::string resolvedPath = g_moduleLoader->resolvePath(specifier);

        // Load the module
        auto module = g_moduleLoader->loadModule(resolvedPath);
        if (!module) {
          auto err = std::make_shared<Error>(ErrorType::Error, "Failed to load module: " + specifier);
          promise->reject(Value(err));
          return Value(promise);
        }

        // Instantiate the module
        if (!module->instantiate(g_moduleLoader.get())) {
          auto err = std::make_shared<Error>(ErrorType::Error, "Failed to instantiate module: " + specifier);
          promise->reject(Value(err));
          return Value(promise);
        }

        // Evaluate the module
        if (!module->evaluate(g_interpreter)) {
          auto err = std::make_shared<Error>(ErrorType::Error, "Failed to evaluate module: " + specifier);
          promise->reject(Value(err));
          return Value(promise);
        }

        // Create namespace object from exports
        auto moduleNamespace = std::make_shared<Object>();
        GarbageCollector::instance().reportAllocation(sizeof(Object));
        moduleNamespace->properties["__esModule"] = Value(true);

        auto exports = module->getAllExports();
        for (const auto& [name, value] : exports) {
          moduleNamespace->properties[name] = value;
        }

        promise->resolve(Value(moduleNamespace));
      } else {
        // Fallback: create a placeholder namespace (for cases where module loader isn't set up)
        auto moduleNamespace = std::make_shared<Object>();
        GarbageCollector::instance().reportAllocation(sizeof(Object));
        moduleNamespace->properties["__esModule"] = Value(true);
        moduleNamespace->properties["__moduleSpecifier"] = Value(specifier);
        promise->resolve(Value(moduleNamespace));
      }
    } catch (const std::exception& e) {
      auto err = std::make_shared<Error>(ErrorType::Error, std::string("Failed to load module: ") + e.what());
      promise->reject(Value(err));
    } catch (...) {
      auto err = std::make_shared<Error>(ErrorType::Error, "Failed to load module: " + specifier);
      promise->reject(Value(err));
    }

    return Value(promise);
  };
  env->define("import", Value(importFn));

  auto regExpConstructor = std::make_shared<Function>();
  regExpConstructor->isNative = true;
  regExpConstructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(Undefined{});
    std::string pattern = args[0].toString();
    std::string flags = args.size() > 1 ? args[1].toString() : "";
    return Value(std::make_shared<Regex>(pattern, flags));
  };
  env->define("RegExp", Value(regExpConstructor));

  // Error constructors
  auto createErrorConstructor = [](ErrorType type) {
    auto func = std::make_shared<Function>();
    func->isNative = true;
    func->nativeFunc = [type](const std::vector<Value>& args) -> Value {
      std::string message = args.empty() ? "" : args[0].toString();
      return Value(std::make_shared<Error>(type, message));
    };
    return Value(func);
  };

  env->define("Error", createErrorConstructor(ErrorType::Error));
  env->define("TypeError", createErrorConstructor(ErrorType::TypeError));
  env->define("ReferenceError", createErrorConstructor(ErrorType::ReferenceError));
  env->define("RangeError", createErrorConstructor(ErrorType::RangeError));
  env->define("SyntaxError", createErrorConstructor(ErrorType::SyntaxError));
  env->define("URIError", createErrorConstructor(ErrorType::URIError));
  env->define("EvalError", createErrorConstructor(ErrorType::EvalError));

  // WeakMap constructor
  auto weakMapConstructor = std::make_shared<Function>();
  weakMapConstructor->isNative = true;
  weakMapConstructor->nativeFunc = [](const std::vector<Value>&) -> Value {
    auto wm = std::make_shared<WeakMap>();
    GarbageCollector::instance().reportAllocation(sizeof(WeakMap));
    return Value(wm);
  };
  env->define("WeakMap", Value(weakMapConstructor));

  // WeakSet constructor
  auto weakSetConstructor = std::make_shared<Function>();
  weakSetConstructor->isNative = true;
  weakSetConstructor->nativeFunc = [](const std::vector<Value>&) -> Value {
    auto ws = std::make_shared<WeakSet>();
    GarbageCollector::instance().reportAllocation(sizeof(WeakSet));
    return Value(ws);
  };
  env->define("WeakSet", Value(weakSetConstructor));

  // Proxy constructor
  auto proxyConstructor = std::make_shared<Function>();
  proxyConstructor->isNative = true;
  proxyConstructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) {
      // Need target and handler
      return Value(Undefined{});
    }
    auto proxy = std::make_shared<Proxy>(args[0], args[1]);
    GarbageCollector::instance().reportAllocation(sizeof(Proxy));
    return Value(proxy);
  };
  env->define("Proxy", Value(proxyConstructor));

  // Reflect object with static methods
  auto reflectObj = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // Reflect.get(target, property)
  auto reflectGet = std::make_shared<Function>();
  reflectGet->isNative = true;
  reflectGet->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(Undefined{});

    const Value& target = args[0];
    std::string prop = args[1].toString();

    if (target.isObject()) {
      auto obj = std::get<std::shared_ptr<Object>>(target.data);
      auto it = obj->properties.find(prop);
      if (it != obj->properties.end()) {
        return it->second;
      }
    }
    return Value(Undefined{});
  };
  reflectObj->properties["get"] = Value(reflectGet);

  // Reflect.set(target, property, value)
  auto reflectSet = std::make_shared<Function>();
  reflectSet->isNative = true;
  reflectSet->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 3) return Value(false);

    const Value& target = args[0];
    std::string prop = args[1].toString();
    const Value& value = args[2];

    if (target.isObject()) {
      auto obj = std::get<std::shared_ptr<Object>>(target.data);
      obj->properties[prop] = value;
      return Value(true);
    }
    return Value(false);
  };
  reflectObj->properties["set"] = Value(reflectSet);

  // Reflect.has(target, property)
  auto reflectHas = std::make_shared<Function>();
  reflectHas->isNative = true;
  reflectHas->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(false);

    const Value& target = args[0];
    std::string prop = args[1].toString();

    if (target.isObject()) {
      auto obj = std::get<std::shared_ptr<Object>>(target.data);
      return Value(obj->properties.find(prop) != obj->properties.end());
    }
    return Value(false);
  };
  reflectObj->properties["has"] = Value(reflectHas);

  // Reflect.deleteProperty(target, property)
  auto reflectDelete = std::make_shared<Function>();
  reflectDelete->isNative = true;
  reflectDelete->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(false);

    const Value& target = args[0];
    std::string prop = args[1].toString();

    if (target.isObject()) {
      auto obj = std::get<std::shared_ptr<Object>>(target.data);
      return Value(obj->properties.erase(prop) > 0);
    }
    return Value(false);
  };
  reflectObj->properties["deleteProperty"] = Value(reflectDelete);

  // Reflect.apply(target, thisArg, argumentsList)
  auto reflectApply = std::make_shared<Function>();
  reflectApply->isNative = true;
  reflectApply->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 3 || !args[0].isFunction()) {
      return Value(Undefined{});
    }

    auto func = std::get<std::shared_ptr<Function>>(args[0].data);
    const Value& thisArg = args[1];

    std::vector<Value> callArgs;
    if (args[2].isArray()) {
      auto arr = std::get<std::shared_ptr<Array>>(args[2].data);
      callArgs = arr->elements;
    }

    if (func->isNative) {
      // For native functions, we can't pass 'this' directly
      // Native functions typically don't use 'this'
      return func->nativeFunc(callArgs);
    }
    // For non-native functions, we'd need interpreter access
    return Value(Undefined{});
  };
  reflectObj->properties["apply"] = Value(reflectApply);

  // Reflect.construct(target, argumentsList, newTarget?)
  auto reflectConstruct = std::make_shared<Function>();
  reflectConstruct->isNative = true;
  reflectConstruct->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2 || !args[0].isFunction()) {
      return Value(Undefined{});
    }

    auto func = std::get<std::shared_ptr<Function>>(args[0].data);

    std::vector<Value> callArgs;
    if (args[1].isArray()) {
      auto arr = std::get<std::shared_ptr<Array>>(args[1].data);
      callArgs = arr->elements;
    }

    if (func->isNative) {
      return func->nativeFunc(callArgs);
    }
    // For non-native functions, we'd need interpreter access
    return Value(Undefined{});
  };
  reflectObj->properties["construct"] = Value(reflectConstruct);

  // Reflect.getPrototypeOf(target) - returns null (limited prototype support)
  auto reflectGetPrototypeOf = std::make_shared<Function>();
  reflectGetPrototypeOf->isNative = true;
  reflectGetPrototypeOf->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(Null{});
    // LightJS has limited prototype support, return null
    return Value(Null{});
  };
  reflectObj->properties["getPrototypeOf"] = Value(reflectGetPrototypeOf);

  // Reflect.setPrototypeOf(target, proto) - returns false (not supported)
  auto reflectSetPrototypeOf = std::make_shared<Function>();
  reflectSetPrototypeOf->isNative = true;
  reflectSetPrototypeOf->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // LightJS doesn't support dynamic prototype modification
    return Value(false);
  };
  reflectObj->properties["setPrototypeOf"] = Value(reflectSetPrototypeOf);

  // Reflect.isExtensible(target) - check if object can be extended
  auto reflectIsExtensible = std::make_shared<Function>();
  reflectIsExtensible->isNative = true;
  reflectIsExtensible->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(false);
    if (args[0].isObject()) {
      auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
      return Value(!obj->sealed && !obj->frozen);
    }
    return Value(false);
  };
  reflectObj->properties["isExtensible"] = Value(reflectIsExtensible);

  // Reflect.preventExtensions(target) - prevent adding new properties
  auto reflectPreventExtensions = std::make_shared<Function>();
  reflectPreventExtensions->isNative = true;
  reflectPreventExtensions->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isObject()) {
      return Value(false);
    }
    auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
    obj->sealed = true;
    return Value(true);
  };
  reflectObj->properties["preventExtensions"] = Value(reflectPreventExtensions);

  // Reflect.ownKeys(target) - return array of own property keys
  auto reflectOwnKeys = std::make_shared<Function>();
  reflectOwnKeys->isNative = true;
  reflectOwnKeys->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto result = std::make_shared<Array>();

    if (args.empty()) return Value(result);

    if (args[0].isObject()) {
      auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
      for (const auto& [key, _] : obj->properties) {
        result->elements.push_back(Value(key));
      }
    } else if (args[0].isArray()) {
      auto arr = std::get<std::shared_ptr<Array>>(args[0].data);
      for (size_t i = 0; i < arr->elements.size(); ++i) {
        result->elements.push_back(Value(std::to_string(i)));
      }
      result->elements.push_back(Value("length"));
    }

    return Value(result);
  };
  reflectObj->properties["ownKeys"] = Value(reflectOwnKeys);

  // Reflect.getOwnPropertyDescriptor(target, propertyKey)
  auto reflectGetOwnPropertyDescriptor = std::make_shared<Function>();
  reflectGetOwnPropertyDescriptor->isNative = true;
  reflectGetOwnPropertyDescriptor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(Undefined{});

    std::string prop = args[1].toString();

    if (args[0].isObject()) {
      auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
      auto it = obj->properties.find(prop);
      if (it == obj->properties.end()) {
        return Value(Undefined{});
      }
      // Create a simplified property descriptor
      auto descriptor = std::make_shared<Object>();
      descriptor->properties["value"] = it->second;
      descriptor->properties["writable"] = Value(!obj->frozen);
      descriptor->properties["enumerable"] = Value(true);
      descriptor->properties["configurable"] = Value(!obj->sealed);
      return Value(descriptor);
    }

    return Value(Undefined{});
  };
  reflectObj->properties["getOwnPropertyDescriptor"] = Value(reflectGetOwnPropertyDescriptor);

  // Reflect.defineProperty(target, propertyKey, attributes)
  auto reflectDefineProperty = std::make_shared<Function>();
  reflectDefineProperty->isNative = true;
  reflectDefineProperty->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 3) return Value(false);
    if (!args[0].isObject()) return Value(false);

    auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
    std::string prop = args[1].toString();

    // Check if object is sealed/frozen
    bool isNewProp = obj->properties.find(prop) == obj->properties.end();
    if ((obj->sealed && isNewProp) || obj->frozen) {
      return Value(false);
    }

    // Get value from descriptor
    if (args[2].isObject()) {
      auto descriptor = std::get<std::shared_ptr<Object>>(args[2].data);
      auto valueIt = descriptor->properties.find("value");
      if (valueIt != descriptor->properties.end()) {
        obj->properties[prop] = valueIt->second;
        return Value(true);
      }
    }

    return Value(false);
  };
  reflectObj->properties["defineProperty"] = Value(reflectDefineProperty);

  env->define("Reflect", Value(reflectObj));

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

  // Number.isInteger
  auto isIntegerFn = std::make_shared<Function>();
  isIntegerFn->isNative = true;
  isIntegerFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isNumber()) return Value(false);
    double num = std::get<double>(args[0].data);
    return Value(std::isfinite(num) && std::floor(num) == num);
  };
  numberObj->properties["isInteger"] = Value(isIntegerFn);

  // Number.isSafeInteger
  auto isSafeIntegerFn = std::make_shared<Function>();
  isSafeIntegerFn->isNative = true;
  isSafeIntegerFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isNumber()) return Value(false);
    double num = std::get<double>(args[0].data);
    // Safe integers are integers in range [-(2^53-1), 2^53-1]
    const double MAX_SAFE_INTEGER = 9007199254740991.0;  // 2^53 - 1
    return Value(std::isfinite(num) && std::floor(num) == num &&
                 num >= -MAX_SAFE_INTEGER && num <= MAX_SAFE_INTEGER);
  };
  numberObj->properties["isSafeInteger"] = Value(isSafeIntegerFn);

  // Number constants
  numberObj->properties["MAX_VALUE"] = Value(std::numeric_limits<double>::max());
  numberObj->properties["MIN_VALUE"] = Value(std::numeric_limits<double>::min());
  numberObj->properties["POSITIVE_INFINITY"] = Value(std::numeric_limits<double>::infinity());
  numberObj->properties["NEGATIVE_INFINITY"] = Value(-std::numeric_limits<double>::infinity());
  numberObj->properties["NaN"] = Value(std::numeric_limits<double>::quiet_NaN());
  numberObj->properties["MAX_SAFE_INTEGER"] = Value(9007199254740991.0);  // 2^53 - 1
  numberObj->properties["MIN_SAFE_INTEGER"] = Value(-9007199254740991.0);  // -(2^53 - 1)
  numberObj->properties["EPSILON"] = Value(2.220446049250313e-16);  // 2^-52

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

  // Promise.allSettled - waits for all promises to settle (resolve or reject)
  auto promiseAllSettled = std::make_shared<Function>();
  promiseAllSettled->isNative = true;
  promiseAllSettled->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      auto promise = std::make_shared<Promise>();
      GarbageCollector::instance().reportAllocation(sizeof(Promise));
      promise->reject(Value(std::string("Promise.allSettled expects an array")));
      return Value(promise);
    }

    auto arr = std::get<std::shared_ptr<Array>>(args[0].data);
    auto resultPromise = std::make_shared<Promise>();
    auto results = std::make_shared<Array>();

    for (const auto& elem : arr->elements) {
      auto resultObj = std::make_shared<Object>();
      if (elem.isPromise()) {
        auto p = std::get<std::shared_ptr<Promise>>(elem.data);
        if (p->state == PromiseState::Fulfilled) {
          resultObj->properties["status"] = Value(std::string("fulfilled"));
          resultObj->properties["value"] = p->result;
        } else if (p->state == PromiseState::Rejected) {
          resultObj->properties["status"] = Value(std::string("rejected"));
          resultObj->properties["reason"] = p->result;
        } else {
          // Pending - treat as fulfilled with the promise itself
          resultObj->properties["status"] = Value(std::string("fulfilled"));
          resultObj->properties["value"] = elem;
        }
      } else {
        // Non-promise values are treated as fulfilled
        resultObj->properties["status"] = Value(std::string("fulfilled"));
        resultObj->properties["value"] = elem;
      }
      results->elements.push_back(Value(resultObj));
    }

    resultPromise->resolve(Value(results));
    return Value(resultPromise);
  };
  promiseConstructor->properties["allSettled"] = Value(promiseAllSettled);

  // Promise.any - resolves when any promise fulfills, rejects if all reject
  auto promiseAny = std::make_shared<Function>();
  promiseAny->isNative = true;
  promiseAny->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      auto promise = std::make_shared<Promise>();
      GarbageCollector::instance().reportAllocation(sizeof(Promise));
      promise->reject(Value(std::string("Promise.any expects an array")));
      return Value(promise);
    }

    auto arr = std::get<std::shared_ptr<Array>>(args[0].data);
    auto resultPromise = std::make_shared<Promise>();

    if (arr->elements.empty()) {
      // Empty array - reject with AggregateError
      auto err = std::make_shared<Error>(ErrorType::Error, "All promises were rejected");
      resultPromise->reject(Value(err));
      return Value(resultPromise);
    }

    auto errors = std::make_shared<Array>();
    bool hasResolved = false;

    for (const auto& elem : arr->elements) {
      if (elem.isPromise()) {
        auto p = std::get<std::shared_ptr<Promise>>(elem.data);
        if (p->state == PromiseState::Fulfilled && !hasResolved) {
          hasResolved = true;
          resultPromise->resolve(p->result);
          break;
        } else if (p->state == PromiseState::Rejected) {
          errors->elements.push_back(p->result);
        }
      } else {
        // Non-promise values are treated as fulfilled
        if (!hasResolved) {
          hasResolved = true;
          resultPromise->resolve(elem);
          break;
        }
      }
    }

    if (!hasResolved) {
      // All rejected - create AggregateError-like object
      auto err = std::make_shared<Error>(ErrorType::Error, "All promises were rejected");
      resultPromise->reject(Value(err));
    }

    return Value(resultPromise);
  };
  promiseConstructor->properties["any"] = Value(promiseAny);

  // Promise.race - resolves or rejects with the first settled promise
  auto promiseRace = std::make_shared<Function>();
  promiseRace->isNative = true;
  promiseRace->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      auto promise = std::make_shared<Promise>();
      GarbageCollector::instance().reportAllocation(sizeof(Promise));
      promise->reject(Value(std::string("Promise.race expects an array")));
      return Value(promise);
    }

    auto arr = std::get<std::shared_ptr<Array>>(args[0].data);
    auto resultPromise = std::make_shared<Promise>();

    for (const auto& elem : arr->elements) {
      if (elem.isPromise()) {
        auto p = std::get<std::shared_ptr<Promise>>(elem.data);
        if (p->state == PromiseState::Fulfilled) {
          resultPromise->resolve(p->result);
          break;
        } else if (p->state == PromiseState::Rejected) {
          resultPromise->reject(p->result);
          break;
        }
      } else {
        // Non-promise value settles immediately
        resultPromise->resolve(elem);
        break;
      }
    }

    return Value(resultPromise);
  };
  promiseConstructor->properties["race"] = Value(promiseRace);

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

  // Object.fromEntries - converts array of [key, value] pairs to object
  auto objectFromEntries = std::make_shared<Function>();
  objectFromEntries->isNative = true;
  objectFromEntries->nativeFunc = Object_fromEntries;
  objectConstructor->properties["fromEntries"] = Value(objectFromEntries);

  // Object.hasOwn - checks if object has own property
  auto objectHasOwn = std::make_shared<Function>();
  objectHasOwn->isNative = true;
  objectHasOwn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(false);
    std::string key = args[1].toString();
    if (args[0].isObject()) {
      auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
      return Value(obj->properties.find(key) != obj->properties.end());
    }
    if (args[0].isArray()) {
      auto arr = std::get<std::shared_ptr<Array>>(args[0].data);
      // Check numeric index
      try {
        size_t idx = std::stoul(key);
        return Value(idx < arr->elements.size());
      } catch (...) {
        return Value(false);
      }
    }
    return Value(false);
  };
  objectConstructor->properties["hasOwn"] = Value(objectHasOwn);

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

  // Object.is - SameValue comparison (handles NaN and -0 correctly)
  auto objectIs = std::make_shared<Function>();
  objectIs->isNative = true;
  objectIs->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(false);
    const Value& a = args[0];
    const Value& b = args[1];

    // Check for same type
    if (a.data.index() != b.data.index()) return Value(false);

    // Handle numbers specially (NaN === NaN, +0 !== -0)
    if (a.isNumber() && b.isNumber()) {
      double x = a.toNumber();
      double y = b.toNumber();
      // NaN is equal to NaN in Object.is
      if (std::isnan(x) && std::isnan(y)) return Value(true);
      // +0 and -0 are different in Object.is
      if (x == 0 && y == 0) {
        return Value(std::signbit(x) == std::signbit(y));
      }
      return Value(x == y);
    }

    // For other types, use regular equality
    if (a.isUndefined() && b.isUndefined()) return Value(true);
    if (a.isNull() && b.isNull()) return Value(true);
    if (a.isBool() && b.isBool()) {
      return Value(std::get<bool>(a.data) == std::get<bool>(b.data));
    }
    if (a.isString() && b.isString()) {
      return Value(std::get<std::string>(a.data) == std::get<std::string>(b.data));
    }
    // For objects, check reference equality
    if (a.isObject() && b.isObject()) {
      return Value(std::get<std::shared_ptr<Object>>(a.data).get() ==
                   std::get<std::shared_ptr<Object>>(b.data).get());
    }
    if (a.isArray() && b.isArray()) {
      return Value(std::get<std::shared_ptr<Array>>(a.data).get() ==
                   std::get<std::shared_ptr<Array>>(b.data).get());
    }
    if (a.isFunction() && b.isFunction()) {
      return Value(std::get<std::shared_ptr<Function>>(a.data).get() ==
                   std::get<std::shared_ptr<Function>>(b.data).get());
    }

    return Value(false);
  };
  objectConstructor->properties["is"] = Value(objectIs);

  // Object.getOwnPropertyDescriptor - get property descriptor
  auto objectGetOwnPropertyDescriptor = std::make_shared<Function>();
  objectGetOwnPropertyDescriptor->isNative = true;
  objectGetOwnPropertyDescriptor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2 || !args[0].isObject()) {
      return Value(Undefined{});
    }
    auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
    std::string key = args[1].toString();

    auto it = obj->properties.find(key);
    if (it == obj->properties.end()) {
      return Value(Undefined{});
    }

    auto descriptor = std::make_shared<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    descriptor->properties["value"] = it->second;
    descriptor->properties["writable"] = Value(!obj->frozen);
    descriptor->properties["enumerable"] = Value(true);
    descriptor->properties["configurable"] = Value(!obj->sealed);
    return Value(descriptor);
  };
  objectConstructor->properties["getOwnPropertyDescriptor"] = Value(objectGetOwnPropertyDescriptor);

  // Object.defineProperty - define property with descriptor
  auto objectDefineProperty = std::make_shared<Function>();
  objectDefineProperty->isNative = true;
  objectDefineProperty->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 3 || !args[0].isObject()) {
      return args.empty() ? Value(Undefined{}) : args[0];
    }
    auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
    std::string key = args[1].toString();

    if (obj->frozen) {
      return args[0];  // Cannot modify frozen object
    }

    // Check if adding new property to sealed object
    if (obj->sealed && obj->properties.find(key) == obj->properties.end()) {
      return args[0];  // Cannot add new property to sealed object
    }

    if (args[2].isObject()) {
      auto descriptor = std::get<std::shared_ptr<Object>>(args[2].data);
      auto valueIt = descriptor->properties.find("value");
      if (valueIt != descriptor->properties.end()) {
        obj->properties[key] = valueIt->second;
      }

      // Check for getter
      auto getIt = descriptor->properties.find("get");
      if (getIt != descriptor->properties.end() && getIt->second.isFunction()) {
        obj->properties["__get_" + key] = getIt->second;
      }

      // Check for setter
      auto setIt = descriptor->properties.find("set");
      if (setIt != descriptor->properties.end() && setIt->second.isFunction()) {
        obj->properties["__set_" + key] = setIt->second;
      }
    }
    return args[0];
  };
  objectConstructor->properties["defineProperty"] = Value(objectDefineProperty);

  // Object.defineProperties - define multiple properties
  auto objectDefineProperties = std::make_shared<Function>();
  objectDefineProperties->isNative = true;
  objectDefineProperties->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2 || !args[0].isObject() || !args[1].isObject()) {
      return args.empty() ? Value(Undefined{}) : args[0];
    }
    auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
    auto props = std::get<std::shared_ptr<Object>>(args[1].data);

    if (obj->frozen) {
      return args[0];
    }

    for (const auto& [key, descriptor] : props->properties) {
      if (obj->sealed && obj->properties.find(key) == obj->properties.end()) {
        continue;  // Skip new properties on sealed object
      }
      if (descriptor.isObject()) {
        auto descObj = std::get<std::shared_ptr<Object>>(descriptor.data);
        auto valueIt = descObj->properties.find("value");
        if (valueIt != descObj->properties.end()) {
          obj->properties[key] = valueIt->second;
        }
      }
    }
    return args[0];
  };
  objectConstructor->properties["defineProperties"] = Value(objectDefineProperties);

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

  auto mathCbrt = std::make_shared<Function>();
  mathCbrt->isNative = true;
  mathCbrt->nativeFunc = Math_cbrt;
  mathObj->properties["cbrt"] = Value(mathCbrt);

  auto mathLog2 = std::make_shared<Function>();
  mathLog2->isNative = true;
  mathLog2->nativeFunc = Math_log2;
  mathObj->properties["log2"] = Value(mathLog2);

  auto mathHypot = std::make_shared<Function>();
  mathHypot->isNative = true;
  mathHypot->nativeFunc = Math_hypot;
  mathObj->properties["hypot"] = Value(mathHypot);

  auto mathExpm1 = std::make_shared<Function>();
  mathExpm1->isNative = true;
  mathExpm1->nativeFunc = Math_expm1;
  mathObj->properties["expm1"] = Value(mathExpm1);

  auto mathLog1p = std::make_shared<Function>();
  mathLog1p->isNative = true;
  mathLog1p->nativeFunc = Math_log1p;
  mathObj->properties["log1p"] = Value(mathLog1p);

  auto mathFround = std::make_shared<Function>();
  mathFround->isNative = true;
  mathFround->nativeFunc = Math_fround;
  mathObj->properties["fround"] = Value(mathFround);

  auto mathClz32 = std::make_shared<Function>();
  mathClz32->isNative = true;
  mathClz32->nativeFunc = Math_clz32;
  mathObj->properties["clz32"] = Value(mathClz32);

  auto mathImul = std::make_shared<Function>();
  mathImul->isNative = true;
  mathImul->nativeFunc = Math_imul;
  mathObj->properties["imul"] = Value(mathImul);

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

  // String constructor with static methods
  auto stringConstructorFn = std::make_shared<Function>();
  stringConstructorFn->isNative = true;
  stringConstructorFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(std::string(""));
    }
    return Value(args[0].toString());
  };

  // Wrap in an Object to hold static methods
  auto stringConstructorObj = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // The constructor itself
  stringConstructorObj->properties["constructor"] = Value(stringConstructorFn);

  // String.fromCharCode
  auto fromCharCode = std::make_shared<Function>();
  fromCharCode->isNative = true;
  fromCharCode->nativeFunc = String_fromCharCode;
  stringConstructorObj->properties["fromCharCode"] = Value(fromCharCode);

  // String.fromCodePoint
  auto fromCodePoint = std::make_shared<Function>();
  fromCodePoint->isNative = true;
  fromCodePoint->nativeFunc = String_fromCodePoint;
  stringConstructorObj->properties["fromCodePoint"] = Value(fromCodePoint);

  // For simplicity, we can make the Object callable by storing the function
  env->define("String", Value(stringConstructorObj));

  // WebAssembly global object
  env->define("WebAssembly", wasm_js::createWebAssemblyGlobal());

  // globalThis - reference to the global object
  // Create a proxy object that reflects the current global environment
  auto globalThisObj = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // Copy all current global bindings into globalThis
  for (const auto& [name, value] : env->bindings_) {
    globalThisObj->properties[name] = value;
  }

  // Define globalThis pointing to the global object
  env->define("globalThis", Value(globalThisObj));

  // Also add globalThis to itself
  globalThisObj->properties["globalThis"] = Value(globalThisObj);

  // Timer functions - setTimeout
  auto setTimeoutFn = std::make_shared<Function>();
  setTimeoutFn->isNative = true;
  setTimeoutFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isFunction()) {
      return Value(Undefined{});
    }

    auto callback = std::get<std::shared_ptr<Function>>(args[0].data);
    int64_t delayMs = args.size() > 1 ? static_cast<int64_t>(args[1].toNumber()) : 0;

    // Create a timer callback that executes the JS function
    auto& loop = EventLoopContext::instance().getLoop();
    TimerId id = loop.setTimeout([callback]() -> Value {
      // Execute the callback function
      if (callback->isNative && callback->nativeFunc) {
        return callback->nativeFunc({});
      }
      // For non-native functions, we can't easily execute them here
      // This will be improved when we integrate with the interpreter
      return Value(Undefined{});
    }, delayMs);

    return Value(static_cast<double>(id));
  };
  env->define("setTimeout", Value(setTimeoutFn));

  // Timer functions - setInterval
  auto setIntervalFn = std::make_shared<Function>();
  setIntervalFn->isNative = true;
  setIntervalFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isFunction()) {
      return Value(Undefined{});
    }

    auto callback = std::get<std::shared_ptr<Function>>(args[0].data);
    int64_t intervalMs = args.size() > 1 ? static_cast<int64_t>(args[1].toNumber()) : 0;

    // Create an interval timer callback
    auto& loop = EventLoopContext::instance().getLoop();
    TimerId id = loop.setInterval([callback]() -> Value {
      // Execute the callback function
      if (callback->isNative && callback->nativeFunc) {
        return callback->nativeFunc({});
      }
      return Value(Undefined{});
    }, intervalMs);

    return Value(static_cast<double>(id));
  };
  env->define("setInterval", Value(setIntervalFn));

  // Timer functions - clearTimeout
  auto clearTimeoutFn = std::make_shared<Function>();
  clearTimeoutFn->isNative = true;
  clearTimeoutFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(Undefined{});
    }

    TimerId id = static_cast<TimerId>(args[0].toNumber());
    auto& loop = EventLoopContext::instance().getLoop();
    loop.clearTimer(id);

    return Value(Undefined{});
  };
  env->define("clearTimeout", Value(clearTimeoutFn));

  // Timer functions - clearInterval (same implementation as clearTimeout)
  auto clearIntervalFn = std::make_shared<Function>();
  clearIntervalFn->isNative = true;
  clearIntervalFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(Undefined{});
    }

    TimerId id = static_cast<TimerId>(args[0].toNumber());
    auto& loop = EventLoopContext::instance().getLoop();
    loop.clearTimer(id);

    return Value(Undefined{});
  };
  env->define("clearInterval", Value(clearIntervalFn));

  // queueMicrotask function
  auto queueMicrotaskFn = std::make_shared<Function>();
  queueMicrotaskFn->isNative = true;
  queueMicrotaskFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isFunction()) {
      return Value(Undefined{});
    }

    auto callback = std::get<std::shared_ptr<Function>>(args[0].data);

    // Queue the microtask
    auto& loop = EventLoopContext::instance().getLoop();
    loop.queueMicrotask([callback]() {
      // Execute the callback function
      if (callback->isNative && callback->nativeFunc) {
        callback->nativeFunc({});
      }
    });

    return Value(Undefined{});
  };
  env->define("queueMicrotask", Value(queueMicrotaskFn));

  // TextEncoder and TextDecoder
  env->define("TextEncoder", Value(createTextEncoderConstructor()));
  env->define("TextDecoder", Value(createTextDecoderConstructor()));

  // URL and URLSearchParams
  env->define("URL", Value(createURLConstructor()));
  env->define("URLSearchParams", Value(createURLSearchParamsConstructor()));

  // AbortController and AbortSignal
  auto abortControllerCtor = std::make_shared<Function>();
  abortControllerCtor->isNative = true;
  abortControllerCtor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto controller = std::make_shared<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));

    // Create AbortSignal
    auto signal = std::make_shared<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    signal->properties["aborted"] = Value(false);
    signal->properties["reason"] = Value(Undefined{});

    // Event listeners storage
    auto listeners = std::make_shared<Array>();
    signal->properties["_listeners"] = Value(listeners);

    // addEventListener method
    auto addEventListenerFn = std::make_shared<Function>();
    addEventListenerFn->isNative = true;
    addEventListenerFn->nativeFunc = [listeners](const std::vector<Value>& args) -> Value {
      if (args.size() >= 2 && args[0].toString() == "abort" && args[1].isFunction()) {
        listeners->elements.push_back(args[1]);
      }
      return Value(Undefined{});
    };
    signal->properties["addEventListener"] = Value(addEventListenerFn);

    // removeEventListener method
    auto removeEventListenerFn = std::make_shared<Function>();
    removeEventListenerFn->isNative = true;
    removeEventListenerFn->nativeFunc = [listeners](const std::vector<Value>& args) -> Value {
      // Simple implementation - just mark for removal
      return Value(Undefined{});
    };
    signal->properties["removeEventListener"] = Value(removeEventListenerFn);

    controller->properties["signal"] = Value(signal);

    // abort method
    auto abortFn = std::make_shared<Function>();
    abortFn->isNative = true;
    abortFn->nativeFunc = [signal, listeners](const std::vector<Value>& args) -> Value {
      // Check if already aborted
      if (signal->properties["aborted"].toBool()) {
        return Value(Undefined{});
      }

      signal->properties["aborted"] = Value(true);
      signal->properties["reason"] = args.empty() ?
          Value(std::string("AbortError: The operation was aborted")) : args[0];

      // Call all abort listeners
      for (const auto& listener : listeners->elements) {
        if (listener.isFunction()) {
          auto fn = std::get<std::shared_ptr<Function>>(listener.data);
          if (fn->isNative && fn->nativeFunc) {
            fn->nativeFunc({});
          }
        }
      }

      return Value(Undefined{});
    };
    controller->properties["abort"] = Value(abortFn);

    return Value(controller);
  };
  env->define("AbortController", Value(abortControllerCtor));

  // AbortSignal.abort() static method
  auto abortSignalObj = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  auto abortStaticFn = std::make_shared<Function>();
  abortStaticFn->isNative = true;
  abortStaticFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto signal = std::make_shared<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    signal->properties["aborted"] = Value(true);
    signal->properties["reason"] = args.empty() ?
        Value(std::string("AbortError: The operation was aborted")) : args[0];
    return Value(signal);
  };
  abortSignalObj->properties["abort"] = Value(abortStaticFn);

  // AbortSignal.timeout() static method
  auto timeoutStaticFn = std::make_shared<Function>();
  timeoutStaticFn->isNative = true;
  timeoutStaticFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto signal = std::make_shared<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    signal->properties["aborted"] = Value(false);
    signal->properties["reason"] = Value(Undefined{});
    // Note: Actual timeout implementation would require event loop integration
    return Value(signal);
  };
  abortSignalObj->properties["timeout"] = Value(timeoutStaticFn);

  env->define("AbortSignal", Value(abortSignalObj));

  // Streams API - ReadableStream
  auto readableStreamCtor = std::make_shared<Function>();
  readableStreamCtor->isNative = true;
  readableStreamCtor->isConstructor = true;
  readableStreamCtor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // Create underlying source callbacks from argument
    std::shared_ptr<Function> startFn = nullptr;
    std::shared_ptr<Function> pullFn = nullptr;
    std::shared_ptr<Function> cancelFn = nullptr;
    double highWaterMark = 1.0;

    if (!args.empty() && args[0].isObject()) {
      auto srcObj = std::get<std::shared_ptr<Object>>(args[0].data);

      // Get start callback
      auto startIt = srcObj->properties.find("start");
      if (startIt != srcObj->properties.end() && startIt->second.isFunction()) {
        startFn = std::get<std::shared_ptr<Function>>(startIt->second.data);
      }

      // Get pull callback
      auto pullIt = srcObj->properties.find("pull");
      if (pullIt != srcObj->properties.end() && pullIt->second.isFunction()) {
        pullFn = std::get<std::shared_ptr<Function>>(pullIt->second.data);
      }

      // Get cancel callback
      auto cancelIt = srcObj->properties.find("cancel");
      if (cancelIt != srcObj->properties.end() && cancelIt->second.isFunction()) {
        cancelFn = std::get<std::shared_ptr<Function>>(cancelIt->second.data);
      }
    }

    // Create the stream
    auto stream = createReadableStream(startFn, pullFn, cancelFn, highWaterMark);
    return Value(stream);
  };
  env->define("ReadableStream", Value(readableStreamCtor));

  // Streams API - WritableStream
  auto writableStreamCtor = std::make_shared<Function>();
  writableStreamCtor->isNative = true;
  writableStreamCtor->isConstructor = true;
  writableStreamCtor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // Create underlying sink callbacks from argument
    std::shared_ptr<Function> startFn = nullptr;
    std::shared_ptr<Function> writeFn = nullptr;
    std::shared_ptr<Function> closeFn = nullptr;
    std::shared_ptr<Function> abortFn = nullptr;
    double highWaterMark = 1.0;

    if (!args.empty() && args[0].isObject()) {
      auto sinkObj = std::get<std::shared_ptr<Object>>(args[0].data);

      // Get start callback
      auto startIt = sinkObj->properties.find("start");
      if (startIt != sinkObj->properties.end() && startIt->second.isFunction()) {
        startFn = std::get<std::shared_ptr<Function>>(startIt->second.data);
      }

      // Get write callback
      auto writeIt = sinkObj->properties.find("write");
      if (writeIt != sinkObj->properties.end() && writeIt->second.isFunction()) {
        writeFn = std::get<std::shared_ptr<Function>>(writeIt->second.data);
      }

      // Get close callback
      auto closeIt = sinkObj->properties.find("close");
      if (closeIt != sinkObj->properties.end() && closeIt->second.isFunction()) {
        closeFn = std::get<std::shared_ptr<Function>>(closeIt->second.data);
      }

      // Get abort callback
      auto abortIt = sinkObj->properties.find("abort");
      if (abortIt != sinkObj->properties.end() && abortIt->second.isFunction()) {
        abortFn = std::get<std::shared_ptr<Function>>(abortIt->second.data);
      }
    }

    // Create the stream
    auto stream = createWritableStream(startFn, writeFn, closeFn, abortFn, highWaterMark);
    return Value(stream);
  };
  env->define("WritableStream", Value(writableStreamCtor));

  // Streams API - TransformStream
  auto transformStreamCtor = std::make_shared<Function>();
  transformStreamCtor->isNative = true;
  transformStreamCtor->isConstructor = true;
  transformStreamCtor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // Create transformer callbacks from argument
    std::shared_ptr<Function> startFn = nullptr;
    std::shared_ptr<Function> transformFn = nullptr;
    std::shared_ptr<Function> flushFn = nullptr;

    if (!args.empty() && args[0].isObject()) {
      auto transformerObj = std::get<std::shared_ptr<Object>>(args[0].data);

      // Get start callback
      auto startIt = transformerObj->properties.find("start");
      if (startIt != transformerObj->properties.end() && startIt->second.isFunction()) {
        startFn = std::get<std::shared_ptr<Function>>(startIt->second.data);
      }

      // Get transform callback
      auto transformIt = transformerObj->properties.find("transform");
      if (transformIt != transformerObj->properties.end() && transformIt->second.isFunction()) {
        transformFn = std::get<std::shared_ptr<Function>>(transformIt->second.data);
      }

      // Get flush callback
      auto flushIt = transformerObj->properties.find("flush");
      if (flushIt != transformerObj->properties.end() && flushIt->second.isFunction()) {
        flushFn = std::get<std::shared_ptr<Function>>(flushIt->second.data);
      }
    }

    // Create the stream
    auto stream = createTransformStream(startFn, transformFn, flushFn);
    return Value(stream);
  };
  env->define("TransformStream", Value(transformStreamCtor));

  // File System module (fs)
  globalThisObj->properties["fs"] = Value(createFSModule());

  // performance.now() - high-resolution timing
  static auto startTime = std::chrono::steady_clock::now();

  auto performanceNowFn = std::make_shared<Function>();
  performanceNowFn->isNative = true;
  performanceNowFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - startTime).count();
    return Value(static_cast<double>(elapsed) / 1000.0);  // Return milliseconds
  };

  auto performanceObj = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  performanceObj->properties["now"] = Value(performanceNowFn);
  env->define("performance", Value(performanceObj));
  globalThisObj->properties["performance"] = Value(performanceObj);

  // structuredClone - deep clone objects, arrays, and primitives
  std::function<Value(const Value&)> deepClone;
  deepClone = [&deepClone](const Value& val) -> Value {
    // Primitives are returned as-is (they're already copies)
    if (val.isUndefined() || val.isNull() || val.isBool() ||
        val.isNumber() || val.isString() || val.isBigInt() || val.isSymbol()) {
      return val;
    }

    // Clone arrays
    if (val.isArray()) {
      auto arr = std::get<std::shared_ptr<Array>>(val.data);
      auto newArr = std::make_shared<Array>();
      GarbageCollector::instance().reportAllocation(sizeof(Array));
      for (const auto& elem : arr->elements) {
        newArr->elements.push_back(deepClone(elem));
      }
      return Value(newArr);
    }

    // Clone objects
    if (val.isObject()) {
      auto obj = std::get<std::shared_ptr<Object>>(val.data);
      auto newObj = std::make_shared<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      for (const auto& [key, value] : obj->properties) {
        newObj->properties[key] = deepClone(value);
      }
      return Value(newObj);
    }

    // Functions, Promises, etc. cannot be cloned - return as-is
    return val;
  };

  auto structuredCloneFn = std::make_shared<Function>();
  structuredCloneFn->isNative = true;
  structuredCloneFn->nativeFunc = [deepClone](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(Undefined{});
    return deepClone(args[0]);
  };
  env->define("structuredClone", Value(structuredCloneFn));
  globalThisObj->properties["structuredClone"] = Value(structuredCloneFn);

  // Base64 encoding table
  static const char base64Chars[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  // btoa - encode string to Base64
  auto btoaFn = std::make_shared<Function>();
  btoaFn->isNative = true;
  btoaFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::string(""));
    std::string input = args[0].toString();
    std::string result;
    result.reserve((input.size() + 2) / 3 * 4);

    for (size_t i = 0; i < input.size(); i += 3) {
      uint32_t n = static_cast<uint8_t>(input[i]) << 16;
      if (i + 1 < input.size()) n |= static_cast<uint8_t>(input[i + 1]) << 8;
      if (i + 2 < input.size()) n |= static_cast<uint8_t>(input[i + 2]);

      result += base64Chars[(n >> 18) & 0x3F];
      result += base64Chars[(n >> 12) & 0x3F];
      result += (i + 1 < input.size()) ? base64Chars[(n >> 6) & 0x3F] : '=';
      result += (i + 2 < input.size()) ? base64Chars[n & 0x3F] : '=';
    }
    return Value(result);
  };
  env->define("btoa", Value(btoaFn));
  globalThisObj->properties["btoa"] = Value(btoaFn);

  // atob - decode Base64 to string
  auto atobFn = std::make_shared<Function>();
  atobFn->isNative = true;
  atobFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::string(""));
    std::string input = args[0].toString();

    // Build decode lookup table
    static int decodeTable[256] = {-1};
    static bool tableInit = false;
    if (!tableInit) {
      for (int i = 0; i < 256; ++i) decodeTable[i] = -1;
      for (int i = 0; i < 64; ++i) decodeTable[static_cast<uint8_t>(base64Chars[i])] = i;
      tableInit = true;
    }

    std::string result;
    result.reserve(input.size() * 3 / 4);

    int bits = 0;
    int bitCount = 0;
    for (char c : input) {
      if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
      int value = decodeTable[static_cast<uint8_t>(c)];
      if (value == -1) continue;  // Skip invalid characters

      bits = (bits << 6) | value;
      bitCount += 6;

      if (bitCount >= 8) {
        bitCount -= 8;
        result += static_cast<char>((bits >> bitCount) & 0xFF);
      }
    }
    return Value(result);
  };
  env->define("atob", Value(atobFn));
  globalThisObj->properties["atob"] = Value(atobFn);

  // encodeURIComponent - encode URI component
  auto encodeURIComponentFn = std::make_shared<Function>();
  encodeURIComponentFn->isNative = true;
  encodeURIComponentFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::string("undefined"));
    std::string input = args[0].toString();
    std::string result;
    result.reserve(input.size() * 3);  // Worst case

    for (unsigned char c : input) {
      // Unreserved characters (RFC 3986): A-Z a-z 0-9 - _ . ~
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
        result += static_cast<char>(c);
      } else {
        char hex[4];
        snprintf(hex, sizeof(hex), "%%%02X", c);
        result += hex;
      }
    }
    return Value(result);
  };
  env->define("encodeURIComponent", Value(encodeURIComponentFn));
  globalThisObj->properties["encodeURIComponent"] = Value(encodeURIComponentFn);

  // decodeURIComponent - decode URI component
  auto decodeURIComponentFn = std::make_shared<Function>();
  decodeURIComponentFn->isNative = true;
  decodeURIComponentFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::string("undefined"));
    std::string input = args[0].toString();
    std::string result;
    result.reserve(input.size());

    for (size_t i = 0; i < input.size(); ++i) {
      if (input[i] == '%' && i + 2 < input.size()) {
        int value = 0;
        if (std::sscanf(input.c_str() + i + 1, "%2x", &value) == 1) {
          result += static_cast<char>(value);
          i += 2;
          continue;
        }
      }
      result += input[i];
    }
    return Value(result);
  };
  env->define("decodeURIComponent", Value(decodeURIComponentFn));
  globalThisObj->properties["decodeURIComponent"] = Value(decodeURIComponentFn);

  // encodeURI - encode full URI (leaves more characters unencoded)
  auto encodeURIFn = std::make_shared<Function>();
  encodeURIFn->isNative = true;
  encodeURIFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::string("undefined"));
    std::string input = args[0].toString();
    std::string result;
    result.reserve(input.size() * 3);

    for (unsigned char c : input) {
      // Reserved and unreserved characters that should NOT be encoded in full URI
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' ||
          c == ':' || c == '/' || c == '?' || c == '#' || c == '[' || c == ']' ||
          c == '@' || c == '!' || c == '$' || c == '&' || c == '\'' ||
          c == '(' || c == ')' || c == '*' || c == '+' || c == ',' || c == ';' || c == '=') {
        result += static_cast<char>(c);
      } else {
        char hex[4];
        snprintf(hex, sizeof(hex), "%%%02X", c);
        result += hex;
      }
    }
    return Value(result);
  };
  env->define("encodeURI", Value(encodeURIFn));
  globalThisObj->properties["encodeURI"] = Value(encodeURIFn);

  // decodeURI - decode full URI
  auto decodeURIFn = std::make_shared<Function>();
  decodeURIFn->isNative = true;
  decodeURIFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::string("undefined"));
    std::string input = args[0].toString();
    std::string result;
    result.reserve(input.size());

    for (size_t i = 0; i < input.size(); ++i) {
      if (input[i] == '%' && i + 2 < input.size()) {
        int value = 0;
        if (std::sscanf(input.c_str() + i + 1, "%2x", &value) == 1) {
          result += static_cast<char>(value);
          i += 2;
          continue;
        }
      }
      result += input[i];
    }
    return Value(result);
  };
  env->define("decodeURI", Value(decodeURIFn));
  globalThisObj->properties["decodeURI"] = Value(decodeURIFn);

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
