#include "environment.h"
#include "crypto.h"
#include "http.h"
#include <iostream>
#include <thread>

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
  consoleObj->properties["log"] = Value(consoleFn);

  env->define("console", Value(consoleObj));
  env->define("undefined", Value(Undefined{}));

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

  // Promise constructor
  auto promiseConstructor = std::make_shared<Object>();

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

  return env;
}

std::shared_ptr<Object> Environment::getGlobal() const {
  auto globalObj = std::make_shared<Object>();

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