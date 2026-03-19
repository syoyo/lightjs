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
#include "unicode.h"
#include "text_encoding.h"
#include "url.h"
#include "fs.h"
#include "streams.h"
#include "lexer.h"
#include "parser.h"
#include <iostream>
#include <thread>
#include <limits>
#include <cmath>
#include <random>
#include <cctype>
#include <array>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <regex>
#include <unordered_set>

namespace lightjs {

namespace {

std::string trimAsciiWhitespace(const std::string& s) {
  size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
    start++;
  }
  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    end--;
  }
  return s.substr(start, end - start);
}

int digitValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
  return -1;
}


bool isModuleNamespaceExportKey(const GCPtr<Object>& obj, const std::string& key) {
  return std::find(obj->moduleExportNames.begin(), obj->moduleExportNames.end(), key) !=
         obj->moduleExportNames.end();
}

constexpr const char* kImportPhaseSourceSentinel = "__lightjs_import_phase_source__";
constexpr const char* kImportPhaseDeferSentinel = "__lightjs_import_phase_defer__";
constexpr const char* kWithScopeObjectBinding = "__with_scope_object__";

size_t typedArrayElementSize(TypedArrayType type) {
  switch (type) {
    case TypedArrayType::Int8:
    case TypedArrayType::Uint8:
    case TypedArrayType::Uint8Clamped:
      return 1;
    case TypedArrayType::Int16:
    case TypedArrayType::Uint16:
    case TypedArrayType::Float16:
      return 2;
    case TypedArrayType::Int32:
    case TypedArrayType::Uint32:
    case TypedArrayType::Float32:
      return 4;
    case TypedArrayType::Float64:
    case TypedArrayType::BigInt64:
    case TypedArrayType::BigUint64:
      return 8;
  }
  return 1;
}

bool isTypedArrayConstructorName(const std::string& name) {
  static const std::unordered_set<std::string> kTypedArrayNames = {
    "Int8Array",
    "Uint8Array",
    "Uint8ClampedArray",
    "Int16Array",
    "Uint16Array",
    "Float16Array",
    "Int32Array",
    "Uint32Array",
    "Float32Array",
    "Float64Array",
    "BigInt64Array",
    "BigUint64Array",
  };
  return kTypedArrayNames.count(name) > 0;
}

bool isInternalPropertyKeyForReflection(const std::string& key) {
  // Hide LightJS internal bookkeeping keys from reflection APIs.
  // Do NOT hide arbitrary "__user__" property names: Test262 relies on them.
  if (key.rfind("__non_writable_", 0) == 0) return true;
  if (key.rfind("__non_enum_", 0) == 0) return true;
  if (key.rfind("__non_configurable_", 0) == 0) return true;
  if (key.rfind("__enum_", 0) == 0) return true;
  if (key.rfind("__get_", 0) == 0) return true;
  if (key.rfind("__set_", 0) == 0) return true;
  if (key.rfind("__mapped_arg_index_", 0) == 0) return true;
  if (key.rfind("__mapped_arg_name_", 0) == 0) return true;
  if (key.rfind("__deleted_", 0) == 0) return true;

  static const std::unordered_set<std::string> internalKeys = {
    "__callable_object__",
    "__constructor_wrapper__",
    "__constructor__",
    "__primitive_value__",
    "__is_arguments_object__",
    "__overridden_length__",
    "__throw_on_new__",
    "__is_arrow_function__",
    "__named_expression__",
    "__bound_target__",
    "__bound_this__",
    "__bound_args__",
    "__reflect_construct__",
    "__eval_deletable_bindings__",
    "__builtin_array_iterator__",
    "__in_class_field_initializer__",
    "__super_called__",
  };
  return internalKeys.count(key) > 0;
}

bool isVisibleWithIdentifier(const std::string& name) {
  if (name.empty()) {
    return false;
  }
  // Hide internal sentinels like "__foo__", but allow user identifiers that
  // merely begin with underscores (e.g. "__x").
  if (name.size() >= 4 && name.rfind("__", 0) == 0 &&
      name.substr(name.size() - 2) == "__") {
    return false;
  }
  return true;
}

bool isObjectLikeValue(const Value& value) {
  return value.isObject() || value.isArray() || value.isFunction() || value.isRegex() ||
         value.isProxy() || value.isPromise() || value.isGenerator() || value.isClass() ||
         value.isMap() || value.isSet() || value.isWeakMap() || value.isWeakSet() ||
         value.isTypedArray() || value.isArrayBuffer() || value.isDataView() || value.isError();
}

Value propertyKeyValueForName(const std::string& name) {
  if (name == WellKnownSymbols::unscopablesKey()) {
    return WellKnownSymbols::unscopables();
  }
  return Value(name);
}

bool parseArrayIndexKey(const std::string& key, size_t& index) {
  if (key.empty()) {
    return false;
  }
  if (key.size() > 1 && key[0] == '0') {
    return false;
  }
  uint64_t parsed = 0;
  constexpr uint64_t kMaxIndex = static_cast<uint64_t>(std::numeric_limits<size_t>::max());
  for (unsigned char ch : key) {
    if (!std::isdigit(ch)) {
      return false;
    }
    uint64_t digit = static_cast<uint64_t>(ch - '0');
    if (parsed > (kMaxIndex - digit) / 10) {
      return false;
    }
    parsed = parsed * 10 + digit;
  }
  index = static_cast<size_t>(parsed);
  return true;
}

std::optional<Value> getPrototypeValue(const Value& receiver) {
  if (receiver.isObject()) {
    auto obj = receiver.getGC<Object>();
    auto it = obj->properties.find("__proto__");
    if (it != obj->properties.end()) return it->second;
  } else if (receiver.isArray()) {
    auto arr = receiver.getGC<Array>();
    auto it = arr->properties.find("__proto__");
    if (it != arr->properties.end()) return it->second;
  } else if (receiver.isFunction()) {
    auto fn = receiver.getGC<Function>();
    auto it = fn->properties.find("__proto__");
    if (it != fn->properties.end()) return it->second;
  } else if (receiver.isRegex()) {
    auto regex = receiver.getGC<Regex>();
    auto it = regex->properties.find("__proto__");
    if (it != regex->properties.end()) return it->second;
  } else if (receiver.isPromise()) {
    auto p = receiver.getGC<Promise>();
    auto it = p->properties.find("__proto__");
    if (it != p->properties.end()) return it->second;
  } else if (receiver.isGenerator()) {
    auto gen = receiver.getGC<Generator>();
    auto it = gen->properties.find("__proto__");
    if (it != gen->properties.end()) return it->second;
  } else if (receiver.isClass()) {
    auto cls = receiver.getGC<Class>();
    auto it = cls->properties.find("__proto__");
    if (it != cls->properties.end()) return it->second;
  } else if (receiver.isMap()) {
    auto map = receiver.getGC<Map>();
    auto it = map->properties.find("__proto__");
    if (it != map->properties.end()) return it->second;
  } else if (receiver.isSet()) {
    auto set = receiver.getGC<Set>();
    auto it = set->properties.find("__proto__");
    if (it != set->properties.end()) return it->second;
  } else if (receiver.isWeakMap()) {
    auto wm = receiver.getGC<WeakMap>();
    auto it = wm->properties.find("__proto__");
    if (it != wm->properties.end()) return it->second;
  } else if (receiver.isWeakSet()) {
    auto ws = receiver.getGC<WeakSet>();
    auto it = ws->properties.find("__proto__");
    if (it != ws->properties.end()) return it->second;
  } else if (receiver.isTypedArray()) {
    auto ta = receiver.getGC<TypedArray>();
    auto it = ta->properties.find("__proto__");
    if (it != ta->properties.end()) return it->second;
  } else if (receiver.isArrayBuffer()) {
    auto ab = receiver.getGC<ArrayBuffer>();
    auto it = ab->properties.find("__proto__");
    if (it != ab->properties.end()) return it->second;
  } else if (receiver.isDataView()) {
    auto dv = receiver.getGC<DataView>();
    auto it = dv->properties.find("__proto__");
    if (it != dv->properties.end()) return it->second;
  } else if (receiver.isError()) {
    auto err = receiver.getGC<Error>();
    auto it = err->properties.find("__proto__");
    if (it != err->properties.end()) return it->second;
  }
  return std::nullopt;
}

Value makeIteratorResultObject(const Value& value, bool done) {
  auto result = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  result->properties["value"] = value;
  result->properties["done"] = Value(done);
  return Value(result);
}

bool isTypedArrayNumericName(const std::string& name) {
  size_t index = 0;
  if (parseArrayIndexKey(name, index)) {
    return true;
  }
  return name == "NaN" || name == "-0" || name == "Infinity" || name == "-Infinity";
}

// Check if a property exists on an object without invoking getters.
// This implements [[HasOwnProperty]] / [[GetOwnProperty]] existence check
// without triggering accessor side effects.
bool hasOwnPropertyNoInvoke(const Value& receiver, const std::string& name) {
  if (receiver.isObject()) {
    auto obj = receiver.getGC<Object>();
    if (obj->properties.count("__get_" + name) > 0) return true;
    if (obj->properties.count("__set_" + name) > 0) return true;
    return obj->properties.count(name) > 0;
  }
  if (receiver.isArray()) {
    auto arr = receiver.getGC<Array>();
    if (name == "length") return true;
    if (arr->properties.count("__get_" + name) > 0) return true;
    if (arr->properties.count("__set_" + name) > 0) return true;
    size_t index = 0;
    if (parseArrayIndexKey(name, index) && index < arr->elements.size()) return true;
    return arr->properties.count(name) > 0;
  }
  if (receiver.isFunction()) {
    auto fn = receiver.getGC<Function>();
    if (fn->properties.count("__get_" + name) > 0) return true;
    if (fn->properties.count("__set_" + name) > 0) return true;
    return fn->properties.count(name) > 0;
  }
  if (receiver.isTypedArray()) {
    auto ta = receiver.getGC<TypedArray>();
    if (ta->properties.count("__get_" + name) > 0) return true;
    if (ta->properties.count("__set_" + name) > 0) return true;
    size_t index = 0;
    if (parseArrayIndexKey(name, index) && index < ta->currentLength()) return true;
    return ta->properties.count(name) > 0;
  }
  return false;
}

// Forward declarations
bool hasPropertyLike(const Value& receiver, const std::string& name);

// Check property existence without invoking getters, walking prototype chain.
bool hasPropertyNoInvoke(const Value& receiver, const std::string& name) {
  if (receiver.isProxy()) {
    // For Proxy, delegate to hasPropertyLike which handles traps
    return hasPropertyLike(receiver, name);
  }
  if (hasOwnPropertyNoInvoke(receiver, name)) {
    return true;
  }
  auto proto = getPrototypeValue(receiver);
  if (!proto.has_value() || proto->isNull() || proto->isUndefined() || !isObjectLikeValue(*proto)) {
    return false;
  }
  return hasPropertyNoInvoke(*proto, name);
}

std::pair<bool, Value> getOwnPropertyLike(const Value& receiver,
                                          const std::string& name,
                                          const Value& accessReceiver) {
  auto* interpreter = getGlobalInterpreter();
  auto callGetter = [&](const Value& getter) -> Value {
    if (!interpreter) {
      return Value(Undefined{});
    }
    return interpreter->callForHarness(getter, {}, accessReceiver);
  };

  if (receiver.isObject()) {
    auto obj = receiver.getGC<Object>();
    auto getterIt = obj->properties.find("__get_" + name);
    if (getterIt != obj->properties.end()) {
      if (getterIt->second.isFunction()) return {true, callGetter(getterIt->second)};
      return {true, Value(Undefined{})};
    }
    if (obj->properties.find("__set_" + name) != obj->properties.end()) {
      return {true, Value(Undefined{})};
    }
    auto it = obj->properties.find(name);
    if (it != obj->properties.end()) return {true, it->second};
    return {false, Value(Undefined{})};
  }
  if (receiver.isArray()) {
    auto arr = receiver.getGC<Array>();
    if (name == "length") return {true, Value(static_cast<double>(arr->elements.size()))};
    auto getterIt = arr->properties.find("__get_" + name);
    if (getterIt != arr->properties.end()) {
      if (getterIt->second.isFunction()) return {true, callGetter(getterIt->second)};
      return {true, Value(Undefined{})};
    }
    if (arr->properties.find("__set_" + name) != arr->properties.end()) return {true, Value(Undefined{})};
    size_t index = 0;
    if (parseArrayIndexKey(name, index) && index < arr->elements.size()) {
      return {true, arr->elements[index]};
    }
    auto it = arr->properties.find(name);
    if (it != arr->properties.end()) return {true, it->second};
    return {false, Value(Undefined{})};
  }
  if (receiver.isTypedArray()) {
    auto ta = receiver.getGC<TypedArray>();
    auto getterIt = ta->properties.find("__get_" + name);
    if (getterIt != ta->properties.end()) {
      if (getterIt->second.isFunction()) return {true, callGetter(getterIt->second)};
      return {true, Value(Undefined{})};
    }
    if (ta->properties.find("__set_" + name) != ta->properties.end()) return {true, Value(Undefined{})};
    size_t index = 0;
    if (parseArrayIndexKey(name, index) && index < ta->currentLength()) {
      if (ta->type == TypedArrayType::BigInt64) {
        return {true, Value(BigInt(ta->getBigIntElement(index)))};
      }
      if (ta->type == TypedArrayType::BigUint64) {
        return {true, Value(BigInt(bigint::BigIntValue(ta->getBigUintElement(index))))};
      }
      return {true, Value(ta->getElement(index))};
    }
    auto it = ta->properties.find(name);
    if (it != ta->properties.end()) return {true, it->second};
    return {false, Value(Undefined{})};
  }
  if (receiver.isFunction()) {
    auto fn = receiver.getGC<Function>();
    auto getterIt = fn->properties.find("__get_" + name);
    if (getterIt != fn->properties.end()) {
      if (getterIt->second.isFunction()) return {true, callGetter(getterIt->second)};
      return {true, Value(Undefined{})};
    }
    if (fn->properties.find("__set_" + name) != fn->properties.end()) return {true, Value(Undefined{})};
    auto it = fn->properties.find(name);
    if (it != fn->properties.end()) return {true, it->second};
    return {false, Value(Undefined{})};
  }
  if (receiver.isRegex()) {
    auto regex = receiver.getGC<Regex>();
    auto getterIt = regex->properties.find("__get_" + name);
    if (getterIt != regex->properties.end()) {
      if (getterIt->second.isFunction()) return {true, callGetter(getterIt->second)};
      return {true, Value(Undefined{})};
    }
    if (regex->properties.find("__set_" + name) != regex->properties.end()) return {true, Value(Undefined{})};
    auto it = regex->properties.find(name);
    if (it != regex->properties.end()) return {true, it->second};
    return {false, Value(Undefined{})};
  }
  if (receiver.isError()) {
    auto err = receiver.getGC<Error>();
    auto getterIt = err->properties.find("__get_" + name);
    if (getterIt != err->properties.end()) {
      if (getterIt->second.isFunction()) return {true, callGetter(getterIt->second)};
      return {true, Value(Undefined{})};
    }
    if (err->properties.find("__set_" + name) != err->properties.end()) return {true, Value(Undefined{})};
    auto it = err->properties.find(name);
    if (it != err->properties.end()) return {true, it->second};
    if (name == "name") return {true, Value(err->getName())};
    if (name == "message") return {true, Value(err->message)};
    return {false, Value(Undefined{})};
  }
  auto getFromBag = [&](const OrderedMap<std::string, Value>& bag) -> std::pair<bool, Value> {
    auto getterIt = bag.find("__get_" + name);
    if (getterIt != bag.end()) {
      if (getterIt->second.isFunction()) return {true, callGetter(getterIt->second)};
      return {true, Value(Undefined{})};
    }
    if (bag.find("__set_" + name) != bag.end()) return {true, Value(Undefined{})};
    auto it = bag.find(name);
    if (it != bag.end()) return {true, it->second};
    return {false, Value(Undefined{})};
  };
  if (receiver.isPromise()) return getFromBag(receiver.getGC<Promise>()->properties);
  if (receiver.isGenerator()) return getFromBag(receiver.getGC<Generator>()->properties);
  if (receiver.isClass()) return getFromBag(receiver.getGC<Class>()->properties);
  if (receiver.isMap()) return getFromBag(receiver.getGC<Map>()->properties);
  if (receiver.isSet()) return getFromBag(receiver.getGC<Set>()->properties);
  if (receiver.isWeakMap()) return getFromBag(receiver.getGC<WeakMap>()->properties);
  if (receiver.isWeakSet()) return getFromBag(receiver.getGC<WeakSet>()->properties);
  if (receiver.isArrayBuffer()) return getFromBag(receiver.getGC<ArrayBuffer>()->properties);
  if (receiver.isDataView()) return getFromBag(receiver.getGC<DataView>()->properties);
  return {false, Value(Undefined{})};
}

std::pair<bool, Value> getPropertyLike(const Value& receiver,
                                       const std::string& name,
                                       const Value& originalReceiver) {
  auto* interpreter = getGlobalInterpreter();
  if (receiver.isProxy()) {
    auto proxy = receiver.getGC<Proxy>();
    if (proxy->handler && proxy->handler->isObject()) {
      auto handlerObj = proxy->handler->getGC<Object>();
      auto getIt = handlerObj->properties.find("get");
      if (getIt != handlerObj->properties.end() && getIt->second.isFunction()) {
        if (!proxy->target) {
          return {true, Value(Undefined{})};
        }
        Value result = interpreter
          ? interpreter->callForHarness(getIt->second,
                                        {*proxy->target, propertyKeyValueForName(name), originalReceiver},
                                        Value(handlerObj))
          : Value(Undefined{});
        return {true, result};
      }
    }
    if (proxy->target) {
      return getPropertyLike(*proxy->target, name, originalReceiver);
    }
    return {false, Value(Undefined{})};
  }

  if (receiver.isTypedArray() && isTypedArrayNumericName(name)) {
    auto [found, value] = getOwnPropertyLike(receiver, name, originalReceiver);
    return {found, value};
  }

  auto [found, value] = getOwnPropertyLike(receiver, name, originalReceiver);
  if (found) {
    return {true, value};
  }

  auto proto = getPrototypeValue(receiver);
  if (!proto.has_value() || proto->isNull() || proto->isUndefined() || !isObjectLikeValue(*proto)) {
    return {false, Value(Undefined{})};
  }
  return getPropertyLike(*proto, name, originalReceiver);
}

bool hasPropertyLike(const Value& receiver, const std::string& name) {
  auto* interpreter = getGlobalInterpreter();
  if (receiver.isProxy()) {
    auto proxy = receiver.getGC<Proxy>();
    if (proxy->handler && proxy->handler->isObject()) {
      auto handlerObj = proxy->handler->getGC<Object>();
      auto hasIt = handlerObj->properties.find("has");
      if (hasIt != handlerObj->properties.end() && hasIt->second.isFunction()) {
        if (!proxy->target || !interpreter) {
          return false;
        }
        Value result = interpreter->callForHarness(
          hasIt->second, {*proxy->target, propertyKeyValueForName(name)}, Value(handlerObj));
        return !interpreter->hasError() && result.toBool();
      }
    }
    if (proxy->target) {
      return hasPropertyLike(*proxy->target, name);
    }
    return false;
  }

  if (receiver.isTypedArray() && isTypedArrayNumericName(name)) {
    return hasOwnPropertyNoInvoke(receiver, name);
  }

  if (hasOwnPropertyNoInvoke(receiver, name)) {
    return true;
  }
  if (interpreter && interpreter->hasError()) {
    return false;
  }
  auto proto = getPrototypeValue(receiver);
  if (!proto.has_value() || proto->isNull() || proto->isUndefined() || !isObjectLikeValue(*proto)) {
    return false;
  }
  return hasPropertyLike(*proto, name);
}

bool isBlockedByUnscopables(const Value& bindings, const std::string& name) {
  if (!isVisibleWithIdentifier(name)) {
    return false;
  }
  auto [foundUnscopables, unscopablesValue] =
    getPropertyLike(bindings, WellKnownSymbols::unscopablesKey(), bindings);
  if (auto* interpreter = getGlobalInterpreter(); interpreter && interpreter->hasError()) {
    return false;
  }
  if (!foundUnscopables || (!unscopablesValue.isObject() && !unscopablesValue.isProxy())) {
    return false;
  }
  auto [foundBlocked, blocked] = getPropertyLike(unscopablesValue, name, unscopablesValue);
  if (auto* interpreter = getGlobalInterpreter(); interpreter && interpreter->hasError()) {
    return false;
  }
  return foundBlocked && blocked.toBool();
}

bool setPropertyLike(const Value& receiver, const std::string& name, const Value& value) {
  auto* interpreter = getGlobalInterpreter();
  if (receiver.isProxy()) {
    auto proxy = receiver.getGC<Proxy>();
    if (proxy->handler && proxy->handler->isObject()) {
      auto handlerObj = proxy->handler->getGC<Object>();
      auto setIt = handlerObj->properties.find("set");
      if (setIt != handlerObj->properties.end() && setIt->second.isFunction()) {
        if (!proxy->target || !interpreter) {
          return false;
        }
        Value result = interpreter->callForHarness(
          setIt->second, {*proxy->target, propertyKeyValueForName(name), value, receiver}, Value(handlerObj));
        return !interpreter->hasError() && result.toBool();
      }
    }
    if (proxy->target) {
      return setPropertyLike(*proxy->target, name, value);
    }
    return false;
  }
  auto setOnBag = [&](auto& bag) -> bool {
    auto setterIt = bag.find("__set_" + name);
    if (setterIt != bag.end() && setterIt->second.isFunction()) {
      if (!interpreter) {
        return false;
      }
      interpreter->callForHarness(setterIt->second, {value}, receiver);
      return !interpreter->hasError();
    }
    if (bag.count("__non_writable_" + name)) {
      return false;
    }
    bag[name] = value;
    return true;
  };

  if (receiver.isObject()) {
    auto obj = receiver.getGC<Object>();
    auto proto = getPrototypeValue(receiver);
    if (proto.has_value() && isObjectLikeValue(*proto)) {
      if (proto->isTypedArray() && isTypedArrayNumericName(name)) {
        return false;
      }
    }
    return setOnBag(obj->properties);
  }

  if (receiver.isArray()) {
    auto arr = receiver.getGC<Array>();
    if (name == "length") {
      return false;
    }
    size_t index = 0;
    if (parseArrayIndexKey(name, index)) {
      auto setterIt = arr->properties.find("__set_" + name);
      if (setterIt != arr->properties.end() && setterIt->second.isFunction()) {
        if (!interpreter) {
          return false;
        }
        interpreter->callForHarness(setterIt->second, {value}, receiver);
        return !interpreter->hasError();
      }
      if (index >= arr->elements.size()) {
        arr->elements.resize(index + 1, Value(Undefined{}));
      }
      arr->elements[index] = value;
      return true;
    }
    return setOnBag(arr->properties);
  }

  if (receiver.isFunction()) {
    auto fn = receiver.getGC<Function>();
    return setOnBag(fn->properties);
  }

  if (receiver.isTypedArray()) {
    auto ta = receiver.getGC<TypedArray>();
    size_t index = 0;
    if (parseArrayIndexKey(name, index)) {
      if ((ta->viewedBuffer && ta->viewedBuffer->detached) || ta->isOutOfBounds() ||
          index >= ta->currentLength()) {
        return false;
      }
      if (ta->type == TypedArrayType::BigInt64) {
        ta->setBigIntElement(index, bigint::toInt64Trunc(value.toBigInt()));
      } else if (ta->type == TypedArrayType::BigUint64) {
        ta->setBigUintElement(index, bigint::toUint64Trunc(value.toBigInt()));
      } else {
        ta->setElement(index, value.toNumber());
      }
      return true;
    }
    return setOnBag(ta->properties);
  }

  if (receiver.isClass()) {
    auto cls = receiver.getGC<Class>();
    return setOnBag(cls->properties);
  }

  return false;
}

std::optional<Value> lookupWithScopeProperty(const Value& scopeValue, const std::string& name) {
  if (!isVisibleWithIdentifier(name)) {
    return std::nullopt;
  }
  if (!hasPropertyLike(scopeValue, name)) {
    return std::nullopt;
  }
  if (auto* interpreter = getGlobalInterpreter(); interpreter && interpreter->hasError()) {
    return std::nullopt;
  }
  if (isBlockedByUnscopables(scopeValue, name)) {
    return std::nullopt;
  }
  if (auto* interpreter = getGlobalInterpreter(); interpreter && interpreter->hasError()) {
    return std::nullopt;
  }
  auto [found, value] = getPropertyLike(scopeValue, name, scopeValue);
  if (!found) {
    return Value(Undefined{});
  }
  return value;
}

bool setWithScopeProperty(const Value& scopeValue,
                          const std::string& name,
                          const Value& value,
                          bool strict) {
  if (!isVisibleWithIdentifier(name)) {
    return false;
  }
  bool stillExists = hasPropertyLike(scopeValue, name);
  if (auto* interpreter = getGlobalInterpreter(); interpreter && interpreter->hasError()) {
    return false;
  }
  if (!stillExists && strict) {
    return false;
  }
  return setPropertyLike(scopeValue, name, value);
}

bool deleteWithScopeProperty(const Value& scopeValue, const std::string& name) {
  if (!isVisibleWithIdentifier(name) || !scopeValue.isObject()) {
    return false;
  }

  auto receiver = scopeValue.getGC<Object>();
  // Check own properties only for delete (no prototype chain)
  auto it = receiver->properties.find(name);
  if (it != receiver->properties.end()) {
    // Check non-configurable
    if (receiver->properties.count("__non_configurable_" + name)) {
      return false;  // Can't delete non-configurable properties
    }
    receiver->properties.erase(name);
    receiver->properties.erase("__non_writable_" + name);
    receiver->properties.erase("__non_enum_" + name);
    receiver->properties.erase("__enum_" + name);
    return true;
  }
  return false;
}

void collectVarNamesFromPattern(const Expression& pattern, std::vector<std::string>& names) {
  if (auto* id = std::get_if<Identifier>(&pattern.node)) {
    names.push_back(id->name);
    return;
  }
  if (auto* assign = std::get_if<AssignmentPattern>(&pattern.node)) {
    if (assign->left) {
      collectVarNamesFromPattern(*assign->left, names);
    }
    return;
  }
  if (auto* arr = std::get_if<ArrayPattern>(&pattern.node)) {
    for (const auto& elem : arr->elements) {
      if (elem) {
        collectVarNamesFromPattern(*elem, names);
      }
    }
    if (arr->rest) {
      collectVarNamesFromPattern(*arr->rest, names);
    }
    return;
  }
  if (auto* obj = std::get_if<ObjectPattern>(&pattern.node)) {
    for (const auto& prop : obj->properties) {
      if (prop.value) {
        collectVarNamesFromPattern(*prop.value, names);
      }
    }
    if (obj->rest) {
      collectVarNamesFromPattern(*obj->rest, names);
    }
  }
}

void collectVarNamesFromStatement(const Statement& stmt, std::vector<std::string>& names) {
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.node)) {
    if (varDecl->kind == VarDeclaration::Kind::Var) {
      for (const auto& declarator : varDecl->declarations) {
        if (declarator.pattern) {
          collectVarNamesFromPattern(*declarator.pattern, names);
        }
      }
    }
    return;
  }
  if (auto* block = std::get_if<BlockStmt>(&stmt.node)) {
    for (const auto& inner : block->body) {
      if (inner) {
        collectVarNamesFromStatement(*inner, names);
      }
    }
    return;
  }
  if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    if (ifStmt->consequent) {
      collectVarNamesFromStatement(*ifStmt->consequent, names);
    }
    if (ifStmt->alternate) {
      collectVarNamesFromStatement(*ifStmt->alternate, names);
    }
    return;
  }
  if (auto* whileStmt = std::get_if<WhileStmt>(&stmt.node)) {
    if (whileStmt->body) {
      collectVarNamesFromStatement(*whileStmt->body, names);
    }
    return;
  }
  if (auto* doWhileStmt = std::get_if<DoWhileStmt>(&stmt.node)) {
    if (doWhileStmt->body) {
      collectVarNamesFromStatement(*doWhileStmt->body, names);
    }
    return;
  }
  if (auto* forStmt = std::get_if<ForStmt>(&stmt.node)) {
    if (forStmt->init) {
      collectVarNamesFromStatement(*forStmt->init, names);
    }
    if (forStmt->body) {
      collectVarNamesFromStatement(*forStmt->body, names);
    }
    return;
  }
  if (auto* forInStmt = std::get_if<ForInStmt>(&stmt.node)) {
    if (forInStmt->left) {
      collectVarNamesFromStatement(*forInStmt->left, names);
    }
    if (forInStmt->body) {
      collectVarNamesFromStatement(*forInStmt->body, names);
    }
    return;
  }
  if (auto* forOfStmt = std::get_if<ForOfStmt>(&stmt.node)) {
    if (forOfStmt->left) {
      collectVarNamesFromStatement(*forOfStmt->left, names);
    }
    if (forOfStmt->body) {
      collectVarNamesFromStatement(*forOfStmt->body, names);
    }
    return;
  }
  if (auto* switchStmt = std::get_if<SwitchStmt>(&stmt.node)) {
    for (const auto& caseClause : switchStmt->cases) {
      for (const auto& cons : caseClause.consequent) {
        if (cons) {
          collectVarNamesFromStatement(*cons, names);
        }
      }
    }
    return;
  }
  if (auto* tryStmt = std::get_if<TryStmt>(&stmt.node)) {
    for (const auto& inner : tryStmt->block) {
      if (inner) {
        collectVarNamesFromStatement(*inner, names);
      }
    }
    if (tryStmt->hasHandler) {
      for (const auto& inner : tryStmt->handler.body) {
        if (inner) {
          collectVarNamesFromStatement(*inner, names);
        }
      }
    }
    if (tryStmt->hasFinalizer) {
      for (const auto& inner : tryStmt->finalizer) {
        if (inner) {
          collectVarNamesFromStatement(*inner, names);
        }
      }
    }
    return;
  }
  if (auto* labelled = std::get_if<LabelledStmt>(&stmt.node)) {
    if (labelled->body) {
      collectVarNamesFromStatement(*labelled->body, names);
    }
    return;
  }
  if (auto* withStmt = std::get_if<WithStmt>(&stmt.node)) {
    if (withStmt->body) {
      collectVarNamesFromStatement(*withStmt->body, names);
    }
  }
}

void collectDeclaredNamesForDirectEval(const Statement& stmt, std::vector<std::string>& names) {
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.node)) {
    for (const auto& declarator : varDecl->declarations) {
      if (declarator.pattern) {
        collectVarNamesFromPattern(*declarator.pattern, names);
      }
    }
    return;
  }
  if (auto* funcDecl = std::get_if<FunctionDeclaration>(&stmt.node)) {
    if (!funcDecl->id.name.empty()) {
      names.push_back(funcDecl->id.name);
    }
    return;
  }
  if (auto* classDecl = std::get_if<ClassDeclaration>(&stmt.node)) {
    if (!classDecl->id.name.empty()) {
      names.push_back(classDecl->id.name);
    }
    return;
  }
  if (auto* block = std::get_if<BlockStmt>(&stmt.node)) {
    for (const auto& inner : block->body) {
      if (inner) {
        collectDeclaredNamesForDirectEval(*inner, names);
      }
    }
    return;
  }
  if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    if (ifStmt->consequent) {
      collectDeclaredNamesForDirectEval(*ifStmt->consequent, names);
    }
    if (ifStmt->alternate) {
      collectDeclaredNamesForDirectEval(*ifStmt->alternate, names);
    }
    return;
  }
  if (auto* whileStmt = std::get_if<WhileStmt>(&stmt.node)) {
    if (whileStmt->body) {
      collectDeclaredNamesForDirectEval(*whileStmt->body, names);
    }
    return;
  }
  if (auto* doWhileStmt = std::get_if<DoWhileStmt>(&stmt.node)) {
    if (doWhileStmt->body) {
      collectDeclaredNamesForDirectEval(*doWhileStmt->body, names);
    }
    return;
  }
  if (auto* forStmt = std::get_if<ForStmt>(&stmt.node)) {
    if (forStmt->init) {
      collectDeclaredNamesForDirectEval(*forStmt->init, names);
    }
    if (forStmt->body) {
      collectDeclaredNamesForDirectEval(*forStmt->body, names);
    }
    return;
  }
  if (auto* forInStmt = std::get_if<ForInStmt>(&stmt.node)) {
    if (forInStmt->left) {
      collectDeclaredNamesForDirectEval(*forInStmt->left, names);
    }
    if (forInStmt->body) {
      collectDeclaredNamesForDirectEval(*forInStmt->body, names);
    }
    return;
  }
  if (auto* forOfStmt = std::get_if<ForOfStmt>(&stmt.node)) {
    if (forOfStmt->left) {
      collectDeclaredNamesForDirectEval(*forOfStmt->left, names);
    }
    if (forOfStmt->body) {
      collectDeclaredNamesForDirectEval(*forOfStmt->body, names);
    }
    return;
  }
  if (auto* switchStmt = std::get_if<SwitchStmt>(&stmt.node)) {
    for (const auto& caseClause : switchStmt->cases) {
      for (const auto& cons : caseClause.consequent) {
        if (cons) {
          collectDeclaredNamesForDirectEval(*cons, names);
        }
      }
    }
    return;
  }
  if (auto* tryStmt = std::get_if<TryStmt>(&stmt.node)) {
    for (const auto& inner : tryStmt->block) {
      if (inner) {
        collectDeclaredNamesForDirectEval(*inner, names);
      }
    }
    if (tryStmt->hasHandler) {
      for (const auto& inner : tryStmt->handler.body) {
        if (inner) {
          collectDeclaredNamesForDirectEval(*inner, names);
        }
      }
    }
    if (tryStmt->hasFinalizer) {
      for (const auto& inner : tryStmt->finalizer) {
        if (inner) {
          collectDeclaredNamesForDirectEval(*inner, names);
        }
      }
    }
    return;
  }
  if (auto* labelled = std::get_if<LabelledStmt>(&stmt.node)) {
    if (labelled->body) {
      collectDeclaredNamesForDirectEval(*labelled->body, names);
    }
    return;
  }
  if (auto* withStmt = std::get_if<WithStmt>(&stmt.node)) {
    if (withStmt->body) {
      collectDeclaredNamesForDirectEval(*withStmt->body, names);
    }
  }
}

bool bodyHasUseStrictDirective(const std::vector<StmtPtr>& body) {
  for (const auto& stmt : body) {
    if (!stmt) {
      break;
    }
    auto* exprStmt = std::get_if<ExpressionStmt>(&stmt->node);
    if (!exprStmt || !exprStmt->expression) {
      break;
    }
    auto* str = std::get_if<StringLiteral>(&exprStmt->expression->node);
    if (!str) {
      break;
    }
    if (str->value == "use strict") {
      return true;
    }
  }
  return false;
}

bool expressionContainsSuperForEval(const Expression& expr);
bool statementContainsSuperForEval(const Statement& stmt);
bool expressionContainsSuperCallForEval(const Expression& expr);
bool statementContainsSuperCallForEval(const Statement& stmt);
bool expressionContainsArgumentsForEval(const Expression& expr);
bool statementContainsArgumentsForEval(const Statement& stmt);
bool expressionContainsNewTargetForEval(const Expression& expr);
bool statementContainsNewTargetForEval(const Statement& stmt);

bool expressionContainsSuperForEval(const Expression& expr) {
  if (std::holds_alternative<SuperExpr>(expr.node)) return true;
  if (auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
    return (binary->left && expressionContainsSuperForEval(*binary->left)) ||
           (binary->right && expressionContainsSuperForEval(*binary->right));
  }
  if (auto* unary = std::get_if<UnaryExpr>(&expr.node)) {
    return unary->argument && expressionContainsSuperForEval(*unary->argument);
  }
  if (auto* assign = std::get_if<AssignmentExpr>(&expr.node)) {
    return (assign->left && expressionContainsSuperForEval(*assign->left)) ||
           (assign->right && expressionContainsSuperForEval(*assign->right));
  }
  if (auto* update = std::get_if<UpdateExpr>(&expr.node)) {
    return update->argument && expressionContainsSuperForEval(*update->argument);
  }
  if (auto* call = std::get_if<CallExpr>(&expr.node)) {
    if (call->callee && expressionContainsSuperForEval(*call->callee)) return true;
    for (const auto& arg : call->arguments) {
      if (arg && expressionContainsSuperForEval(*arg)) return true;
    }
    return false;
  }
  if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
    return (member->object && expressionContainsSuperForEval(*member->object)) ||
           (member->property && expressionContainsSuperForEval(*member->property));
  }
  if (auto* cond = std::get_if<ConditionalExpr>(&expr.node)) {
    return (cond->test && expressionContainsSuperForEval(*cond->test)) ||
           (cond->consequent && expressionContainsSuperForEval(*cond->consequent)) ||
           (cond->alternate && expressionContainsSuperForEval(*cond->alternate));
  }
  if (auto* seq = std::get_if<SequenceExpr>(&expr.node)) {
    for (const auto& e : seq->expressions) {
      if (e && expressionContainsSuperForEval(*e)) return true;
    }
    return false;
  }
  if (auto* arr = std::get_if<ArrayExpr>(&expr.node)) {
    for (const auto& e : arr->elements) {
      if (e && expressionContainsSuperForEval(*e)) return true;
    }
    return false;
  }
  if (auto* obj = std::get_if<ObjectExpr>(&expr.node)) {
    for (const auto& prop : obj->properties) {
      if (prop.key && expressionContainsSuperForEval(*prop.key)) return true;
      if (prop.value && expressionContainsSuperForEval(*prop.value)) return true;
    }
    return false;
  }
  if (auto* awaitExpr = std::get_if<AwaitExpr>(&expr.node)) {
    return awaitExpr->argument && expressionContainsSuperForEval(*awaitExpr->argument);
  }
  if (auto* yieldExpr = std::get_if<YieldExpr>(&expr.node)) {
    return yieldExpr->argument && expressionContainsSuperForEval(*yieldExpr->argument);
  }
  if (auto* spread = std::get_if<SpreadElement>(&expr.node)) {
    return spread->argument && expressionContainsSuperForEval(*spread->argument);
  }
  if (auto* newExpr = std::get_if<NewExpr>(&expr.node)) {
    if (newExpr->callee && expressionContainsSuperForEval(*newExpr->callee)) return true;
    for (const auto& arg : newExpr->arguments) {
      if (arg && expressionContainsSuperForEval(*arg)) return true;
    }
    return false;
  }
  if (auto* arrPat = std::get_if<ArrayPattern>(&expr.node)) {
    for (const auto& e : arrPat->elements) {
      if (e && expressionContainsSuperForEval(*e)) return true;
    }
    return arrPat->rest && expressionContainsSuperForEval(*arrPat->rest);
  }
  if (auto* objPat = std::get_if<ObjectPattern>(&expr.node)) {
    for (const auto& prop : objPat->properties) {
      if (prop.key && expressionContainsSuperForEval(*prop.key)) return true;
      if (prop.value && expressionContainsSuperForEval(*prop.value)) return true;
    }
    return objPat->rest && expressionContainsSuperForEval(*objPat->rest);
  }
  if (auto* assignPat = std::get_if<AssignmentPattern>(&expr.node)) {
    return (assignPat->left && expressionContainsSuperForEval(*assignPat->left)) ||
           (assignPat->right && expressionContainsSuperForEval(*assignPat->right));
  }
  return false;
}

bool statementContainsSuperForEval(const Statement& stmt) {
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.node)) {
    for (const auto& decl : varDecl->declarations) {
      if (decl.pattern && expressionContainsSuperForEval(*decl.pattern)) return true;
      if (decl.init && expressionContainsSuperForEval(*decl.init)) return true;
    }
    return false;
  }
  if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt.node)) {
    return exprStmt->expression && expressionContainsSuperForEval(*exprStmt->expression);
  }
  if (auto* ret = std::get_if<ReturnStmt>(&stmt.node)) {
    return ret->argument && expressionContainsSuperForEval(*ret->argument);
  }
  if (auto* block = std::get_if<BlockStmt>(&stmt.node)) {
    for (const auto& s : block->body) {
      if (s && statementContainsSuperForEval(*s)) return true;
    }
    return false;
  }
  if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    if (ifStmt->test && expressionContainsSuperForEval(*ifStmt->test)) return true;
    if (ifStmt->consequent && statementContainsSuperForEval(*ifStmt->consequent)) return true;
    return ifStmt->alternate && statementContainsSuperForEval(*ifStmt->alternate);
  }
  if (auto* whileStmt = std::get_if<WhileStmt>(&stmt.node)) {
    if (whileStmt->test && expressionContainsSuperForEval(*whileStmt->test)) return true;
    return whileStmt->body && statementContainsSuperForEval(*whileStmt->body);
  }
  if (auto* forStmt = std::get_if<ForStmt>(&stmt.node)) {
    if (forStmt->init && statementContainsSuperForEval(*forStmt->init)) return true;
    if (forStmt->test && expressionContainsSuperForEval(*forStmt->test)) return true;
    if (forStmt->update && expressionContainsSuperForEval(*forStmt->update)) return true;
    return forStmt->body && statementContainsSuperForEval(*forStmt->body);
  }
  if (auto* withStmt = std::get_if<WithStmt>(&stmt.node)) {
    if (withStmt->object && expressionContainsSuperForEval(*withStmt->object)) return true;
    return withStmt->body && statementContainsSuperForEval(*withStmt->body);
  }
  if (auto* forInStmt = std::get_if<ForInStmt>(&stmt.node)) {
    if (forInStmt->left && statementContainsSuperForEval(*forInStmt->left)) return true;
    if (forInStmt->right && expressionContainsSuperForEval(*forInStmt->right)) return true;
    return forInStmt->body && statementContainsSuperForEval(*forInStmt->body);
  }
  if (auto* forOfStmt = std::get_if<ForOfStmt>(&stmt.node)) {
    if (forOfStmt->left && statementContainsSuperForEval(*forOfStmt->left)) return true;
    if (forOfStmt->right && expressionContainsSuperForEval(*forOfStmt->right)) return true;
    return forOfStmt->body && statementContainsSuperForEval(*forOfStmt->body);
  }
  if (auto* doWhileStmt = std::get_if<DoWhileStmt>(&stmt.node)) {
    if (doWhileStmt->body && statementContainsSuperForEval(*doWhileStmt->body)) return true;
    return doWhileStmt->test && expressionContainsSuperForEval(*doWhileStmt->test);
  }
  if (auto* switchStmt = std::get_if<SwitchStmt>(&stmt.node)) {
    if (switchStmt->discriminant && expressionContainsSuperForEval(*switchStmt->discriminant)) return true;
    for (const auto& c : switchStmt->cases) {
      if (c.test && expressionContainsSuperForEval(*c.test)) return true;
      for (const auto& s : c.consequent) {
        if (s && statementContainsSuperForEval(*s)) return true;
      }
    }
    return false;
  }
  if (auto* label = std::get_if<LabelledStmt>(&stmt.node)) {
    return label->body && statementContainsSuperForEval(*label->body);
  }
  if (auto* throwStmt = std::get_if<ThrowStmt>(&stmt.node)) {
    return throwStmt->argument && expressionContainsSuperForEval(*throwStmt->argument);
  }
  if (auto* tryStmt = std::get_if<TryStmt>(&stmt.node)) {
    for (const auto& s : tryStmt->block) {
      if (s && statementContainsSuperForEval(*s)) return true;
    }
    if (tryStmt->handler.paramPattern && expressionContainsSuperForEval(*tryStmt->handler.paramPattern)) return true;
    for (const auto& s : tryStmt->handler.body) {
      if (s && statementContainsSuperForEval(*s)) return true;
    }
    for (const auto& s : tryStmt->finalizer) {
      if (s && statementContainsSuperForEval(*s)) return true;
    }
    return false;
  }
  if (auto* exportDefault = std::get_if<ExportDefaultDeclaration>(&stmt.node)) {
    return exportDefault->declaration && expressionContainsSuperForEval(*exportDefault->declaration);
  }
  return false;
}

bool expressionContainsSuperCallForEval(const Expression& expr) {
  if (auto* fn = std::get_if<FunctionExpr>(&expr.node)) {
    // Arrow functions inherit super-call semantics from the surrounding
    // context, so Contains(SuperCall) must recurse into arrow bodies.
    if (!fn->isArrow) {
      return false;
    }
    for (const auto& p : fn->params) {
      if (p.defaultValue && expressionContainsSuperCallForEval(*p.defaultValue)) {
        return true;
      }
    }
    for (const auto& s : fn->body) {
      if (s && statementContainsSuperCallForEval(*s)) {
        return true;
      }
    }
    return false;
  }
  if (auto* call = std::get_if<CallExpr>(&expr.node)) {
    if (call->callee && std::holds_alternative<SuperExpr>(call->callee->node)) {
      return true;
    }
    if (call->callee && expressionContainsSuperCallForEval(*call->callee)) return true;
    for (const auto& arg : call->arguments) {
      if (arg && expressionContainsSuperCallForEval(*arg)) return true;
    }
    return false;
  }
  if (auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
    return (binary->left && expressionContainsSuperCallForEval(*binary->left)) ||
           (binary->right && expressionContainsSuperCallForEval(*binary->right));
  }
  if (auto* unary = std::get_if<UnaryExpr>(&expr.node)) {
    return unary->argument && expressionContainsSuperCallForEval(*unary->argument);
  }
  if (auto* assign = std::get_if<AssignmentExpr>(&expr.node)) {
    return (assign->left && expressionContainsSuperCallForEval(*assign->left)) ||
           (assign->right && expressionContainsSuperCallForEval(*assign->right));
  }
  if (auto* update = std::get_if<UpdateExpr>(&expr.node)) {
    return update->argument && expressionContainsSuperCallForEval(*update->argument);
  }
  if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
    return (member->object && expressionContainsSuperCallForEval(*member->object)) ||
           (member->property && expressionContainsSuperCallForEval(*member->property));
  }
  if (auto* cond = std::get_if<ConditionalExpr>(&expr.node)) {
    return (cond->test && expressionContainsSuperCallForEval(*cond->test)) ||
           (cond->consequent && expressionContainsSuperCallForEval(*cond->consequent)) ||
           (cond->alternate && expressionContainsSuperCallForEval(*cond->alternate));
  }
  if (auto* seq = std::get_if<SequenceExpr>(&expr.node)) {
    for (const auto& e : seq->expressions) {
      if (e && expressionContainsSuperCallForEval(*e)) return true;
    }
    return false;
  }
  if (auto* arr = std::get_if<ArrayExpr>(&expr.node)) {
    for (const auto& e : arr->elements) {
      if (e && expressionContainsSuperCallForEval(*e)) return true;
    }
    return false;
  }
  if (auto* obj = std::get_if<ObjectExpr>(&expr.node)) {
    for (const auto& prop : obj->properties) {
      if (prop.key && expressionContainsSuperCallForEval(*prop.key)) return true;
      if (prop.value && expressionContainsSuperCallForEval(*prop.value)) return true;
    }
    return false;
  }
  if (auto* awaitExpr = std::get_if<AwaitExpr>(&expr.node)) {
    return awaitExpr->argument && expressionContainsSuperCallForEval(*awaitExpr->argument);
  }
  if (auto* yieldExpr = std::get_if<YieldExpr>(&expr.node)) {
    return yieldExpr->argument && expressionContainsSuperCallForEval(*yieldExpr->argument);
  }
  if (auto* spread = std::get_if<SpreadElement>(&expr.node)) {
    return spread->argument && expressionContainsSuperCallForEval(*spread->argument);
  }
  if (auto* newExpr = std::get_if<NewExpr>(&expr.node)) {
    if (newExpr->callee && expressionContainsSuperCallForEval(*newExpr->callee)) return true;
    for (const auto& arg : newExpr->arguments) {
      if (arg && expressionContainsSuperCallForEval(*arg)) return true;
    }
    return false;
  }
  if (auto* assignPat = std::get_if<AssignmentPattern>(&expr.node)) {
    return (assignPat->left && expressionContainsSuperCallForEval(*assignPat->left)) ||
           (assignPat->right && expressionContainsSuperCallForEval(*assignPat->right));
  }
  return false;
}

bool statementContainsSuperCallForEval(const Statement& stmt) {
  if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt.node)) {
    return exprStmt->expression && expressionContainsSuperCallForEval(*exprStmt->expression);
  }
  if (auto* ret = std::get_if<ReturnStmt>(&stmt.node)) {
    return ret->argument && expressionContainsSuperCallForEval(*ret->argument);
  }
  if (auto* throwStmt = std::get_if<ThrowStmt>(&stmt.node)) {
    return throwStmt->argument && expressionContainsSuperCallForEval(*throwStmt->argument);
  }
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.node)) {
    for (const auto& decl : varDecl->declarations) {
      if (decl.pattern && expressionContainsSuperCallForEval(*decl.pattern)) return true;
      if (decl.init && expressionContainsSuperCallForEval(*decl.init)) return true;
    }
    return false;
  }
  if (auto* block = std::get_if<BlockStmt>(&stmt.node)) {
    for (const auto& s : block->body) {
      if (s && statementContainsSuperCallForEval(*s)) return true;
    }
    return false;
  }
  if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    if (ifStmt->test && expressionContainsSuperCallForEval(*ifStmt->test)) return true;
    if (ifStmt->consequent && statementContainsSuperCallForEval(*ifStmt->consequent)) return true;
    return ifStmt->alternate && statementContainsSuperCallForEval(*ifStmt->alternate);
  }
  if (auto* whileStmt = std::get_if<WhileStmt>(&stmt.node)) {
    if (whileStmt->test && expressionContainsSuperCallForEval(*whileStmt->test)) return true;
    return whileStmt->body && statementContainsSuperCallForEval(*whileStmt->body);
  }
  if (auto* forStmt = std::get_if<ForStmt>(&stmt.node)) {
    if (forStmt->init && statementContainsSuperCallForEval(*forStmt->init)) return true;
    if (forStmt->test && expressionContainsSuperCallForEval(*forStmt->test)) return true;
    if (forStmt->update && expressionContainsSuperCallForEval(*forStmt->update)) return true;
    return forStmt->body && statementContainsSuperCallForEval(*forStmt->body);
  }
  if (auto* forInStmt = std::get_if<ForInStmt>(&stmt.node)) {
    if (forInStmt->left && statementContainsSuperCallForEval(*forInStmt->left)) return true;
    if (forInStmt->right && expressionContainsSuperCallForEval(*forInStmt->right)) return true;
    return forInStmt->body && statementContainsSuperCallForEval(*forInStmt->body);
  }
  if (auto* forOfStmt = std::get_if<ForOfStmt>(&stmt.node)) {
    if (forOfStmt->left && statementContainsSuperCallForEval(*forOfStmt->left)) return true;
    if (forOfStmt->right && expressionContainsSuperCallForEval(*forOfStmt->right)) return true;
    return forOfStmt->body && statementContainsSuperCallForEval(*forOfStmt->body);
  }
  if (auto* doWhileStmt = std::get_if<DoWhileStmt>(&stmt.node)) {
    if (doWhileStmt->body && statementContainsSuperCallForEval(*doWhileStmt->body)) return true;
    return doWhileStmt->test && expressionContainsSuperCallForEval(*doWhileStmt->test);
  }
  if (auto* switchStmt = std::get_if<SwitchStmt>(&stmt.node)) {
    if (switchStmt->discriminant && expressionContainsSuperCallForEval(*switchStmt->discriminant)) return true;
    for (const auto& c : switchStmt->cases) {
      if (c.test && expressionContainsSuperCallForEval(*c.test)) return true;
      for (const auto& s : c.consequent) {
        if (s && statementContainsSuperCallForEval(*s)) return true;
      }
    }
    return false;
  }
  if (auto* label = std::get_if<LabelledStmt>(&stmt.node)) {
    return label->body && statementContainsSuperCallForEval(*label->body);
  }
  if (auto* tryStmt = std::get_if<TryStmt>(&stmt.node)) {
    for (const auto& s : tryStmt->block) {
      if (s && statementContainsSuperCallForEval(*s)) return true;
    }
    if (tryStmt->handler.paramPattern && expressionContainsSuperCallForEval(*tryStmt->handler.paramPattern)) return true;
    for (const auto& s : tryStmt->handler.body) {
      if (s && statementContainsSuperCallForEval(*s)) return true;
    }
    for (const auto& s : tryStmt->finalizer) {
      if (s && statementContainsSuperCallForEval(*s)) return true;
    }
    return false;
  }
  if (auto* withStmt = std::get_if<WithStmt>(&stmt.node)) {
    if (withStmt->object && expressionContainsSuperCallForEval(*withStmt->object)) return true;
    return withStmt->body && statementContainsSuperCallForEval(*withStmt->body);
  }
  return false;
}

bool expressionContainsArgumentsForEval(const Expression& expr) {
  if (auto* id = std::get_if<Identifier>(&expr.node)) {
    return id->name == "arguments";
  }
  if (auto* fn = std::get_if<FunctionExpr>(&expr.node)) {
    if (!fn->isArrow) {
      return false;
    }
    for (const auto& p : fn->params) {
      if (p.defaultValue && expressionContainsArgumentsForEval(*p.defaultValue)) {
        return true;
      }
    }
    for (const auto& s : fn->body) {
      if (s && statementContainsArgumentsForEval(*s)) {
        return true;
      }
    }
    return false;
  }
  if (std::holds_alternative<ClassExpr>(expr.node)) {
    return false;
  }
  if (auto* call = std::get_if<CallExpr>(&expr.node)) {
    if (call->callee && expressionContainsArgumentsForEval(*call->callee)) return true;
    for (const auto& arg : call->arguments) {
      if (arg && expressionContainsArgumentsForEval(*arg)) return true;
    }
    return false;
  }
  if (auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
    return (binary->left && expressionContainsArgumentsForEval(*binary->left)) ||
           (binary->right && expressionContainsArgumentsForEval(*binary->right));
  }
  if (auto* unary = std::get_if<UnaryExpr>(&expr.node)) {
    return unary->argument && expressionContainsArgumentsForEval(*unary->argument);
  }
  if (auto* assign = std::get_if<AssignmentExpr>(&expr.node)) {
    return (assign->left && expressionContainsArgumentsForEval(*assign->left)) ||
           (assign->right && expressionContainsArgumentsForEval(*assign->right));
  }
  if (auto* update = std::get_if<UpdateExpr>(&expr.node)) {
    return update->argument && expressionContainsArgumentsForEval(*update->argument);
  }
  if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
    return (member->object && expressionContainsArgumentsForEval(*member->object)) ||
           (member->property && expressionContainsArgumentsForEval(*member->property));
  }
  if (auto* cond = std::get_if<ConditionalExpr>(&expr.node)) {
    return (cond->test && expressionContainsArgumentsForEval(*cond->test)) ||
           (cond->consequent && expressionContainsArgumentsForEval(*cond->consequent)) ||
           (cond->alternate && expressionContainsArgumentsForEval(*cond->alternate));
  }
  if (auto* seq = std::get_if<SequenceExpr>(&expr.node)) {
    for (const auto& e : seq->expressions) {
      if (e && expressionContainsArgumentsForEval(*e)) return true;
    }
    return false;
  }
  if (auto* arr = std::get_if<ArrayExpr>(&expr.node)) {
    for (const auto& e : arr->elements) {
      if (e && expressionContainsArgumentsForEval(*e)) return true;
    }
    return false;
  }
  if (auto* obj = std::get_if<ObjectExpr>(&expr.node)) {
    for (const auto& prop : obj->properties) {
      if (prop.key && expressionContainsArgumentsForEval(*prop.key)) return true;
      if (prop.value && expressionContainsArgumentsForEval(*prop.value)) return true;
    }
    return false;
  }
  if (auto* awaitExpr = std::get_if<AwaitExpr>(&expr.node)) {
    return awaitExpr->argument && expressionContainsArgumentsForEval(*awaitExpr->argument);
  }
  if (auto* yieldExpr = std::get_if<YieldExpr>(&expr.node)) {
    return yieldExpr->argument && expressionContainsArgumentsForEval(*yieldExpr->argument);
  }
  if (auto* spread = std::get_if<SpreadElement>(&expr.node)) {
    return spread->argument && expressionContainsArgumentsForEval(*spread->argument);
  }
  if (auto* newExpr = std::get_if<NewExpr>(&expr.node)) {
    if (newExpr->callee && expressionContainsArgumentsForEval(*newExpr->callee)) return true;
    for (const auto& arg : newExpr->arguments) {
      if (arg && expressionContainsArgumentsForEval(*arg)) return true;
    }
    return false;
  }
  if (auto* tmpl = std::get_if<TemplateLiteral>(&expr.node)) {
    for (const auto& e : tmpl->expressions) {
      if (e && expressionContainsArgumentsForEval(*e)) return true;
    }
    return false;
  }
  if (auto* assignPat = std::get_if<AssignmentPattern>(&expr.node)) {
    return (assignPat->left && expressionContainsArgumentsForEval(*assignPat->left)) ||
           (assignPat->right && expressionContainsArgumentsForEval(*assignPat->right));
  }
  return false;
}

bool statementContainsArgumentsForEval(const Statement& stmt) {
  if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt.node)) {
    return exprStmt->expression && expressionContainsArgumentsForEval(*exprStmt->expression);
  }
  if (auto* ret = std::get_if<ReturnStmt>(&stmt.node)) {
    return ret->argument && expressionContainsArgumentsForEval(*ret->argument);
  }
  if (auto* throwStmt = std::get_if<ThrowStmt>(&stmt.node)) {
    return throwStmt->argument && expressionContainsArgumentsForEval(*throwStmt->argument);
  }
  if (auto* block = std::get_if<BlockStmt>(&stmt.node)) {
    for (const auto& s : block->body) {
      if (s && statementContainsArgumentsForEval(*s)) return true;
    }
    return false;
  }
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.node)) {
    for (const auto& d : varDecl->declarations) {
      if (d.pattern && expressionContainsArgumentsForEval(*d.pattern)) return true;
      if (d.init && expressionContainsArgumentsForEval(*d.init)) return true;
    }
    return false;
  }
  if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    if (ifStmt->test && expressionContainsArgumentsForEval(*ifStmt->test)) return true;
    if (ifStmt->consequent && statementContainsArgumentsForEval(*ifStmt->consequent)) return true;
    return ifStmt->alternate && statementContainsArgumentsForEval(*ifStmt->alternate);
  }
  if (auto* whileStmt = std::get_if<WhileStmt>(&stmt.node)) {
    if (whileStmt->test && expressionContainsArgumentsForEval(*whileStmt->test)) return true;
    return whileStmt->body && statementContainsArgumentsForEval(*whileStmt->body);
  }
  if (auto* forStmt = std::get_if<ForStmt>(&stmt.node)) {
    if (forStmt->init && statementContainsArgumentsForEval(*forStmt->init)) return true;
    if (forStmt->test && expressionContainsArgumentsForEval(*forStmt->test)) return true;
    if (forStmt->update && expressionContainsArgumentsForEval(*forStmt->update)) return true;
    return forStmt->body && statementContainsArgumentsForEval(*forStmt->body);
  }
  if (auto* forInStmt = std::get_if<ForInStmt>(&stmt.node)) {
    if (forInStmt->left && statementContainsArgumentsForEval(*forInStmt->left)) return true;
    if (forInStmt->right && expressionContainsArgumentsForEval(*forInStmt->right)) return true;
    return forInStmt->body && statementContainsArgumentsForEval(*forInStmt->body);
  }
  if (auto* forOfStmt = std::get_if<ForOfStmt>(&stmt.node)) {
    if (forOfStmt->left && statementContainsArgumentsForEval(*forOfStmt->left)) return true;
    if (forOfStmt->right && expressionContainsArgumentsForEval(*forOfStmt->right)) return true;
    return forOfStmt->body && statementContainsArgumentsForEval(*forOfStmt->body);
  }
  if (auto* doWhileStmt = std::get_if<DoWhileStmt>(&stmt.node)) {
    if (doWhileStmt->body && statementContainsArgumentsForEval(*doWhileStmt->body)) return true;
    return doWhileStmt->test && expressionContainsArgumentsForEval(*doWhileStmt->test);
  }
  if (auto* switchStmt = std::get_if<SwitchStmt>(&stmt.node)) {
    if (switchStmt->discriminant && expressionContainsArgumentsForEval(*switchStmt->discriminant)) return true;
    for (const auto& c : switchStmt->cases) {
      if (c.test && expressionContainsArgumentsForEval(*c.test)) return true;
      for (const auto& s : c.consequent) {
        if (s && statementContainsArgumentsForEval(*s)) return true;
      }
    }
    return false;
  }
  if (auto* label = std::get_if<LabelledStmt>(&stmt.node)) {
    return label->body && statementContainsArgumentsForEval(*label->body);
  }
  if (auto* tryStmt = std::get_if<TryStmt>(&stmt.node)) {
    for (const auto& s : tryStmt->block) {
      if (s && statementContainsArgumentsForEval(*s)) return true;
    }
    if (tryStmt->handler.paramPattern &&
        expressionContainsArgumentsForEval(*tryStmt->handler.paramPattern)) {
      return true;
    }
    for (const auto& s : tryStmt->handler.body) {
      if (s && statementContainsArgumentsForEval(*s)) return true;
    }
    for (const auto& s : tryStmt->finalizer) {
      if (s && statementContainsArgumentsForEval(*s)) return true;
    }
    return false;
  }
  if (auto* withStmt = std::get_if<WithStmt>(&stmt.node)) {
    if (withStmt->object && expressionContainsArgumentsForEval(*withStmt->object)) return true;
    return withStmt->body && statementContainsArgumentsForEval(*withStmt->body);
  }
  if (auto* exportDefault = std::get_if<ExportDefaultDeclaration>(&stmt.node)) {
    return exportDefault->declaration &&
           expressionContainsArgumentsForEval(*exportDefault->declaration);
  }
  return false;
}

bool expressionContainsNewTargetForEval(const Expression& expr) {
  if (auto* fn = std::get_if<FunctionExpr>(&expr.node)) {
    if (!fn->isArrow) {
      return false;
    }
    for (const auto& p : fn->params) {
      if (p.defaultValue && expressionContainsNewTargetForEval(*p.defaultValue)) {
        return true;
      }
    }
    for (const auto& s : fn->body) {
      if (s && statementContainsNewTargetForEval(*s)) {
        return true;
      }
    }
    return false;
  }
  if (auto* meta = std::get_if<MetaProperty>(&expr.node)) {
    return meta->meta == "new" && meta->property == "target";
  }
  if (auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
    return (binary->left && expressionContainsNewTargetForEval(*binary->left)) ||
           (binary->right && expressionContainsNewTargetForEval(*binary->right));
  }
  if (auto* unary = std::get_if<UnaryExpr>(&expr.node)) {
    return unary->argument && expressionContainsNewTargetForEval(*unary->argument);
  }
  if (auto* assign = std::get_if<AssignmentExpr>(&expr.node)) {
    return (assign->left && expressionContainsNewTargetForEval(*assign->left)) ||
           (assign->right && expressionContainsNewTargetForEval(*assign->right));
  }
  if (auto* update = std::get_if<UpdateExpr>(&expr.node)) {
    return update->argument && expressionContainsNewTargetForEval(*update->argument);
  }
  if (auto* call = std::get_if<CallExpr>(&expr.node)) {
    if (call->callee && expressionContainsNewTargetForEval(*call->callee)) return true;
    for (const auto& arg : call->arguments) {
      if (arg && expressionContainsNewTargetForEval(*arg)) return true;
    }
    return false;
  }
  if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
    return (member->object && expressionContainsNewTargetForEval(*member->object)) ||
           (member->property && expressionContainsNewTargetForEval(*member->property));
  }
  if (auto* cond = std::get_if<ConditionalExpr>(&expr.node)) {
    return (cond->test && expressionContainsNewTargetForEval(*cond->test)) ||
           (cond->consequent && expressionContainsNewTargetForEval(*cond->consequent)) ||
           (cond->alternate && expressionContainsNewTargetForEval(*cond->alternate));
  }
  if (auto* seq = std::get_if<SequenceExpr>(&expr.node)) {
    for (const auto& e : seq->expressions) {
      if (e && expressionContainsNewTargetForEval(*e)) return true;
    }
    return false;
  }
  if (auto* arr = std::get_if<ArrayExpr>(&expr.node)) {
    for (const auto& e : arr->elements) {
      if (e && expressionContainsNewTargetForEval(*e)) return true;
    }
    return false;
  }
  if (auto* obj = std::get_if<ObjectExpr>(&expr.node)) {
    for (const auto& prop : obj->properties) {
      if (prop.key && expressionContainsNewTargetForEval(*prop.key)) return true;
      if (prop.value && expressionContainsNewTargetForEval(*prop.value)) return true;
    }
    return false;
  }
  if (auto* awaitExpr = std::get_if<AwaitExpr>(&expr.node)) {
    return awaitExpr->argument && expressionContainsNewTargetForEval(*awaitExpr->argument);
  }
  if (auto* yieldExpr = std::get_if<YieldExpr>(&expr.node)) {
    return yieldExpr->argument && expressionContainsNewTargetForEval(*yieldExpr->argument);
  }
  if (auto* spread = std::get_if<SpreadElement>(&expr.node)) {
    return spread->argument && expressionContainsNewTargetForEval(*spread->argument);
  }
  if (auto* newExpr = std::get_if<NewExpr>(&expr.node)) {
    if (newExpr->callee && expressionContainsNewTargetForEval(*newExpr->callee)) return true;
    for (const auto& arg : newExpr->arguments) {
      if (arg && expressionContainsNewTargetForEval(*arg)) return true;
    }
    return false;
  }
  if (auto* arrPat = std::get_if<ArrayPattern>(&expr.node)) {
    for (const auto& e : arrPat->elements) {
      if (e && expressionContainsNewTargetForEval(*e)) return true;
    }
    return arrPat->rest && expressionContainsNewTargetForEval(*arrPat->rest);
  }
  if (auto* objPat = std::get_if<ObjectPattern>(&expr.node)) {
    for (const auto& prop : objPat->properties) {
      if (prop.key && expressionContainsNewTargetForEval(*prop.key)) return true;
      if (prop.value && expressionContainsNewTargetForEval(*prop.value)) return true;
    }
    return objPat->rest && expressionContainsNewTargetForEval(*objPat->rest);
  }
  if (auto* assignPat = std::get_if<AssignmentPattern>(&expr.node)) {
    return (assignPat->left && expressionContainsNewTargetForEval(*assignPat->left)) ||
           (assignPat->right && expressionContainsNewTargetForEval(*assignPat->right));
  }
  return false;
}

bool statementContainsNewTargetForEval(const Statement& stmt) {
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.node)) {
    for (const auto& decl : varDecl->declarations) {
      if (decl.pattern && expressionContainsNewTargetForEval(*decl.pattern)) return true;
      if (decl.init && expressionContainsNewTargetForEval(*decl.init)) return true;
    }
    return false;
  }
  if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt.node)) {
    return exprStmt->expression && expressionContainsNewTargetForEval(*exprStmt->expression);
  }
  if (auto* ret = std::get_if<ReturnStmt>(&stmt.node)) {
    return ret->argument && expressionContainsNewTargetForEval(*ret->argument);
  }
  if (auto* block = std::get_if<BlockStmt>(&stmt.node)) {
    for (const auto& s : block->body) {
      if (s && statementContainsNewTargetForEval(*s)) return true;
    }
    return false;
  }
  if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    if (ifStmt->test && expressionContainsNewTargetForEval(*ifStmt->test)) return true;
    if (ifStmt->consequent && statementContainsNewTargetForEval(*ifStmt->consequent)) return true;
    return ifStmt->alternate && statementContainsNewTargetForEval(*ifStmt->alternate);
  }
  if (auto* whileStmt = std::get_if<WhileStmt>(&stmt.node)) {
    if (whileStmt->test && expressionContainsNewTargetForEval(*whileStmt->test)) return true;
    return whileStmt->body && statementContainsNewTargetForEval(*whileStmt->body);
  }
  if (auto* forStmt = std::get_if<ForStmt>(&stmt.node)) {
    if (forStmt->init && statementContainsNewTargetForEval(*forStmt->init)) return true;
    if (forStmt->test && expressionContainsNewTargetForEval(*forStmt->test)) return true;
    if (forStmt->update && expressionContainsNewTargetForEval(*forStmt->update)) return true;
    return forStmt->body && statementContainsNewTargetForEval(*forStmt->body);
  }
  if (auto* withStmt = std::get_if<WithStmt>(&stmt.node)) {
    if (withStmt->object && expressionContainsNewTargetForEval(*withStmt->object)) return true;
    return withStmt->body && statementContainsNewTargetForEval(*withStmt->body);
  }
  if (auto* forInStmt = std::get_if<ForInStmt>(&stmt.node)) {
    if (forInStmt->left && statementContainsNewTargetForEval(*forInStmt->left)) return true;
    if (forInStmt->right && expressionContainsNewTargetForEval(*forInStmt->right)) return true;
    return forInStmt->body && statementContainsNewTargetForEval(*forInStmt->body);
  }
  if (auto* forOfStmt = std::get_if<ForOfStmt>(&stmt.node)) {
    if (forOfStmt->left && statementContainsNewTargetForEval(*forOfStmt->left)) return true;
    if (forOfStmt->right && expressionContainsNewTargetForEval(*forOfStmt->right)) return true;
    return forOfStmt->body && statementContainsNewTargetForEval(*forOfStmt->body);
  }
  if (auto* doWhileStmt = std::get_if<DoWhileStmt>(&stmt.node)) {
    if (doWhileStmt->body && statementContainsNewTargetForEval(*doWhileStmt->body)) return true;
    return doWhileStmt->test && expressionContainsNewTargetForEval(*doWhileStmt->test);
  }
  if (auto* switchStmt = std::get_if<SwitchStmt>(&stmt.node)) {
    if (switchStmt->discriminant && expressionContainsNewTargetForEval(*switchStmt->discriminant)) return true;
    for (const auto& c : switchStmt->cases) {
      if (c.test && expressionContainsNewTargetForEval(*c.test)) return true;
      for (const auto& s : c.consequent) {
        if (s && statementContainsNewTargetForEval(*s)) return true;
      }
    }
    return false;
  }
  if (auto* label = std::get_if<LabelledStmt>(&stmt.node)) {
    return label->body && statementContainsNewTargetForEval(*label->body);
  }
  if (auto* throwStmt = std::get_if<ThrowStmt>(&stmt.node)) {
    return throwStmt->argument && expressionContainsNewTargetForEval(*throwStmt->argument);
  }
  if (auto* tryStmt = std::get_if<TryStmt>(&stmt.node)) {
    for (const auto& s : tryStmt->block) {
      if (s && statementContainsNewTargetForEval(*s)) return true;
    }
    if (tryStmt->handler.paramPattern && expressionContainsNewTargetForEval(*tryStmt->handler.paramPattern)) return true;
    for (const auto& s : tryStmt->handler.body) {
      if (s && statementContainsNewTargetForEval(*s)) return true;
    }
    for (const auto& s : tryStmt->finalizer) {
      if (s && statementContainsNewTargetForEval(*s)) return true;
    }
    return false;
  }
  if (auto* exportDefault = std::get_if<ExportDefaultDeclaration>(&stmt.node)) {
    return exportDefault->declaration && expressionContainsNewTargetForEval(*exportDefault->declaration);
  }
  return false;
}

bool programContainsSuperForEval(const Program& program) {
  for (const auto& stmt : program.body) {
    if (stmt && statementContainsSuperForEval(*stmt)) return true;
  }
  return false;
}

bool programContainsSuperCallForEval(const Program& program) {
  for (const auto& stmt : program.body) {
    if (stmt && statementContainsSuperCallForEval(*stmt)) return true;
  }
  return false;
}

bool programContainsArgumentsForEval(const Program& program) {
  for (const auto& stmt : program.body) {
    if (stmt && statementContainsArgumentsForEval(*stmt)) return true;
  }
  return false;
}

bool programContainsNewTargetForEval(const Program& program) {
  for (const auto& stmt : program.body) {
    if (stmt && statementContainsNewTargetForEval(*stmt)) return true;
  }
  return false;
}

void collectTopLevelFunctionDeclarationNames(const Program& program, std::vector<std::string>& names) {
  for (const auto& stmt : program.body) {
    if (!stmt) continue;
    if (auto* funcDecl = std::get_if<FunctionDeclaration>(&stmt->node)) {
      if (!funcDecl->id.name.empty()) {
        names.push_back(funcDecl->id.name);
      }
    }
  }
}

bool isGlobalObjectExtensible(const GCPtr<Object>& globalObj) {
  return globalObj && !globalObj->sealed && !globalObj->frozen && !globalObj->nonExtensible;
}

bool isGlobalPropertyConfigurable(const GCPtr<Object>& globalObj, const std::string& name) {
  return globalObj->properties.find("__non_configurable_" + name) == globalObj->properties.end();
}

bool isGlobalPropertyWritable(const GCPtr<Object>& globalObj, const std::string& name) {
  return globalObj->properties.find("__non_writable_" + name) == globalObj->properties.end();
}

bool isGlobalPropertyEnumerable(const GCPtr<Object>& globalObj, const std::string& name) {
  return globalObj->properties.find("__non_enum_" + name) == globalObj->properties.end();
}

bool canDeclareGlobalVarBinding(const GCPtr<Object>& globalObj, const std::string& name) {
  if (globalObj->properties.find(name) != globalObj->properties.end()) {
    return true;
  }
  return isGlobalObjectExtensible(globalObj);
}

bool canDeclareGlobalFunctionBinding(const GCPtr<Object>& globalObj,
                                     const std::string& name,
                                     bool& shouldResetAttributes) {
  auto existing = globalObj->properties.find(name);
  if (existing == globalObj->properties.end()) {
    shouldResetAttributes = true;
    return isGlobalObjectExtensible(globalObj);
  }

  if (isGlobalPropertyConfigurable(globalObj, name)) {
    shouldResetAttributes = true;
    return true;
  }

  shouldResetAttributes = false;
  return isGlobalPropertyWritable(globalObj, name) &&
         isGlobalPropertyEnumerable(globalObj, name);
}

void resetGlobalDataPropertyAttributes(const GCPtr<Object>& globalObj, const std::string& name) {
  globalObj->properties.erase("__non_writable_" + name);
  globalObj->properties.erase("__non_enum_" + name);
  globalObj->properties.erase("__non_configurable_" + name);
  globalObj->properties.erase("__enum_" + name);
}

bool defineModuleNamespaceProperty(const GCPtr<Object>& obj,
                                   const std::string& key,
                                   const GCPtr<Object>& descriptor) {
  const std::string& toStringTagKey = WellKnownSymbols::toStringTagKey();
  const bool isExport = isModuleNamespaceExportKey(obj, key);
  const bool isToStringTag = (key == toStringTagKey);
  if (!isExport && !isToStringTag) {
    return false;
  }

  auto has = [&](const std::string& name) {
    return descriptor->properties.find(name) != descriptor->properties.end();
  };
  auto boolFieldMatches = [&](const std::string& name, bool expected) {
    if (!has(name)) return true;
    return descriptor->properties.at(name).toBool() == expected;
  };

  if (has("get") || has("set")) {
    return false;
  }

  if (isExport) {
    if (has("value")) {
      return false;
    }
    return boolFieldMatches("writable", true) &&
           boolFieldMatches("enumerable", true) &&
           boolFieldMatches("configurable", false);
  }

  if (has("value") && descriptor->properties.at("value").toString() != "Module") {
    return false;
  }
  return boolFieldMatches("writable", false) &&
         boolFieldMatches("enumerable", false) &&
         boolFieldMatches("configurable", false);
}

std::optional<std::string> readTextFile(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return std::nullopt;
  }
  std::stringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

std::vector<std::string> parseStaticImportSpecifiers(const std::string& source) {
  std::vector<std::string> specifiers;
  static const std::regex kImportRegex(
    R"((?:^|[\n\r])\s*import\s+(?:[^'";\n]*\s+from\s+)?["']([^"']+)["']\s*;)");
  for (std::sregex_iterator it(source.begin(), source.end(), kImportRegex), end; it != end; ++it) {
    specifiers.push_back((*it)[1].str());
  }
  return specifiers;
}

bool hasTopLevelAwaitInSource(const std::string& source) {
  static const std::regex kTopLevelAwaitRegex(R"((?:^|[\n\r])\s*await\b)");
  return std::regex_search(source, kTopLevelAwaitRegex);
}

void gatherAsyncTransitiveDependencies(const std::string& modulePath,
                                       ModuleLoader* loader,
                                       std::unordered_set<std::string>& visitedModules,
                                       std::unordered_set<std::string>& queuedAsyncModules,
                                       std::vector<std::string>& orderedAsyncModules) {
  if (!loader) {
    return;
  }
  if (!visitedModules.insert(modulePath).second) {
    return;
  }

  auto sourceOpt = readTextFile(modulePath);
  if (!sourceOpt.has_value()) {
    return;
  }

  if (hasTopLevelAwaitInSource(*sourceOpt)) {
    if (queuedAsyncModules.insert(modulePath).second) {
      orderedAsyncModules.push_back(modulePath);
    }
    return;
  }

  auto specifiers = parseStaticImportSpecifiers(*sourceOpt);
  for (const auto& specifier : specifiers) {
    std::string resolvedDependency = loader->resolvePath(specifier, modulePath);
    gatherAsyncTransitiveDependencies(
      resolvedDependency, loader, visitedModules, queuedAsyncModules, orderedAsyncModules);
  }
}

}  // namespace

// Global module loader and interpreter for dynamic imports
static std::shared_ptr<ModuleLoader> g_moduleLoader;
static Interpreter* g_interpreter = nullptr;
static Value g_arrayPrototype;

void setGlobalModuleLoader(std::shared_ptr<ModuleLoader> loader) {
  g_moduleLoader = loader;
}

void setGlobalInterpreter(Interpreter* interpreter) {
  g_interpreter = interpreter;
}

Interpreter* getGlobalInterpreter() {
  return g_interpreter;
}

void setGlobalArrayPrototype(const Value& proto) {
  g_arrayPrototype = proto;
}

Value getGlobalArrayPrototype() {
  return g_arrayPrototype;
}

GCPtr<Array> makeArrayWithPrototype() {
  auto arr = GarbageCollector::makeGC<Array>();
  GarbageCollector::instance().reportAllocation(sizeof(Array));
  if (!g_arrayPrototype.isUndefined()) {
    arr->properties["__proto__"] = g_arrayPrototype;
  }
  return arr;
}

Environment::Environment(Environment* parent)
  : parent_(parent) {
  GarbageCollector::instance().reportAllocation(sizeof(Environment));
}

void Environment::define(const std::string& name, const Value& value, bool isConst) {
  bindings_[name] = value;
  tdzBindings_.erase(name);  // Remove TDZ when initialized
  if (isConst) {
    constants_[name] = true;
  } else {
    constants_.erase(name);
  }
  silentImmutables_.erase(name);
  if (!parent_) {
    auto it = bindings_.find("globalThis");
    if (it != bindings_.end() && it->second.isObject()) {
      auto globalObj = it->second.getGC<Object>();
      globalObj->properties[name] = value;
    }
  }
}

void Environment::setBindingDirect(const std::string& name, const Value& value) {
  bindings_[name] = value;
  tdzBindings_.erase(name);
  constants_.erase(name);
  silentImmutables_.erase(name);
  // Do NOT sync to globalThis - caller manages that
}

void Environment::defineImmutableNFE(const std::string& name, const Value& value) {
  bindings_[name] = value;
  silentImmutables_[name] = true;
}

void Environment::defineLexical(const std::string& name, const Value& value, bool isConst) {
  bindings_[name] = value;
  tdzBindings_.erase(name);
  lexicalBindings_[name] = true;
  if (isConst) {
    constants_[name] = true;
  } else {
    constants_.erase(name);
  }
  silentImmutables_.erase(name);
}

void Environment::defineTDZ(const std::string& name) {
  bindings_[name] = Value(Undefined{});
  tdzBindings_[name] = true;
  lexicalBindings_[name] = true;
  constants_.erase(name);
  silentImmutables_.erase(name);
}

void Environment::removeTDZ(const std::string& name) {
  tdzBindings_.erase(name);
}

bool Environment::isTDZ(const std::string& name) const {
  // Check if binding exists in this scope and is in TDZ
  auto bindIt = bindings_.find(name);
  if (bindIt != bindings_.end()) {
    return tdzBindings_.find(name) != tdzBindings_.end();
  }
  // If not found in this scope, check parent
  if (parent_) {
    return parent_->isTDZ(name);
  }
  return false;
}

std::optional<Value> Environment::get(const std::string& name) const {
  auto it = bindings_.find(name);
  if (it != bindings_.end()) {
    return it->second;
  }
  auto withScopeIt = bindings_.find(kWithScopeObjectBinding);
  if (withScopeIt != bindings_.end()) {
    if (auto scopeValue = resolveWithScopeValue(name)) {
      auto withValue = getWithScopeBindingValue(*scopeValue, name, false);
      if (withValue.has_value()) {
        return withValue;
      }
    }
  }
  if (parent_) {
    return parent_->get(name);
  }
  // At global scope, fall back to globalThis properties.
  // In JavaScript, the global object acts as the variable environment
  // for global code, so properties on globalThis are accessible as variables.
  auto globalIt = bindings_.find("globalThis");
  if (globalIt != bindings_.end() && globalIt->second.isObject()) {
    auto globalObj = globalIt->second.getGC<Object>();
    auto getterIt = globalObj->properties.find("__get_" + name);
    if (getterIt != globalObj->properties.end() && getterIt->second.isFunction()) {
      if (auto* interpreter = getGlobalInterpreter()) {
        return interpreter->callForHarness(getterIt->second, {}, Value(globalObj));
      }
      return Value(Undefined{});
    }
    if (globalObj->properties.find("__set_" + name) != globalObj->properties.end()) {
      return Value(Undefined{});
    }
    auto propIt = globalObj->properties.find(name);
    if (propIt != globalObj->properties.end()) {
      return propIt->second;
    }
  }
  return std::nullopt;
}

std::optional<Value> Environment::getIgnoringWith(const std::string& name) const {
  auto it = bindings_.find(name);
  if (it != bindings_.end()) {
    return it->second;
  }
  if (parent_) {
    return parent_->getIgnoringWith(name);
  }
  auto globalIt = bindings_.find("globalThis");
  if (globalIt != bindings_.end() && globalIt->second.isObject()) {
    auto globalObj = globalIt->second.getGC<Object>();
    auto getterIt = globalObj->properties.find("__get_" + name);
    if (getterIt != globalObj->properties.end() && getterIt->second.isFunction()) {
      if (auto* interpreter = getGlobalInterpreter()) {
        return interpreter->callForHarness(getterIt->second, {}, Value(globalObj));
      }
      return Value(Undefined{});
    }
    if (globalObj->properties.find("__set_" + name) != globalObj->properties.end()) {
      return Value(Undefined{});
    }
    auto propIt = globalObj->properties.find(name);
    if (propIt != globalObj->properties.end()) {
      return propIt->second;
    }
  }
  return std::nullopt;
}

bool Environment::set(const std::string& name, const Value& value) {
  auto it = bindings_.find(name);
  if (it != bindings_.end()) {
    if (silentImmutables_.find(name) != silentImmutables_.end()) {
      return false;  // silently ignore writes to NFE name bindings
    }
    if (constants_.find(name) != constants_.end()) {
      return false;
    }
    bindings_[name] = value;
    // Keep existing global object properties in sync with root-scope bindings.
    if (!parent_) {
      auto globalIt = bindings_.find("globalThis");
      if (globalIt != bindings_.end() && globalIt->second.isObject()) {
        auto globalObj = globalIt->second.getGC<Object>();
        if (globalObj->properties.find(name) != globalObj->properties.end()) {
          globalObj->properties[name] = value;
        }
      }
    }
    return true;
  }
  if (auto scopeValue = resolveWithScopeValue(name)) {
    if (setWithScopeProperty(*scopeValue, name, value, false)) {
      return true;
    }
  }
  if (parent_) {
    return parent_->set(name, value);
  }
  // At global scope, also check/set globalThis properties
  if (!parent_) {
    auto globalIt = bindings_.find("globalThis");
    if (globalIt != bindings_.end() && globalIt->second.isObject()) {
      auto globalObj = globalIt->second.getGC<Object>();
      auto propIt = globalObj->properties.find(name);
      if (propIt != globalObj->properties.end()) {
        propIt->second = value;
        return true;
      }
    }
  }
  return false;
}

bool Environment::has(const std::string& name) const {
  if (bindings_.find(name) != bindings_.end()) {
    return true;
  }
  auto withScopeIt = bindings_.find(kWithScopeObjectBinding);
  if (withScopeIt != bindings_.end() &&
      resolveWithScopeValue(name).has_value()) {
    return true;
  }
  if (parent_) {
    return parent_->has(name);
  }
  // At global scope, also check globalThis properties
  auto globalIt = bindings_.find("globalThis");
  if (globalIt != bindings_.end() && globalIt->second.isObject()) {
    auto globalObj = globalIt->second.getGC<Object>();
    if (globalObj->properties.find(name) != globalObj->properties.end()) {
      return true;
    }
  }
  return false;
}

bool Environment::hasLocal(const std::string& name) const {
  return bindings_.find(name) != bindings_.end();
}

bool Environment::hasLexicalLocal(const std::string& name) const {
  auto it = lexicalBindings_.find(name);
  return it != lexicalBindings_.end() && it->second;
}

bool Environment::deleteLocalMutable(const std::string& name) {
  auto it = bindings_.find(name);
  if (it == bindings_.end()) {
    return false;
  }
  if (constants_.find(name) != constants_.end()) {
    return false;
  }
  auto lexIt = lexicalBindings_.find(name);
  if (lexIt != lexicalBindings_.end() && lexIt->second) {
    return false;
  }
  bindings_.erase(it);
  constants_.erase(name);
  lexicalBindings_.erase(name);
  tdzBindings_.erase(name);
  return true;
}

int Environment::deleteFromWithScope(const std::string& name) {
  auto withScopeIt = bindings_.find(kWithScopeObjectBinding);
  if (withScopeIt != bindings_.end()) {
    if (deleteWithScopeProperty(withScopeIt->second, name)) {
      return 1;  // deleted
    }
    // Check if it exists but is non-configurable
    if (lookupWithScopeProperty(withScopeIt->second, name).has_value()) {
      return -1;  // exists but non-configurable
    }
  }
  if (parent_) {
    return parent_->deleteFromWithScope(name);
  }
  return 0;  // not found in any with scope
}

bool Environment::setVar(const std::string& name, const Value& value) {
  // For var declarations: walk the scope chain looking for bindings,
  // but skip with-scope objects. This ensures var assignments go to the
  // hoisted variable in the function/global scope, not to with-scope properties.
  auto it = bindings_.find(name);
  if (it != bindings_.end()) {
    it->second = value;
    return true;
  }
  if (parent_) {
    return parent_->setVar(name, value);
  }
  return false;
}

Environment* Environment::resolveBindingEnvironment(const std::string& name) {
  if (bindings_.find(name) != bindings_.end()) {
    return this;
  }
  if (parent_) {
    return parent_->resolveBindingEnvironment(name);
  }
  return nullptr;
}

std::optional<Value> Environment::resolveWithScopeValue(const std::string& name) const {
  auto it = bindings_.find(name);
  if (it != bindings_.end()) {
    return std::nullopt;
  }
  auto withScopeIt = bindings_.find(kWithScopeObjectBinding);
  if (withScopeIt != bindings_.end()) {
    if (hasPropertyLike(withScopeIt->second, name)) {
      if (auto* interpreter = getGlobalInterpreter(); interpreter && interpreter->hasError()) {
        return withScopeIt->second;
      }
      if (!isBlockedByUnscopables(withScopeIt->second, name)) {
        if (auto* interpreter = getGlobalInterpreter(); interpreter && interpreter->hasError()) {
          return withScopeIt->second;
        }
        return withScopeIt->second;
      }
      if (auto* interpreter = getGlobalInterpreter(); interpreter && interpreter->hasError()) {
        return withScopeIt->second;
      }
    }
  }
  if (parent_) {
    return parent_->resolveWithScopeValue(name);
  }
  return std::nullopt;
}

std::optional<Value> Environment::getWithScopeBindingValue(const Value& scopeValue,
                                                           const std::string& name,
                                                           bool strict) const {
  if (!hasPropertyLike(scopeValue, name)) {
    if (auto* interpreter = getGlobalInterpreter(); interpreter && interpreter->hasError()) {
      return std::nullopt;
    }
    if (strict) {
      return std::nullopt;
    }
    return Value(Undefined{});
  }
  if (auto* interpreter = getGlobalInterpreter(); interpreter && interpreter->hasError()) {
    return std::nullopt;
  }
  auto [found, value] = getPropertyLike(scopeValue, name, scopeValue);
  if (auto* interpreter = getGlobalInterpreter(); interpreter && interpreter->hasError()) {
    return std::nullopt;
  }
  if (!found) {
    return Value(Undefined{});
  }
  return value;
}

bool Environment::setWithScopeBindingValue(const Value& scopeValue,
                                           const std::string& name,
                                           const Value& value,
                                           bool strict) const {
  return setWithScopeProperty(scopeValue, name, value, strict);
}

GCPtr<Object> Environment::resolveWithScopeObject(const std::string& name) const {
  auto scopeValue = resolveWithScopeValue(name);
  if (scopeValue && scopeValue->isObject()) {
    return scopeValue->getGC<Object>();
  }
  return nullptr;
}

bool Environment::isConst(const std::string& name) const {
  if (constants_.find(name) != constants_.end()) {
    return true;
  }
  if (parent_) {
    return parent_->isConst(name);
  }
  return false;
}

bool Environment::isSilentImmutable(const std::string& name) const {
  if (silentImmutables_.find(name) != silentImmutables_.end()) {
    return true;
  }
  if (parent_) {
    return parent_->isSilentImmutable(name);
  }
  return false;
}

GCPtr<Environment> Environment::createChild() {
  return GarbageCollector::makeGC<Environment>(this);
}

GCPtr<Environment> Environment::createGlobal() {
  auto env = GarbageCollector::makeGC<Environment>();

  auto consoleFn = GarbageCollector::makeGC<Function>();
  consoleFn->isNative = true;
  consoleFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    for (const auto& arg : args) {
      std::cout << arg.toString() << " ";
    }
    std::cout << std::endl;
    return Value(Undefined{});
  };

  // console.error - writes to stderr
  auto consoleErrorFn = GarbageCollector::makeGC<Function>();
  consoleErrorFn->isNative = true;
  consoleErrorFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    for (const auto& arg : args) {
      std::cerr << arg.toString() << " ";
    }
    std::cerr << std::endl;
    return Value(Undefined{});
  };

  // console.warn - writes to stderr
  auto consoleWarnFn = GarbageCollector::makeGC<Function>();
  consoleWarnFn->isNative = true;
  consoleWarnFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    for (const auto& arg : args) {
      std::cerr << arg.toString() << " ";
    }
    std::cerr << std::endl;
    return Value(Undefined{});
  };

  // console.info - same as log
  auto consoleInfoFn = GarbageCollector::makeGC<Function>();
  consoleInfoFn->isNative = true;
  consoleInfoFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    for (const auto& arg : args) {
      std::cout << arg.toString() << " ";
    }
    std::cout << std::endl;
    return Value(Undefined{});
  };

  // console.debug - same as log
  auto consoleDebugFn = GarbageCollector::makeGC<Function>();
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

  auto consoleTimeFn = GarbageCollector::makeGC<Function>();
  consoleTimeFn->isNative = true;
  consoleTimeFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    std::string label = args.empty() ? "default" : args[0].toString();
    consoleTimers[label] = std::chrono::steady_clock::now();
    return Value(Undefined{});
  };

  auto consoleTimeEndFn = GarbageCollector::makeGC<Function>();
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
  auto consoleAssertFn = GarbageCollector::makeGC<Function>();
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

  auto consoleObj = GarbageCollector::makeGC<Object>();
  consoleObj->properties["log"] = Value(consoleFn);
  consoleObj->properties["error"] = Value(consoleErrorFn);
  consoleObj->properties["warn"] = Value(consoleWarnFn);
  consoleObj->properties["info"] = Value(consoleInfoFn);
  consoleObj->properties["debug"] = Value(consoleDebugFn);
  consoleObj->properties["time"] = Value(consoleTimeFn);
  consoleObj->properties["timeEnd"] = Value(consoleTimeEndFn);
  consoleObj->properties["assert"] = Value(consoleAssertFn);

  env->define("console", Value(consoleObj));

  auto evalFn = GarbageCollector::makeGC<Function>();
  evalFn->isNative = true;
  evalFn->nativeFunc = [env](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(Undefined{});
    }
    if (!args[0].isString()) {
      return args[0];
    }

    Interpreter* prevInterpreter = getGlobalInterpreter();
    if (!prevInterpreter) {
      return Value(Undefined{});
    }
    bool isDirectEval = prevInterpreter->inDirectEvalInvocation();

    auto evalEnv = env;
    if (isDirectEval) {
      auto callerEnv = prevInterpreter->getEnvironment();
      if (callerEnv) {
        evalEnv = callerEnv;
      }
    }

    std::string source = args[0].toString();
    Lexer lexer(source);
    auto tokens = lexer.tokenize();

    for (size_t i = 0; i + 2 < tokens.size(); ++i) {
      if (tokens[i].type == TokenType::Import &&
          !tokens[i].escaped &&
          tokens[i + 1].type == TokenType::Dot &&
          tokens[i + 2].type == TokenType::Identifier &&
          !tokens[i + 2].escaped &&
          tokens[i + 2].value == "meta") {
        throw std::runtime_error("SyntaxError: import.meta is not allowed in eval");
      }
    }

    // Direct eval inherits strict mode from calling context (ES2020 18.2.1.1)
    bool inheritStrict = isDirectEval && prevInterpreter->isStrictMode();

    Parser parser(tokens);
    parser.setEvalContext(true);
    if (inheritStrict) {
      parser.setStrictMode(true);
    }
    if (isDirectEval) {
      auto allowedPrivateNames = prevInterpreter->activePrivateNamesForEval();
      if (!allowedPrivateNames.empty()) {
        parser.setAllowedPrivateNames(allowedPrivateNames);
      }
    }
    auto program = parser.parse();
    if (!program) {
      throw std::runtime_error("SyntaxError: Parse error");
    }

    bool strictEval = inheritStrict || bodyHasUseStrictDirective(program->body);

    auto varEnv = evalEnv;
    if (isDirectEval) {
      auto scope = evalEnv;
      while (scope && !scope->hasLocal("__var_scope__")) {
        auto parent = scope->getParentPtr();
        if (!parent) {
          break;
        }
        scope = parent;
      }
      if (scope) {
        varEnv = scope;
      } else if (evalEnv) {
        varEnv = evalEnv->getRoot();
      }
    } else if (env) {
      varEnv = env->getRoot();
    }

    if (isDirectEval) {
      bool inFieldInitializer = prevInterpreter->inFieldInitializerEvaluation() ||
                                evalEnv->get("__in_class_field_initializer__").has_value();
      bool inArrow = prevInterpreter->activeFunctionIsArrow();
      bool inMethod = (prevInterpreter->activeFunctionHasHomeObject() ||
                       prevInterpreter->activeFunctionHasSuperClassBinding() ||
                       evalEnv->hasLocal("__super__"));
      // Constructor-ness is dynamic (`[[Construct]]` call state), not whether
      // the active function object is constructable.
      bool inConstructor = false;
      {
        auto ntVal = evalEnv->get("__new_target__");
        if (ntVal && !ntVal->isUndefined()) {
          inConstructor = true;
        }
      }
      bool allowNewTarget = (!inArrow && (prevInterpreter->hasActiveFunction() ||
                                          evalEnv->hasLocal("__new_target__")));
      if (inFieldInitializer) {
        // Additional eval-in-initializer rules: treat as outside constructor,
        // inside method, and inside function.
        inMethod = true;
        inConstructor = false;
        allowNewTarget = true;
      } else if (inArrow) {
        inMethod = false;
        inConstructor = false;
      }

      if (!inMethod && programContainsSuperForEval(*program)) {
        throw std::runtime_error("SyntaxError: 'super' keyword is not valid here");
      }
      if (!inConstructor && programContainsSuperCallForEval(*program)) {
        throw std::runtime_error("SyntaxError: 'super' keyword is not valid here");
      }
      if (inFieldInitializer && programContainsArgumentsForEval(*program)) {
        throw std::runtime_error("SyntaxError: 'arguments' is not allowed in this context");
      }
      if (!allowNewTarget && programContainsNewTargetForEval(*program)) {
        throw std::runtime_error("SyntaxError: new.target is not allowed in this context");
      }

      if (prevInterpreter->inParameterInitializerEvaluation() && evalEnv->hasLocal("arguments")) {
        std::vector<std::string> declaredNames;
        for (const auto& stmt : program->body) {
          if (stmt) {
            collectDeclaredNamesForDirectEval(*stmt, declaredNames);
          }
        }
        for (const auto& name : declaredNames) {
          if (name == "arguments") {
            throw std::runtime_error("SyntaxError: Identifier 'arguments' has already been declared");
          }
        }
      }

      if (!strictEval) {
        std::vector<std::string> varNames;
        for (const auto& stmt : program->body) {
          if (stmt) {
            collectVarNamesFromStatement(*stmt, varNames);
          }
        }
        std::vector<std::string> topLevelFnNames;
        collectTopLevelFunctionDeclarationNames(*program, topLevelFnNames);
        varNames.insert(varNames.end(), topLevelFnNames.begin(), topLevelFnNames.end());
        std::unordered_set<std::string> seenNames;
        for (const auto& varName : varNames) {
          if (!seenNames.insert(varName).second) {
            continue;
          }
          auto scope = evalEnv;
          while (scope && scope.get() != varEnv.get()) {
            // Skip with-scope object environments (cannot contain lexical declarations).
            if (scope->hasLocal(kWithScopeObjectBinding)) {
              scope = scope->getParentPtr();
              continue;
            }
            if (scope->hasLexicalLocal(varName)) {
              throw std::runtime_error("SyntaxError: Identifier '" + varName + "' has already been declared");
            }
            scope = scope->getParentPtr();
          }
          // Also reject conflicts in the variable environment itself (not just at global scope).
          // This covers parameter bindings and top-level lexical declarations in sloppy functions.
          if (varEnv && varEnv->hasLexicalLocal(varName)) {
            throw std::runtime_error("SyntaxError: Identifier '" + varName + "' has already been declared");
          }
        }
      }
    } else {
      if (programContainsSuperForEval(*program) || programContainsSuperCallForEval(*program)) {
        throw std::runtime_error("SyntaxError: 'super' keyword is not valid here");
      }
      if (programContainsNewTargetForEval(*program)) {
        throw std::runtime_error("SyntaxError: new.target is not allowed in this context");
      }

      if (!strictEval && varEnv && !varEnv->getParent()) {
        std::vector<std::string> varNames;
        for (const auto& stmt : program->body) {
          if (stmt) {
            collectVarNamesFromStatement(*stmt, varNames);
          }
        }
        std::vector<std::string> topLevelFnNames;
        collectTopLevelFunctionDeclarationNames(*program, topLevelFnNames);
        varNames.insert(varNames.end(), topLevelFnNames.begin(), topLevelFnNames.end());

        std::unordered_set<std::string> seenNames;
        for (const auto& varName : varNames) {
          if (!seenNames.insert(varName).second) {
            continue;
          }
          if (varEnv->hasLexicalLocal(varName)) {
            throw std::runtime_error("SyntaxError: Identifier '" + varName + "' has already been declared");
          }
        }
      }
    }

    GCPtr<Environment> execOuterEnv = isDirectEval ? evalEnv : GCPtr<Environment>(nullptr);
    if (!isDirectEval && env) {
      execOuterEnv = env->getRoot();
    }
    if (!execOuterEnv) {
      execOuterEnv = varEnv ? varEnv : env;
    }
    auto execEnv = execOuterEnv->createChild();

    std::vector<std::string> varScopedNames;
    for (const auto& stmt : program->body) {
      if (stmt) {
        collectVarNamesFromStatement(*stmt, varScopedNames);
      }
    }
    std::vector<std::string> topLevelFnNames;
    collectTopLevelFunctionDeclarationNames(*program, topLevelFnNames);
    varScopedNames.insert(varScopedNames.end(), topLevelFnNames.begin(), topLevelFnNames.end());

    struct GlobalFunctionPlan {
      std::string name;
      bool resetAttributes = false;
      bool preserveExistingAttributes = false;
      bool hadNonWritable = false;
      bool hadNonEnumerable = false;
      bool hadNonConfigurable = false;
      bool hadEnumMarker = false;
    };
    struct GlobalVarPlan {
      std::string name;
      bool created = false;
      bool preserveExistingAttributes = false;
      bool hadNonWritable = false;
      bool hadNonEnumerable = false;
      bool hadNonConfigurable = false;
      bool hadEnumMarker = false;
    };
    std::vector<GlobalFunctionPlan> globalFunctionPlans;
    std::vector<GlobalVarPlan> globalVarPlans;
    GCPtr<Object> globalObj;

    if (!strictEval && varEnv && !varEnv->getParent()) {
      auto globalThis = varEnv->get("globalThis");
      if (globalThis && globalThis->isObject()) {
        globalObj = globalThis->getGC<Object>();

        std::vector<std::string> topLevelFnNames;
        collectTopLevelFunctionDeclarationNames(*program, topLevelFnNames);
        std::unordered_set<std::string> declaredFunctionNames;
        for (auto it = topLevelFnNames.rbegin(); it != topLevelFnNames.rend(); ++it) {
          if (!declaredFunctionNames.insert(*it).second) {
            continue;
          }
          bool resetAttrs = true;
          if (!canDeclareGlobalFunctionBinding(globalObj, *it, resetAttrs)) {
            throw std::runtime_error("TypeError: Cannot declare global function '" + *it + "'");
          }
          GlobalFunctionPlan plan;
          plan.name = *it;
          plan.resetAttributes = resetAttrs;
          if (globalObj->properties.find(*it) != globalObj->properties.end() && !resetAttrs) {
            plan.preserveExistingAttributes = true;
            plan.hadNonWritable = globalObj->properties.find("__non_writable_" + *it) != globalObj->properties.end();
            plan.hadNonEnumerable = globalObj->properties.find("__non_enum_" + *it) != globalObj->properties.end();
            plan.hadNonConfigurable = globalObj->properties.find("__non_configurable_" + *it) != globalObj->properties.end();
            plan.hadEnumMarker = globalObj->properties.find("__enum_" + *it) != globalObj->properties.end();
          }
          globalFunctionPlans.push_back(plan);
        }

        std::vector<std::string> declaredVarNames;
        for (const auto& stmt : program->body) {
          if (stmt) {
            collectVarNamesFromStatement(*stmt, declaredVarNames);
          }
        }
        std::unordered_set<std::string> seenVarNames;
        for (const auto& vn : declaredVarNames) {
          if (declaredFunctionNames.count(vn) != 0) {
            continue;
          }
          if (!seenVarNames.insert(vn).second) {
            continue;
          }
          bool exists = globalObj->properties.find(vn) != globalObj->properties.end();
          if (!canDeclareGlobalVarBinding(globalObj, vn)) {
            throw std::runtime_error("TypeError: Cannot declare global variable '" + vn + "'");
          }
          GlobalVarPlan plan;
          plan.name = vn;
          plan.created = !exists;
          if (exists) {
            plan.preserveExistingAttributes = true;
            plan.hadNonWritable = globalObj->properties.find("__non_writable_" + vn) != globalObj->properties.end();
            plan.hadNonEnumerable = globalObj->properties.find("__non_enum_" + vn) != globalObj->properties.end();
            plan.hadNonConfigurable = globalObj->properties.find("__non_configurable_" + vn) != globalObj->properties.end();
            plan.hadEnumMarker = globalObj->properties.find("__enum_" + vn) != globalObj->properties.end();
          }
          globalVarPlans.push_back(plan);
        }
      }
    }

    if (!strictEval && varEnv) {
      std::unordered_set<std::string> seeded;
      for (const auto& name : varScopedNames) {
        if (!seeded.insert(name).second) {
          continue;
        }
        // For direct eval in function scope, var declarations are instantiated in
        // the variable environment record (the nearest __var_scope__), and must
        // not observe bindings in outer environments (Test262 S11.13.1_A6_T1).
        // For global eval, preserve existing global bindings/properties.
        bool varExistsInVarEnv = false;
        if (varEnv->getParent() == nullptr) {
          varExistsInVarEnv = varEnv->has(name);
        } else {
          varExistsInVarEnv = varEnv->hasLocal(name);
        }
        if (!varExistsInVarEnv || varEnv->hasLexicalLocal(name)) {
          continue;
        }
        auto existing = varEnv->get(name);
        if (existing.has_value()) {
          execEnv->define(name, *existing);
        }
      }
    }

    if (!strictEval && isDirectEval && varEnv && varEnv->getParent()) {
      execEnv->define("__eval_deletable_bindings__", Value(true), true);
    }

    // Move program to heap so functions created during eval can keep AST alive
    auto programPtr = std::make_shared<Program>(std::move(*program));

    Interpreter evalInterpreter(execEnv);
    if (isDirectEval) {
      evalInterpreter.inheritDirectEvalContextFrom(*prevInterpreter);
    }
    if (strictEval) {
      evalInterpreter.setStrictMode(true);
    }
    evalInterpreter.setSourceKeepAlive(programPtr);
    Value result = Value(Undefined{});
    try {
      setGlobalInterpreter(&evalInterpreter);
      auto task = evalInterpreter.evaluate(*programPtr);
      while (!task.done()) {
        task.resume();
      }
      result = task.result();
      if (evalInterpreter.hasError()) {
        Value err = evalInterpreter.getError();
        evalInterpreter.clearError();
        setGlobalInterpreter(prevInterpreter);
        throw JsValueException(err);
      }
    } catch (...) {
      setGlobalInterpreter(prevInterpreter);
      throw;
    }

    if (!strictEval && varEnv) {
      std::unordered_set<std::string> propagated;
      for (const auto& name : varScopedNames) {
        if (!propagated.insert(name).second) {
          continue;
        }
        if (!execEnv->hasLocal(name)) {
          continue;
        }
        auto value = execEnv->get(name);
        if (!value.has_value()) {
          continue;
        }
        if (varEnv->hasLocal(name)) {
          varEnv->set(name, *value);
        } else {
          varEnv->define(name, *value);
        }
      }

      if (globalObj) {
        for (const auto& plan : globalFunctionPlans) {
          if (globalObj->properties.find(plan.name) == globalObj->properties.end()) {
            continue;
          }
          if (plan.resetAttributes) {
            resetGlobalDataPropertyAttributes(globalObj, plan.name);
            continue;
          }
          if (plan.preserveExistingAttributes) {
            if (plan.hadNonWritable) globalObj->properties["__non_writable_" + plan.name] = Value(true);
            else globalObj->properties.erase("__non_writable_" + plan.name);
            if (plan.hadNonEnumerable) globalObj->properties["__non_enum_" + plan.name] = Value(true);
            else globalObj->properties.erase("__non_enum_" + plan.name);
            if (plan.hadNonConfigurable) globalObj->properties["__non_configurable_" + plan.name] = Value(true);
            else globalObj->properties.erase("__non_configurable_" + plan.name);
            if (plan.hadEnumMarker) globalObj->properties["__enum_" + plan.name] = Value(true);
            else globalObj->properties.erase("__enum_" + plan.name);
          }
        }
        for (const auto& plan : globalVarPlans) {
          if (globalObj->properties.find(plan.name) == globalObj->properties.end()) {
            continue;
          }
          if (plan.created) {
            resetGlobalDataPropertyAttributes(globalObj, plan.name);
            continue;
          }
          if (plan.preserveExistingAttributes) {
            if (plan.hadNonWritable) globalObj->properties["__non_writable_" + plan.name] = Value(true);
            else globalObj->properties.erase("__non_writable_" + plan.name);
            if (plan.hadNonEnumerable) globalObj->properties["__non_enum_" + plan.name] = Value(true);
            else globalObj->properties.erase("__non_enum_" + plan.name);
            if (plan.hadNonConfigurable) globalObj->properties["__non_configurable_" + plan.name] = Value(true);
            else globalObj->properties.erase("__non_configurable_" + plan.name);
            if (plan.hadEnumMarker) globalObj->properties["__enum_" + plan.name] = Value(true);
            else globalObj->properties.erase("__enum_" + plan.name);
          }
        }
      }
    }

    setGlobalInterpreter(prevInterpreter);
    return result;
  };
  evalFn->properties["__is_intrinsic_eval__"] = Value(true);
  evalFn->properties["name"] = Value(std::string("eval"));
  evalFn->properties["__non_writable_name"] = Value(true);
  evalFn->properties["__non_enum_name"] = Value(true);
  evalFn->properties["length"] = Value(1.0);
  evalFn->properties["__non_writable_length"] = Value(true);
  evalFn->properties["__non_enum_length"] = Value(true);
  env->define("eval", Value(evalFn));

  // Symbol constructor
  auto symbolFn = GarbageCollector::makeGC<Function>();
  symbolFn->isNative = true;
  symbolFn->isConstructor = true;
  symbolFn->properties["name"] = Value(std::string("Symbol"));
  symbolFn->properties["__non_writable_name"] = Value(true);
  symbolFn->properties["__non_enum_name"] = Value(true);
  symbolFn->properties["length"] = Value(0.0);
  symbolFn->properties["__non_writable_length"] = Value(true);
  symbolFn->properties["__non_enum_length"] = Value(true);
  // Symbol is a constructor whose [[Construct]] always throws (per spec).
  // Keep it as a constructor so it can be used in `extends`, but prevent `new Symbol()`.
  symbolFn->properties["__throw_on_new__"] = Value(true);
  symbolFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || args[0].isUndefined()) {
      return Value(Symbol::withoutDescription());
    }
    if (args[0].isSymbol()) {
      throw std::runtime_error("TypeError: Cannot convert a Symbol value to a string");
    }
    // ES spec: ToString(description) - must call ToPrimitive for objects
    std::string description;
    if (args[0].isObject() || args[0].isArray() || args[0].isFunction()) {
      // Call toString() on the object via interpreter
      Interpreter* interp = getGlobalInterpreter();
      if (interp) {
        // Try toString first, then valueOf
        Value obj = args[0];
        Value result;
        bool gotPrimitive = false;
        // Try toString
        if (obj.isObject()) {
          auto objPtr = obj.getGC<Object>();
          auto toStringIt = objPtr->properties.find("toString");
          if (toStringIt != objPtr->properties.end() && toStringIt->second.isFunction()) {
            result = interp->callForHarness(toStringIt->second, {}, obj);
            if (interp->hasError()) {
              Value err = interp->getError();
              interp->clearError();
              throw std::runtime_error(err.toString());
            }
            if (!result.isObject() && !result.isArray() && !result.isFunction()) {
              gotPrimitive = true;
            }
          }
          if (!gotPrimitive) {
            // Try valueOf
            auto valueOfIt = objPtr->properties.find("valueOf");
            if (valueOfIt != objPtr->properties.end() && valueOfIt->second.isFunction()) {
              result = interp->callForHarness(valueOfIt->second, {}, obj);
              if (interp->hasError()) {
                Value err = interp->getError();
                interp->clearError();
                throw std::runtime_error(err.toString());
              }
              if (!result.isObject() && !result.isArray() && !result.isFunction()) {
                gotPrimitive = true;
              }
            }
          }
        }
        if (gotPrimitive) {
          if (result.isSymbol()) {
            throw std::runtime_error("TypeError: Cannot convert a Symbol value to a string");
          }
          description = result.toString();
        } else {
          throw std::runtime_error("TypeError: Cannot convert object to primitive value");
        }
      } else {
        description = args[0].toString();
      }
    } else {
      description = args[0].toString();
    }
    return Value(Symbol(description));
  };
  {
    auto symbolPrototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    symbolFn->properties["prototype"] = Value(symbolPrototype);
    symbolPrototype->properties["constructor"] = Value(symbolFn);
    symbolPrototype->properties["__non_enum_constructor"] = Value(true);

    // Helper to extract symbol from this value
    auto thisSymbolValue = [](const std::vector<Value>& args) -> const Symbol* {
      // With __uses_this_arg__, this is args[0]
      if (args.empty()) return nullptr;
      const auto& thisVal = args[0];
      if (thisVal.isSymbol()) {
        return &std::get<Symbol>(thisVal.data);
      }
      // Symbol wrapper object: has __primitive_value__ that is a Symbol
      if (thisVal.isObject()) {
        auto obj = thisVal.getGC<Object>();
        auto it = obj->properties.find("__primitive_value__");
        if (it != obj->properties.end() && it->second.isSymbol()) {
          return &std::get<Symbol>(it->second.data);
        }
      }
      return nullptr;
    };

    // Symbol.prototype.toString()
    auto symToString = GarbageCollector::makeGC<Function>();
    symToString->isNative = true;
    symToString->properties["__uses_this_arg__"] = Value(true);
    symToString->properties["name"] = Value(std::string("toString"));
    symToString->properties["__non_writable_name"] = Value(true);
    symToString->properties["__non_enum_name"] = Value(true);
    symToString->properties["length"] = Value(0.0);
    symToString->properties["__non_writable_length"] = Value(true);
    symToString->properties["__non_enum_length"] = Value(true);
    symToString->nativeFunc = [thisSymbolValue](const std::vector<Value>& args) -> Value {
      auto sym = thisSymbolValue(args);
      if (!sym) {
        throw std::runtime_error("TypeError: Symbol.prototype.toString requires that 'this' be a Symbol");
      }
      if (sym->hasDescription) {
        return Value(std::string("Symbol(") + sym->description + ")");
      } else {
        return Value(std::string("Symbol()"));
      }
    };
    symbolPrototype->properties["toString"] = Value(symToString);
    symbolPrototype->properties["__non_enum_toString"] = Value(true);

    // Symbol.prototype.valueOf()
    auto symValueOf = GarbageCollector::makeGC<Function>();
    symValueOf->isNative = true;
    symValueOf->properties["__uses_this_arg__"] = Value(true);
    symValueOf->properties["name"] = Value(std::string("valueOf"));
    symValueOf->properties["__non_writable_name"] = Value(true);
    symValueOf->properties["__non_enum_name"] = Value(true);
    symValueOf->properties["length"] = Value(0.0);
    symValueOf->properties["__non_writable_length"] = Value(true);
    symValueOf->properties["__non_enum_length"] = Value(true);
    symValueOf->nativeFunc = [thisSymbolValue](const std::vector<Value>& args) -> Value {
      auto sym = thisSymbolValue(args);
      if (!sym) {
        throw std::runtime_error("TypeError: Symbol.prototype.valueOf requires that 'this' be a Symbol");
      }
      Value result;
      result.data = *sym;
      return result;
    };
    symbolPrototype->properties["valueOf"] = Value(symValueOf);
    symbolPrototype->properties["__non_enum_valueOf"] = Value(true);

    // Symbol.prototype[Symbol.toPrimitive](hint)
    auto symToPrimitive = GarbageCollector::makeGC<Function>();
    symToPrimitive->isNative = true;
    symToPrimitive->properties["__uses_this_arg__"] = Value(true);
    symToPrimitive->properties["name"] = Value(std::string("[Symbol.toPrimitive]"));
    symToPrimitive->properties["__non_writable_name"] = Value(true);
    symToPrimitive->properties["__non_enum_name"] = Value(true);
    symToPrimitive->properties["length"] = Value(1.0);
    symToPrimitive->properties["__non_writable_length"] = Value(true);
    symToPrimitive->properties["__non_enum_length"] = Value(true);
    symToPrimitive->nativeFunc = [thisSymbolValue](const std::vector<Value>& args) -> Value {
      auto sym = thisSymbolValue(args);
      if (!sym) {
        throw std::runtime_error("TypeError: Symbol.prototype[Symbol.toPrimitive] requires that 'this' be a Symbol");
      }
      Value result;
      result.data = *sym;
      return result;
    };
    symbolPrototype->properties[WellKnownSymbols::toPrimitiveKey()] = Value(symToPrimitive);
    symbolPrototype->properties["__non_writable_" + WellKnownSymbols::toPrimitiveKey()] = Value(true);
    symbolPrototype->properties["__non_enum_" + WellKnownSymbols::toPrimitiveKey()] = Value(true);

    // Symbol.prototype.description (getter)
    auto descGetter = GarbageCollector::makeGC<Function>();
    descGetter->isNative = true;
    descGetter->properties["__uses_this_arg__"] = Value(true);
    descGetter->properties["name"] = Value(std::string("get description"));
    descGetter->properties["__non_writable_name"] = Value(true);
    descGetter->properties["__non_enum_name"] = Value(true);
    descGetter->properties["length"] = Value(0.0);
    descGetter->properties["__non_writable_length"] = Value(true);
    descGetter->properties["__non_enum_length"] = Value(true);
    descGetter->nativeFunc = [thisSymbolValue](const std::vector<Value>& args) -> Value {
      auto sym = thisSymbolValue(args);
      if (!sym) {
        throw std::runtime_error("TypeError: Symbol.prototype.description requires that 'this' be a Symbol");
      }
      if (!sym->hasDescription) {
        return Value(Undefined{});
      }
      return Value(sym->description);
    };
    symbolPrototype->properties["__get_description"] = Value(descGetter);
    symbolPrototype->properties["__non_enum_description"] = Value(true);

    // Symbol.prototype[Symbol.toStringTag] = "Symbol"
    symbolPrototype->properties[WellKnownSymbols::toStringTagKey()] = Value(std::string("Symbol"));
    symbolPrototype->properties["__non_writable_" + WellKnownSymbols::toStringTagKey()] = Value(true);
    symbolPrototype->properties["__non_enum_" + WellKnownSymbols::toStringTagKey()] = Value(true);
  }
  symbolFn->properties["iterator"] = WellKnownSymbols::iterator();
  symbolFn->properties["asyncIterator"] = WellKnownSymbols::asyncIterator();
  symbolFn->properties["toStringTag"] = WellKnownSymbols::toStringTag();
  symbolFn->properties["toPrimitive"] = WellKnownSymbols::toPrimitive();
  symbolFn->properties["matchAll"] = WellKnownSymbols::matchAll();
  symbolFn->properties["unscopables"] = WellKnownSymbols::unscopables();
  symbolFn->properties["hasInstance"] = WellKnownSymbols::hasInstance();
  symbolFn->properties["species"] = WellKnownSymbols::species();
  symbolFn->properties["isConcatSpreadable"] = WellKnownSymbols::isConcatSpreadable();
  symbolFn->properties["match"] = WellKnownSymbols::match();
  symbolFn->properties["replace"] = WellKnownSymbols::replace();
  symbolFn->properties["search"] = WellKnownSymbols::search();
  symbolFn->properties["split"] = WellKnownSymbols::split();
  // Well-known symbol properties on Symbol are non-writable and non-configurable.
  const char* wellKnownNames[] = {
    "iterator", "asyncIterator", "toStringTag", "toPrimitive",
    "matchAll", "unscopables", "hasInstance", "species",
    "isConcatSpreadable", "match", "replace", "search", "split"
  };
  for (const char* name : wellKnownNames) {
    symbolFn->properties[std::string("__non_writable_") + name] = Value(true);
    symbolFn->properties[std::string("__non_configurable_") + name] = Value(true);
  }

  // Symbol.for() - global symbol registry
  static std::unordered_map<std::string, Value> globalSymbolRegistry;
  auto symbolFor = GarbageCollector::makeGC<Function>();
  symbolFor->isNative = true;
  symbolFor->properties["name"] = Value(std::string("for"));
  symbolFor->properties["__non_writable_name"] = Value(true);
  symbolFor->properties["__non_enum_name"] = Value(true);
  symbolFor->properties["length"] = Value(1.0);
  symbolFor->properties["__non_writable_length"] = Value(true);
  symbolFor->properties["__non_enum_length"] = Value(true);
  symbolFor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (!args.empty() && args[0].isSymbol()) {
      throw std::runtime_error("TypeError: Cannot convert a Symbol value to a string");
    }
    std::string key;
    if (args.empty() || args[0].isUndefined()) {
      key = "undefined";
    } else if (args[0].isObject() || args[0].isArray() || args[0].isFunction()) {
      // Call ToPrimitive/ToString on object args
      Interpreter* interp = getGlobalInterpreter();
      if (interp) {
        Value obj = args[0];
        Value result;
        bool gotPrimitive = false;
        if (obj.isObject()) {
          auto objPtr = obj.getGC<Object>();
          auto toStringIt = objPtr->properties.find("toString");
          if (toStringIt != objPtr->properties.end() && toStringIt->second.isFunction()) {
            result = interp->callForHarness(toStringIt->second, {}, obj);
            if (interp->hasError()) {
              Value err = interp->getError();
              interp->clearError();
              throw std::runtime_error(err.toString());
            }
            if (!result.isObject() && !result.isArray() && !result.isFunction()) {
              gotPrimitive = true;
            }
          }
          if (!gotPrimitive) {
            auto valueOfIt = objPtr->properties.find("valueOf");
            if (valueOfIt != objPtr->properties.end() && valueOfIt->second.isFunction()) {
              result = interp->callForHarness(valueOfIt->second, {}, obj);
              if (interp->hasError()) {
                Value err = interp->getError();
                interp->clearError();
                throw std::runtime_error(err.toString());
              }
              if (!result.isObject() && !result.isArray() && !result.isFunction()) {
                gotPrimitive = true;
              }
            }
          }
        }
        if (gotPrimitive) {
          if (result.isSymbol()) {
            throw std::runtime_error("TypeError: Cannot convert a Symbol value to a string");
          }
          key = result.toString();
        } else {
          throw std::runtime_error("TypeError: Cannot convert object to primitive value");
        }
      } else {
        key = args[0].toString();
      }
    } else {
      key = args[0].toString();
    }
    auto it = globalSymbolRegistry.find(key);
    if (it != globalSymbolRegistry.end()) {
      return it->second;
    }
    Symbol s(key);
    Value sym;
    sym.data = s;
    globalSymbolRegistry[key] = sym;
    return sym;
  };
  symbolFn->properties["for"] = Value(symbolFor);

  // Symbol.keyFor() - reverse lookup in global registry
  auto symbolKeyFor = GarbageCollector::makeGC<Function>();
  symbolKeyFor->isNative = true;
  symbolKeyFor->properties["name"] = Value(std::string("keyFor"));
  symbolKeyFor->properties["__non_writable_name"] = Value(true);
  symbolKeyFor->properties["__non_enum_name"] = Value(true);
  symbolKeyFor->properties["length"] = Value(1.0);
  symbolKeyFor->properties["__non_writable_length"] = Value(true);
  symbolKeyFor->properties["__non_enum_length"] = Value(true);
  symbolKeyFor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isSymbol()) {
      throw std::runtime_error("TypeError: Symbol.keyFor requires a Symbol argument");
    }
    const auto& sym = std::get<Symbol>(args[0].data);
    for (const auto& [key, val] : globalSymbolRegistry) {
      const auto& regSym = std::get<Symbol>(val.data);
      if (sym.id == regSym.id) {
        return Value(key);
      }
    }
    return Value(Undefined{});
  };
  symbolFn->properties["keyFor"] = Value(symbolKeyFor);
  env->define("Symbol", Value(symbolFn));

  // BigInt constructor/function
  auto bigIntFn = GarbageCollector::makeGC<Function>();
  bigIntFn->isNative = true;
  bigIntFn->isConstructor = true;

  auto arrayToString = [](const GCPtr<Array>& arr) -> std::string {
    std::string out;
    for (size_t i = 0; i < arr->elements.size(); i++) {
      if (i > 0) out += ",";
      out += arr->elements[i].toString();
    }
    return out;
  };

  auto isObjectLike = [](const Value& value) -> bool {
    // Keep this aligned with Interpreter::isObjectLike so builtins using ToPrimitive
    // (e.g. String(), Number(), BigInt()) behave consistently with the interpreter.
    return value.isObject() || value.isArray() || value.isFunction() || value.isRegex() ||
           value.isProxy() || value.isPromise() || value.isGenerator() || value.isClass() ||
           value.isMap() || value.isSet() || value.isWeakMap() || value.isWeakSet() ||
           value.isTypedArray() || value.isArrayBuffer() || value.isDataView() || value.isError();
  };

  auto callChecked = [](const Value& callee,
                        const std::vector<Value>& callArgs,
                        const Value& thisArg) -> Value {
    if (!callee.isFunction()) {
      return Value(Undefined{});
    }

    auto fn = callee.getGC<Function>();
    if (fn->isNative) {
      auto itUsesThis = fn->properties.find("__uses_this_arg__");
      if (itUsesThis != fn->properties.end() &&
          itUsesThis->second.isBool() &&
          itUsesThis->second.toBool()) {
        std::vector<Value> nativeArgs;
        nativeArgs.reserve(callArgs.size() + 1);
        nativeArgs.push_back(thisArg);
        nativeArgs.insert(nativeArgs.end(), callArgs.begin(), callArgs.end());
        return fn->nativeFunc(nativeArgs);
      }
      return fn->nativeFunc(callArgs);
    }

    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) {
      throw std::runtime_error("TypeError: Interpreter unavailable for callable conversion");
    }
    interpreter->clearError();
    Value result = interpreter->callForHarness(callee, callArgs, thisArg);
    if (interpreter->hasError()) {
      Value err = interpreter->getError();
      interpreter->clearError();
      throw JsValueException(err);
    }
    return result;
  };

  auto getObjectProperty = [callChecked](const GCPtr<Object>& obj,
                                         const Value& receiver,
                                         const std::string& key) -> std::pair<bool, Value> {
    auto current = obj;
    int depth = 0;
    while (current && depth <= 16) {
      depth++;

      std::string getterKey = "__get_" + key;
      auto getterIt = current->properties.find(getterKey);
      if (getterIt != current->properties.end()) {
        if (getterIt->second.isFunction()) {
          return {true, callChecked(getterIt->second, {}, receiver)};
        }
        return {true, Value(Undefined{})};
      }

      auto it = current->properties.find(key);
      if (it != current->properties.end()) {
        return {true, it->second};
      }

      auto protoIt = current->properties.find("__proto__");
      if (protoIt == current->properties.end() || !protoIt->second.isObject()) {
        break;
      }
      current = protoIt->second.getGC<Object>();
    }

    return {false, Value(Undefined{})};
  };

  auto getPropertyFromBag = [getObjectProperty, callChecked](const Value& receiver,
                                                            OrderedMap<std::string, Value>& bag,
                                                            const std::string& key) -> std::pair<bool, Value> {
    std::string getterKey = "__get_" + key;
    auto getterIt = bag.find(getterKey);
    if (getterIt != bag.end()) {
      if (getterIt->second.isFunction()) {
        return {true, callChecked(getterIt->second, {}, receiver)};
      }
      return {true, Value(Undefined{})};
    }

    auto it = bag.find(key);
    if (it != bag.end()) {
      return {true, it->second};
    }

    auto protoIt = bag.find("__proto__");
    if (protoIt != bag.end() && protoIt->second.isObject()) {
      return getObjectProperty(protoIt->second.getGC<Object>(), receiver, key);
    }

    return {false, Value(Undefined{})};
  };

  std::function<std::pair<bool, Value>(const Value&, const std::string&)> getProperty;
  getProperty = [env, getObjectProperty, getPropertyFromBag, callChecked, &getProperty](const Value& receiver,
                                                                                         const std::string& key) -> std::pair<bool, Value> {
    if (receiver.isObject()) {
      return getObjectProperty(receiver.getGC<Object>(), receiver, key);
    }
    if (receiver.isFunction()) {
      auto fn = receiver.getGC<Function>();
      return getPropertyFromBag(receiver, fn->properties, key);
    }
    if (receiver.isRegex()) {
      auto regex = receiver.getGC<Regex>();
      std::string getterKey = "__get_" + key;
      auto getterIt = regex->properties.find(getterKey);
      if (getterIt != regex->properties.end()) {
        if (getterIt->second.isFunction()) {
          return {true, callChecked(getterIt->second, {}, receiver)};
        }
        return {true, Value(Undefined{})};
      }
      auto it = regex->properties.find(key);
      if (it != regex->properties.end()) {
        return {true, it->second};
      }
      return {false, Value(Undefined{})};
    }
    if (receiver.isGenerator()) {
      auto gen = receiver.getGC<Generator>();
      return getPropertyFromBag(receiver, gen->properties, key);
    }
    if (receiver.isPromise()) {
      auto p = receiver.getGC<Promise>();
      return getPropertyFromBag(receiver, p->properties, key);
    }
    if (receiver.isClass()) {
      auto cls = receiver.getGC<Class>();
      return getPropertyFromBag(receiver, cls->properties, key);
    }
    if (receiver.isMap()) {
      auto m = receiver.getGC<Map>();
      return getPropertyFromBag(receiver, m->properties, key);
    }
    if (receiver.isSet()) {
      auto s = receiver.getGC<Set>();
      return getPropertyFromBag(receiver, s->properties, key);
    }
    if (receiver.isWeakMap()) {
      auto m = receiver.getGC<WeakMap>();
      return getPropertyFromBag(receiver, m->properties, key);
    }
    if (receiver.isWeakSet()) {
      auto s = receiver.getGC<WeakSet>();
      return getPropertyFromBag(receiver, s->properties, key);
    }
    if (receiver.isTypedArray()) {
      auto ta = receiver.getGC<TypedArray>();
      return getPropertyFromBag(receiver, ta->properties, key);
    }
    if (receiver.isArrayBuffer()) {
      auto ab = receiver.getGC<ArrayBuffer>();
      return getPropertyFromBag(receiver, ab->properties, key);
    }
    if (receiver.isDataView()) {
      auto dv = receiver.getGC<DataView>();
      return getPropertyFromBag(receiver, dv->properties, key);
    }
    if (receiver.isError()) {
      auto err = receiver.getGC<Error>();
      auto result = getPropertyFromBag(receiver, err->properties, key);
      if (result.first) {
        return result;
      }
      if (key == "name") {
        return {true, Value(err->getName())};
      }
      if (key == "message") {
        return {true, Value(err->message)};
      }
      if (key == "constructor") {
        if (auto ctor = env->get(err->getName())) {
          return {true, *ctor};
        }
        return {true, Value(Undefined{})};
      }
      return {false, Value(Undefined{})};
    }
    if (receiver.isProxy()) {
      auto proxy = receiver.getGC<Proxy>();
      if (proxy->target) {
        return getProperty(*proxy->target, key);
      }
    }
    return {false, Value(Undefined{})};
  };

  auto toPrimitive = [isObjectLike, arrayToString, getProperty, callChecked](const Value& input,
                                                                              bool preferString) -> Value {
    if (!isObjectLike(input)) {
      return input;
    }

    const std::string& toPrimitiveKey = WellKnownSymbols::toPrimitiveKey();
    auto [hasExotic, exotic] = getProperty(input, toPrimitiveKey);
    if (hasExotic && !exotic.isUndefined() && !exotic.isNull()) {
      if (!exotic.isFunction()) {
        throw std::runtime_error("TypeError: @@toPrimitive is not callable");
      }
      std::string hint = preferString ? "string" : "number";
      Value result = callChecked(exotic, {Value(hint)}, input);
      if (isObjectLike(result)) {
        throw std::runtime_error("TypeError: @@toPrimitive must return a primitive value");
      }
      return result;
    }

    std::array<std::string, 2> methodOrder = preferString
      ? std::array<std::string, 2>{"toString", "valueOf"}
      : std::array<std::string, 2>{"valueOf", "toString"};

    for (const auto& methodName : methodOrder) {
      auto [found, method] = getProperty(input, methodName);
      if (found) {
        if (method.isFunction()) {
          Value result = callChecked(method, {}, input);
          if (!isObjectLike(result)) {
            return result;
          }
        }
        continue;
      }

      if (methodName == "toString") {
        if (input.isArray()) {
          return Value(arrayToString(input.getGC<Array>()));
        }
        if (input.isObject()) {
          return Value(std::string("[object Object]"));
        }
        if (input.isFunction()) {
          return Value(std::string("[Function]"));
        }
        if (input.isClass()) {
          return Value(std::string("[Function]"));
        }
        if (input.isRegex()) {
          return Value(input.toString());
        }
      }
    }

    throw std::runtime_error("TypeError: Cannot convert object to primitive value");
  };

  auto toBigIntFromPrimitive = [](const Value& v) -> bigint::BigIntValue {
    if (v.isBigInt()) {
      return v.toBigInt();
    }
    if (v.isBool()) {
      return v.toBool() ? 1 : 0;
    }
    if (v.isString()) {
      bigint::BigIntValue parsed = 0;
      if (!bigint::parseBigIntString(std::get<std::string>(v.data), parsed)) {
        throw std::runtime_error("SyntaxError: Cannot convert string to BigInt");
      }
      return parsed;
    }
    throw std::runtime_error("TypeError: Cannot convert value to BigInt");
  };

  auto toBigIntFromValue = [toPrimitive, toBigIntFromPrimitive](const Value& v) -> bigint::BigIntValue {
    Value primitive = toPrimitive(v, false);
    return toBigIntFromPrimitive(primitive);
  };

  auto toIndex = [toPrimitive](const Value& v) -> uint64_t {
    Value primitive = toPrimitive(v, false);
    if (primitive.isBigInt() || primitive.isSymbol()) {
      throw std::runtime_error("TypeError: Cannot convert to index");
    }
    double n = primitive.toNumber();
    if (std::isnan(n)) return 0;
    if (!std::isfinite(n)) {
      throw std::runtime_error("RangeError: Invalid index");
    }
    n = std::trunc(n);
    if (n < 0.0 || n > 9007199254740991.0) {
      throw std::runtime_error("RangeError: Invalid index");
    }
    return static_cast<uint64_t>(n);
  };

  auto thisBigIntValue = [](const Value& thisValue) -> bigint::BigIntValue {
    if (thisValue.isBigInt()) {
      return thisValue.toBigInt();
    }
    if (thisValue.isObject()) {
      auto obj = thisValue.getGC<Object>();
      auto primitiveIt = obj->properties.find("__primitive_value__");
      if (primitiveIt != obj->properties.end() && primitiveIt->second.isBigInt()) {
        return primitiveIt->second.toBigInt();
      }
    }
    throw std::runtime_error("TypeError: BigInt method called on incompatible receiver");
  };

  auto formatBigInt = [](const bigint::BigIntValue& value, int radix) -> std::string {
    return bigint::toString(value, radix);
  };

  bigIntFn->nativeFunc = [toPrimitive, toBigIntFromPrimitive](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      throw std::runtime_error("TypeError: Cannot convert undefined to BigInt");
    }

    Value primitive = toPrimitive(args[0], false);
    if (primitive.isNumber()) {
      double n = primitive.toNumber();
      if (!std::isfinite(n) || std::floor(n) != n) {
        throw std::runtime_error("RangeError: Cannot convert Number to BigInt");
      }
      bigint::BigIntValue converted = 0;
      if (!bigint::fromIntegralDouble(n, converted)) {
        throw std::runtime_error("RangeError: Cannot convert Number to BigInt");
      }
      return Value(BigInt(converted));
    }

    return Value(BigInt(toBigIntFromPrimitive(primitive)));
  };
  bigIntFn->properties["name"] = Value(std::string("BigInt"));
  bigIntFn->properties["length"] = Value(1.0);
  bigIntFn->properties["__throw_on_new__"] = Value(true);

  auto asUintN = GarbageCollector::makeGC<Function>();
  asUintN->isNative = true;
  asUintN->properties["__throw_on_new__"] = Value(true);
  asUintN->properties["name"] = Value(std::string("asUintN"));
  asUintN->properties["length"] = Value(2.0);
  asUintN->nativeFunc = [toIndex, toBigIntFromValue](const std::vector<Value>& args) -> Value {
    uint64_t bits = toIndex(args.empty() ? Value(Undefined{}) : args[0]);
    auto n = toBigIntFromValue(args.size() > 1 ? args[1] : Value(Undefined{}));
    return Value(BigInt(bigint::asUintN(bits, n)));
  };
  bigIntFn->properties["asUintN"] = Value(asUintN);

  auto asIntN = GarbageCollector::makeGC<Function>();
  asIntN->isNative = true;
  asIntN->properties["__throw_on_new__"] = Value(true);
  asIntN->properties["name"] = Value(std::string("asIntN"));
  asIntN->properties["length"] = Value(2.0);
  asIntN->nativeFunc = [toIndex, toBigIntFromValue](const std::vector<Value>& args) -> Value {
    uint64_t bits = toIndex(args.empty() ? Value(Undefined{}) : args[0]);
    auto n = toBigIntFromValue(args.size() > 1 ? args[1] : Value(Undefined{}));
    return Value(BigInt(bigint::asIntN(bits, n)));
  };
  bigIntFn->properties["asIntN"] = Value(asIntN);

  auto bigIntProto = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  auto bigIntProtoToString = GarbageCollector::makeGC<Function>();
  bigIntProtoToString->isNative = true;
  bigIntProtoToString->properties["__throw_on_new__"] = Value(true);
  bigIntProtoToString->properties["__uses_this_arg__"] = Value(true);
  bigIntProtoToString->properties["name"] = Value(std::string("toString"));
  bigIntProtoToString->properties["length"] = Value(0.0);
  bigIntProtoToString->nativeFunc = [thisBigIntValue, formatBigInt](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      throw std::runtime_error("TypeError: BigInt.prototype.toString requires BigInt");
    }

    auto n = thisBigIntValue(args[0]);
    int radix = 10;
    if (args.size() > 1 && !args[1].isUndefined()) {
      radix = static_cast<int>(std::trunc(args[1].toNumber()));
      if (radix < 2 || radix > 36) {
        throw std::runtime_error("RangeError: radix must be between 2 and 36");
      }
    }
    return Value(formatBigInt(n, radix));
  };
  bigIntProto->properties["toString"] = Value(bigIntProtoToString);

  auto bigIntProtoValueOf = GarbageCollector::makeGC<Function>();
  bigIntProtoValueOf->isNative = true;
  bigIntProtoValueOf->properties["__throw_on_new__"] = Value(true);
  bigIntProtoValueOf->properties["__uses_this_arg__"] = Value(true);
  bigIntProtoValueOf->properties["name"] = Value(std::string("valueOf"));
  bigIntProtoValueOf->properties["length"] = Value(0.0);
  bigIntProtoValueOf->nativeFunc = [thisBigIntValue](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      throw std::runtime_error("TypeError: BigInt.prototype.valueOf requires BigInt");
    }
    return Value(BigInt(thisBigIntValue(args[0])));
  };
  bigIntProto->properties["valueOf"] = Value(bigIntProtoValueOf);

  auto bigIntProtoToLocaleString = GarbageCollector::makeGC<Function>();
  bigIntProtoToLocaleString->isNative = true;
  bigIntProtoToLocaleString->properties["__throw_on_new__"] = Value(true);
  bigIntProtoToLocaleString->properties["__uses_this_arg__"] = Value(true);
  bigIntProtoToLocaleString->properties["name"] = Value(std::string("toLocaleString"));
  bigIntProtoToLocaleString->properties["length"] = Value(0.0);
  bigIntProtoToLocaleString->nativeFunc = [thisBigIntValue](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      throw std::runtime_error("TypeError: BigInt.prototype.toLocaleString requires BigInt");
    }
    return Value(bigint::toString(thisBigIntValue(args[0])));
  };
  bigIntProto->properties["toLocaleString"] = Value(bigIntProtoToLocaleString);

  // BigInt.prototype.constructor = BigInt
  bigIntProto->properties["constructor"] = Value(bigIntFn);

  // BigInt.prototype[Symbol.toStringTag] = "BigInt" (non-writable)
  bigIntProto->properties[WellKnownSymbols::toStringTagKey()] = Value(std::string("BigInt"));
  bigIntProto->properties["__non_writable_" + WellKnownSymbols::toStringTagKey()] = Value(true);
  bigIntProto->properties["__non_enum_" + WellKnownSymbols::toStringTagKey()] = Value(true);

  // Mark all BigInt.prototype properties as non-enumerable
  bigIntProto->properties["__non_enum_toString"] = Value(true);
  bigIntProto->properties["__non_enum_valueOf"] = Value(true);
  bigIntProto->properties["__non_enum_toLocaleString"] = Value(true);
  bigIntProto->properties["__non_enum_constructor"] = Value(true);

  bigIntFn->properties["prototype"] = Value(bigIntProto);
  bigIntFn->properties["__non_writable_prototype"] = Value(true);
  bigIntFn->properties["__non_configurable_prototype"] = Value(true);

  env->define("BigInt", Value(bigIntFn));

  auto createTypedArrayConstructor = [](TypedArrayType type, const std::string& name) {
    auto makeStandaloneTypedArray = [type](size_t length) {
      auto typedArray = GarbageCollector::makeGC<TypedArray>(type, length);
      auto backingBuffer =
        GarbageCollector::makeGC<ArrayBuffer>(length * typedArrayElementSize(type));
      typedArray->viewedBuffer = backingBuffer;
      typedArray->byteOffset = 0;
      typedArray->length = length;
      typedArray->lengthTracking = false;
      backingBuffer->views.push_back(typedArray);
      return typedArray;
    };
    auto func = GarbageCollector::makeGC<Function>();
    func->isNative = true;
    func->isConstructor = true;
    func->properties["name"] = Value(name);
    func->properties["length"] = Value(1.0);
    func->properties["__non_writable_name"] = Value(true);
    func->properties["__non_enum_name"] = Value(true);
    func->properties["__non_writable_length"] = Value(true);
    func->properties["__non_enum_length"] = Value(true);
    func->properties["BYTES_PER_ELEMENT"] = Value(static_cast<double>(typedArrayElementSize(type)));
    func->properties["__non_writable_BYTES_PER_ELEMENT"] = Value(true);
    func->properties["__non_enum_BYTES_PER_ELEMENT"] = Value(true);
    {
      auto speciesGetter = GarbageCollector::makeGC<Function>();
      speciesGetter->isNative = true;
      speciesGetter->isConstructor = false;
      speciesGetter->properties["name"] = Value(std::string("get [Symbol.species]"));
      speciesGetter->properties["length"] = Value(0.0);
      speciesGetter->properties["__non_writable_name"] = Value(true);
      speciesGetter->properties["__non_enum_name"] = Value(true);
      speciesGetter->properties["__non_writable_length"] = Value(true);
      speciesGetter->properties["__non_enum_length"] = Value(true);
      speciesGetter->properties["__uses_this_arg__"] = Value(true);
      speciesGetter->properties["__throw_on_new__"] = Value(true);
      speciesGetter->nativeFunc = [](const std::vector<Value>& args) -> Value {
        return args.empty() ? Value(Undefined{}) : args[0];
      };
      const auto& speciesKey = WellKnownSymbols::speciesKey();
      func->properties["__get_" + speciesKey] = Value(speciesGetter);
      func->properties["__non_enum_" + speciesKey] = Value(true);
    }
    auto constructImpl = GarbageCollector::makeGC<Function>();
    constructImpl->isNative = true;
    constructImpl->isConstructor = true;
    constructImpl->nativeFunc = [type, makeStandaloneTypedArray](const std::vector<Value>& args) -> Value {
      if (args.empty()) {
        return Value(makeStandaloneTypedArray(0));
      }

      if (args[0].isArrayBuffer()) {
        auto buffer = args[0].getGC<ArrayBuffer>();
        if (buffer->detached) {
          throw std::runtime_error("TypeError: Cannot construct TypedArray from detached ArrayBuffer");
        }
        size_t elementSize = typedArrayElementSize(type);
        size_t byteOffset = 0;
        if (args.size() > 1 && !args[1].isUndefined()) {
          double offsetNum = args[1].toNumber();
          if (std::isnan(offsetNum) || std::isinf(offsetNum) || offsetNum < 0) {
            throw std::runtime_error("RangeError: Invalid typed array offset");
          }
          byteOffset = static_cast<size_t>(offsetNum);
        }
        if (byteOffset % elementSize != 0) {
          throw std::runtime_error("RangeError: Invalid typed array offset");
        }
        if (byteOffset > buffer->byteLength) {
          throw std::runtime_error("RangeError: Invalid typed array offset");
        }

        bool lengthTracking = args.size() <= 2 || args[2].isUndefined();
        size_t length = 0;
        if (!lengthTracking) {
          double lengthNum = args[2].toNumber();
          if (std::isnan(lengthNum) || std::isinf(lengthNum) || lengthNum < 0) {
            throw std::runtime_error("RangeError: Invalid typed array length");
          }
          length = static_cast<size_t>(lengthNum);
          if (length > (std::numeric_limits<size_t>::max() - byteOffset) / elementSize ||
              byteOffset + length * elementSize > buffer->byteLength) {
            throw std::runtime_error("RangeError: Invalid typed array length");
          }
        } else if (byteOffset <= buffer->byteLength) {
          length = (buffer->byteLength - byteOffset) / elementSize;
        }

        auto typedArray =
          GarbageCollector::makeGC<TypedArray>(type, buffer, byteOffset, length, lengthTracking);
        buffer->views.push_back(typedArray);
        return Value(typedArray);
      }

      // Check if first argument is an array
      if (std::holds_alternative<GCPtr<Array>>(args[0].data)) {
        auto arr = args[0].getGC<Array>();
        auto typedArray = makeStandaloneTypedArray(arr->elements.size());

        // Fill the typed array with values from the regular array
        for (size_t i = 0; i < arr->elements.size(); ++i) {
          if (type == TypedArrayType::BigInt64) {
            typedArray->setBigIntElement(i, bigint::toInt64Trunc(arr->elements[i].toBigInt()));
          } else if (type == TypedArrayType::BigUint64) {
            typedArray->setBigUintElement(i, bigint::toUint64Trunc(arr->elements[i].toBigInt()));
          } else {
            typedArray->setElement(i, arr->elements[i].toNumber());
          }
        }

        return Value(typedArray);
      }

      // Otherwise treat as length
      double lengthNum = args[0].toNumber();
      if (std::isnan(lengthNum) || std::isinf(lengthNum) || lengthNum < 0) {
        // Invalid length, return empty array
        return Value(makeStandaloneTypedArray(0));
      }

      size_t length = static_cast<size_t>(lengthNum);
      // Sanity check: prevent allocating huge arrays
      if (length > 1000000000) { // 1GB limit
        return Value(makeStandaloneTypedArray(0));
      }

      return Value(makeStandaloneTypedArray(length));
    };
    func->nativeFunc = [name](const std::vector<Value>&) -> Value {
      throw std::runtime_error("TypeError: Constructor " + name + " requires 'new'");
    };
    func->properties["__native_construct__"] = Value(constructImpl);
    auto prototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    func->properties["prototype"] = Value(prototype);
    func->properties["__non_writable_prototype"] = Value(true);
    func->properties["__non_enum_prototype"] = Value(true);
    func->properties["__non_configurable_prototype"] = Value(true);
    prototype->properties["constructor"] = Value(func);
    prototype->properties["__non_enum_constructor"] = Value(true);
    prototype->properties["BYTES_PER_ELEMENT"] = Value(static_cast<double>(typedArrayElementSize(type)));
    prototype->properties["__non_writable_BYTES_PER_ELEMENT"] = Value(true);
    prototype->properties["__non_enum_BYTES_PER_ELEMENT"] = Value(true);
    return Value(func);
  };

  auto typedArrayConstructor = GarbageCollector::makeGC<Function>();
  typedArrayConstructor->isNative = true;
  typedArrayConstructor->isConstructor = true;
  typedArrayConstructor->properties["name"] = Value(std::string("TypedArray"));
  typedArrayConstructor->properties["length"] = Value(0.0);
  typedArrayConstructor->properties["__non_writable_name"] = Value(true);
  typedArrayConstructor->properties["__non_enum_name"] = Value(true);
  typedArrayConstructor->properties["__non_writable_length"] = Value(true);
  typedArrayConstructor->properties["__non_enum_length"] = Value(true);
  auto typedArrayConstructImpl = GarbageCollector::makeGC<Function>();
  typedArrayConstructImpl->isNative = true;
  typedArrayConstructImpl->isConstructor = true;
  typedArrayConstructImpl->nativeFunc = [](const std::vector<Value>&) -> Value {
    throw std::runtime_error("TypeError: TypedArray is not a constructor");
  };
  typedArrayConstructor->nativeFunc = [](const std::vector<Value>&) -> Value {
    throw std::runtime_error("TypeError: TypedArray is not a constructor");
  };
  typedArrayConstructor->properties["__native_construct__"] = Value(typedArrayConstructImpl);
  typedArrayConstructor->properties["__non_writable_prototype"] = Value(true);
  typedArrayConstructor->properties["__non_enum_prototype"] = Value(true);
  typedArrayConstructor->properties["__non_configurable_prototype"] = Value(true);
  {
    auto speciesGetter = GarbageCollector::makeGC<Function>();
    speciesGetter->isNative = true;
    speciesGetter->isConstructor = false;
    speciesGetter->properties["name"] = Value(std::string("get [Symbol.species]"));
    speciesGetter->properties["length"] = Value(0.0);
    speciesGetter->properties["__non_writable_name"] = Value(true);
    speciesGetter->properties["__non_enum_name"] = Value(true);
    speciesGetter->properties["__non_writable_length"] = Value(true);
    speciesGetter->properties["__non_enum_length"] = Value(true);
    speciesGetter->properties["__uses_this_arg__"] = Value(true);
    speciesGetter->properties["__throw_on_new__"] = Value(true);
    speciesGetter->nativeFunc = [](const std::vector<Value>& args) -> Value {
      return args.empty() ? Value(Undefined{}) : args[0];
    };
    const auto& speciesKey = WellKnownSymbols::speciesKey();
    typedArrayConstructor->properties["__get_" + speciesKey] = Value(speciesGetter);
    typedArrayConstructor->properties["__non_enum_" + speciesKey] = Value(true);
  }
  {
    auto isConstructorValue = [](const Value& value) -> bool {
      if (value.isFunction()) {
        return value.getGC<Function>()->isConstructor;
      }
      if (value.isClass()) {
        return true;
      }
      if (value.isObject()) {
        auto obj = value.getGC<Object>();
        auto callableIt = obj->properties.find("__callable_object__");
        auto ctorIt = obj->properties.find("constructor");
        if (callableIt != obj->properties.end() &&
            callableIt->second.isBool() &&
            callableIt->second.toBool() &&
            ctorIt != obj->properties.end()) {
          return (ctorIt->second.isFunction() && ctorIt->second.getGC<Function>()->isConstructor) ||
                 ctorIt->second.isClass();
        }
      }
      return false;
    };

    auto typedArrayOf = GarbageCollector::makeGC<Function>();
    typedArrayOf->isNative = true;
    typedArrayOf->isConstructor = false;
    typedArrayOf->properties["name"] = Value(std::string("of"));
    typedArrayOf->properties["length"] = Value(0.0);
    typedArrayOf->properties["__non_writable_name"] = Value(true);
    typedArrayOf->properties["__non_enum_name"] = Value(true);
    typedArrayOf->properties["__non_writable_length"] = Value(true);
    typedArrayOf->properties["__non_enum_length"] = Value(true);
    typedArrayOf->properties["__uses_this_arg__"] = Value(true);
    typedArrayOf->properties["__throw_on_new__"] = Value(true);
    typedArrayOf->nativeFunc = [isConstructorValue, toPrimitive](const std::vector<Value>& args) -> Value {
      auto isBigIntTypedArray = [](TypedArrayType typeValue) {
        return typeValue == TypedArrayType::BigInt64 || typeValue == TypedArrayType::BigUint64;
      };
      auto toNumberForTypedArray = [toPrimitive](const Value& input) -> double {
        Value primitive = toPrimitive(input, false);
        if (primitive.isBigInt() || primitive.isSymbol()) {
          throw std::runtime_error("TypeError: Cannot convert value to number");
        }
        return primitive.toNumber();
      };
      auto toBigIntForTypedArray = [toPrimitive](const Value& input) -> bigint::BigIntValue {
        Value primitive = toPrimitive(input, false);
        if (primitive.isBigInt()) {
          return primitive.toBigInt();
        }
        if (primitive.isBool()) {
          return primitive.toBool() ? 1 : 0;
        }
        if (primitive.isString()) {
          bigint::BigIntValue parsed = 0;
          if (!bigint::parseBigIntString(std::get<std::string>(primitive.data), parsed)) {
            throw std::runtime_error("SyntaxError: Cannot convert string to BigInt");
          }
          return parsed;
        }
        throw std::runtime_error("TypeError: Cannot convert value to BigInt");
      };

      Value ctor = args.empty() ? Value(Undefined{}) : args[0];
      if (!isConstructorValue(ctor)) {
        throw std::runtime_error("TypeError: TypedArray.of requires a constructor");
      }

      Interpreter* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        throw std::runtime_error("TypeError: Interpreter unavailable for TypedArray.of");
      }

      size_t length = args.size() > 0 ? args.size() - 1 : 0;
      Value outValue = interpreter->constructFromNative(ctor, {Value(static_cast<double>(length))});
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw JsValueException(err);
      }
      if (!outValue.isTypedArray()) {
        throw std::runtime_error("TypeError: TypedArray.of constructor must return a TypedArray");
      }

      auto target = outValue.getGC<TypedArray>();
      for (size_t i = 0; i < length; ++i) {
        Value item = args[i + 1];
        if (isBigIntTypedArray(target->type)) {
          bigint::BigIntValue bigintValue = toBigIntForTypedArray(item);
          if (target->viewedBuffer && target->viewedBuffer->detached) {
            continue;
          }
          if (target->isOutOfBounds() || i >= target->currentLength()) {
            continue;
          }
          if (target->type == TypedArrayType::BigUint64) {
            target->setBigUintElement(i, bigint::toUint64Trunc(bigintValue));
          } else {
            target->setBigIntElement(i, bigint::toInt64Trunc(bigintValue));
          }
        } else {
          double numberValue = toNumberForTypedArray(item);
          if (target->viewedBuffer && target->viewedBuffer->detached) {
            continue;
          }
          if (target->isOutOfBounds() || i >= target->currentLength()) {
            continue;
          }
          target->setElement(i, numberValue);
        }
      }
      return outValue;
    };
    typedArrayConstructor->properties["of"] = Value(typedArrayOf);
    typedArrayConstructor->properties["__non_enum_of"] = Value(true);

    auto typedArrayFrom = GarbageCollector::makeGC<Function>();
    typedArrayFrom->isNative = true;
    typedArrayFrom->isConstructor = false;
    typedArrayFrom->properties["name"] = Value(std::string("from"));
    typedArrayFrom->properties["length"] = Value(1.0);
    typedArrayFrom->properties["__non_writable_name"] = Value(true);
    typedArrayFrom->properties["__non_enum_name"] = Value(true);
    typedArrayFrom->properties["__non_writable_length"] = Value(true);
    typedArrayFrom->properties["__non_enum_length"] = Value(true);
    typedArrayFrom->properties["__uses_this_arg__"] = Value(true);
    typedArrayFrom->properties["__throw_on_new__"] = Value(true);
    typedArrayFrom->nativeFunc = [isConstructorValue, toPrimitive](const std::vector<Value>& args) -> Value {
      auto isBigIntTypedArray = [](TypedArrayType typeValue) {
        return typeValue == TypedArrayType::BigInt64 || typeValue == TypedArrayType::BigUint64;
      };
      auto toNumberForTypedArray = [toPrimitive](const Value& input) -> double {
        Value primitive = toPrimitive(input, false);
        if (primitive.isBigInt() || primitive.isSymbol()) {
          throw std::runtime_error("TypeError: Cannot convert value to number");
        }
        return primitive.toNumber();
      };
      auto toBigIntForTypedArray = [toPrimitive](const Value& input) -> bigint::BigIntValue {
        Value primitive = toPrimitive(input, false);
        if (primitive.isBigInt()) {
          return primitive.toBigInt();
        }
        if (primitive.isBool()) {
          return primitive.toBool() ? 1 : 0;
        }
        if (primitive.isString()) {
          bigint::BigIntValue parsed = 0;
          if (!bigint::parseBigIntString(std::get<std::string>(primitive.data), parsed)) {
            throw std::runtime_error("SyntaxError: Cannot convert string to BigInt");
          }
          return parsed;
        }
        throw std::runtime_error("TypeError: Cannot convert value to BigInt");
      };

      Value ctor = args.empty() ? Value(Undefined{}) : args[0];
      if (!isConstructorValue(ctor)) {
        throw std::runtime_error("TypeError: TypedArray.from requires a constructor");
      }
      if (args.size() < 2) {
        throw std::runtime_error("TypeError: TypedArray.from requires a source");
      }

      Value source = args[1];
      Value mapFn = args.size() > 2 ? args[2] : Value(Undefined{});
      Value thisArg = args.size() > 3 ? args[3] : Value(Undefined{});
      bool hasMapFn = !mapFn.isUndefined();
      if (hasMapFn && !mapFn.isFunction()) {
        throw std::runtime_error("TypeError: TypedArray.from mapper must be a function");
      }

      Interpreter* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        throw std::runtime_error("TypeError: Interpreter unavailable for TypedArray.from");
      }

      auto callChecked = [interpreter](const Value& callee,
                                       const std::vector<Value>& callArgs,
                                       const Value& thisArgValue) -> Value {
        Value out = interpreter->callForHarness(callee, callArgs, thisArgValue);
        if (interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw JsValueException(err);
        }
        return out;
      };

      std::vector<Value> values;
      bool usedIterator = false;
      auto [hasIterator, iteratorMethod] = getPropertyLike(source, WellKnownSymbols::iteratorKey(), source);
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw JsValueException(err);
      }
      if (hasIterator && !iteratorMethod.isUndefined()) {
        if (!iteratorMethod.isFunction()) {
          throw std::runtime_error("TypeError: @@iterator is not callable");
        }
        Value iterator = callChecked(iteratorMethod, {}, source);
        while (true) {
          auto [hasNext, nextMethod] = getPropertyLike(iterator, "next", iterator);
          if (interpreter->hasError()) {
            Value err = interpreter->getError();
            interpreter->clearError();
            throw JsValueException(err);
          }
          if (!hasNext || !nextMethod.isFunction()) {
            break;
          }
          Value step = callChecked(nextMethod, {}, iterator);
          if (!step.isObject()) {
            throw std::runtime_error("TypeError: Iterator result is not an object");
          }
          auto [hasDone, doneValue] = getPropertyLike(step, "done", step);
          if (interpreter->hasError()) {
            Value err = interpreter->getError();
            interpreter->clearError();
            throw JsValueException(err);
          }
          if (hasDone && doneValue.toBool()) {
            break;
          }
          auto [hasValue, stepValue] = getPropertyLike(step, "value", step);
          if (interpreter->hasError()) {
            Value err = interpreter->getError();
            interpreter->clearError();
            throw JsValueException(err);
          }
          values.push_back(hasValue ? stepValue : Value(Undefined{}));
        }
        usedIterator = true;
      }

      if (!usedIterator) {
        auto [foundLength, lengthValue] = getPropertyLike(source, "length", source);
        if (interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw JsValueException(err);
        }
        size_t length = 0;
        if (foundLength) {
          Value numericLength = isObjectLikeValue(lengthValue) ? toPrimitive(lengthValue, false) : lengthValue;
          if (numericLength.isBigInt() || numericLength.isSymbol()) {
            throw std::runtime_error("TypeError: Invalid array-like length");
          }
          double rawLength = numericLength.toNumber();
          if (!std::isnan(rawLength) && rawLength > 0.0) {
            if (std::isinf(rawLength)) {
              length = 9007199254740991ull;
            } else {
              double integer = std::floor(rawLength);
              if (integer > 9007199254740991.0) {
                integer = 9007199254740991.0;
              }
              length = static_cast<size_t>(integer);
            }
          }
        }
        values.reserve(length);
        for (size_t i = 0; i < length; ++i) {
          auto [foundValue, element] = getPropertyLike(source, std::to_string(i), source);
          if (interpreter->hasError()) {
            Value err = interpreter->getError();
            interpreter->clearError();
            throw JsValueException(err);
          }
          values.push_back(foundValue ? element : Value(Undefined{}));
        }
      }

      Value outValue = interpreter->constructFromNative(ctor, {Value(static_cast<double>(values.size()))});
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw JsValueException(err);
      }
      if (!outValue.isTypedArray()) {
        throw std::runtime_error("TypeError: TypedArray.from constructor must return a TypedArray");
      }

      auto target = outValue.getGC<TypedArray>();
      for (size_t i = 0; i < values.size(); ++i) {
        Value mapped = values[i];
        if (hasMapFn) {
          mapped = callChecked(mapFn, {values[i], Value(static_cast<double>(i))}, thisArg);
        }
        if (target->viewedBuffer && target->viewedBuffer->detached) {
          continue;
        }
        if (target->isOutOfBounds() || i >= target->currentLength()) {
          continue;
        }
        if (isBigIntTypedArray(target->type)) {
          bigint::BigIntValue bigintValue = toBigIntForTypedArray(mapped);
          if (target->viewedBuffer && target->viewedBuffer->detached) {
            continue;
          }
          if (target->isOutOfBounds() || i >= target->currentLength()) {
            continue;
          }
          if (target->type == TypedArrayType::BigUint64) {
            target->setBigUintElement(i, bigint::toUint64Trunc(bigintValue));
          } else {
            target->setBigIntElement(i, bigint::toInt64Trunc(bigintValue));
          }
        } else {
          double numberValue = toNumberForTypedArray(mapped);
          if (target->viewedBuffer && target->viewedBuffer->detached) {
            continue;
          }
          if (target->isOutOfBounds() || i >= target->currentLength()) {
            continue;
          }
          target->setElement(i, numberValue);
        }
      }
      return outValue;
    };
    typedArrayConstructor->properties["from"] = Value(typedArrayFrom);
    typedArrayConstructor->properties["__non_enum_from"] = Value(true);
  }
  {
    auto prototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    typedArrayConstructor->properties["prototype"] = Value(prototype);
    prototype->properties["constructor"] = Value(typedArrayConstructor);
    prototype->properties["__non_enum_constructor"] = Value(true);

    auto installGetter = [&](const std::string& propName, auto impl) {
      auto getter = GarbageCollector::makeGC<Function>();
      getter->isNative = true;
      getter->isConstructor = false;
      getter->properties["name"] = Value(std::string("get " + propName));
      getter->properties["length"] = Value(0.0);
      getter->properties["__non_writable_name"] = Value(true);
      getter->properties["__non_enum_name"] = Value(true);
      getter->properties["__non_writable_length"] = Value(true);
      getter->properties["__non_enum_length"] = Value(true);
      getter->properties["__uses_this_arg__"] = Value(true);
      getter->properties["__throw_on_new__"] = Value(true);
      getter->nativeFunc = impl;
      prototype->properties["__get_" + propName] = Value(getter);
      prototype->properties["__non_enum_" + propName] = Value(true);
    };

    auto installMethod = [&](const std::string& propName, double length, auto impl) {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->isConstructor = false;
      fn->properties["name"] = Value(propName);
      fn->properties["length"] = Value(length);
      fn->properties["__non_writable_name"] = Value(true);
      fn->properties["__non_enum_name"] = Value(true);
      fn->properties["__non_writable_length"] = Value(true);
      fn->properties["__non_enum_length"] = Value(true);
      fn->properties["__uses_this_arg__"] = Value(true);
      fn->properties["__throw_on_new__"] = Value(true);
      fn->nativeFunc = impl;
      prototype->properties[propName] = Value(fn);
      prototype->properties["__non_enum_" + propName] = Value(true);
    };

    auto requireTypedArray = [](const std::vector<Value>& args,
                                const std::string& operation) -> GCPtr<TypedArray> {
      if (args.empty() || !args[0].isTypedArray()) {
        throw std::runtime_error("TypeError: " + operation + " called on incompatible receiver");
      }
      return args[0].getGC<TypedArray>();
    };

    auto toIntegerOffset = [toPrimitive](const Value& input) -> int64_t {
      if (input.isUndefined()) {
        return 0;
      }
      Value numeric = isObjectLikeValue(input) ? toPrimitive(input, false) : input;
      if (numeric.isBigInt() || numeric.isSymbol()) {
        throw std::runtime_error("TypeError: Cannot convert value to integer");
      }
      double number = numeric.toNumber();
      if (std::isnan(number) || number == 0.0) {
        return 0;
      }
      if (std::isinf(number)) {
        throw std::runtime_error("RangeError: Invalid typed array offset");
      }
      return static_cast<int64_t>(std::trunc(number));
    };

    auto targetIndexFromOffset = [toIntegerOffset](const Value& input) -> size_t {
      int64_t offset = toIntegerOffset(input);
      if (offset < 0) {
        throw std::runtime_error("RangeError: Invalid typed array offset");
      }
      return static_cast<size_t>(offset);
    };

    auto isBigIntTypedArray = [](TypedArrayType typeValue) {
      return typeValue == TypedArrayType::BigInt64 || typeValue == TypedArrayType::BigUint64;
    };

    auto toNumberForTypedArray = [toPrimitive](const Value& input) -> double {
      Value primitive = isObjectLikeValue(input) ? toPrimitive(input, false) : input;
      if (primitive.isBigInt() || primitive.isSymbol()) {
        throw std::runtime_error("TypeError: Cannot convert value to number");
      }
      return primitive.toNumber();
    };

    auto toBigIntForTypedArray = [toPrimitive](const Value& input) -> bigint::BigIntValue {
      Value primitive = isObjectLikeValue(input) ? toPrimitive(input, false) : input;
      if (primitive.isBigInt()) {
        return primitive.toBigInt();
      }
      if (primitive.isBool()) {
        return primitive.toBool() ? 1 : 0;
      }
      if (primitive.isString()) {
        std::string text = primitive.toString();
        size_t start = 0;
        while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
          start++;
        }
        size_t end = text.size();
        while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
          end--;
        }
        std::string trimmed = text.substr(start, end - start);
        if (trimmed.empty()) {
          return 0;
        }
        size_t pos = 0;
        if (trimmed[0] == '+' || trimmed[0] == '-') {
          pos = 1;
        }
        if (pos == trimmed.size()) {
          throw std::runtime_error("SyntaxError: Cannot convert string to BigInt");
        }
        for (; pos < trimmed.size(); ++pos) {
          if (!std::isdigit(static_cast<unsigned char>(trimmed[pos]))) {
            throw std::runtime_error("SyntaxError: Cannot convert string to BigInt");
          }
        }
        bigint::BigIntValue parsed;
        if (!bigint::parseBigIntString(trimmed, parsed)) {
          throw std::runtime_error("SyntaxError: Cannot convert string to BigInt");
        }
        return parsed;
      }
      throw std::runtime_error("TypeError: Cannot convert value to BigInt");
    };

    auto toLengthForTypedArray = [toPrimitive](const Value& input) -> size_t {
      Value primitive = isObjectLikeValue(input) ? toPrimitive(input, false) : input;
      if (primitive.isBigInt() || primitive.isSymbol()) {
        throw std::runtime_error("TypeError: Cannot convert value to length");
      }
      double number = primitive.toNumber();
      if (std::isnan(number) || number <= 0.0) {
        return 0;
      }
      if (std::isinf(number)) {
        return 9007199254740991ull;
      }
      double integer = std::floor(number);
      if (integer > 9007199254740991.0) {
        return 9007199254740991ull;
      }
      return static_cast<size_t>(integer);
    };

    auto toIntegerOrInfinity = [toPrimitive](const Value& input) -> double {
      if (input.isUndefined()) {
        return 0.0;
      }
      Value primitive = isObjectLikeValue(input) ? toPrimitive(input, false) : input;
      if (primitive.isBigInt() || primitive.isSymbol()) {
        throw std::runtime_error("TypeError: Cannot convert value to integer");
      }
      double number = primitive.toNumber();
      if (std::isnan(number) || number == 0.0) {
        return 0.0;
      }
      if (!std::isfinite(number)) {
        return number;
      }
      return std::trunc(number);
    };

    auto clampRelativeIndex = [toIntegerOrInfinity](const Value& input, size_t length,
                                                    size_t defaultValue) -> size_t {
      if (input.isUndefined()) {
        return defaultValue;
      }
      double integer = toIntegerOrInfinity(input);
      if (integer == -std::numeric_limits<double>::infinity()) {
        return 0;
      }
      if (integer == std::numeric_limits<double>::infinity()) {
        return length;
      }
      if (integer < 0.0) {
        double shifted = static_cast<double>(length) + integer;
        return shifted <= 0.0 ? 0 : static_cast<size_t>(shifted);
      }
      if (integer >= static_cast<double>(length)) {
        return length;
      }
      return static_cast<size_t>(integer);
    };

    auto toSourceObject = [](const Value& input) -> Value {
      if (isObjectLikeValue(input)) {
        return input;
      }
      if (input.isUndefined() || input.isNull()) {
        throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
      }
      if (input.isString()) {
        const std::string& text = input.toString();
        auto obj = GarbageCollector::makeGC<Object>();
        GarbageCollector::instance().reportAllocation(sizeof(Object));
        obj->properties["length"] = Value(static_cast<double>(text.size()));
        for (size_t i = 0; i < text.size(); ++i) {
          obj->properties[std::to_string(i)] = Value(std::string(1, text[i]));
        }
        return Value(obj);
      }
      auto obj = GarbageCollector::makeGC<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      return Value(obj);
    };

    auto typedArrayElementValue = [](const GCPtr<TypedArray>& ta, size_t index) -> Value {
      if (ta->type == TypedArrayType::BigInt64 || ta->type == TypedArrayType::BigUint64) {
        if (ta->type == TypedArrayType::BigUint64) {
          return Value(BigInt(bigint::BigIntValue(ta->getBigUintElement(index))));
        }
        return Value(BigInt(ta->getBigIntElement(index)));
      }
      return Value(ta->getElement(index));
    };

    auto requireTypedArrayWithBuffer = [requireTypedArray](const std::vector<Value>& args,
                                                           const std::string& operation) -> GCPtr<TypedArray> {
      auto ta = requireTypedArray(args, operation);
      if (ta->viewedBuffer && ta->viewedBuffer->detached) {
        throw std::runtime_error("TypeError: TypedArray has a detached buffer");
      }
      if (ta->isOutOfBounds()) {
        throw std::runtime_error("TypeError: TypedArray is out of bounds");
      }
      return ta;
    };

    auto strictEqualValue = [](const Value& left, const Value& right) -> bool {
      if (left.data.index() != right.data.index()) {
        return false;
      }
      if (left.isSymbol() && right.isSymbol()) {
        auto& lsym = std::get<Symbol>(left.data);
        auto& rsym = std::get<Symbol>(right.data);
        return lsym.id == rsym.id;
      }
      if (left.isBigInt() && right.isBigInt()) return left.toBigInt() == right.toBigInt();
      if (left.isNumber() && right.isNumber()) return left.toNumber() == right.toNumber();
      if (left.isString() && right.isString()) return std::get<std::string>(left.data) == std::get<std::string>(right.data);
      if (left.isBool() && right.isBool()) return std::get<bool>(left.data) == std::get<bool>(right.data);
      if ((left.isNull() && right.isNull()) || (left.isUndefined() && right.isUndefined())) return true;
      if (left.isObject() && right.isObject()) return left.getGC<Object>().get() == right.getGC<Object>().get();
      if (left.isArray() && right.isArray()) return left.getGC<Array>().get() == right.getGC<Array>().get();
      if (left.isFunction() && right.isFunction()) return left.getGC<Function>().get() == right.getGC<Function>().get();
      if (left.isTypedArray() && right.isTypedArray()) return left.getGC<TypedArray>().get() == right.getGC<TypedArray>().get();
      if (left.isPromise() && right.isPromise()) return left.getGC<Promise>().get() == right.getGC<Promise>().get();
      if (left.isRegex() && right.isRegex()) return left.getGC<Regex>().get() == right.getGC<Regex>().get();
      if (left.isMap() && right.isMap()) return left.getGC<Map>().get() == right.getGC<Map>().get();
      if (left.isSet() && right.isSet()) return left.getGC<Set>().get() == right.getGC<Set>().get();
      if (left.isError() && right.isError()) return left.getGC<Error>().get() == right.getGC<Error>().get();
      if (left.isGenerator() && right.isGenerator()) return left.getGC<Generator>().get() == right.getGC<Generator>().get();
      if (left.isProxy() && right.isProxy()) return left.getGC<Proxy>().get() == right.getGC<Proxy>().get();
      if (left.isWeakMap() && right.isWeakMap()) return left.getGC<WeakMap>().get() == right.getGC<WeakMap>().get();
      if (left.isWeakSet() && right.isWeakSet()) return left.getGC<WeakSet>().get() == right.getGC<WeakSet>().get();
      if (left.isArrayBuffer() && right.isArrayBuffer()) return left.getGC<ArrayBuffer>().get() == right.getGC<ArrayBuffer>().get();
      if (left.isDataView() && right.isDataView()) return left.getGC<DataView>().get() == right.getGC<DataView>().get();
      if (left.isClass() && right.isClass()) return left.getGC<Class>().get() == right.getGC<Class>().get();
      if (left.isWasmInstance() && right.isWasmInstance()) return left.getGC<WasmInstanceJS>().get() == right.getGC<WasmInstanceJS>().get();
      if (left.isWasmMemory() && right.isWasmMemory()) return left.getGC<WasmMemoryJS>().get() == right.getGC<WasmMemoryJS>().get();
      if (left.isReadableStream() && right.isReadableStream()) return left.getGC<ReadableStream>().get() == right.getGC<ReadableStream>().get();
      if (left.isWritableStream() && right.isWritableStream()) return left.getGC<WritableStream>().get() == right.getGC<WritableStream>().get();
      if (left.isTransformStream() && right.isTransformStream()) return left.getGC<TransformStream>().get() == right.getGC<TransformStream>().get();
      return false;
    };

    auto sameValueZeroValue = [strictEqualValue](const Value& left, const Value& right) -> bool {
      if (left.isNumber() && right.isNumber()) {
        double lhs = left.toNumber();
        double rhs = right.toNumber();
        if (std::isnan(lhs) && std::isnan(rhs)) {
          return true;
        }
      }
      return strictEqualValue(left, right);
    };

    installGetter("buffer", [requireTypedArray](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArray(args, "get TypedArray.prototype.buffer");
      return ta->viewedBuffer ? Value(ta->viewedBuffer) : Value(Undefined{});
    });

    installGetter("byteLength", [requireTypedArray](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArray(args, "get TypedArray.prototype.byteLength");
      if (ta->viewedBuffer && ta->viewedBuffer->detached) {
        return Value(0.0);
      }
      return Value(static_cast<double>(ta->currentByteLength()));
    });

    installGetter("byteOffset", [requireTypedArray](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArray(args, "get TypedArray.prototype.byteOffset");
      if (ta->viewedBuffer && ta->viewedBuffer->detached) {
        return Value(0.0);
      }
      if (ta->viewedBuffer && ta->isOutOfBounds()) {
        return Value(0.0);
      }
      return Value(static_cast<double>(ta->byteOffset));
    });

    installGetter("length", [requireTypedArray](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArray(args, "get TypedArray.prototype.length");
      if (ta->viewedBuffer && ta->viewedBuffer->detached) {
        return Value(0.0);
      }
      return Value(static_cast<double>(ta->currentLength()));
    });

    {
      auto getter = GarbageCollector::makeGC<Function>();
      getter->isNative = true;
      getter->isConstructor = false;
      getter->properties["name"] = Value(std::string("get [Symbol.toStringTag]"));
      getter->properties["length"] = Value(0.0);
      getter->properties["__non_writable_name"] = Value(true);
      getter->properties["__non_enum_name"] = Value(true);
      getter->properties["__non_writable_length"] = Value(true);
      getter->properties["__non_enum_length"] = Value(true);
      getter->properties["__uses_this_arg__"] = Value(true);
      getter->properties["__throw_on_new__"] = Value(true);
      getter->nativeFunc = [](const std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].isTypedArray()) {
          return Value(Undefined{});
        }
        auto ta = args[0].getGC<TypedArray>();
        switch (ta->type) {
          case TypedArrayType::Int8: return Value(std::string("Int8Array"));
          case TypedArrayType::Uint8: return Value(std::string("Uint8Array"));
          case TypedArrayType::Uint8Clamped: return Value(std::string("Uint8ClampedArray"));
          case TypedArrayType::Int16: return Value(std::string("Int16Array"));
          case TypedArrayType::Uint16: return Value(std::string("Uint16Array"));
          case TypedArrayType::Float16: return Value(std::string("Float16Array"));
          case TypedArrayType::Int32: return Value(std::string("Int32Array"));
          case TypedArrayType::Uint32: return Value(std::string("Uint32Array"));
          case TypedArrayType::Float32: return Value(std::string("Float32Array"));
          case TypedArrayType::Float64: return Value(std::string("Float64Array"));
          case TypedArrayType::BigInt64: return Value(std::string("BigInt64Array"));
          case TypedArrayType::BigUint64: return Value(std::string("BigUint64Array"));
        }
        return Value(Undefined{});
      };
      const auto& tagKey = WellKnownSymbols::toStringTagKey();
      prototype->properties["__get_" + tagKey] = Value(getter);
      prototype->properties["__non_enum_" + tagKey] = Value(true);
    }

    installMethod("join", 1, [requireTypedArray, toPrimitive, typedArrayElementValue](const std::vector<Value>& args) -> Value {
      auto toStringForJoin = [toPrimitive](const Value& input) -> std::string {
        Value primitive = isObjectLikeValue(input) ? toPrimitive(input, true) : input;
        if (primitive.isSymbol()) {
          throw std::runtime_error("TypeError: Cannot convert Symbol to string");
        }
        return primitive.toString();
      };

      auto ta = requireTypedArray(args, "TypedArray.prototype.join");
      if (ta->viewedBuffer && ta->viewedBuffer->detached) {
        throw std::runtime_error("TypeError: TypedArray has a detached buffer");
      }
      if (ta->isOutOfBounds()) {
        throw std::runtime_error("TypeError: TypedArray is out of bounds");
      }

      size_t length = ta->currentLength();
      Value separatorValue = args.size() > 1 ? args[1] : Value(Undefined{});
      std::string separator = ",";
      if (!separatorValue.isUndefined()) {
        separator = toStringForJoin(separatorValue);
      }

      std::ostringstream out;
      for (size_t i = 0; i < length; ++i) {
        if (i > 0) {
          out << separator;
        }
        Value element = (!ta->viewedBuffer || (!ta->viewedBuffer->detached && !ta->isOutOfBounds())) &&
                          i < ta->currentLength()
                          ? typedArrayElementValue(ta, i)
                          : Value(Undefined{});
        if (!element.isUndefined() && !element.isNull()) {
          out << toStringForJoin(element);
        }
      }
      return Value(out.str());
    });

    installMethod("find", 1, [requireTypedArrayWithBuffer, typedArrayElementValue](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArrayWithBuffer(args, "TypedArray.prototype.find");
      if (args.size() < 2 || !args[1].isFunction()) {
        throw std::runtime_error("TypeError: TypedArray.prototype.find requires a callback function");
      }
      Interpreter* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        throw std::runtime_error("TypeError: Interpreter unavailable");
      }
      Value callback = args[1];
      Value thisArg = args.size() > 2 ? args[2] : Value(Undefined{});
      size_t length = ta->currentLength();
      for (size_t i = 0; i < length; ++i) {
        Value element = (!ta->viewedBuffer || (!ta->viewedBuffer->detached && !ta->isOutOfBounds())) &&
                          i < ta->currentLength()
                          ? typedArrayElementValue(ta, i)
                          : Value(Undefined{});
        Value matched = interpreter->callForHarness(
          callback, {element, Value(static_cast<double>(i)), args[0]}, thisArg);
        if (interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw JsValueException(err);
        }
        if (matched.toBool()) {
          return element;
        }
      }
      return Value(Undefined{});
    });

    installMethod("findIndex", 1, [requireTypedArrayWithBuffer, typedArrayElementValue](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArrayWithBuffer(args, "TypedArray.prototype.findIndex");
      if (args.size() < 2 || !args[1].isFunction()) {
        throw std::runtime_error("TypeError: TypedArray.prototype.findIndex requires a callback function");
      }
      Interpreter* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        throw std::runtime_error("TypeError: Interpreter unavailable");
      }
      Value callback = args[1];
      Value thisArg = args.size() > 2 ? args[2] : Value(Undefined{});
      size_t length = ta->currentLength();
      for (size_t i = 0; i < length; ++i) {
        Value element = (!ta->viewedBuffer || (!ta->viewedBuffer->detached && !ta->isOutOfBounds())) &&
                          i < ta->currentLength()
                          ? typedArrayElementValue(ta, i)
                          : Value(Undefined{});
        Value matched = interpreter->callForHarness(
          callback, {element, Value(static_cast<double>(i)), args[0]}, thisArg);
        if (interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw JsValueException(err);
        }
        if (matched.toBool()) {
          return Value(static_cast<double>(i));
        }
      }
      return Value(-1.0);
    });

    installMethod("findLast", 1, [requireTypedArrayWithBuffer, typedArrayElementValue](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArrayWithBuffer(args, "TypedArray.prototype.findLast");
      if (args.size() < 2 || !args[1].isFunction()) {
        throw std::runtime_error("TypeError: TypedArray.prototype.findLast requires a callback function");
      }
      Interpreter* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        throw std::runtime_error("TypeError: Interpreter unavailable");
      }
      Value callback = args[1];
      Value thisArg = args.size() > 2 ? args[2] : Value(Undefined{});
      size_t length = ta->currentLength();
      for (int64_t i = static_cast<int64_t>(length) - 1; i >= 0; --i) {
        size_t index = static_cast<size_t>(i);
        Value element = (!ta->viewedBuffer || (!ta->viewedBuffer->detached && !ta->isOutOfBounds())) &&
                          index < ta->currentLength()
                          ? typedArrayElementValue(ta, index)
                          : Value(Undefined{});
        Value matched = interpreter->callForHarness(
          callback, {element, Value(static_cast<double>(index)), args[0]}, thisArg);
        if (interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw JsValueException(err);
        }
        if (matched.toBool()) {
          return element;
        }
      }
      return Value(Undefined{});
    });

    installMethod("findLastIndex", 1, [requireTypedArrayWithBuffer, typedArrayElementValue](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArrayWithBuffer(args, "TypedArray.prototype.findLastIndex");
      if (args.size() < 2 || !args[1].isFunction()) {
        throw std::runtime_error("TypeError: TypedArray.prototype.findLastIndex requires a callback function");
      }
      Interpreter* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        throw std::runtime_error("TypeError: Interpreter unavailable");
      }
      Value callback = args[1];
      Value thisArg = args.size() > 2 ? args[2] : Value(Undefined{});
      size_t length = ta->currentLength();
      for (int64_t i = static_cast<int64_t>(length) - 1; i >= 0; --i) {
        size_t index = static_cast<size_t>(i);
        Value element = (!ta->viewedBuffer || (!ta->viewedBuffer->detached && !ta->isOutOfBounds())) &&
                          index < ta->currentLength()
                          ? typedArrayElementValue(ta, index)
                          : Value(Undefined{});
        Value matched = interpreter->callForHarness(
          callback, {element, Value(static_cast<double>(index)), args[0]}, thisArg);
        if (interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw JsValueException(err);
        }
        if (matched.toBool()) {
          return Value(static_cast<double>(index));
        }
      }
      return Value(-1.0);
    });

    installMethod("every", 1, [requireTypedArrayWithBuffer, typedArrayElementValue](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArrayWithBuffer(args, "TypedArray.prototype.every");
      if (args.size() < 2 || !args[1].isFunction()) {
        throw std::runtime_error("TypeError: TypedArray.prototype.every requires a callback function");
      }
      Interpreter* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        throw std::runtime_error("TypeError: Interpreter unavailable");
      }
      Value callback = args[1];
      Value thisArg = args.size() > 2 ? args[2] : Value(Undefined{});
      size_t length = ta->currentLength();
      for (size_t i = 0; i < length; ++i) {
        Value element = (!ta->viewedBuffer || (!ta->viewedBuffer->detached && !ta->isOutOfBounds())) &&
                          i < ta->currentLength()
                          ? typedArrayElementValue(ta, i)
                          : Value(Undefined{});
        Value matched = interpreter->callForHarness(
          callback, {element, Value(static_cast<double>(i)), args[0]}, thisArg);
        if (interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw JsValueException(err);
        }
        if (!matched.toBool()) {
          return Value(false);
        }
      }
      return Value(true);
    });

    installMethod("some", 1, [requireTypedArrayWithBuffer, typedArrayElementValue](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArrayWithBuffer(args, "TypedArray.prototype.some");
      if (args.size() < 2 || !args[1].isFunction()) {
        throw std::runtime_error("TypeError: TypedArray.prototype.some requires a callback function");
      }
      Interpreter* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        throw std::runtime_error("TypeError: Interpreter unavailable");
      }
      Value callback = args[1];
      Value thisArg = args.size() > 2 ? args[2] : Value(Undefined{});
      size_t length = ta->currentLength();
      for (size_t i = 0; i < length; ++i) {
        Value element = (!ta->viewedBuffer || (!ta->viewedBuffer->detached && !ta->isOutOfBounds())) &&
                          i < ta->currentLength()
                          ? typedArrayElementValue(ta, i)
                          : Value(Undefined{});
        Value matched = interpreter->callForHarness(
          callback, {element, Value(static_cast<double>(i)), args[0]}, thisArg);
        if (interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw JsValueException(err);
        }
        if (matched.toBool()) {
          return Value(true);
        }
      }
      return Value(false);
    });

    installMethod("forEach", 1, [requireTypedArrayWithBuffer, typedArrayElementValue](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArrayWithBuffer(args, "TypedArray.prototype.forEach");
      if (args.size() < 2 || !args[1].isFunction()) {
        throw std::runtime_error("TypeError: TypedArray.prototype.forEach requires a callback function");
      }
      Interpreter* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        throw std::runtime_error("TypeError: Interpreter unavailable");
      }
      Value callback = args[1];
      Value thisArg = args.size() > 2 ? args[2] : Value(Undefined{});
      size_t length = ta->currentLength();
      for (size_t i = 0; i < length; ++i) {
        Value element = (!ta->viewedBuffer || (!ta->viewedBuffer->detached && !ta->isOutOfBounds())) &&
                          i < ta->currentLength()
                          ? typedArrayElementValue(ta, i)
                          : Value(Undefined{});
        interpreter->callForHarness(
          callback, {element, Value(static_cast<double>(i)), args[0]}, thisArg);
        if (interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw JsValueException(err);
        }
      }
      return Value(Undefined{});
    });

    installMethod("reduce", 1, [requireTypedArrayWithBuffer, typedArrayElementValue](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArrayWithBuffer(args, "TypedArray.prototype.reduce");
      if (args.size() < 2 || !args[1].isFunction()) {
        throw std::runtime_error("TypeError: TypedArray.prototype.reduce requires a callback function");
      }
      size_t length = ta->currentLength();
      if (length == 0 && args.size() < 3) {
        throw std::runtime_error("TypeError: Reduce of empty typed array with no initial value");
      }
      Value callback = args[1];
      size_t start = 0;
      Value accumulator;
      if (args.size() > 2) {
        accumulator = args[2];
      } else {
        accumulator = typedArrayElementValue(ta, 0);
        start = 1;
      }
      Interpreter* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        throw std::runtime_error("TypeError: Interpreter unavailable");
      }
      for (size_t i = start; i < length; ++i) {
        Value element = (!ta->viewedBuffer || (!ta->viewedBuffer->detached && !ta->isOutOfBounds())) &&
                          i < ta->currentLength()
                          ? typedArrayElementValue(ta, i)
                          : Value(Undefined{});
        accumulator = interpreter->callForHarness(
          callback,
          {accumulator, element, Value(static_cast<double>(i)), args[0]},
          Value(Undefined{}));
        if (interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw JsValueException(err);
        }
      }
      return accumulator;
    });

    installMethod("reduceRight", 1, [requireTypedArrayWithBuffer, typedArrayElementValue](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArrayWithBuffer(args, "TypedArray.prototype.reduceRight");
      if (args.size() < 2 || !args[1].isFunction()) {
        throw std::runtime_error("TypeError: TypedArray.prototype.reduceRight requires a callback function");
      }
      size_t length = ta->currentLength();
      if (length == 0 && args.size() < 3) {
        throw std::runtime_error("TypeError: Reduce of empty typed array with no initial value");
      }

      Value callback = args[1];
      int64_t index = static_cast<int64_t>(length) - 1;
      Value accumulator;
      if (args.size() > 2) {
        accumulator = args[2];
      } else {
        accumulator = typedArrayElementValue(ta, static_cast<size_t>(index));
        --index;
      }

      Interpreter* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        throw std::runtime_error("TypeError: Interpreter unavailable");
      }

      for (; index >= 0; --index) {
        size_t i = static_cast<size_t>(index);
        Value element = (!ta->viewedBuffer || (!ta->viewedBuffer->detached && !ta->isOutOfBounds())) &&
                          i < ta->currentLength()
                          ? typedArrayElementValue(ta, i)
                          : Value(Undefined{});
        accumulator = interpreter->callForHarness(
          callback,
          {accumulator, element, Value(static_cast<double>(i)), args[0]},
          Value(Undefined{}));
        if (interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw JsValueException(err);
        }
      }
      return accumulator;
    });

    installMethod("at", 1, [requireTypedArray, toIntegerOrInfinity, typedArrayElementValue](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArray(args, "TypedArray.prototype.at");
      if (ta->viewedBuffer && ta->viewedBuffer->detached) {
        throw std::runtime_error("TypeError: TypedArray has a detached buffer");
      }
      if (ta->isOutOfBounds()) {
        throw std::runtime_error("TypeError: TypedArray is out of bounds");
      }

      size_t length = ta->currentLength();
      double relativeIndex = args.size() > 1 ? toIntegerOrInfinity(args[1]) : 0.0;
      double kValue = relativeIndex >= 0.0
                        ? relativeIndex
                        : static_cast<double>(length) + relativeIndex;
      if (!std::isfinite(kValue) || kValue < 0.0 || kValue >= static_cast<double>(length)) {
        return Value(Undefined{});
      }

      size_t index = static_cast<size_t>(kValue);
      bool present = (!ta->viewedBuffer || (!ta->viewedBuffer->detached && !ta->isOutOfBounds())) &&
                     index < ta->currentLength();
      return present ? typedArrayElementValue(ta, index) : Value(Undefined{});
    });

    installMethod("includes", 1, [requireTypedArray, toIntegerOrInfinity, sameValueZeroValue, typedArrayElementValue](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArray(args, "TypedArray.prototype.includes");
      if (ta->viewedBuffer && ta->viewedBuffer->detached) {
        throw std::runtime_error("TypeError: TypedArray has a detached buffer");
      }
      if (ta->isOutOfBounds()) {
        throw std::runtime_error("TypeError: TypedArray is out of bounds");
      }

      Value searchElement = args.size() > 1 ? args[1] : Value(Undefined{});
      size_t length = ta->currentLength();
      if (length == 0) {
        return Value(false);
      }

      double n = args.size() > 2 ? toIntegerOrInfinity(args[2]) : 0.0;
      if (n == std::numeric_limits<double>::infinity()) {
        return Value(false);
      }
      if (n == -std::numeric_limits<double>::infinity()) {
        n = 0.0;
      }

      double kValue = n >= 0.0 ? n : static_cast<double>(length) + n;
      if (kValue < 0.0) {
        kValue = 0.0;
      }
      if (kValue >= static_cast<double>(length)) {
        return Value(false);
      }

      for (size_t index = static_cast<size_t>(kValue); index < length; ++index) {
        Value element = (!ta->viewedBuffer || (!ta->viewedBuffer->detached && !ta->isOutOfBounds())) &&
                          index < ta->currentLength()
                          ? typedArrayElementValue(ta, index)
                          : Value(Undefined{});
        if (sameValueZeroValue(searchElement, element)) {
          return Value(true);
        }
      }
      return Value(false);
    });

    installMethod("indexOf", 1, [requireTypedArray, toIntegerOrInfinity, strictEqualValue, typedArrayElementValue](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArray(args, "TypedArray.prototype.indexOf");
      if (ta->viewedBuffer && ta->viewedBuffer->detached) {
        throw std::runtime_error("TypeError: TypedArray has a detached buffer");
      }
      if (ta->isOutOfBounds()) {
        throw std::runtime_error("TypeError: TypedArray is out of bounds");
      }

      Value searchElement = args.size() > 1 ? args[1] : Value(Undefined{});
      size_t length = ta->currentLength();
      if (length == 0) {
        return Value(-1.0);
      }

      double n = args.size() > 2 ? toIntegerOrInfinity(args[2]) : 0.0;
      if (n == std::numeric_limits<double>::infinity()) {
        return Value(-1.0);
      }
      if (n == -std::numeric_limits<double>::infinity()) {
        n = 0.0;
      }

      double kValue = n >= 0.0 ? n : static_cast<double>(length) + n;
      if (kValue < 0.0) {
        kValue = 0.0;
      }
      if (kValue >= static_cast<double>(length)) {
        return Value(-1.0);
      }

      for (size_t index = static_cast<size_t>(kValue); index < length; ++index) {
        bool present = (!ta->viewedBuffer || (!ta->viewedBuffer->detached && !ta->isOutOfBounds())) &&
                       index < ta->currentLength();
        if (!present) {
          continue;
        }
        Value element = typedArrayElementValue(ta, index);
        if (strictEqualValue(searchElement, element)) {
          return Value(static_cast<double>(index));
        }
      }
      return Value(-1.0);
    });

    installMethod("filter", 1, [env, requireTypedArrayWithBuffer, typedArrayElementValue](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArrayWithBuffer(args, "TypedArray.prototype.filter");
      if (args.size() < 2 || !args[1].isFunction()) {
        throw std::runtime_error("TypeError: TypedArray.prototype.filter requires a callback function");
      }

      auto isConstructorValue = [](const Value& value) -> bool {
        if (value.isFunction()) {
          return value.getGC<Function>()->isConstructor;
        }
        if (value.isClass()) {
          return true;
        }
        if (value.isObject()) {
          auto obj = value.getGC<Object>();
          auto callableIt = obj->properties.find("__callable_object__");
          auto ctorIt = obj->properties.find("constructor");
          if (callableIt != obj->properties.end() &&
              callableIt->second.isBool() &&
              callableIt->second.toBool() &&
              ctorIt != obj->properties.end()) {
            return (ctorIt->second.isFunction() && ctorIt->second.getGC<Function>()->isConstructor) ||
                   ctorIt->second.isClass();
          }
        }
        return false;
      };

      Interpreter* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        throw std::runtime_error("TypeError: Interpreter unavailable");
      }

      Value callback = args[1];
      Value thisArg = args.size() > 2 ? args[2] : Value(Undefined{});
      size_t length = ta->currentLength();
      std::vector<Value> kept;
      kept.reserve(length);
      for (size_t i = 0; i < length; ++i) {
        Value element = (!ta->viewedBuffer || (!ta->viewedBuffer->detached && !ta->isOutOfBounds())) &&
                          i < ta->currentLength()
                          ? typedArrayElementValue(ta, i)
                          : Value(Undefined{});
        Value selected = interpreter->callForHarness(
          callback, {element, Value(static_cast<double>(i)), args[0]}, thisArg);
        if (interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw JsValueException(err);
        }
        if (selected.toBool()) {
          kept.push_back(element);
        }
      }

      const char* ctorName = nullptr;
      switch (ta->type) {
        case TypedArrayType::Int8: ctorName = "Int8Array"; break;
        case TypedArrayType::Uint8: ctorName = "Uint8Array"; break;
        case TypedArrayType::Uint8Clamped: ctorName = "Uint8ClampedArray"; break;
        case TypedArrayType::Int16: ctorName = "Int16Array"; break;
        case TypedArrayType::Uint16: ctorName = "Uint16Array"; break;
        case TypedArrayType::Float16: ctorName = "Float16Array"; break;
        case TypedArrayType::Int32: ctorName = "Int32Array"; break;
        case TypedArrayType::Uint32: ctorName = "Uint32Array"; break;
        case TypedArrayType::Float32: ctorName = "Float32Array"; break;
        case TypedArrayType::Float64: ctorName = "Float64Array"; break;
        case TypedArrayType::BigInt64: ctorName = "BigInt64Array"; break;
        case TypedArrayType::BigUint64: ctorName = "BigUint64Array"; break;
      }

      Value defaultCtor = Value(Undefined{});
      if (ctorName) {
        if (auto ctor = env->get(ctorName); ctor.has_value()) {
          defaultCtor = *ctor;
        }
      }

      Value ctorValue = defaultCtor;
      bool ownConstructorOverride = hasOwnPropertyNoInvoke(args[0], "constructor");
      auto [hasCtor, ctorProp] = getPropertyLike(args[0], "constructor", args[0]);
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw JsValueException(err);
      }
      if (hasCtor && !ctorProp.isUndefined()) {
        if (!isObjectLikeValue(ctorProp)) {
          throw std::runtime_error("TypeError: TypedArray constructor property is not an object");
        }
        auto [hasSpecies, speciesProp] = getPropertyLike(ctorProp, WellKnownSymbols::speciesKey(), ctorProp);
        if (interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw JsValueException(err);
        }
        if (hasSpecies && !speciesProp.isUndefined() && !speciesProp.isNull()) {
          if (!isConstructorValue(speciesProp)) {
            throw std::runtime_error("TypeError: TypedArray species is not a constructor");
          }
          ctorValue = speciesProp;
        } else if (!ownConstructorOverride && isConstructorValue(ctorProp)) {
          // Built-in TypedArray constructors expose @@species returning `this`.
          // Use the inherited constructor when there is no own override.
          ctorValue = ctorProp;
        }
      }

      Value outValue = interpreter->constructFromNative(
        ctorValue, {Value(static_cast<double>(kept.size()))});
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw JsValueException(err);
      }
      if (!outValue.isTypedArray()) {
        throw std::runtime_error("TypeError: TypedArray species constructor must return a TypedArray");
      }

      auto result = outValue.getGC<TypedArray>();
      if (result->viewedBuffer && result->viewedBuffer->detached) {
        throw std::runtime_error("TypeError: TypedArray species constructor returned a detached TypedArray");
      }
      if (result->isOutOfBounds() || result->currentLength() < kept.size()) {
        throw std::runtime_error("TypeError: TypedArray species constructor returned a too-small TypedArray");
      }

      if (result->type == TypedArrayType::BigInt64 || result->type == TypedArrayType::BigUint64) {
        for (size_t i = 0; i < kept.size(); ++i) {
          bigint::BigIntValue bigintValue = kept[i].toBigInt();
          if (result->type == TypedArrayType::BigUint64) {
            result->setBigUintElement(i, bigint::toUint64Trunc(bigintValue));
          } else {
            result->setBigIntElement(i, bigint::toInt64Trunc(bigintValue));
          }
        }
      } else {
        for (size_t i = 0; i < kept.size(); ++i) {
          result->setElement(i, kept[i].toNumber());
        }
      }
      return outValue;
    });

    installMethod("map", 1, [env, requireTypedArrayWithBuffer, typedArrayElementValue](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArrayWithBuffer(args, "TypedArray.prototype.map");
      if (args.size() < 2 || !args[1].isFunction()) {
        throw std::runtime_error("TypeError: TypedArray.prototype.map requires a callback function");
      }

      auto isConstructorValue = [](const Value& value) -> bool {
        if (value.isFunction()) {
          return value.getGC<Function>()->isConstructor;
        }
        if (value.isClass()) {
          return true;
        }
        if (value.isObject()) {
          auto obj = value.getGC<Object>();
          auto callableIt = obj->properties.find("__callable_object__");
          auto ctorIt = obj->properties.find("constructor");
          if (callableIt != obj->properties.end() &&
              callableIt->second.isBool() &&
              callableIt->second.toBool() &&
              ctorIt != obj->properties.end()) {
            return (ctorIt->second.isFunction() && ctorIt->second.getGC<Function>()->isConstructor) ||
                   ctorIt->second.isClass();
          }
        }
        return false;
      };

      Interpreter* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        throw std::runtime_error("TypeError: Interpreter unavailable");
      }

      Value callback = args[1];
      Value thisArg = args.size() > 2 ? args[2] : Value(Undefined{});
      size_t length = ta->currentLength();

      const char* ctorName = nullptr;
      switch (ta->type) {
        case TypedArrayType::Int8: ctorName = "Int8Array"; break;
        case TypedArrayType::Uint8: ctorName = "Uint8Array"; break;
        case TypedArrayType::Uint8Clamped: ctorName = "Uint8ClampedArray"; break;
        case TypedArrayType::Int16: ctorName = "Int16Array"; break;
        case TypedArrayType::Uint16: ctorName = "Uint16Array"; break;
        case TypedArrayType::Float16: ctorName = "Float16Array"; break;
        case TypedArrayType::Int32: ctorName = "Int32Array"; break;
        case TypedArrayType::Uint32: ctorName = "Uint32Array"; break;
        case TypedArrayType::Float32: ctorName = "Float32Array"; break;
        case TypedArrayType::Float64: ctorName = "Float64Array"; break;
        case TypedArrayType::BigInt64: ctorName = "BigInt64Array"; break;
        case TypedArrayType::BigUint64: ctorName = "BigUint64Array"; break;
      }

      Value defaultCtor = Value(Undefined{});
      if (ctorName) {
        if (auto ctor = env->get(ctorName); ctor.has_value()) {
          defaultCtor = *ctor;
        }
      }

      Value ctorValue = defaultCtor;
      bool ownConstructorOverride = hasOwnPropertyNoInvoke(args[0], "constructor");
      auto [hasCtor, ctorProp] = getPropertyLike(args[0], "constructor", args[0]);
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw JsValueException(err);
      }
      if (hasCtor && !ctorProp.isUndefined()) {
        if (!isObjectLikeValue(ctorProp)) {
          throw std::runtime_error("TypeError: TypedArray constructor property is not an object");
        }
        auto [hasSpecies, speciesProp] = getPropertyLike(ctorProp, WellKnownSymbols::speciesKey(), ctorProp);
        if (interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw JsValueException(err);
        }
        if (hasSpecies && !speciesProp.isUndefined() && !speciesProp.isNull()) {
          if (!isConstructorValue(speciesProp)) {
            throw std::runtime_error("TypeError: TypedArray species is not a constructor");
          }
          ctorValue = speciesProp;
        } else if (!ownConstructorOverride && isConstructorValue(ctorProp)) {
          ctorValue = ctorProp;
        }
      }

      Value outValue = interpreter->constructFromNative(
        ctorValue, {Value(static_cast<double>(length))});
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw JsValueException(err);
      }
      if (!outValue.isTypedArray()) {
        throw std::runtime_error("TypeError: TypedArray species constructor must return a TypedArray");
      }

      auto result = outValue.getGC<TypedArray>();
      if (result->viewedBuffer && result->viewedBuffer->detached) {
        throw std::runtime_error("TypeError: TypedArray species constructor returned a detached TypedArray");
      }
      if (result->isOutOfBounds() || result->currentLength() < length) {
        throw std::runtime_error("TypeError: TypedArray species constructor returned a too-small TypedArray");
      }

      for (size_t i = 0; i < length; ++i) {
        Value element = (!ta->viewedBuffer || (!ta->viewedBuffer->detached && !ta->isOutOfBounds())) &&
                          i < ta->currentLength()
                          ? typedArrayElementValue(ta, i)
                          : Value(Undefined{});
        Value mapped = interpreter->callForHarness(
          callback, {element, Value(static_cast<double>(i)), args[0]}, thisArg);
        if (interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw JsValueException(err);
        }

        if (result->type == TypedArrayType::BigUint64) {
          result->setBigUintElement(i, bigint::toUint64Trunc(mapped.toBigInt()));
        } else if (result->type == TypedArrayType::BigInt64) {
          result->setBigIntElement(i, bigint::toInt64Trunc(mapped.toBigInt()));
        } else {
          result->setElement(i, mapped.toNumber());
        }
      }
      return outValue;
    });

    installMethod("lastIndexOf", 1, [requireTypedArray, typedArrayElementValue, toIntegerOrInfinity, strictEqualValue](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArray(args, "TypedArray.prototype.lastIndexOf");
      if (ta->viewedBuffer && ta->viewedBuffer->detached) {
        throw std::runtime_error("TypeError: TypedArray has a detached buffer");
      }
      if (ta->isOutOfBounds()) {
        throw std::runtime_error("TypeError: TypedArray is out of bounds");
      }

      Value searchElement = args.size() > 1 ? args[1] : Value(Undefined{});
      size_t length = ta->currentLength();
      if (length == 0) {
        return Value(-1.0);
      }

      double n = args.size() > 2 ? toIntegerOrInfinity(args[2])
                                 : static_cast<double>(length) - 1.0;
      if (n == -std::numeric_limits<double>::infinity()) {
        return Value(-1.0);
      }

      double kValue = n >= 0.0
                        ? std::min(n, static_cast<double>(length) - 1.0)
                        : static_cast<double>(length) + n;
      if (kValue < 0.0) {
        return Value(-1.0);
      }

      for (int64_t k = static_cast<int64_t>(kValue); k >= 0; --k) {
        size_t index = static_cast<size_t>(k);
        bool present = (!ta->viewedBuffer || (!ta->viewedBuffer->detached && !ta->isOutOfBounds())) &&
                       index < ta->currentLength();
        if (!present) {
          continue;
        }
        Value element = typedArrayElementValue(ta, index);
        if (strictEqualValue(searchElement, element)) {
          return Value(static_cast<double>(index));
        }
      }
      return Value(-1.0);
    });

    installMethod("toReversed", 0, [env, requireTypedArrayWithBuffer, typedArrayElementValue](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArrayWithBuffer(args, "TypedArray.prototype.toReversed");
      size_t length = ta->currentLength();

      const char* ctorName = nullptr;
      switch (ta->type) {
        case TypedArrayType::Int8: ctorName = "Int8Array"; break;
        case TypedArrayType::Uint8: ctorName = "Uint8Array"; break;
        case TypedArrayType::Uint8Clamped: ctorName = "Uint8ClampedArray"; break;
        case TypedArrayType::Int16: ctorName = "Int16Array"; break;
        case TypedArrayType::Uint16: ctorName = "Uint16Array"; break;
        case TypedArrayType::Float16: ctorName = "Float16Array"; break;
        case TypedArrayType::Int32: ctorName = "Int32Array"; break;
        case TypedArrayType::Uint32: ctorName = "Uint32Array"; break;
        case TypedArrayType::Float32: ctorName = "Float32Array"; break;
        case TypedArrayType::Float64: ctorName = "Float64Array"; break;
        case TypedArrayType::BigInt64: ctorName = "BigInt64Array"; break;
        case TypedArrayType::BigUint64: ctorName = "BigUint64Array"; break;
      }

      if (!ctorName) {
        throw std::runtime_error("TypeError: Unknown TypedArray constructor");
      }
      auto ctor = env->get(ctorName);
      if (!ctor.has_value()) {
        throw std::runtime_error("TypeError: TypedArray constructor unavailable");
      }

      auto* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        throw std::runtime_error("TypeError: Interpreter unavailable");
      }

      Value outValue = interpreter->constructFromNative(*ctor, {Value(static_cast<double>(length))});
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw JsValueException(err);
      }
      if (!outValue.isTypedArray()) {
        throw std::runtime_error("TypeError: TypedArray.prototype.toReversed constructor must return a TypedArray");
      }

      auto result = outValue.getGC<TypedArray>();
      for (size_t i = 0; i < length; ++i) {
        Value value = typedArrayElementValue(ta, length - 1 - i);
        if (result->type == TypedArrayType::BigUint64) {
          result->setBigUintElement(i, bigint::toUint64Trunc(value.toBigInt()));
        } else if (result->type == TypedArrayType::BigInt64) {
          result->setBigIntElement(i, bigint::toInt64Trunc(value.toBigInt()));
        } else {
          result->setElement(i, value.toNumber());
        }
      }
      return outValue;
    });

    installMethod("toSorted", 1, [env, requireTypedArrayWithBuffer, typedArrayElementValue, toPrimitive](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArrayWithBuffer(args, "TypedArray.prototype.toSorted");
      Value compareFn = args.size() > 1 ? args[1] : Value(Undefined{});
      bool useCustomCompare = !compareFn.isUndefined();
      if (useCustomCompare && !compareFn.isFunction()) {
        throw std::runtime_error("TypeError: TypedArray.prototype.toSorted requires a callable comparator");
      }

      size_t length = ta->currentLength();
      std::vector<Value> values;
      values.reserve(length);
      for (size_t i = 0; i < length; ++i) {
        values.push_back(typedArrayElementValue(ta, i));
      }

      Interpreter* interpreter = getGlobalInterpreter();
      if (useCustomCompare && !interpreter) {
        throw std::runtime_error("TypeError: Interpreter unavailable");
      }

      auto defaultLess = [](const Value& a, const Value& b) {
        if (a.isBigInt() || b.isBigInt()) {
          bigint::BigIntValue av = a.toBigInt();
          bigint::BigIntValue bv = b.toBigInt();
          return av < bv;
        }

        double av = a.toNumber();
        double bv = b.toNumber();
        bool aNaN = std::isnan(av);
        bool bNaN = std::isnan(bv);
        if (aNaN || bNaN) {
          return !aNaN && bNaN;
        }
        if (av == 0.0 && bv == 0.0) {
          return std::signbit(av) && !std::signbit(bv);
        }
        return av < bv;
      };

      auto compareLess = [&](const Value& a, const Value& b) {
        if (!useCustomCompare) {
          return defaultLess(a, b);
        }
        Value result = interpreter->callForHarness(compareFn, {a, b}, Value(Undefined{}));
        if (interpreter->hasError()) {
          return false;
        }
        Value numeric = isObjectLikeValue(result) ? toPrimitive(result, false) : result;
        double order = numeric.toNumber();
        if (std::isnan(order) || order == 0.0) {
          return false;
        }
        return order < 0.0;
      };

      for (size_t i = 1; i < values.size(); ++i) {
        Value current = values[i];
        size_t j = i;
        while (j > 0) {
          if (!compareLess(current, values[j - 1])) {
            break;
          }
          values[j] = values[j - 1];
          --j;
        }
        values[j] = current;
      }
      if (interpreter && interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw JsValueException(err);
      }

      const char* ctorName = nullptr;
      switch (ta->type) {
        case TypedArrayType::Int8: ctorName = "Int8Array"; break;
        case TypedArrayType::Uint8: ctorName = "Uint8Array"; break;
        case TypedArrayType::Uint8Clamped: ctorName = "Uint8ClampedArray"; break;
        case TypedArrayType::Int16: ctorName = "Int16Array"; break;
        case TypedArrayType::Uint16: ctorName = "Uint16Array"; break;
        case TypedArrayType::Float16: ctorName = "Float16Array"; break;
        case TypedArrayType::Int32: ctorName = "Int32Array"; break;
        case TypedArrayType::Uint32: ctorName = "Uint32Array"; break;
        case TypedArrayType::Float32: ctorName = "Float32Array"; break;
        case TypedArrayType::Float64: ctorName = "Float64Array"; break;
        case TypedArrayType::BigInt64: ctorName = "BigInt64Array"; break;
        case TypedArrayType::BigUint64: ctorName = "BigUint64Array"; break;
      }

      if (!ctorName) {
        throw std::runtime_error("TypeError: Unknown TypedArray constructor");
      }
      auto ctor = env->get(ctorName);
      if (!ctor.has_value()) {
        throw std::runtime_error("TypeError: TypedArray constructor unavailable");
      }
      if (!interpreter) {
        throw std::runtime_error("TypeError: Interpreter unavailable");
      }
      Value outValue = interpreter->constructFromNative(*ctor, {Value(static_cast<double>(length))});
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw JsValueException(err);
      }
      if (!outValue.isTypedArray()) {
        throw std::runtime_error("TypeError: TypedArray.prototype.toSorted constructor must return a TypedArray");
      }

      auto result = outValue.getGC<TypedArray>();
      for (size_t i = 0; i < values.size(); ++i) {
        if (result->type == TypedArrayType::BigUint64) {
          result->setBigUintElement(i, bigint::toUint64Trunc(values[i].toBigInt()));
        } else if (result->type == TypedArrayType::BigInt64) {
          result->setBigIntElement(i, bigint::toInt64Trunc(values[i].toBigInt()));
        } else {
          result->setElement(i, values[i].toNumber());
        }
      }
      return outValue;
    });

    installMethod("with", 2, [env, requireTypedArrayWithBuffer, isBigIntTypedArray,
                              toIntegerOrInfinity, toNumberForTypedArray,
                              toBigIntForTypedArray, typedArrayElementValue](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArrayWithBuffer(args, "TypedArray.prototype.with");
      size_t originalLength = ta->currentLength();

      Value index = args.size() > 1 ? args[1] : Value(Undefined{});
      double relativeIndex = toIntegerOrInfinity(index);
      double actualIndex = relativeIndex >= 0.0
                             ? relativeIndex
                             : static_cast<double>(originalLength) + relativeIndex;

      bool isBigInt = isBigIntTypedArray(ta->type);
      Value replacementValue = Value(Undefined{});
      if (isBigInt) {
        replacementValue = Value(BigInt(toBigIntForTypedArray(args.size() > 2 ? args[2] : Value(Undefined{}))));
      } else {
        replacementValue = Value(toNumberForTypedArray(args.size() > 2 ? args[2] : Value(Undefined{})));
      }

      size_t liveLength = (!ta->viewedBuffer || (!ta->viewedBuffer->detached && !ta->isOutOfBounds()))
                            ? ta->currentLength()
                            : 0;
      if (!std::isfinite(actualIndex) || actualIndex < 0.0 ||
          actualIndex >= static_cast<double>(liveLength)) {
        throw std::runtime_error("RangeError: TypedArray.prototype.with index out of range");
      }

      const char* ctorName = nullptr;
      switch (ta->type) {
        case TypedArrayType::Int8: ctorName = "Int8Array"; break;
        case TypedArrayType::Uint8: ctorName = "Uint8Array"; break;
        case TypedArrayType::Uint8Clamped: ctorName = "Uint8ClampedArray"; break;
        case TypedArrayType::Int16: ctorName = "Int16Array"; break;
        case TypedArrayType::Uint16: ctorName = "Uint16Array"; break;
        case TypedArrayType::Float16: ctorName = "Float16Array"; break;
        case TypedArrayType::Int32: ctorName = "Int32Array"; break;
        case TypedArrayType::Uint32: ctorName = "Uint32Array"; break;
        case TypedArrayType::Float32: ctorName = "Float32Array"; break;
        case TypedArrayType::Float64: ctorName = "Float64Array"; break;
        case TypedArrayType::BigInt64: ctorName = "BigInt64Array"; break;
        case TypedArrayType::BigUint64: ctorName = "BigUint64Array"; break;
      }

      if (!ctorName) {
        throw std::runtime_error("TypeError: Unknown TypedArray constructor");
      }
      auto ctor = env->get(ctorName);
      if (!ctor.has_value()) {
        throw std::runtime_error("TypeError: TypedArray constructor unavailable");
      }

      auto* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        throw std::runtime_error("TypeError: Interpreter unavailable");
      }

      Value outValue = interpreter->constructFromNative(*ctor, {Value(static_cast<double>(originalLength))});
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw JsValueException(err);
      }
      if (!outValue.isTypedArray()) {
        throw std::runtime_error("TypeError: TypedArray.prototype.with constructor must return a TypedArray");
      }

      auto result = outValue.getGC<TypedArray>();
      size_t readablePrefix = (!ta->viewedBuffer || (!ta->viewedBuffer->detached && !ta->isOutOfBounds()))
                                ? std::min(originalLength, ta->currentLength())
                                : 0;
      for (size_t i = 0; i < readablePrefix; ++i) {
        Value element = typedArrayElementValue(ta, i);
        if (result->type == TypedArrayType::BigUint64) {
          result->setBigUintElement(i, bigint::toUint64Trunc(element.toBigInt()));
        } else if (result->type == TypedArrayType::BigInt64) {
          result->setBigIntElement(i, bigint::toInt64Trunc(element.toBigInt()));
        } else {
          result->setElement(i, element.toNumber());
        }
      }

      if (actualIndex >= 0.0 && actualIndex < static_cast<double>(originalLength)) {
        size_t replacementIndex = static_cast<size_t>(actualIndex);
        if (result->type == TypedArrayType::BigUint64) {
          result->setBigUintElement(replacementIndex, bigint::toUint64Trunc(replacementValue.toBigInt()));
        } else if (result->type == TypedArrayType::BigInt64) {
          result->setBigIntElement(replacementIndex, bigint::toInt64Trunc(replacementValue.toBigInt()));
        } else {
          result->setElement(replacementIndex, replacementValue.toNumber());
        }
      }

      return outValue;
    });

    installMethod("fill", 1, [requireTypedArray, isBigIntTypedArray, toIntegerOrInfinity,
                              toNumberForTypedArray, toBigIntForTypedArray](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArray(args, "TypedArray.prototype.fill");
      if (ta->viewedBuffer && ta->viewedBuffer->detached) {
        throw std::runtime_error("TypeError: TypedArray has a detached buffer");
      }
      if (ta->isOutOfBounds()) {
        throw std::runtime_error("TypeError: TypedArray is out of bounds");
      }
      size_t length = ta->currentLength();
      Value fillValue = args.size() > 1 ? args[1] : Value(Undefined{});
      std::optional<bigint::BigIntValue> bigintValue;
      std::optional<double> numberValue;
      if (isBigIntTypedArray(ta->type)) {
        bigintValue = toBigIntForTypedArray(fillValue);
      } else {
        numberValue = toNumberForTypedArray(fillValue);
      }
      double startNumber = args.size() > 2 ? toIntegerOrInfinity(args[2]) : 0.0;
      bool useLengthAsEnd = args.size() <= 3 || args[3].isUndefined();
      double endNumber = useLengthAsEnd ? std::numeric_limits<double>::infinity()
                                        : toIntegerOrInfinity(args[3]);
      if (ta->viewedBuffer && ta->viewedBuffer->detached) {
        throw std::runtime_error("TypeError: TypedArray has a detached buffer");
      }
      if (ta->isOutOfBounds()) {
        throw std::runtime_error("TypeError: TypedArray is out of bounds");
      }
      auto clampNumericIndex = [length](double integer) -> size_t {
        if (integer == -std::numeric_limits<double>::infinity()) {
          return 0;
        }
        if (integer == std::numeric_limits<double>::infinity()) {
          return length;
        }
        if (integer < 0.0) {
          double shifted = static_cast<double>(length) + integer;
          return shifted <= 0.0 ? 0 : static_cast<size_t>(shifted);
        }
        if (integer >= static_cast<double>(length)) {
          return length;
        }
        return static_cast<size_t>(integer);
      };
      size_t start = args.size() > 2 ? clampNumericIndex(startNumber) : 0;
      size_t end = useLengthAsEnd ? length : clampNumericIndex(endNumber);
      if (end < start) {
        end = start;
      }
      if (isBigIntTypedArray(ta->type)) {
        for (size_t i = start; i < end; ++i) {
          if (ta->type == TypedArrayType::BigUint64) {
            ta->setBigUintElement(i, bigint::toUint64Trunc(*bigintValue));
          } else {
            ta->setBigIntElement(i, bigint::toInt64Trunc(*bigintValue));
          }
        }
      } else {
        for (size_t i = start; i < end; ++i) {
          ta->setElement(i, *numberValue);
        }
      }
      return args[0];
    });

    installMethod("copyWithin", 2, [requireTypedArray, toIntegerOrInfinity](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArray(args, "TypedArray.prototype.copyWithin");
      if (ta->viewedBuffer && ta->viewedBuffer->detached) {
        throw std::runtime_error("TypeError: TypedArray has a detached buffer");
      }
      if (ta->isOutOfBounds()) {
        throw std::runtime_error("TypeError: TypedArray is out of bounds");
      }

      size_t initialLength = ta->currentLength();
      double relativeTarget = args.size() > 1 ? toIntegerOrInfinity(args[1]) : 0.0;
      double relativeStart = args.size() > 2 ? toIntegerOrInfinity(args[2]) : 0.0;
      double relativeEnd = (args.size() > 3 && !args[3].isUndefined())
                             ? toIntegerOrInfinity(args[3])
                             : std::numeric_limits<double>::infinity();

      if (ta->viewedBuffer && ta->viewedBuffer->detached) {
        throw std::runtime_error("TypeError: TypedArray has a detached buffer");
      }
      if (ta->isOutOfBounds()) {
        throw std::runtime_error("TypeError: TypedArray is out of bounds");
      }

      size_t currentLength = ta->currentLength();
      auto clampIndex = [initialLength](double relative) -> size_t {
        if (relative == -std::numeric_limits<double>::infinity()) {
          return 0;
        }
        if (relative == std::numeric_limits<double>::infinity()) {
          return initialLength;
        }
        if (relative < 0.0) {
          double shifted = static_cast<double>(initialLength) + relative;
          return shifted <= 0.0 ? 0 : static_cast<size_t>(shifted);
        }
        if (relative >= static_cast<double>(initialLength)) {
          return initialLength;
        }
        return static_cast<size_t>(relative);
      };

      size_t to = clampIndex(relativeTarget);
      size_t from = clampIndex(relativeStart);
      size_t final = clampIndex(relativeEnd);
      if (final < from) {
        final = from;
      }

      size_t count = std::min(final - from, initialLength - to);
      size_t availableSource = from < currentLength ? currentLength - from : 0;
      size_t availableTarget = to < currentLength ? currentLength - to : 0;
      count = std::min(count, std::min(availableSource, availableTarget));
      if (count == 0) {
        return args[0];
      }

      size_t elementSize = ta->elementSize();
      size_t srcByteOffset = ta->byteOffset + from * elementSize;
      size_t dstByteOffset = ta->byteOffset + to * elementSize;
      auto& bytes = ta->storage();
      std::memmove(&bytes[dstByteOffset], &bytes[srcByteOffset], count * elementSize);
      return args[0];
    });

    installMethod("reverse", 0, [requireTypedArray](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArray(args, "TypedArray.prototype.reverse");
      if (ta->viewedBuffer && ta->viewedBuffer->detached) {
        throw std::runtime_error("TypeError: TypedArray has a detached buffer");
      }
      if (ta->isOutOfBounds()) {
        throw std::runtime_error("TypeError: TypedArray is out of bounds");
      }

      size_t length = ta->currentLength();
      size_t elementSize = ta->elementSize();
      auto& bytes = ta->storage();
      std::array<uint8_t, 8> tmp{};
      for (size_t lower = 0, upper = length == 0 ? 0 : length - 1; lower < upper; ++lower, --upper) {
        size_t lowerByteOffset = ta->byteOffset + lower * elementSize;
        size_t upperByteOffset = ta->byteOffset + upper * elementSize;
        std::memcpy(tmp.data(), &bytes[lowerByteOffset], elementSize);
        std::memmove(&bytes[lowerByteOffset], &bytes[upperByteOffset], elementSize);
        std::memcpy(&bytes[upperByteOffset], tmp.data(), elementSize);
      }
      return args[0];
    });

    installMethod("slice", 2, [env, requireTypedArray, toIntegerOrInfinity, typedArrayElementValue,
                               isBigIntTypedArray, toNumberForTypedArray, toBigIntForTypedArray](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArray(args, "TypedArray.prototype.slice");
      if (ta->viewedBuffer && ta->viewedBuffer->detached) {
        throw std::runtime_error("TypeError: TypedArray has a detached buffer");
      }
      if (ta->isOutOfBounds()) {
        throw std::runtime_error("TypeError: TypedArray is out of bounds");
      }

      auto clampIndex = [](double relative, size_t len) -> size_t {
        double lenNumber = static_cast<double>(len);
        double clamped = relative < 0 ? std::max(lenNumber + relative, 0.0)
                                      : std::min(relative, lenNumber);
        return static_cast<size_t>(clamped);
      };
      auto isConstructorValue = [](const Value& value) -> bool {
        if (value.isFunction()) {
          return value.getGC<Function>()->isConstructor;
        }
        if (value.isClass()) {
          return true;
        }
        if (value.isObject()) {
          auto obj = value.getGC<Object>();
          auto callableIt = obj->properties.find("__callable_object__");
          auto ctorIt = obj->properties.find("constructor");
          if (callableIt != obj->properties.end() &&
              callableIt->second.isBool() &&
              callableIt->second.toBool() &&
              ctorIt != obj->properties.end()) {
            return (ctorIt->second.isFunction() && ctorIt->second.getGC<Function>()->isConstructor) ||
                   ctorIt->second.isClass();
          }
        }
        return false;
      };

      size_t initialLength = ta->currentLength();
      double relativeStart = args.size() > 1 ? toIntegerOrInfinity(args[1]) : 0.0;
      double relativeEnd = (args.size() > 2 && !args[2].isUndefined())
                             ? toIntegerOrInfinity(args[2])
                             : static_cast<double>(initialLength);

      size_t begin = clampIndex(relativeStart, initialLength);
      size_t end = clampIndex(relativeEnd, initialLength);
      if (end < begin) {
        end = begin;
      }
      size_t count = end - begin;

      const char* ctorName = nullptr;
      switch (ta->type) {
        case TypedArrayType::Int8: ctorName = "Int8Array"; break;
        case TypedArrayType::Uint8: ctorName = "Uint8Array"; break;
        case TypedArrayType::Uint8Clamped: ctorName = "Uint8ClampedArray"; break;
        case TypedArrayType::Int16: ctorName = "Int16Array"; break;
        case TypedArrayType::Uint16: ctorName = "Uint16Array"; break;
        case TypedArrayType::Float16: ctorName = "Float16Array"; break;
        case TypedArrayType::Int32: ctorName = "Int32Array"; break;
        case TypedArrayType::Uint32: ctorName = "Uint32Array"; break;
        case TypedArrayType::Float32: ctorName = "Float32Array"; break;
        case TypedArrayType::Float64: ctorName = "Float64Array"; break;
        case TypedArrayType::BigInt64: ctorName = "BigInt64Array"; break;
        case TypedArrayType::BigUint64: ctorName = "BigUint64Array"; break;
      }

      Value defaultCtor = Value(Undefined{});
      if (ctorName) {
        if (auto ctor = env->get(ctorName); ctor.has_value()) {
          defaultCtor = *ctor;
        }
      }

      Value ctorValue = defaultCtor;
      auto [hasCtor, ctorProp] = getPropertyLike(args[0], "constructor", args[0]);
      if (auto* currentInterpreter = getGlobalInterpreter(); currentInterpreter && currentInterpreter->hasError()) {
        Value err = currentInterpreter->getError();
        currentInterpreter->clearError();
        throw JsValueException(err);
      }
      if (hasCtor && !ctorProp.isUndefined()) {
        if (!isObjectLikeValue(ctorProp)) {
          throw std::runtime_error("TypeError: TypedArray constructor property is not an object");
        }
        auto [hasSpecies, speciesProp] = getPropertyLike(ctorProp, WellKnownSymbols::speciesKey(), ctorProp);
        if (auto* currentInterpreter = getGlobalInterpreter(); currentInterpreter && currentInterpreter->hasError()) {
          Value err = currentInterpreter->getError();
          currentInterpreter->clearError();
          throw JsValueException(err);
        }
        if (hasSpecies && !speciesProp.isUndefined() && !speciesProp.isNull()) {
          if (!isConstructorValue(speciesProp)) {
            throw std::runtime_error("TypeError: TypedArray species is not a constructor");
          }
          ctorValue = speciesProp;
        }
      }

      auto* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        throw std::runtime_error("TypeError: Interpreter unavailable for TypedArray.prototype.slice");
      }

      Value outValue = interpreter->constructFromNative(ctorValue, {Value(static_cast<double>(count))});
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw JsValueException(err);
      }
      if (!outValue.isTypedArray()) {
        throw std::runtime_error("TypeError: TypedArray species constructor must return a TypedArray");
      }

      auto result = outValue.getGC<TypedArray>();
      if (result->currentLength() < count) {
        throw std::runtime_error("TypeError: TypedArray species result is too short");
      }
      if (count == 0) {
        return outValue;
      }

      if (ta->viewedBuffer && ta->viewedBuffer->detached) {
        throw std::runtime_error("TypeError: TypedArray has a detached buffer");
      }
      if (ta->isOutOfBounds()) {
        throw std::runtime_error("TypeError: TypedArray is out of bounds");
      }

      size_t currentLength = ta->currentLength();
      size_t availableCount = begin < currentLength ? std::min(count, currentLength - begin) : 0;
      if (availableCount == 0) {
        return outValue;
      }

      if (ta->type == result->type) {
        size_t elementSize = ta->elementSize();
        const auto& sourceBytes = ta->storage();
        auto& targetBytes = result->storage();
        size_t srcByteIndex = ta->byteOffset + begin * elementSize;
        size_t targetByteIndex = result->byteOffset;
        size_t limit = targetByteIndex + availableCount * elementSize;
        while (targetByteIndex < limit) {
          targetBytes[targetByteIndex++] = sourceBytes[srcByteIndex++];
        }
        return outValue;
      }

      for (size_t i = 0; i < availableCount; ++i) {
        Value value = typedArrayElementValue(ta, begin + i);
        if (isBigIntTypedArray(result->type)) {
          bigint::BigIntValue bigintValue = toBigIntForTypedArray(value);
          if (result->type == TypedArrayType::BigUint64) {
            result->setBigUintElement(i, bigint::toUint64Trunc(bigintValue));
          } else {
            result->setBigIntElement(i, bigint::toInt64Trunc(bigintValue));
          }
        } else {
          result->setElement(i, toNumberForTypedArray(value));
        }
      }
      return outValue;
    });

    installMethod("subarray", 2, [env, requireTypedArray, toIntegerOrInfinity](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArray(args, "TypedArray.prototype.subarray");

      auto clampIndex = [](double relative, size_t len) -> size_t {
        double lenNumber = static_cast<double>(len);
        double clamped = relative < 0 ? std::max(lenNumber + relative, 0.0)
                                      : std::min(relative, lenNumber);
        return static_cast<size_t>(clamped);
      };
      auto isConstructorValue = [](const Value& value) -> bool {
        if (value.isFunction()) {
          return value.getGC<Function>()->isConstructor;
        }
        if (value.isClass()) {
          return true;
        }
        if (value.isObject()) {
          auto obj = value.getGC<Object>();
          auto callableIt = obj->properties.find("__callable_object__");
          auto ctorIt = obj->properties.find("constructor");
          if (callableIt != obj->properties.end() &&
              callableIt->second.isBool() &&
              callableIt->second.toBool() &&
              ctorIt != obj->properties.end()) {
            return (ctorIt->second.isFunction() && ctorIt->second.getGC<Function>()->isConstructor) ||
                   ctorIt->second.isClass();
          }
        }
        return false;
      };

      size_t srcLength = ta->currentLength();
      size_t srcByteOffset = ta->byteOffset;
      size_t elementSize = ta->elementSize();

      double relativeBegin = args.size() > 1 ? toIntegerOrInfinity(args[1]) : 0.0;
      double relativeEnd = (args.size() > 2 && !args[2].isUndefined())
                             ? toIntegerOrInfinity(args[2])
                             : static_cast<double>(srcLength);
      size_t begin = clampIndex(relativeBegin, srcLength);
      size_t end = clampIndex(relativeEnd, srcLength);
      if (end < begin) {
        end = begin;
      }
      size_t newLength = end - begin;
      size_t beginByteOffset = srcByteOffset + begin * elementSize;

      const char* ctorName = nullptr;
      switch (ta->type) {
        case TypedArrayType::Int8: ctorName = "Int8Array"; break;
        case TypedArrayType::Uint8: ctorName = "Uint8Array"; break;
        case TypedArrayType::Uint8Clamped: ctorName = "Uint8ClampedArray"; break;
        case TypedArrayType::Int16: ctorName = "Int16Array"; break;
        case TypedArrayType::Uint16: ctorName = "Uint16Array"; break;
        case TypedArrayType::Float16: ctorName = "Float16Array"; break;
        case TypedArrayType::Int32: ctorName = "Int32Array"; break;
        case TypedArrayType::Uint32: ctorName = "Uint32Array"; break;
        case TypedArrayType::Float32: ctorName = "Float32Array"; break;
        case TypedArrayType::Float64: ctorName = "Float64Array"; break;
        case TypedArrayType::BigInt64: ctorName = "BigInt64Array"; break;
        case TypedArrayType::BigUint64: ctorName = "BigUint64Array"; break;
      }

      Value defaultCtor = Value(Undefined{});
      if (ctorName) {
        if (auto ctor = env->get(ctorName); ctor.has_value()) {
          defaultCtor = *ctor;
        }
      }

      Value ctorValue = defaultCtor;
      auto [hasCtor, ctorProp] = getPropertyLike(args[0], "constructor", args[0]);
      if (auto* interpreter = getGlobalInterpreter(); interpreter && interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw JsValueException(err);
      }
      if (hasCtor && !ctorProp.isUndefined()) {
        if (!isObjectLikeValue(ctorProp)) {
          throw std::runtime_error("TypeError: TypedArray constructor property is not an object");
        }
        auto [hasSpecies, speciesProp] = getPropertyLike(ctorProp, WellKnownSymbols::speciesKey(), ctorProp);
        if (auto* interpreter = getGlobalInterpreter(); interpreter && interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw JsValueException(err);
        }
        if (hasSpecies && !speciesProp.isUndefined() && !speciesProp.isNull()) {
          if (!isConstructorValue(speciesProp)) {
            throw std::runtime_error("TypeError: TypedArray species is not a constructor");
          }
          ctorValue = speciesProp;
        }
      }

      auto* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        throw std::runtime_error("TypeError: Interpreter unavailable for TypedArray.prototype.subarray");
      }

      std::vector<Value> ctorArgs;
      ctorArgs.push_back(ta->viewedBuffer ? Value(ta->viewedBuffer) : Value(Undefined{}));
      ctorArgs.push_back(Value(static_cast<double>(beginByteOffset)));
      bool useAutoLength = ta->lengthTracking && (args.size() <= 2 || args[2].isUndefined());
      if (!useAutoLength) {
        ctorArgs.push_back(Value(static_cast<double>(newLength)));
      }

      Value outValue = interpreter->constructFromNative(ctorValue, ctorArgs);
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw JsValueException(err);
      }
      if (!outValue.isTypedArray()) {
        throw std::runtime_error("TypeError: TypedArray species constructor must return a TypedArray");
      }
      return outValue;
    });

    installMethod("toLocaleString", 0, [env, requireTypedArrayWithBuffer, typedArrayElementValue, toPrimitive, callChecked](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArrayWithBuffer(args, "TypedArray.prototype.toLocaleString");
      size_t length = ta->currentLength();
      if (length == 0) {
        return Value(std::string(""));
      }

      auto invokeElementToLocaleString = [&](const Value& element) -> std::string {
        if (element.isUndefined() || element.isNull()) {
          return std::string("");
        }

        Value method(Undefined{});
        if (element.isNumber()) {
          if (auto numberCtor = env->get("Number"); numberCtor.has_value()) {
            auto [foundProto, proto] = getPropertyLike(*numberCtor, "prototype", element);
            if (foundProto) {
              auto [foundMethod, value] = getPropertyLike(proto, "toLocaleString", element);
              if (foundMethod) {
                method = value;
              }
            }
          }
        } else if (element.isBigInt()) {
          if (auto bigIntCtor = env->get("BigInt"); bigIntCtor.has_value()) {
            auto [foundProto, proto] = getPropertyLike(*bigIntCtor, "prototype", element);
            if (foundProto) {
              auto [foundMethod, value] = getPropertyLike(proto, "toLocaleString", element);
              if (foundMethod) {
                method = value;
              }
            }
          }
        }

        if (!method.isFunction()) {
          throw std::runtime_error("TypeError: undefined is not a function");
        }

        std::vector<Value> invokeArgs;
        if (args.size() > 1) invokeArgs.push_back(args[1]);
        if (args.size() > 2) invokeArgs.push_back(args[2]);
        Value result = callChecked(method, invokeArgs, element);
        Value primitive = isObjectLikeValue(result) ? toPrimitive(result, true) : result;
        return primitive.toString();
      };

      std::ostringstream out;
      for (size_t i = 0; i < length; ++i) {
        if (i > 0) {
          out << ",";
        }
        Value element = (!ta->viewedBuffer || (!ta->viewedBuffer->detached && !ta->isOutOfBounds())) &&
                          i < ta->currentLength()
                          ? typedArrayElementValue(ta, i)
                          : Value(Undefined{});
        out << invokeElementToLocaleString(element);
      }
      return Value(out.str());
    });

    installMethod("sort", 1, [requireTypedArray, typedArrayElementValue, toPrimitive](const std::vector<Value>& args) -> Value {
      auto ta = requireTypedArray(args, "TypedArray.prototype.sort");
      if (ta->viewedBuffer && ta->viewedBuffer->detached) {
        throw std::runtime_error("TypeError: TypedArray has a detached buffer");
      }
      if (ta->isOutOfBounds()) {
        throw std::runtime_error("TypeError: TypedArray is out of bounds");
      }

      Value compareFn = args.size() > 1 ? args[1] : Value(Undefined{});
      bool useCustomCompare = !compareFn.isUndefined();
      if (useCustomCompare && !compareFn.isFunction()) {
        throw std::runtime_error("TypeError: TypedArray.prototype.sort requires a callable comparator");
      }

      size_t length = ta->currentLength();
      std::vector<Value> values;
      values.reserve(length);
      for (size_t i = 0; i < length; ++i) {
        values.push_back(typedArrayElementValue(ta, i));
      }

      Interpreter* interpreter = getGlobalInterpreter();
      if (useCustomCompare && !interpreter) {
        throw std::runtime_error("TypeError: Interpreter unavailable");
      }

      auto defaultLess = [](const Value& a, const Value& b) {
        if (a.isBigInt() || b.isBigInt()) {
          bigint::BigIntValue av = a.toBigInt();
          bigint::BigIntValue bv = b.toBigInt();
          return av < bv;
        }

        double av = a.toNumber();
        double bv = b.toNumber();
        bool aNaN = std::isnan(av);
        bool bNaN = std::isnan(bv);
        if (aNaN || bNaN) {
          return !aNaN && bNaN;
        }
        if (av == 0.0 && bv == 0.0) {
          return std::signbit(av) && !std::signbit(bv);
        }
        return av < bv;
      };

      auto compareLess = [&](const Value& a, const Value& b) {
        if (!useCustomCompare) {
          return defaultLess(a, b);
        }
        Value result = interpreter->callForHarness(compareFn, {a, b}, Value(Undefined{}));
        if (interpreter->hasError()) {
          return false;
        }
        Value numeric = isObjectLikeValue(result) ? toPrimitive(result, false) : result;
        double order = numeric.toNumber();
        if (std::isnan(order) || order == 0.0) {
          return false;
        }
        return order < 0.0;
      };

      for (size_t i = 1; i < values.size(); ++i) {
        Value current = values[i];
        size_t j = i;
        while (j > 0) {
          if (!compareLess(current, values[j - 1])) {
            break;
          }
          values[j] = values[j - 1];
          --j;
        }
        values[j] = current;
      }
      if (interpreter && interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw JsValueException(err);
      }

      if (ta->type == TypedArrayType::BigInt64 || ta->type == TypedArrayType::BigUint64) {
        for (size_t i = 0; i < values.size(); ++i) {
          if (ta->type == TypedArrayType::BigUint64) {
            ta->setBigUintElement(i, bigint::toUint64Trunc(values[i].toBigInt()));
          } else {
            ta->setBigIntElement(i, bigint::toInt64Trunc(values[i].toBigInt()));
          }
        }
      } else {
        for (size_t i = 0; i < values.size(); ++i) {
          ta->setElement(i, values[i].toNumber());
        }
      }
      return args[0];
    });

    installMethod("set", 1, [requireTypedArray, targetIndexFromOffset, isBigIntTypedArray,
                             toNumberForTypedArray, toBigIntForTypedArray, toLengthForTypedArray,
                             toSourceObject](const std::vector<Value>& args) -> Value {
      auto target = requireTypedArray(args, "TypedArray.prototype.set");
      Value source = args.size() > 1 ? args[1] : Value(Undefined{});
      size_t targetOffset = args.size() > 2 ? targetIndexFromOffset(args[2]) : 0;

      if (target->viewedBuffer && target->viewedBuffer->detached) {
        throw std::runtime_error("TypeError: TypedArray has a detached buffer");
      }

      size_t targetLength = target->currentLength();

      if (source.isTypedArray()) {
        auto src = source.getGC<TypedArray>();
        if (src->viewedBuffer && src->viewedBuffer->detached) {
          throw std::runtime_error("TypeError: TypedArray has a detached buffer");
        }
        if (src->isOutOfBounds()) {
          throw std::runtime_error("TypeError: TypedArray source is out of bounds");
        }
        if (isBigIntTypedArray(target->type) != isBigIntTypedArray(src->type)) {
          throw std::runtime_error("TypeError: Cannot mix BigInt and Number typed arrays");
        }
        size_t srcLength = src->currentLength();
        if (targetOffset > targetLength || srcLength > targetLength - targetOffset) {
          throw std::runtime_error("RangeError: Source is too large");
        }

        if (isBigIntTypedArray(target->type)) {
          if (target->type == TypedArrayType::BigUint64) {
            std::vector<uint64_t> temp(srcLength);
            for (size_t i = 0; i < srcLength; ++i) {
              temp[i] = src->type == TypedArrayType::BigUint64
                          ? src->getBigUintElement(i)
                          : static_cast<uint64_t>(src->getBigIntElement(i));
            }
            for (size_t i = 0; i < srcLength; ++i) {
              target->setBigUintElement(targetOffset + i, temp[i]);
            }
          } else {
            std::vector<int64_t> temp(srcLength);
            for (size_t i = 0; i < srcLength; ++i) {
              temp[i] = src->getBigIntElement(i);
            }
            for (size_t i = 0; i < srcLength; ++i) {
              target->setBigIntElement(targetOffset + i, temp[i]);
            }
          }
        } else {
          bool sameBackingBuffer = src->viewedBuffer && target->viewedBuffer &&
                                   src->viewedBuffer.get() == target->viewedBuffer.get();
          if (!sameBackingBuffer) {
            target->copyFrom(*src, 0, targetOffset, srcLength);
          } else {
            std::vector<double> temp(srcLength);
            for (size_t i = 0; i < srcLength; ++i) {
              temp[i] = src->getElement(i);
            }
            for (size_t i = 0; i < srcLength; ++i) {
              target->setElement(targetOffset + i, temp[i]);
            }
          }
        }
        return Value(Undefined{});
      }

      Value sourceObject = toSourceObject(source);
      auto [lengthFound, lengthValue] = getPropertyLike(sourceObject, "length", sourceObject);
      if (auto* interpreter = getGlobalInterpreter(); interpreter && interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw JsValueException(err);
      }
      size_t sourceLength = 0;
      if (lengthFound) {
        sourceLength = toLengthForTypedArray(lengthValue);
      }
      if (targetOffset > targetLength || sourceLength > targetLength - targetOffset) {
        throw std::runtime_error("RangeError: Source is too large");
      }
      for (size_t i = 0; i < sourceLength; ++i) {
        auto [foundValue, nextValue] = getPropertyLike(sourceObject, std::to_string(i), sourceObject);
        if (auto* interpreter = getGlobalInterpreter(); interpreter && interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw JsValueException(err);
        }
        Value element = foundValue ? nextValue : Value(Undefined{});
        if (isBigIntTypedArray(target->type)) {
          if (target->type == TypedArrayType::BigUint64) {
            target->setBigUintElement(targetOffset + i, bigint::toUint64Trunc(toBigIntForTypedArray(element)));
          } else {
            target->setBigIntElement(targetOffset + i, bigint::toInt64Trunc(toBigIntForTypedArray(element)));
          }
        } else {
          target->setElement(targetOffset + i, toNumberForTypedArray(element));
        }
      }
      return Value(Undefined{});
    });

    auto typedArrayIteratorFactory =
      [requireTypedArrayWithBuffer, typedArrayElementValue](const std::string& name, int kind) {
        auto fn = GarbageCollector::makeGC<Function>();
        fn->isNative = true;
        fn->isConstructor = false;
        fn->properties["name"] = Value(name);
        fn->properties["length"] = Value(0.0);
        fn->properties["__non_writable_name"] = Value(true);
        fn->properties["__non_enum_name"] = Value(true);
        fn->properties["__non_writable_length"] = Value(true);
        fn->properties["__non_enum_length"] = Value(true);
        fn->properties["__uses_this_arg__"] = Value(true);
        fn->properties["__throw_on_new__"] = Value(true);
        fn->nativeFunc = [requireTypedArrayWithBuffer, typedArrayElementValue, kind](const std::vector<Value>& args) -> Value {
          auto ta = requireTypedArrayWithBuffer(args,
            kind == 0 ? "TypedArray.prototype.values" :
            kind == 1 ? "TypedArray.prototype.keys" :
                        "TypedArray.prototype.entries");
          auto iterator = GarbageCollector::makeGC<Object>();
          GarbageCollector::instance().reportAllocation(sizeof(Object));
          auto index = std::make_shared<size_t>(0);
          auto nextFn = GarbageCollector::makeGC<Function>();
          nextFn->isNative = true;
          auto exhausted = std::make_shared<bool>(false);
          nextFn->nativeFunc = [ta, index, exhausted, kind, typedArrayElementValue](const std::vector<Value>&) -> Value {
            if (*exhausted) {
              return makeIteratorResultObject(Value(Undefined{}), true);
            }
            if (ta->viewedBuffer && ta->viewedBuffer->detached) {
              *exhausted = true;
              return makeIteratorResultObject(Value(Undefined{}), true);
            }
            size_t length = ta->currentLength();
            if (ta->isOutOfBounds() || *index >= length) {
              *exhausted = true;
              return makeIteratorResultObject(Value(Undefined{}), true);
            }
            size_t current = (*index)++;
            if (kind == 1) {
              return makeIteratorResultObject(Value(static_cast<double>(current)), false);
            }
            Value value = typedArrayElementValue(ta, current);
            if (kind == 2) {
              auto pair = GarbageCollector::makeGC<Array>();
              GarbageCollector::instance().reportAllocation(sizeof(Array));
              pair->elements.push_back(Value(static_cast<double>(current)));
              pair->elements.push_back(value);
              return makeIteratorResultObject(Value(pair), false);
            }
            return makeIteratorResultObject(value, false);
          };
          auto selfFn = GarbageCollector::makeGC<Function>();
          selfFn->isNative = true;
          selfFn->properties["__uses_this_arg__"] = Value(true);
          selfFn->properties["__throw_on_new__"] = Value(true);
          selfFn->nativeFunc = [](const std::vector<Value>& selfArgs) -> Value {
            return selfArgs.empty() ? Value(Undefined{}) : selfArgs[0];
          };
          iterator->properties["next"] = Value(nextFn);
          iterator->properties[WellKnownSymbols::iteratorKey()] = Value(selfFn);
          iterator->properties["__non_enum_" + WellKnownSymbols::iteratorKey()] = Value(true);
          return Value(iterator);
        };
        return fn;
      };

    auto valuesFn = typedArrayIteratorFactory("values", 0);
    prototype->properties["values"] = Value(valuesFn);
    prototype->properties["__non_enum_values"] = Value(true);
    prototype->properties[WellKnownSymbols::iteratorKey()] = Value(valuesFn);
    prototype->properties["__non_enum_" + WellKnownSymbols::iteratorKey()] = Value(true);

    auto keysFn = typedArrayIteratorFactory("keys", 1);
    prototype->properties["keys"] = Value(keysFn);
    prototype->properties["__non_enum_keys"] = Value(true);

    auto entriesFn = typedArrayIteratorFactory("entries", 2);
    prototype->properties["entries"] = Value(entriesFn);
    prototype->properties["__non_enum_entries"] = Value(true);
  }

  env->define("TypedArray", Value(typedArrayConstructor));
  env->define("__intrinsic_TypedArray__", Value(typedArrayConstructor));

  env->define("Int8Array", createTypedArrayConstructor(TypedArrayType::Int8, "Int8Array"));
  env->define("Uint8Array", createTypedArrayConstructor(TypedArrayType::Uint8, "Uint8Array"));
  env->define("Uint8ClampedArray", createTypedArrayConstructor(TypedArrayType::Uint8Clamped, "Uint8ClampedArray"));
  env->define("Int16Array", createTypedArrayConstructor(TypedArrayType::Int16, "Int16Array"));
  env->define("Uint16Array", createTypedArrayConstructor(TypedArrayType::Uint16, "Uint16Array"));
  env->define("Float16Array", createTypedArrayConstructor(TypedArrayType::Float16, "Float16Array"));
  env->define("Int32Array", createTypedArrayConstructor(TypedArrayType::Int32, "Int32Array"));
  env->define("Uint32Array", createTypedArrayConstructor(TypedArrayType::Uint32, "Uint32Array"));
  env->define("Float32Array", createTypedArrayConstructor(TypedArrayType::Float32, "Float32Array"));
  env->define("Float64Array", createTypedArrayConstructor(TypedArrayType::Float64, "Float64Array"));
  env->define("BigInt64Array", createTypedArrayConstructor(TypedArrayType::BigInt64, "BigInt64Array"));
  env->define("BigUint64Array", createTypedArrayConstructor(TypedArrayType::BigUint64, "BigUint64Array"));

  const char* concreteTypedArrayNames[] = {
    "Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array",
    "Float16Array", "Int32Array", "Uint32Array", "Float32Array", "Float64Array",
    "BigInt64Array", "BigUint64Array"
  };
  for (const char* ctorName : concreteTypedArrayNames) {
    if (auto ctorValue = env->get(ctorName); ctorValue && ctorValue->isFunction()) {
      auto ctor = ctorValue->getGC<Function>();
      ctor->properties["__proto__"] = Value(typedArrayConstructor);
      if (auto ofIt = typedArrayConstructor->properties.find("of"); ofIt != typedArrayConstructor->properties.end()) {
        ctor->properties["of"] = ofIt->second;
        ctor->properties["__non_enum_of"] = Value(true);
      }
      if (auto fromIt = typedArrayConstructor->properties.find("from"); fromIt != typedArrayConstructor->properties.end()) {
        ctor->properties["from"] = fromIt->second;
        ctor->properties["__non_enum_from"] = Value(true);
      }
    }
  }
  for (const char* ctorName : {"Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array",
                               "Uint16Array", "Float16Array", "Int32Array", "Uint32Array",
                               "Float32Array", "Float64Array", "BigInt64Array", "BigUint64Array"}) {
    auto ctorValue = env->get(ctorName);
    if (!ctorValue || !ctorValue->isFunction()) {
      continue;
    }
    auto ctor = ctorValue->getGC<Function>();
    ctor->properties["__proto__"] = Value(typedArrayConstructor);
  }

  // ArrayBuffer constructor
  auto arrayBufferConstructor = GarbageCollector::makeGC<Function>();
  arrayBufferConstructor->isNative = true;
  arrayBufferConstructor->isConstructor = true;
  arrayBufferConstructor->properties["name"] = Value(std::string("ArrayBuffer"));
  arrayBufferConstructor->properties["length"] = Value(1.0);
  auto arrayBufferConstructImpl = GarbageCollector::makeGC<Function>();
  arrayBufferConstructImpl->isNative = true;
  arrayBufferConstructImpl->isConstructor = true;
  arrayBufferConstructImpl->nativeFunc = [toPrimitive](const std::vector<Value>& args) -> Value {
    constexpr size_t kMaxRequestedArrayBufferLength = static_cast<size_t>(7) * 1125899906842624ull;
    auto toIndex = [toPrimitive](const Value& input) -> size_t {
      if (input.isUndefined()) {
        return 0;
      }
      Value numeric = isObjectLikeValue(input) ? toPrimitive(input, false) : input;
      if (numeric.isBigInt() || numeric.isSymbol()) {
        throw std::runtime_error("TypeError: Cannot convert value to index");
      }
      double number = numeric.toNumber();
      if (std::isnan(number) || number == 0.0) {
        return 0;
      }
      if (std::isinf(number)) {
        throw std::runtime_error("RangeError: Invalid ArrayBuffer length");
      }
      double integer = std::trunc(number);
      if (integer < 0.0 || integer > 9007199254740991.0) {
        throw std::runtime_error("RangeError: Invalid ArrayBuffer length");
      }
      return static_cast<size_t>(integer);
    };

    size_t length = args.empty() ? 0 : toIndex(args[0]);
    bool hasMaxByteLength = false;
    size_t maxByteLength = length;
    if (args.size() > 1 && args[1].isObject()) {
      auto [foundMax, maxValue] = getPropertyLike(args[1], "maxByteLength", args[1]);
      if (foundMax && !maxValue.isUndefined()) {
        hasMaxByteLength = true;
        maxByteLength = toIndex(maxValue);
        if (maxByteLength < length) {
          throw std::runtime_error("RangeError: Invalid maxByteLength");
        }
        if (maxByteLength >= kMaxRequestedArrayBufferLength) {
          throw std::runtime_error("RangeError: Invalid maxByteLength");
        }
      }
    }

    auto buffer = GarbageCollector::makeGC<ArrayBuffer>(length, maxByteLength);
    buffer->resizable = hasMaxByteLength;
    GarbageCollector::instance().reportAllocation(length);
    return Value(buffer);
  };
  arrayBufferConstructor->nativeFunc = [](const std::vector<Value>&) -> Value {
    throw std::runtime_error("TypeError: Constructor ArrayBuffer requires 'new'");
  };
  arrayBufferConstructor->properties["__native_construct__"] = Value(arrayBufferConstructImpl);
  {
    auto speciesGetter = GarbageCollector::makeGC<Function>();
    speciesGetter->isNative = true;
    speciesGetter->isConstructor = false;
    speciesGetter->properties["name"] = Value(std::string("get [Symbol.species]"));
    speciesGetter->properties["length"] = Value(0.0);
    speciesGetter->properties["__non_writable_name"] = Value(true);
    speciesGetter->properties["__non_enum_name"] = Value(true);
    speciesGetter->properties["__non_writable_length"] = Value(true);
    speciesGetter->properties["__non_enum_length"] = Value(true);
    speciesGetter->properties["__uses_this_arg__"] = Value(true);
    speciesGetter->properties["__throw_on_new__"] = Value(true);
    speciesGetter->nativeFunc = [](const std::vector<Value>& args) -> Value {
      return args.empty() ? Value(Undefined{}) : args[0];
    };
    const auto& speciesKey = WellKnownSymbols::speciesKey();
    arrayBufferConstructor->properties["__get_" + speciesKey] = Value(speciesGetter);
    arrayBufferConstructor->properties["__non_enum_" + speciesKey] = Value(true);
  }
  {
    auto prototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    arrayBufferConstructor->properties["prototype"] = Value(prototype);
    prototype->properties["constructor"] = Value(arrayBufferConstructor);
    prototype->properties["__non_enum_constructor"] = Value(true);
    prototype->properties["name"] = Value(std::string("ArrayBuffer"));
  }
  env->define("ArrayBuffer", Value(arrayBufferConstructor));

  // SharedArrayBuffer (ES2017) - backed by ArrayBuffer in this runtime.
  auto sharedArrayBufferConstructor = GarbageCollector::makeGC<Function>();
  sharedArrayBufferConstructor->isNative = true;
  sharedArrayBufferConstructor->isConstructor = true;
  sharedArrayBufferConstructor->properties["name"] = Value(std::string("SharedArrayBuffer"));
  sharedArrayBufferConstructor->properties["length"] = Value(1.0);
  sharedArrayBufferConstructor->properties["__non_writable_name"] = Value(true);
  sharedArrayBufferConstructor->properties["__non_enum_name"] = Value(true);
  sharedArrayBufferConstructor->properties["__non_writable_length"] = Value(true);
  sharedArrayBufferConstructor->properties["__non_enum_length"] = Value(true);
  sharedArrayBufferConstructor->properties["__non_writable_prototype"] = Value(true);
  sharedArrayBufferConstructor->properties["__non_configurable_prototype"] = Value(true);
  auto sharedArrayBufferConstructImpl = GarbageCollector::makeGC<Function>();
  sharedArrayBufferConstructImpl->isNative = true;
  sharedArrayBufferConstructImpl->isConstructor = true;
  sharedArrayBufferConstructImpl->nativeFunc = [toPrimitive](const std::vector<Value>& args) -> Value {
    constexpr size_t kMaxRequestedArrayBufferLength = static_cast<size_t>(7) * 1125899906842624ull;
    auto toIndex = [toPrimitive](const Value& input) -> size_t {
      if (input.isUndefined()) {
        return 0;
      }
      Value numeric = isObjectLikeValue(input) ? toPrimitive(input, false) : input;
      if (numeric.isBigInt() || numeric.isSymbol()) {
        throw std::runtime_error("TypeError: Cannot convert value to index");
      }
      double number = numeric.toNumber();
      if (std::isnan(number) || number == 0.0) {
        return 0;
      }
      if (std::isinf(number)) {
        throw std::runtime_error("RangeError: Invalid ArrayBuffer length");
      }
      double integer = std::trunc(number);
      if (integer < 0.0 || integer > 9007199254740991.0) {
        throw std::runtime_error("RangeError: Invalid ArrayBuffer length");
      }
      return static_cast<size_t>(integer);
    };

    size_t length = args.empty() ? 0 : toIndex(args[0]);
    bool hasMaxByteLength = false;
    size_t maxByteLength = length;
    if (args.size() > 1 && args[1].isObject()) {
      auto [foundMax, maxValue] = getPropertyLike(args[1], "maxByteLength", args[1]);
      if (foundMax && !maxValue.isUndefined()) {
        hasMaxByteLength = true;
        maxByteLength = toIndex(maxValue);
        if (maxByteLength < length) {
          throw std::runtime_error("RangeError: Invalid maxByteLength");
        }
        if (maxByteLength >= kMaxRequestedArrayBufferLength) {
          throw std::runtime_error("RangeError: Invalid maxByteLength");
        }
      }
    }

    auto buffer = GarbageCollector::makeGC<ArrayBuffer>(length, maxByteLength);
    buffer->resizable = hasMaxByteLength;
    buffer->properties["__shared_array_buffer__"] = Value(true);
    GarbageCollector::instance().reportAllocation(length);
    return Value(buffer);
  };
  sharedArrayBufferConstructor->nativeFunc = [](const std::vector<Value>&) -> Value {
    throw std::runtime_error("TypeError: Constructor SharedArrayBuffer requires 'new'");
  };
  sharedArrayBufferConstructor->properties["__native_construct__"] = Value(sharedArrayBufferConstructImpl);
  {
    auto prototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    sharedArrayBufferConstructor->properties["prototype"] = Value(prototype);
    prototype->properties["constructor"] = Value(sharedArrayBufferConstructor);
    prototype->properties["__non_enum_constructor"] = Value(true);
    prototype->properties[WellKnownSymbols::toStringTagKey()] = Value(std::string("SharedArrayBuffer"));
    prototype->properties["__non_writable_" + WellKnownSymbols::toStringTagKey()] = Value(true);
    prototype->properties["__non_enum_" + WellKnownSymbols::toStringTagKey()] = Value(true);

    auto isSharedArrayBufferLike = [](const GCPtr<ArrayBuffer>& buffer) {
      if (!buffer) return false;
      auto markerIt = buffer->properties.find("__shared_array_buffer__");
      return markerIt != buffer->properties.end() &&
             markerIt->second.isBool() &&
             markerIt->second.toBool();
    };
    auto installGetter = [&](const std::string& name, auto impl) {
      auto getter = GarbageCollector::makeGC<Function>();
      getter->isNative = true;
      getter->isConstructor = false;
      getter->properties["name"] = Value(std::string("get ") + name);
      getter->properties["length"] = Value(0.0);
      getter->properties["__non_writable_name"] = Value(true);
      getter->properties["__non_enum_name"] = Value(true);
      getter->properties["__non_writable_length"] = Value(true);
      getter->properties["__non_enum_length"] = Value(true);
      getter->properties["__uses_this_arg__"] = Value(true);
      getter->properties["__throw_on_new__"] = Value(true);
      getter->nativeFunc = impl;
      prototype->properties["__get_" + name] = Value(getter);
      prototype->properties["__non_enum_" + name] = Value(true);
    };
    auto installMethod = [&](const std::string& name, int length, auto impl) {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->isConstructor = false;
      fn->properties["name"] = Value(name);
      fn->properties["length"] = Value(static_cast<double>(length));
      fn->properties["__non_writable_name"] = Value(true);
      fn->properties["__non_enum_name"] = Value(true);
      fn->properties["__non_writable_length"] = Value(true);
      fn->properties["__non_enum_length"] = Value(true);
      fn->properties["__uses_this_arg__"] = Value(true);
      fn->properties["__throw_on_new__"] = Value(true);
      fn->nativeFunc = impl;
      prototype->properties[name] = Value(fn);
      prototype->properties["__non_enum_" + name] = Value(true);
    };
    auto toIndex = [toPrimitive](const Value& input, const std::string& rangeMessage) -> size_t {
      Value numeric = isObjectLikeValue(input) ? toPrimitive(input, false) : input;
      if (numeric.isBigInt() || numeric.isSymbol()) {
        throw std::runtime_error("TypeError: Cannot convert value to index");
      }
      double number = numeric.toNumber();
      if (std::isnan(number) || number == 0.0) {
        return 0;
      }
      if (std::isinf(number)) {
        throw std::runtime_error(rangeMessage);
      }
      double integer = std::trunc(number);
      if (integer < 0.0 || integer > 9007199254740991.0) {
        throw std::runtime_error(rangeMessage);
      }
      return static_cast<size_t>(integer);
    };
    auto toIntegerOrInfinity = [toPrimitive](const Value& input) -> double {
      Value numeric = isObjectLikeValue(input) ? toPrimitive(input, false) : input;
      if (numeric.isBigInt() || numeric.isSymbol()) {
        throw std::runtime_error("TypeError: Cannot convert value to number");
      }
      double number = numeric.toNumber();
      if (std::isnan(number) || number == 0.0) {
        return 0.0;
      }
      if (std::isinf(number)) {
        return number;
      }
      return std::trunc(number);
    };

    installGetter("byteLength", [isSharedArrayBufferLike](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isArrayBuffer()) {
        throw std::runtime_error("TypeError: get SharedArrayBuffer.prototype.byteLength called on incompatible receiver");
      }
      auto buffer = args[0].getGC<ArrayBuffer>();
      if (!isSharedArrayBufferLike(buffer)) {
        throw std::runtime_error("TypeError: get SharedArrayBuffer.prototype.byteLength called on incompatible receiver");
      }
      return Value(static_cast<double>(buffer->byteLength));
    });

    installGetter("maxByteLength", [isSharedArrayBufferLike](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isArrayBuffer()) {
        throw std::runtime_error("TypeError: get SharedArrayBuffer.prototype.maxByteLength called on incompatible receiver");
      }
      auto buffer = args[0].getGC<ArrayBuffer>();
      if (!isSharedArrayBufferLike(buffer)) {
        throw std::runtime_error("TypeError: get SharedArrayBuffer.prototype.maxByteLength called on incompatible receiver");
      }
      return Value(static_cast<double>(buffer->resizable ? buffer->maxByteLength : buffer->byteLength));
    });

    installGetter("growable", [isSharedArrayBufferLike](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isArrayBuffer()) {
        throw std::runtime_error("TypeError: get SharedArrayBuffer.prototype.growable called on incompatible receiver");
      }
      auto buffer = args[0].getGC<ArrayBuffer>();
      if (!isSharedArrayBufferLike(buffer)) {
        throw std::runtime_error("TypeError: get SharedArrayBuffer.prototype.growable called on incompatible receiver");
      }
      return Value(buffer->resizable);
    });

    installMethod("grow", 1, [isSharedArrayBufferLike, toIntegerOrInfinity](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isArrayBuffer()) {
        throw std::runtime_error("TypeError: SharedArrayBuffer.prototype.grow called on incompatible receiver");
      }
      auto buffer = args[0].getGC<ArrayBuffer>();
      if (!isSharedArrayBufferLike(buffer) || !buffer->resizable) {
        throw std::runtime_error("TypeError: SharedArrayBuffer.prototype.grow called on incompatible receiver");
      }

      double requestedLength = args.size() > 1 ? toIntegerOrInfinity(args[1]) : 0.0;
      if (std::isinf(requestedLength) || requestedLength < 0) {
        throw std::runtime_error("RangeError: Invalid SharedArrayBuffer grow length");
      }
      size_t newByteLength = static_cast<size_t>(requestedLength);
      if (newByteLength < buffer->byteLength || newByteLength > buffer->maxByteLength) {
        throw std::runtime_error("RangeError: Invalid SharedArrayBuffer grow length");
      }
      buffer->data.resize(newByteLength, 0);
      buffer->byteLength = newByteLength;
      return Value(Undefined{});
    });

    installMethod("slice", 2, [env, sharedArrayBufferConstructor, prototype, isSharedArrayBufferLike, toPrimitive, toIntegerOrInfinity](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isArrayBuffer()) {
        throw std::runtime_error("TypeError: SharedArrayBuffer.prototype.slice called on incompatible receiver");
      }
      auto thisBuf = args[0].getGC<ArrayBuffer>();
      if (!isSharedArrayBufferLike(thisBuf)) {
        throw std::runtime_error("TypeError: SharedArrayBuffer.prototype.slice called on incompatible receiver");
      }

      auto clampIndex = [](double relative, size_t len) -> size_t {
        double lenNumber = static_cast<double>(len);
        double clamped = relative < 0 ? std::max(lenNumber + relative, 0.0)
                                      : std::min(relative, lenNumber);
        return static_cast<size_t>(clamped);
      };
      auto isConstructorValue = [](const Value& value) -> bool {
        if (value.isFunction()) {
          return value.getGC<Function>()->isConstructor;
        }
        if (value.isClass()) {
          return true;
        }
        if (value.isObject()) {
          auto obj = value.getGC<Object>();
          auto callableIt = obj->properties.find("__callable_object__");
          auto ctorIt = obj->properties.find("constructor");
          if (callableIt != obj->properties.end() &&
              callableIt->second.isBool() &&
              callableIt->second.toBool() &&
              ctorIt != obj->properties.end()) {
            return (ctorIt->second.isFunction() && ctorIt->second.getGC<Function>()->isConstructor) ||
                   ctorIt->second.isClass();
          }
        }
        return false;
      };

      size_t len = thisBuf->byteLength;
      double relativeStart = args.size() > 1 ? toIntegerOrInfinity(args[1]) : 0.0;
      double relativeEnd = (args.size() > 2 && !args[2].isUndefined())
                             ? toIntegerOrInfinity(args[2])
                             : static_cast<double>(len);
      size_t start = clampIndex(relativeStart, len);
      size_t finish = clampIndex(relativeEnd, len);
      if (finish < start) {
        finish = start;
      }
      size_t newLen = finish - start;

      Value ctorValue = Value(sharedArrayBufferConstructor);
      auto [hasCtor, ctorProp] = getPropertyLike(args[0], "constructor", args[0]);
      if (hasCtor && !ctorProp.isUndefined()) {
        if (!isObjectLikeValue(ctorProp)) {
          throw std::runtime_error("TypeError: SharedArrayBuffer constructor property is not an object");
        }
        auto [hasSpecies, speciesProp] = getPropertyLike(ctorProp, WellKnownSymbols::speciesKey(), ctorProp);
        if (hasSpecies && !speciesProp.isUndefined() && !speciesProp.isNull()) {
          if (!isConstructorValue(speciesProp)) {
            throw std::runtime_error("TypeError: SharedArrayBuffer species is not a constructor");
          }
          ctorValue = speciesProp;
        }
      }

      auto* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        throw std::runtime_error("TypeError: Interpreter unavailable for SharedArrayBuffer.prototype.slice");
      }
      Value outValue = interpreter->constructFromNative(ctorValue, {Value(static_cast<double>(newLen))});
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw std::runtime_error(err.toString());
      }
      if (!outValue.isArrayBuffer()) {
        throw std::runtime_error("TypeError: SharedArrayBuffer species constructor must return a SharedArrayBuffer");
      }
      auto outBuf = outValue.getGC<ArrayBuffer>();
      if (!isSharedArrayBufferLike(outBuf)) {
        throw std::runtime_error("TypeError: SharedArrayBuffer species constructor must return a SharedArrayBuffer");
      }
      if (outBuf.get() == thisBuf.get()) {
        throw std::runtime_error("TypeError: SharedArrayBuffer species constructor returned the same buffer");
      }
      if (outBuf->byteLength < newLen) {
        throw std::runtime_error("TypeError: SharedArrayBuffer species constructor returned a too-small buffer");
      }
      if (outBuf->data.size() < outBuf->byteLength) {
        outBuf->data.resize(outBuf->byteLength, 0);
      }
      if (newLen > 0) {
        std::copy_n(thisBuf->data.begin() + static_cast<std::ptrdiff_t>(start),
                    static_cast<std::ptrdiff_t>(newLen),
                    outBuf->data.begin());
      }
      if (prototype) {
        outBuf->properties["__proto__"] = Value(prototype);
      }
      return outValue;
    });
  }
  env->define("SharedArrayBuffer", Value(sharedArrayBufferConstructor));

  {
    auto arrayBufferPrototype = arrayBufferConstructor->properties["prototype"].getGC<Object>();
    auto isSharedArrayBufferLike = [](const GCPtr<ArrayBuffer>& buffer) {
      if (!buffer) return false;
      auto markerIt = buffer->properties.find("__shared_array_buffer__");
      return markerIt != buffer->properties.end() &&
             markerIt->second.isBool() &&
             markerIt->second.toBool();
    };
    auto installArrayBufferGetter =
        [&](const std::string& name,
            std::function<Value(const std::vector<Value>&)> nativeFunc) {
          auto getter = GarbageCollector::makeGC<Function>();
          getter->isNative = true;
          getter->isConstructor = false;
          getter->properties["name"] = Value(std::string("get ") + name);
          getter->properties["length"] = Value(0.0);
          getter->properties["__non_writable_name"] = Value(true);
          getter->properties["__non_enum_name"] = Value(true);
          getter->properties["__non_writable_length"] = Value(true);
          getter->properties["__non_enum_length"] = Value(true);
          getter->properties["__uses_this_arg__"] = Value(true);
          getter->properties["__throw_on_new__"] = Value(true);
          getter->nativeFunc = std::move(nativeFunc);
          arrayBufferPrototype->properties["__get_" + name] = Value(getter);
          arrayBufferPrototype->properties["__non_enum_" + name] = Value(true);
        };
    auto installArrayBufferMethod =
        [&](const std::string& name,
            int length,
            std::function<Value(const std::vector<Value>&)> nativeFunc) {
          auto fn = GarbageCollector::makeGC<Function>();
          fn->isNative = true;
          fn->isConstructor = false;
          fn->properties["name"] = Value(name);
          fn->properties["length"] = Value(static_cast<double>(length));
          fn->properties["__non_writable_name"] = Value(true);
          fn->properties["__non_enum_name"] = Value(true);
          fn->properties["__non_writable_length"] = Value(true);
          fn->properties["__non_enum_length"] = Value(true);
          fn->properties["__uses_this_arg__"] = Value(true);
          fn->properties["__throw_on_new__"] = Value(true);
          fn->nativeFunc = std::move(nativeFunc);
          arrayBufferPrototype->properties[name] = Value(fn);
          arrayBufferPrototype->properties["__non_enum_" + name] = Value(true);
        };
    auto toIndex = [toPrimitive](const Value& input) -> size_t {
      Value numeric = isObjectLikeValue(input) ? toPrimitive(input, false) : input;
      if (numeric.isBigInt() || numeric.isSymbol()) {
        throw std::runtime_error("TypeError: Cannot convert value to index");
      }
      double number = numeric.toNumber();
      if (std::isnan(number) || number == 0.0) {
        return 0;
      }
      if (std::isinf(number)) {
        throw std::runtime_error("RangeError: Invalid ArrayBuffer length");
      }
      double integer = std::trunc(number);
      if (integer < 0.0 || integer > 9007199254740991.0) {
        throw std::runtime_error("RangeError: Invalid ArrayBuffer length");
      }
      return static_cast<size_t>(integer);
    };
    auto copyAndDetach = [arrayBufferPrototype, arrayBufferConstructor, isSharedArrayBufferLike, toIndex](
                            const std::vector<Value>& args,
                            bool preserveResizability) -> Value {
      if (args.empty() || !args[0].isArrayBuffer()) {
        throw std::runtime_error("TypeError: ArrayBuffer copy-and-detach called on non-ArrayBuffer");
      }
      auto source = args[0].getGC<ArrayBuffer>();
      if (isSharedArrayBufferLike(source)) {
        throw std::runtime_error("TypeError: ArrayBuffer copy-and-detach called on SharedArrayBuffer");
      }

      size_t newByteLength = source->byteLength;
      if (args.size() > 1 && !args[1].isUndefined()) {
        newByteLength = toIndex(args[1]);
      }

      if (source->detached) {
        throw std::runtime_error("TypeError: ArrayBuffer copy-and-detach called on detached ArrayBuffer");
      }
      if (source->immutable) {
        throw std::runtime_error("TypeError: ArrayBuffer copy-and-detach called on immutable ArrayBuffer");
      }

      GCPtr<ArrayBuffer> dest;
      if (preserveResizability && source->resizable) {
        if (newByteLength > source->maxByteLength) {
          throw std::runtime_error("RangeError: Invalid ArrayBuffer length");
        }
        dest = GarbageCollector::makeGC<ArrayBuffer>(newByteLength, source->maxByteLength);
        dest->resizable = true;
      } else {
        dest = GarbageCollector::makeGC<ArrayBuffer>(newByteLength);
        dest->resizable = false;
        dest->maxByteLength = newByteLength;
      }
      GarbageCollector::instance().reportAllocation(newByteLength);
      if (arrayBufferConstructor) {
        dest->properties["__constructor__"] = Value(arrayBufferConstructor);
      }
      if (arrayBufferPrototype) {
        dest->properties["__proto__"] = Value(arrayBufferPrototype);
      }

      size_t copyLength = std::min(newByteLength, source->byteLength);
      if (copyLength > 0) {
        std::copy_n(source->data.begin(),
                    static_cast<std::ptrdiff_t>(copyLength),
                    dest->data.begin());
      }

      source->detach();
      return Value(dest);
    };

    installArrayBufferGetter("byteLength", [isSharedArrayBufferLike](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isArrayBuffer()) {
        throw std::runtime_error("TypeError: get ArrayBuffer.prototype.byteLength called on incompatible receiver");
      }
      auto buffer = args[0].getGC<ArrayBuffer>();
      if (isSharedArrayBufferLike(buffer)) {
        throw std::runtime_error("TypeError: get ArrayBuffer.prototype.byteLength called on SharedArrayBuffer");
      }
      return Value(static_cast<double>(buffer->detached ? 0 : buffer->byteLength));
    });

    installArrayBufferGetter("maxByteLength", [isSharedArrayBufferLike](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isArrayBuffer()) {
        throw std::runtime_error("TypeError: get ArrayBuffer.prototype.maxByteLength called on incompatible receiver");
      }
      auto buffer = args[0].getGC<ArrayBuffer>();
      if (isSharedArrayBufferLike(buffer)) {
        throw std::runtime_error("TypeError: get ArrayBuffer.prototype.maxByteLength called on SharedArrayBuffer");
      }
      return Value(static_cast<double>(buffer->detached ? 0 : buffer->maxByteLength));
    });

    installArrayBufferGetter("resizable", [isSharedArrayBufferLike](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isArrayBuffer()) {
        throw std::runtime_error("TypeError: get ArrayBuffer.prototype.resizable called on incompatible receiver");
      }
      auto buffer = args[0].getGC<ArrayBuffer>();
      if (isSharedArrayBufferLike(buffer)) {
        throw std::runtime_error("TypeError: get ArrayBuffer.prototype.resizable called on SharedArrayBuffer");
      }
      return Value(buffer->resizable);
    });

    installArrayBufferGetter("detached", [isSharedArrayBufferLike](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isArrayBuffer()) {
        throw std::runtime_error("TypeError: get ArrayBuffer.prototype.detached called on incompatible receiver");
      }
      auto buffer = args[0].getGC<ArrayBuffer>();
      if (isSharedArrayBufferLike(buffer)) {
        throw std::runtime_error("TypeError: get ArrayBuffer.prototype.detached called on SharedArrayBuffer");
      }
      return Value(buffer->detached);
    });

    installArrayBufferMethod("resize", 1, [toPrimitive](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isArrayBuffer()) {
        throw std::runtime_error("TypeError: ArrayBuffer.prototype.resize called on non-ArrayBuffer");
      }
      auto buffer = args[0].getGC<ArrayBuffer>();
      if (buffer->immutable || !buffer->resizable) {
        throw std::runtime_error("TypeError: ArrayBuffer.prototype.resize called on incompatible receiver");
      }

      auto toIntegerOrInfinity = [toPrimitive](const Value& input) -> double {
        Value numeric = isObjectLikeValue(input) ? toPrimitive(input, false) : input;
        double number = numeric.toNumber();
        if (std::isnan(number) || number == 0.0) {
          return 0.0;
        }
        if (std::isinf(number)) {
          return number;
        }
        return std::trunc(number);
      };

      double requestedLength = 0.0;
      if (args.size() > 1) {
        requestedLength = toIntegerOrInfinity(args[1]);
      }

      if (buffer->detached) {
        throw std::runtime_error("TypeError: ArrayBuffer.prototype.resize called on incompatible receiver");
      }

      if (std::isinf(requestedLength) || requestedLength < 0) {
        throw std::runtime_error("RangeError: Invalid ArrayBuffer resize length");
      }
      size_t newByteLength = static_cast<size_t>(requestedLength);
      if (!buffer->resize(newByteLength)) {
        throw std::runtime_error("RangeError: Invalid ArrayBuffer resize length");
      }
      return Value(Undefined{});
    });

    installArrayBufferMethod("transferToImmutable", 0, [arrayBufferPrototype](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isArrayBuffer()) {
        throw std::runtime_error("TypeError: ArrayBuffer.prototype.transferToImmutable called on non-ArrayBuffer");
      }
      auto buffer = args[0].getGC<ArrayBuffer>();
      if (buffer->detached) {
        throw std::runtime_error("TypeError: ArrayBuffer.prototype.transferToImmutable called on detached ArrayBuffer");
      }

      auto outBuf = GarbageCollector::makeGC<ArrayBuffer>(buffer->data);
      outBuf->immutable = true;
      outBuf->maxByteLength = outBuf->byteLength;
      outBuf->resizable = false;
      if (auto ctorIt = buffer->properties.find("__constructor__");
          ctorIt != buffer->properties.end()) {
        outBuf->properties["__constructor__"] = ctorIt->second;
      }
      if (arrayBufferPrototype) {
        outBuf->properties["__proto__"] = Value(arrayBufferPrototype);
      }
      buffer->detach();
      return Value(outBuf);
    });

    installArrayBufferMethod("transfer", 0, [copyAndDetach](const std::vector<Value>& args) -> Value {
      return copyAndDetach(args, true);
    });

    installArrayBufferMethod("transferToFixedLength", 0, [copyAndDetach](const std::vector<Value>& args) -> Value {
      return copyAndDetach(args, false);
    });

    installArrayBufferMethod("slice", 2, [env, arrayBufferPrototype, arrayBufferConstructor, isSharedArrayBufferLike, toPrimitive](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isArrayBuffer()) {
        throw std::runtime_error("TypeError: ArrayBuffer.prototype.slice called on non-ArrayBuffer");
      }
      auto thisBuf = args[0].getGC<ArrayBuffer>();
      if (isSharedArrayBufferLike(thisBuf)) {
        throw std::runtime_error("TypeError: ArrayBuffer.prototype.slice called on SharedArrayBuffer");
      }
      if (thisBuf->detached) {
        throw std::runtime_error("TypeError: ArrayBuffer.prototype.slice called on detached ArrayBuffer");
      }

      auto* interpreter = getGlobalInterpreter();
      auto toIntegerOrInfinity = [toPrimitive](const Value& input) -> double {
        Value numeric = isObjectLikeValue(input) ? toPrimitive(input, false) : input;
        double number = numeric.toNumber();
        if (std::isnan(number) || number == 0.0) {
          return 0.0;
        }
        if (std::isinf(number)) {
          return number;
        }
        return std::trunc(number);
      };
      auto clampIndex = [](double relative, size_t len) -> size_t {
        double lenNumber = static_cast<double>(len);
        double clamped = relative < 0 ? std::max(lenNumber + relative, 0.0)
                                      : std::min(relative, lenNumber);
        return static_cast<size_t>(clamped);
      };
      auto isConstructorValue = [](const Value& value) -> bool {
        if (value.isFunction()) {
          return value.getGC<Function>()->isConstructor;
        }
        if (value.isClass()) {
          return true;
        }
        if (value.isObject()) {
          auto obj = value.getGC<Object>();
          auto callableIt = obj->properties.find("__callable_object__");
          auto ctorIt = obj->properties.find("constructor");
          if (callableIt != obj->properties.end() &&
              callableIt->second.isBool() &&
              callableIt->second.toBool() &&
              ctorIt != obj->properties.end()) {
            return (ctorIt->second.isFunction() && ctorIt->second.getGC<Function>()->isConstructor) ||
                   ctorIt->second.isClass();
          }
        }
        return false;
      };

      size_t len = thisBuf->byteLength;
      double relativeStart = args.size() > 1 ? toIntegerOrInfinity(args[1]) : 0.0;
      double relativeEnd = (args.size() > 2 && !args[2].isUndefined())
                             ? toIntegerOrInfinity(args[2])
                             : static_cast<double>(len);
      size_t start = clampIndex(relativeStart, len);
      size_t finish = clampIndex(relativeEnd, len);
      if (finish < start) {
        finish = start;
      }
      size_t newLen = finish - start;

      Value ctorValue = Value(arrayBufferConstructor);
      auto [hasCtor, ctorProp] = getPropertyLike(args[0], "constructor", args[0]);
      if (hasCtor && !ctorProp.isUndefined()) {
        if (!isObjectLikeValue(ctorProp)) {
          throw std::runtime_error("TypeError: ArrayBuffer constructor property is not an object");
        }
        auto [hasSpecies, speciesProp] = getPropertyLike(ctorProp, WellKnownSymbols::speciesKey(), ctorProp);
        if (hasSpecies && !speciesProp.isUndefined() && !speciesProp.isNull()) {
          if (!isConstructorValue(speciesProp)) {
            throw std::runtime_error("TypeError: ArrayBuffer species is not a constructor");
          }
          ctorValue = speciesProp;
        }
      }

      if (!interpreter) {
        throw std::runtime_error("TypeError: Interpreter unavailable for ArrayBuffer.prototype.slice");
      }
      Value outValue = interpreter->constructFromNative(ctorValue, {Value(static_cast<double>(newLen))});
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw std::runtime_error(err.toString());
      }
      if (!outValue.isArrayBuffer()) {
        throw std::runtime_error("TypeError: ArrayBuffer species constructor must return an ArrayBuffer");
      }
      auto outBuf = outValue.getGC<ArrayBuffer>();
      if (outBuf.get() == thisBuf.get()) {
        throw std::runtime_error("TypeError: ArrayBuffer species constructor returned the same buffer");
      }
      if (outBuf->immutable) {
        throw std::runtime_error("TypeError: ArrayBuffer species constructor returned an immutable buffer");
      }
      if (outBuf->byteLength < newLen) {
        throw std::runtime_error("TypeError: ArrayBuffer species constructor returned a too-small buffer");
      }
      if (outBuf->data.size() < outBuf->byteLength) {
        outBuf->data.resize(outBuf->byteLength, 0);
      }
      if (newLen > 0) {
        std::copy_n(thisBuf->data.begin() + static_cast<std::ptrdiff_t>(start),
                    static_cast<std::ptrdiff_t>(newLen),
                    outBuf->data.begin());
      }
      return outValue;
    });

    arrayBufferPrototype->properties[WellKnownSymbols::toStringTagKey()] = Value(std::string("ArrayBuffer"));
    arrayBufferPrototype->properties["__non_writable_" + WellKnownSymbols::toStringTagKey()] = Value(true);
    arrayBufferPrototype->properties["__non_enum_" + WellKnownSymbols::toStringTagKey()] = Value(true);
  }

  auto arrayBufferIsView = GarbageCollector::makeGC<Function>();
  arrayBufferIsView->isNative = true;
  arrayBufferIsView->isConstructor = false;
  arrayBufferIsView->properties["name"] = Value(std::string("isView"));
  arrayBufferIsView->properties["length"] = Value(1.0);
  arrayBufferIsView->properties["__non_writable_name"] = Value(true);
  arrayBufferIsView->properties["__non_enum_name"] = Value(true);
  arrayBufferIsView->properties["__non_writable_length"] = Value(true);
  arrayBufferIsView->properties["__non_enum_length"] = Value(true);
  arrayBufferIsView->properties["__throw_on_new__"] = Value(true);
  arrayBufferIsView->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(false);
    }
    return Value(args[0].isTypedArray() || args[0].isDataView());
  };
  arrayBufferConstructor->properties["isView"] = Value(arrayBufferIsView);
  arrayBufferConstructor->properties["__non_enum_isView"] = Value(true);

  // DataView constructor
  auto dataViewConstructor = GarbageCollector::makeGC<Function>();
  dataViewConstructor->isNative = true;
  dataViewConstructor->isConstructor = true;
  dataViewConstructor->properties["name"] = Value(std::string("DataView"));
  dataViewConstructor->properties["length"] = Value(1.0);
  dataViewConstructor->properties["__non_writable_name"] = Value(true);
  dataViewConstructor->properties["__non_enum_name"] = Value(true);
  dataViewConstructor->properties["__non_writable_length"] = Value(true);
  dataViewConstructor->properties["__non_enum_length"] = Value(true);
  dataViewConstructor->properties["__non_writable_prototype"] = Value(true);
  dataViewConstructor->properties["__non_configurable_prototype"] = Value(true);
  auto dataViewConstructImpl = GarbageCollector::makeGC<Function>();
  dataViewConstructImpl->isNative = true;
  dataViewConstructImpl->isConstructor = true;
  dataViewConstructImpl->nativeFunc = [toPrimitive](const std::vector<Value>& args) -> Value {
    auto toIndex = [toPrimitive](const Value& input) -> size_t {
      if (input.isUndefined()) {
        return 0;
      }
      Value numeric = isObjectLikeValue(input) ? toPrimitive(input, false) : input;
      if (numeric.isBigInt() || numeric.isSymbol()) {
        throw std::runtime_error("TypeError: Cannot convert value to index");
      }
      double number = numeric.toNumber();
      if (std::isnan(number) || number == 0.0) {
        return 0;
      }
      if (std::isinf(number)) {
        throw std::runtime_error("RangeError: Invalid DataView offset");
      }
      double integer = std::trunc(number);
      if (integer < 0.0 || integer > 9007199254740991.0) {
        throw std::runtime_error("RangeError: Invalid DataView offset");
      }
      return static_cast<size_t>(integer);
    };

    if (args.empty() || !args[0].isArrayBuffer()) {
      throw std::runtime_error("TypeError: DataView requires an ArrayBuffer");
    }

    auto buffer = args[0].getGC<ArrayBuffer>();
    size_t byteOffset = args.size() > 1 ? toIndex(args[1]) : 0;
    if (buffer->detached) {
      throw std::runtime_error("TypeError: DataView requires a non-detached ArrayBuffer");
    }
    if (byteOffset > buffer->byteLength) {
      throw std::runtime_error("RangeError: Invalid DataView offset");
    }

    bool hasByteLength = args.size() > 2 && !args[2].isUndefined();
    size_t byteLength = hasByteLength ? toIndex(args[2]) : (buffer->byteLength - byteOffset);
    if (byteOffset > buffer->byteLength ||
        byteLength > buffer->byteLength - byteOffset) {
      throw std::runtime_error("RangeError: Invalid DataView length");
    }

    auto dataView = GarbageCollector::makeGC<DataView>(buffer, byteOffset, byteLength);
    dataView->lengthTracking = !hasByteLength;
    GarbageCollector::instance().reportAllocation(sizeof(DataView));
    return Value(dataView);
  };
  dataViewConstructor->nativeFunc = [](const std::vector<Value>&) -> Value {
    throw std::runtime_error("TypeError: Constructor DataView requires 'new'");
  };
  dataViewConstructor->properties["__native_construct__"] = Value(dataViewConstructImpl);
  {
    auto prototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    dataViewConstructor->properties["prototype"] = Value(prototype);
    prototype->properties["constructor"] = Value(dataViewConstructor);
    prototype->properties["__non_enum_constructor"] = Value(true);
    prototype->properties[WellKnownSymbols::toStringTagKey()] = Value(std::string("DataView"));
    prototype->properties["__non_writable_" + WellKnownSymbols::toStringTagKey()] = Value(true);
    prototype->properties["__non_enum_" + WellKnownSymbols::toStringTagKey()] = Value(true);

    auto toIndex = [toPrimitive](const Value& input) -> size_t {
      if (input.isUndefined()) {
        return 0;
      }
      Value numeric = isObjectLikeValue(input) ? toPrimitive(input, false) : input;
      if (numeric.isBigInt() || numeric.isSymbol()) {
        throw std::runtime_error("TypeError: Cannot convert value to index");
      }
      double number = numeric.toNumber();
      if (std::isnan(number) || number == 0.0) {
        return 0;
      }
      if (std::isinf(number)) {
        throw std::runtime_error("RangeError: Invalid DataView offset");
      }
      double integer = std::trunc(number);
      if (integer < 0.0 || integer > 9007199254740991.0) {
        throw std::runtime_error("RangeError: Invalid DataView offset");
      }
      return static_cast<size_t>(integer);
    };
    auto toNumberValue = [toPrimitive](const Value& input) -> double {
      Value numeric = isObjectLikeValue(input) ? toPrimitive(input, false) : input;
      if (numeric.isBigInt() || numeric.isSymbol()) {
        throw std::runtime_error("TypeError: Cannot convert value to number");
      }
      return numeric.toNumber();
    };
    auto toBigIntValue = [toPrimitive](const Value& input) -> bigint::BigIntValue {
      Value primitive = isObjectLikeValue(input) ? toPrimitive(input, false) : input;
      if (primitive.isBigInt()) {
        return primitive.toBigInt();
      }
      if (primitive.isBool()) {
        return primitive.toBool() ? bigint::BigIntValue(1) : bigint::BigIntValue(0);
      }
      if (primitive.isString()) {
        bigint::BigIntValue parsed = 0;
        if (!bigint::parseBigIntString(primitive.toString(), parsed)) {
          throw std::runtime_error("SyntaxError: Cannot convert value to BigInt");
        }
        return parsed;
      }
      throw std::runtime_error("TypeError: Cannot convert value to BigInt");
    };
    auto requireMutableBuffer = [](const GCPtr<DataView>& view, const char* methodName) {
      if (!view->buffer || view->buffer->immutable) {
        throw std::runtime_error(std::string("TypeError: ") + methodName + " called on immutable buffer");
      }
    };
    auto toInt8Value = [](double value) -> int8_t {
      if (!std::isfinite(value) || value == 0.0) {
        return 0;
      }
      double intPart = std::trunc(value);
      double wrapped = std::fmod(intPart, 256.0);
      if (wrapped < 0) wrapped += 256.0;
      if (wrapped >= 128.0) wrapped -= 256.0;
      return static_cast<int8_t>(wrapped);
    };
    auto toUint8Value = [](double value) -> uint8_t {
      if (!std::isfinite(value) || value == 0.0) {
        return 0;
      }
      double intPart = std::trunc(value);
      double wrapped = std::fmod(intPart, 256.0);
      if (wrapped < 0) wrapped += 256.0;
      return static_cast<uint8_t>(wrapped);
    };
    auto toInt16Value = [](double value) -> int16_t {
      if (!std::isfinite(value) || value == 0.0) {
        return 0;
      }
      double intPart = std::trunc(value);
      double wrapped = std::fmod(intPart, 65536.0);
      if (wrapped < 0) wrapped += 65536.0;
      if (wrapped >= 32768.0) wrapped -= 65536.0;
      return static_cast<int16_t>(wrapped);
    };
    auto toUint16Value = [](double value) -> uint16_t {
      if (!std::isfinite(value) || value == 0.0) {
        return 0;
      }
      double intPart = std::trunc(value);
      double wrapped = std::fmod(intPart, 65536.0);
      if (wrapped < 0) wrapped += 65536.0;
      return static_cast<uint16_t>(wrapped);
    };
    auto toInt32Value = [](double value) -> int32_t {
      if (!std::isfinite(value) || value == 0.0) {
        return 0;
      }
      double intPart = std::trunc(value);
      constexpr double kTwo32 = 4294967296.0;
      double wrapped = std::fmod(intPart, kTwo32);
      if (wrapped < 0) wrapped += kTwo32;
      if (wrapped >= 2147483648.0) wrapped -= kTwo32;
      return static_cast<int32_t>(wrapped);
    };
    auto toUint32Value = [](double value) -> uint32_t {
      if (!std::isfinite(value) || value == 0.0) {
        return 0;
      }
      double intPart = std::trunc(value);
      constexpr double kTwo32 = 4294967296.0;
      double wrapped = std::fmod(intPart, kTwo32);
      if (wrapped < 0) wrapped += kTwo32;
      return static_cast<uint32_t>(wrapped);
    };

    auto installGetter = [&](const std::string& name, auto impl) {
      auto getter = GarbageCollector::makeGC<Function>();
      getter->isNative = true;
      getter->isConstructor = false;
      getter->properties["name"] = Value(std::string("get " + name));
      getter->properties["length"] = Value(0.0);
      getter->properties["__non_writable_name"] = Value(true);
      getter->properties["__non_enum_name"] = Value(true);
      getter->properties["__non_writable_length"] = Value(true);
      getter->properties["__non_enum_length"] = Value(true);
      getter->properties["__uses_this_arg__"] = Value(true);
      getter->properties["__throw_on_new__"] = Value(true);
      getter->nativeFunc = impl;
      prototype->properties["__get_" + name] = Value(getter);
      prototype->properties["__non_enum_" + name] = Value(true);
    };

    auto installMethod = [&](const std::string& name, double length, auto impl) {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->isConstructor = false;
      fn->properties["name"] = Value(name);
      fn->properties["length"] = Value(length);
      fn->properties["__non_writable_name"] = Value(true);
      fn->properties["__non_enum_name"] = Value(true);
      fn->properties["__non_writable_length"] = Value(true);
      fn->properties["__non_enum_length"] = Value(true);
      fn->properties["__throw_on_new__"] = Value(true);
      fn->properties["__uses_this_arg__"] = Value(true);
      fn->nativeFunc = impl;
      prototype->properties[name] = Value(fn);
      prototype->properties["__non_enum_" + name] = Value(true);
    };

    installGetter("buffer", [](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isDataView()) {
        throw std::runtime_error("TypeError: get DataView.prototype.buffer called on incompatible receiver");
      }
      auto view = args[0].getGC<DataView>();
      if (!view->buffer) {
        throw std::runtime_error("TypeError: get DataView.prototype.buffer called on incompatible receiver");
      }
      return Value(view->buffer);
    });

    installGetter("byteOffset", [](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isDataView()) {
        throw std::runtime_error("TypeError: get DataView.prototype.byteOffset called on incompatible receiver");
      }
      auto view = args[0].getGC<DataView>();
      if (!view->buffer || view->buffer->detached) {
        throw std::runtime_error("TypeError: DataView has a detached buffer");
      }
      return Value(static_cast<double>(view->byteOffset));
    });

    installGetter("byteLength", [](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isDataView()) {
        throw std::runtime_error("TypeError: get DataView.prototype.byteLength called on incompatible receiver");
      }
      auto view = args[0].getGC<DataView>();
      if (!view->buffer || view->buffer->detached) {
        throw std::runtime_error("TypeError: DataView has a detached buffer");
      }
      return Value(static_cast<double>(view->currentByteLength()));
    });

    installMethod("getInt8", 1, [toIndex](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isDataView()) {
        throw std::runtime_error("TypeError: DataView.prototype.getInt8 called on incompatible receiver");
      }
      auto view = args[0].getGC<DataView>();
      size_t byteOffset = args.size() > 1 ? toIndex(args[1]) : 0;
      return Value(static_cast<double>(view->getInt8(byteOffset)));
    });

    installMethod("getUint8", 1, [toIndex](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isDataView()) {
        throw std::runtime_error("TypeError: DataView.prototype.getUint8 called on incompatible receiver");
      }
      auto view = args[0].getGC<DataView>();
      size_t byteOffset = args.size() > 1 ? toIndex(args[1]) : 0;
      return Value(static_cast<double>(view->getUint8(byteOffset)));
    });

    installMethod("getInt16", 1, [toIndex](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isDataView()) {
        throw std::runtime_error("TypeError: DataView.prototype.getInt16 called on incompatible receiver");
      }
      auto view = args[0].getGC<DataView>();
      size_t byteOffset = args.size() > 1 ? toIndex(args[1]) : 0;
      bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
      return Value(static_cast<double>(view->getInt16(byteOffset, littleEndian)));
    });

    installMethod("getUint16", 1, [toIndex](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isDataView()) {
        throw std::runtime_error("TypeError: DataView.prototype.getUint16 called on incompatible receiver");
      }
      auto view = args[0].getGC<DataView>();
      size_t byteOffset = args.size() > 1 ? toIndex(args[1]) : 0;
      bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
      return Value(static_cast<double>(view->getUint16(byteOffset, littleEndian)));
    });

    installMethod("getInt32", 1, [toIndex](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isDataView()) {
        throw std::runtime_error("TypeError: DataView.prototype.getInt32 called on incompatible receiver");
      }
      auto view = args[0].getGC<DataView>();
      size_t byteOffset = args.size() > 1 ? toIndex(args[1]) : 0;
      bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
      return Value(static_cast<double>(view->getInt32(byteOffset, littleEndian)));
    });

    installMethod("getUint32", 1, [toIndex](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isDataView()) {
        throw std::runtime_error("TypeError: DataView.prototype.getUint32 called on incompatible receiver");
      }
      auto view = args[0].getGC<DataView>();
      size_t byteOffset = args.size() > 1 ? toIndex(args[1]) : 0;
      bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
      return Value(static_cast<double>(view->getUint32(byteOffset, littleEndian)));
    });

    installMethod("getFloat16", 1, [toIndex](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isDataView()) {
        throw std::runtime_error("TypeError: DataView.prototype.getFloat16 called on incompatible receiver");
      }
      auto view = args[0].getGC<DataView>();
      size_t byteOffset = args.size() > 1 ? toIndex(args[1]) : 0;
      bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
      return Value(static_cast<double>(view->getFloat16(byteOffset, littleEndian)));
    });

    installMethod("getFloat32", 1, [toIndex](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isDataView()) {
        throw std::runtime_error("TypeError: DataView.prototype.getFloat32 called on incompatible receiver");
      }
      auto view = args[0].getGC<DataView>();
      size_t byteOffset = args.size() > 1 ? toIndex(args[1]) : 0;
      bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
      return Value(static_cast<double>(view->getFloat32(byteOffset, littleEndian)));
    });

    installMethod("getFloat64", 1, [toIndex](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isDataView()) {
        throw std::runtime_error("TypeError: DataView.prototype.getFloat64 called on incompatible receiver");
      }
      auto view = args[0].getGC<DataView>();
      size_t byteOffset = args.size() > 1 ? toIndex(args[1]) : 0;
      bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
      return Value(view->getFloat64(byteOffset, littleEndian));
    });

    installMethod("getBigInt64", 1, [toIndex](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isDataView()) {
        throw std::runtime_error("TypeError: DataView.prototype.getBigInt64 called on incompatible receiver");
      }
      auto view = args[0].getGC<DataView>();
      size_t byteOffset = args.size() > 1 ? toIndex(args[1]) : 0;
      bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
      return Value(BigInt(view->getBigInt64(byteOffset, littleEndian)));
    });

    installMethod("getBigUint64", 1, [toIndex](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isDataView()) {
        throw std::runtime_error("TypeError: DataView.prototype.getBigUint64 called on incompatible receiver");
      }
      auto view = args[0].getGC<DataView>();
      size_t byteOffset = args.size() > 1 ? toIndex(args[1]) : 0;
      bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
      return Value(BigInt(bigint::BigIntValue(view->getBigUint64(byteOffset, littleEndian))));
    });

    installMethod("setInt8", 2, [requireMutableBuffer, toIndex, toNumberValue, toInt8Value](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isDataView()) {
        throw std::runtime_error("TypeError: DataView.prototype.setInt8 called on incompatible receiver");
      }
      auto view = args[0].getGC<DataView>();
      requireMutableBuffer(view, "DataView.prototype.setInt8");
      size_t byteOffset = args.size() > 1 ? toIndex(args[1]) : 0;
      double numberValue = args.size() > 2 ? toNumberValue(args[2]) : std::numeric_limits<double>::quiet_NaN();
      view->setInt8(byteOffset, toInt8Value(numberValue));
      return Value(Undefined{});
    });

    installMethod("setUint8", 2, [requireMutableBuffer, toIndex, toNumberValue, toUint8Value](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isDataView()) {
        throw std::runtime_error("TypeError: DataView.prototype.setUint8 called on incompatible receiver");
      }
      auto view = args[0].getGC<DataView>();
      requireMutableBuffer(view, "DataView.prototype.setUint8");
      size_t byteOffset = args.size() > 1 ? toIndex(args[1]) : 0;
      double numberValue = args.size() > 2 ? toNumberValue(args[2]) : std::numeric_limits<double>::quiet_NaN();
      view->setUint8(byteOffset, toUint8Value(numberValue));
      return Value(Undefined{});
    });

    installMethod("setInt16", 2, [requireMutableBuffer, toIndex, toNumberValue, toInt16Value](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isDataView()) {
        throw std::runtime_error("TypeError: DataView.prototype.setInt16 called on incompatible receiver");
      }
      auto view = args[0].getGC<DataView>();
      requireMutableBuffer(view, "DataView.prototype.setInt16");
      size_t byteOffset = args.size() > 1 ? toIndex(args[1]) : 0;
      double numberValue = args.size() > 2 ? toNumberValue(args[2]) : std::numeric_limits<double>::quiet_NaN();
      bool littleEndian = args.size() > 3 ? args[3].toBool() : false;
      view->setInt16(byteOffset, toInt16Value(numberValue), littleEndian);
      return Value(Undefined{});
    });

    installMethod("setUint16", 2, [requireMutableBuffer, toIndex, toNumberValue, toUint16Value](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isDataView()) {
        throw std::runtime_error("TypeError: DataView.prototype.setUint16 called on incompatible receiver");
      }
      auto view = args[0].getGC<DataView>();
      requireMutableBuffer(view, "DataView.prototype.setUint16");
      size_t byteOffset = args.size() > 1 ? toIndex(args[1]) : 0;
      double numberValue = args.size() > 2 ? toNumberValue(args[2]) : std::numeric_limits<double>::quiet_NaN();
      bool littleEndian = args.size() > 3 ? args[3].toBool() : false;
      view->setUint16(byteOffset, toUint16Value(numberValue), littleEndian);
      return Value(Undefined{});
    });

    installMethod("setInt32", 2, [requireMutableBuffer, toIndex, toNumberValue, toInt32Value](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isDataView()) {
        throw std::runtime_error("TypeError: DataView.prototype.setInt32 called on incompatible receiver");
      }
      auto view = args[0].getGC<DataView>();
      requireMutableBuffer(view, "DataView.prototype.setInt32");
      size_t byteOffset = args.size() > 1 ? toIndex(args[1]) : 0;
      double numberValue = args.size() > 2 ? toNumberValue(args[2]) : std::numeric_limits<double>::quiet_NaN();
      bool littleEndian = args.size() > 3 ? args[3].toBool() : false;
      view->setInt32(byteOffset, toInt32Value(numberValue), littleEndian);
      return Value(Undefined{});
    });

    installMethod("setUint32", 2, [requireMutableBuffer, toIndex, toNumberValue, toUint32Value](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isDataView()) {
        throw std::runtime_error("TypeError: DataView.prototype.setUint32 called on incompatible receiver");
      }
      auto view = args[0].getGC<DataView>();
      requireMutableBuffer(view, "DataView.prototype.setUint32");
      size_t byteOffset = args.size() > 1 ? toIndex(args[1]) : 0;
      double numberValue = args.size() > 2 ? toNumberValue(args[2]) : std::numeric_limits<double>::quiet_NaN();
      bool littleEndian = args.size() > 3 ? args[3].toBool() : false;
      view->setUint32(byteOffset, toUint32Value(numberValue), littleEndian);
      return Value(Undefined{});
    });

    installMethod("setFloat16", 2, [requireMutableBuffer, toIndex, toNumberValue](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isDataView()) {
        throw std::runtime_error("TypeError: DataView.prototype.setFloat16 called on incompatible receiver");
      }
      auto view = args[0].getGC<DataView>();
      requireMutableBuffer(view, "DataView.prototype.setFloat16");
      size_t byteOffset = args.size() > 1 ? toIndex(args[1]) : 0;
      double numberValue = args.size() > 2 ? toNumberValue(args[2]) : std::numeric_limits<double>::quiet_NaN();
      bool littleEndian = args.size() > 3 ? args[3].toBool() : false;
      view->setFloat16(byteOffset, numberValue, littleEndian);
      return Value(Undefined{});
    });

    installMethod("setFloat32", 2, [requireMutableBuffer, toIndex, toNumberValue](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isDataView()) {
        throw std::runtime_error("TypeError: DataView.prototype.setFloat32 called on incompatible receiver");
      }
      auto view = args[0].getGC<DataView>();
      requireMutableBuffer(view, "DataView.prototype.setFloat32");
      size_t byteOffset = args.size() > 1 ? toIndex(args[1]) : 0;
      double numberValue = args.size() > 2 ? toNumberValue(args[2]) : std::numeric_limits<double>::quiet_NaN();
      bool littleEndian = args.size() > 3 ? args[3].toBool() : false;
      view->setFloat32(byteOffset, static_cast<float>(numberValue), littleEndian);
      return Value(Undefined{});
    });

    installMethod("setFloat64", 2, [requireMutableBuffer, toIndex, toNumberValue](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isDataView()) {
        throw std::runtime_error("TypeError: DataView.prototype.setFloat64 called on incompatible receiver");
      }
      auto view = args[0].getGC<DataView>();
      requireMutableBuffer(view, "DataView.prototype.setFloat64");
      size_t byteOffset = args.size() > 1 ? toIndex(args[1]) : 0;
      double numberValue = args.size() > 2 ? toNumberValue(args[2]) : std::numeric_limits<double>::quiet_NaN();
      bool littleEndian = args.size() > 3 ? args[3].toBool() : false;
      view->setFloat64(byteOffset, numberValue, littleEndian);
      return Value(Undefined{});
    });

    installMethod("setBigInt64", 2, [requireMutableBuffer, toIndex, toBigIntValue](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isDataView()) {
        throw std::runtime_error("TypeError: DataView.prototype.setBigInt64 called on incompatible receiver");
      }
      auto view = args[0].getGC<DataView>();
      requireMutableBuffer(view, "DataView.prototype.setBigInt64");
      size_t byteOffset = args.size() > 1 ? toIndex(args[1]) : 0;
      bigint::BigIntValue numberValue = args.size() > 2 ? toBigIntValue(args[2]) : throw std::runtime_error("TypeError: Cannot convert value to BigInt");
      bool littleEndian = args.size() > 3 ? args[3].toBool() : false;
      view->setBigInt64(byteOffset, bigint::toInt64Trunc(numberValue), littleEndian);
      return Value(Undefined{});
    });

    installMethod("setBigUint64", 2, [requireMutableBuffer, toIndex, toBigIntValue](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isDataView()) {
        throw std::runtime_error("TypeError: DataView.prototype.setBigUint64 called on incompatible receiver");
      }
      auto view = args[0].getGC<DataView>();
      requireMutableBuffer(view, "DataView.prototype.setBigUint64");
      size_t byteOffset = args.size() > 1 ? toIndex(args[1]) : 0;
      bigint::BigIntValue numberValue = args.size() > 2 ? toBigIntValue(args[2]) : throw std::runtime_error("TypeError: Cannot convert value to BigInt");
      bool littleEndian = args.size() > 3 ? args[3].toBool() : false;
      view->setBigUint64(byteOffset, bigint::toUint64Trunc(numberValue), littleEndian);
      return Value(Undefined{});
    });
  }
  env->define("DataView", Value(dataViewConstructor));

  auto cryptoObj = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  auto sha256Fn = GarbageCollector::makeGC<Function>();
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

  auto hmacFn = GarbageCollector::makeGC<Function>();
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

  auto toHexFn = GarbageCollector::makeGC<Function>();
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
  auto randomUUIDFn = GarbageCollector::makeGC<Function>();
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
  auto getRandomValuesFn = GarbageCollector::makeGC<Function>();
  getRandomValuesFn->isNative = true;
  getRandomValuesFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isTypedArray()) {
      return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "getRandomValues requires a TypedArray"));
    }

    auto typedArray = args[0].getGC<TypedArray>();
    std::random_device rd;
    std::mt19937 gen(rd());
    auto& bytes = typedArray->storage();

    // Fill buffer with random bytes
    std::uniform_int_distribution<uint32_t> dis(0, 255);
    for (size_t i = 0; i < typedArray->currentByteLength(); ++i) {
      bytes[typedArray->byteOffset + i] = static_cast<uint8_t>(dis(gen));
    }

    return args[0];  // Return the same array
  };
  cryptoObj->properties["getRandomValues"] = Value(getRandomValuesFn);

  env->define("crypto", Value(cryptoObj));

  auto fetchFn = GarbageCollector::makeGC<Function>();
  fetchFn->isNative = true;
  fetchFn->nativeFunc = [env](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(Undefined{});
    }

    std::string url = args[0].toString();

    auto promise = GarbageCollector::makeGC<Promise>();
    if (auto intrinsicPromise = env->get("__intrinsic_Promise__");
        intrinsicPromise && intrinsicPromise->isFunction()) {
      auto intrinsicPromiseFn = intrinsicPromise->getGC<Function>();
      promise->properties["__constructor__"] = *intrinsicPromise;
      auto promiseProtoIt = intrinsicPromiseFn->properties.find("prototype");
      if (promiseProtoIt != intrinsicPromiseFn->properties.end() && promiseProtoIt->second.isObject()) {
        promise->properties["__proto__"] = promiseProtoIt->second;
      }
    }
    http::HTTPClient client;

    try {
      http::Response httpResp = client.get(url);

      auto respObj = GarbageCollector::makeGC<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      respObj->properties["status"] = Value(static_cast<double>(httpResp.statusCode));
      respObj->properties["statusText"] = Value(httpResp.statusText);
      respObj->properties["ok"] = Value(httpResp.statusCode >= 200 && httpResp.statusCode < 300);

      auto textFn = GarbageCollector::makeGC<Function>();
      textFn->isNative = true;
      std::string bodyText = httpResp.bodyAsString();
      textFn->nativeFunc = [bodyText](const std::vector<Value>&) -> Value {
        return Value(bodyText);
      };
      respObj->properties["text"] = Value(textFn);

      auto headersObj = GarbageCollector::makeGC<Object>();
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
  auto importFn = GarbageCollector::makeGC<Function>();
  importFn->isNative = true;
  importFn->nativeFunc = [arrayToString, env](const std::vector<Value>& args) -> Value {
    auto initializeIntrinsicPromise = [env](const GCPtr<Promise>& p) {
      if (!p) return;
      if (auto intrinsicPromise = env->get("__intrinsic_Promise__");
          intrinsicPromise && intrinsicPromise->isFunction()) {
        auto intrinsicPromiseFn = intrinsicPromise->getGC<Function>();
        p->properties["__constructor__"] = *intrinsicPromise;
        auto promiseProtoIt = intrinsicPromiseFn->properties.find("prototype");
        if (promiseProtoIt != intrinsicPromiseFn->properties.end() && promiseProtoIt->second.isObject()) {
          p->properties["__proto__"] = promiseProtoIt->second;
        }
      }
    };

    if (args.empty()) {
      auto promise = GarbageCollector::makeGC<Promise>();
      initializeIntrinsicPromise(promise);
      auto err = GarbageCollector::makeGC<Error>(ErrorType::TypeError, "import() requires a module specifier");
      promise->reject(Value(err));
      return Value(promise);
    }

    auto promise = GarbageCollector::makeGC<Promise>();
    initializeIntrinsicPromise(promise);
    std::string specifier;
    enum class ImportPhase {
      Normal,
      Source,
      Defer,
    };
    ImportPhase importPhase = ImportPhase::Normal;
    bool hasImportOptions = false;
    Value importOptions = Value(Undefined{});
    if (args.size() > 1 && args[1].isString()) {
      const std::string& maybePhase = std::get<std::string>(args[1].data);
      if (maybePhase == kImportPhaseSourceSentinel) {
        importPhase = ImportPhase::Source;
      } else if (maybePhase == kImportPhaseDeferSentinel) {
        importPhase = ImportPhase::Defer;
      } else {
        hasImportOptions = true;
        importOptions = args[1];
      }
    } else if (args.size() > 1) {
      hasImportOptions = true;
      importOptions = args[1];
    }

    auto rejectWith = [&](ErrorType fallbackType, const std::string& fallbackMessage, const std::optional<Value>& candidate = std::nullopt) {
      if (candidate.has_value()) {
        promise->reject(*candidate);
      } else {
        auto err = GarbageCollector::makeGC<Error>(fallbackType, fallbackMessage);
        promise->reject(Value(err));
      }
    };

    auto isObjectLikeImport = [](const Value& value) -> bool {
      return value.isObject() || value.isArray() || value.isFunction() || value.isRegex() || value.isProxy();
    };

    auto valueFromErrorMessage = [](std::string message) -> Value {
      ErrorType errorType = ErrorType::Error;
      auto consumePrefix = [&](const std::string& prefix, ErrorType type) {
        if (message.rfind(prefix, 0) == 0) {
          errorType = type;
          message = message.substr(prefix.size());
          return true;
        }
        return false;
      };
      if (!consumePrefix("TypeError: ", ErrorType::TypeError) &&
          !consumePrefix("ReferenceError: ", ErrorType::ReferenceError) &&
          !consumePrefix("RangeError: ", ErrorType::RangeError) &&
          !consumePrefix("SyntaxError: ", ErrorType::SyntaxError) &&
          !consumePrefix("URIError: ", ErrorType::URIError) &&
          !consumePrefix("EvalError: ", ErrorType::EvalError) &&
          !consumePrefix("Error: ", ErrorType::Error)) {
        return Value(message);
      }
      return Value(GarbageCollector::makeGC<Error>(errorType, message));
    };

    auto makeError = [](ErrorType type, const std::string& message) -> Value {
      return Value(GarbageCollector::makeGC<Error>(type, message));
    };

    auto callImportCallable = [&](const Value& callee,
                                  const std::vector<Value>& callArgs,
                                  const Value& thisArg,
                                  Value& out,
                                  Value& abrupt) -> bool {
      if (!callee.isFunction()) {
        out = Value(Undefined{});
        return true;
      }

      auto fn = callee.getGC<Function>();
      if (fn->isNative) {
        try {
          auto itUsesThis = fn->properties.find("__uses_this_arg__");
          if (itUsesThis != fn->properties.end() &&
              itUsesThis->second.isBool() &&
              itUsesThis->second.toBool()) {
            std::vector<Value> nativeArgs;
            nativeArgs.reserve(callArgs.size() + 1);
            nativeArgs.push_back(thisArg);
            nativeArgs.insert(nativeArgs.end(), callArgs.begin(), callArgs.end());
            out = fn->nativeFunc(nativeArgs);
          } else {
            out = fn->nativeFunc(callArgs);
          }
          return true;
        } catch (const std::exception& e) {
          abrupt = valueFromErrorMessage(e.what());
          return false;
        } catch (...) {
          abrupt = makeError(ErrorType::Error, "Unknown native error");
          return false;
        }
      }

      Interpreter* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        abrupt = makeError(ErrorType::TypeError, "Interpreter unavailable for callable conversion");
        return false;
      }
      interpreter->clearError();
      out = interpreter->callForHarness(callee, callArgs, thisArg);
      if (interpreter->hasError()) {
        abrupt = interpreter->getError();
        interpreter->clearError();
        return false;
      }
      return true;
    };

    std::function<bool(const Value&, const std::string&, bool&, Value&, Value&)> getImportProperty;
    getImportProperty =
      [&](const Value& receiver, const std::string& key, bool& found, Value& out, Value& abrupt) -> bool {
      if (receiver.isProxy()) {
        auto proxyPtr = receiver.getGC<Proxy>();
        if (proxyPtr->handler && proxyPtr->handler->isObject()) {
          auto handlerObj = std::get<GCPtr<Object>>(proxyPtr->handler->data);
          auto getTrapIt = handlerObj->properties.find("get");
          if (getTrapIt != handlerObj->properties.end() && getTrapIt->second.isFunction()) {
            Value trapOut = Value(Undefined{});
            if (!callImportCallable(getTrapIt->second, {*proxyPtr->target, Value(key), receiver}, Value(Undefined{}), trapOut, abrupt)) {
              return false;
            }
            found = true;
            out = trapOut;
            return true;
          }
        }
        if (proxyPtr->target) {
          return getImportProperty(*proxyPtr->target, key, found, out, abrupt);
        }
        found = false;
        out = Value(Undefined{});
        return true;
      }

      if (receiver.isObject()) {
        auto current = receiver.getGC<Object>();
        int depth = 0;
        while (current && depth <= 16) {
          depth++;
          std::string getterKey = "__get_" + key;
          auto getterIt = current->properties.find(getterKey);
          if (getterIt != current->properties.end()) {
            if (getterIt->second.isFunction()) {
              Value getterOut = Value(Undefined{});
              if (!callImportCallable(getterIt->second, {}, receiver, getterOut, abrupt)) {
                return false;
              }
              found = true;
              out = getterOut;
              return true;
            }
            found = true;
            out = Value(Undefined{});
            return true;
          }

          auto it = current->properties.find(key);
          if (it != current->properties.end()) {
            found = true;
            out = it->second;
            return true;
          }

          auto protoIt = current->properties.find("__proto__");
          if (protoIt == current->properties.end() || !protoIt->second.isObject()) {
            break;
          }
          current = protoIt->second.getGC<Object>();
        }
        found = false;
        out = Value(Undefined{});
        return true;
      }

      if (receiver.isFunction()) {
        auto fn = receiver.getGC<Function>();
        auto it = fn->properties.find(key);
        if (it != fn->properties.end()) {
          found = true;
          out = it->second;
          return true;
        }
        found = false;
        out = Value(Undefined{});
        return true;
      }

      if (receiver.isRegex()) {
        auto regex = receiver.getGC<Regex>();
        std::string getterKey = "__get_" + key;
        auto getterIt = regex->properties.find(getterKey);
        if (getterIt != regex->properties.end()) {
          if (getterIt->second.isFunction()) {
            Value getterOut = Value(Undefined{});
            if (!callImportCallable(getterIt->second, {}, receiver, getterOut, abrupt)) {
              return false;
            }
            found = true;
            out = getterOut;
            return true;
          }
          found = true;
          out = Value(Undefined{});
          return true;
        }
        auto it = regex->properties.find(key);
        if (it != regex->properties.end()) {
          found = true;
          out = it->second;
          return true;
        }
      }

      found = false;
      out = Value(Undefined{});
      return true;
    };

    auto toPrimitiveImport = [&](const Value& input, bool preferString, Value& out, Value& abrupt) -> bool {
      if (!isObjectLikeImport(input)) {
        out = input;
        return true;
      }

      const std::string& toPrimitiveKey = WellKnownSymbols::toPrimitiveKey();
      bool hasExotic = false;
      Value exotic = Value(Undefined{});
      if (!getImportProperty(input, toPrimitiveKey, hasExotic, exotic, abrupt)) {
        return false;
      }
      if (hasExotic && !exotic.isUndefined() && !exotic.isNull()) {
        if (!exotic.isFunction()) {
          abrupt = makeError(ErrorType::TypeError, "@@toPrimitive is not callable");
          return false;
        }
        std::string hint = preferString ? "string" : "number";
        Value result = Value(Undefined{});
        if (!callImportCallable(exotic, {Value(hint)}, input, result, abrupt)) {
          return false;
        }
        if (isObjectLikeImport(result)) {
          abrupt = makeError(ErrorType::TypeError, "@@toPrimitive must return a primitive value");
          return false;
        }
        out = result;
        return true;
      }

      std::array<std::string, 2> methodOrder = preferString
        ? std::array<std::string, 2>{"toString", "valueOf"}
        : std::array<std::string, 2>{"valueOf", "toString"};
      for (const auto& methodName : methodOrder) {
        bool found = false;
        Value method = Value(Undefined{});
        if (!getImportProperty(input, methodName, found, method, abrupt)) {
          return false;
        }
        if (found && method.isFunction()) {
          Value result = Value(Undefined{});
          if (!callImportCallable(method, {}, input, result, abrupt)) {
            return false;
          }
          if (!isObjectLikeImport(result)) {
            out = result;
            return true;
          }
        }
      }

      if (input.isArray()) {
        out = Value(arrayToString(input.getGC<Array>()));
        return true;
      }
      if (input.isObject() || input.isFunction() || input.isProxy()) {
        out = Value(std::string("[object Object]"));
        return true;
      }
      if (input.isRegex()) {
        out = Value(input.toString());
        return true;
      }

      abrupt = makeError(ErrorType::TypeError, "Cannot convert object to primitive value");
      return false;
    };

    std::function<bool(const Value&, std::vector<std::string>&, Value&)> enumerateImportAttributeKeys;
    enumerateImportAttributeKeys =
      [&](const Value& source, std::vector<std::string>& keys, Value& abrupt) -> bool {
      std::unordered_set<std::string> seen;
      auto pushKey = [&](const std::string& key) {
        if (seen.find(key) != seen.end()) return;
        seen.insert(key);
        keys.push_back(key);
      };

      if (source.isProxy()) {
        auto proxyPtr = source.getGC<Proxy>();
        if (proxyPtr->handler && proxyPtr->handler->isObject()) {
          auto handlerObj = std::get<GCPtr<Object>>(proxyPtr->handler->data);
          auto ownKeysIt = handlerObj->properties.find("ownKeys");
          if (ownKeysIt != handlerObj->properties.end() && ownKeysIt->second.isFunction()) {
            Value ownKeysResult = Value(Undefined{});
            if (!callImportCallable(ownKeysIt->second, {*proxyPtr->target}, Value(Undefined{}), ownKeysResult, abrupt)) {
              return false;
            }

            std::vector<std::string> candidateKeys;
            if (ownKeysResult.isArray()) {
              auto arr = ownKeysResult.getGC<Array>();
              for (const auto& entry : arr->elements) {
                if (entry.isString()) {
                  candidateKeys.push_back(std::get<std::string>(entry.data));
                }
              }
            }

            auto gopdIt = handlerObj->properties.find("getOwnPropertyDescriptor");
            for (const auto& key : candidateKeys) {
              bool enumerable = true;
              if (gopdIt != handlerObj->properties.end() && gopdIt->second.isFunction()) {
                Value desc = Value(Undefined{});
                if (!callImportCallable(gopdIt->second, {*proxyPtr->target, Value(key)}, Value(Undefined{}), desc, abrupt)) {
                  return false;
                }
                if (desc.isUndefined()) {
                  enumerable = false;
                } else {
                  bool foundEnumerable = false;
                  Value enumerableValue = Value(Undefined{});
                  if (!getImportProperty(desc, "enumerable", foundEnumerable, enumerableValue, abrupt)) {
                    return false;
                  }
                  enumerable = foundEnumerable && enumerableValue.toBool();
                }
              }
              if (enumerable) {
                pushKey(key);
              }
            }
            return true;
          }
        }

        if (proxyPtr->target) {
          return enumerateImportAttributeKeys(*proxyPtr->target, keys, abrupt);
        }
        return true;
      }

      if (source.isObject()) {
        auto withObj = source.getGC<Object>();
        for (const auto& [key, _] : withObj->properties) {
          if (key.rfind("__get_", 0) == 0) {
            pushKey(key.substr(6));
            continue;
          }
          if (key.rfind("__", 0) == 0) {
            continue;
          }
          pushKey(key);
        }
      } else if (source.isArray()) {
        auto arr = source.getGC<Array>();
        for (size_t i = 0; i < arr->elements.size(); ++i) {
          pushKey(std::to_string(i));
        }
      } else if (source.isFunction()) {
        auto fn = source.getGC<Function>();
        for (const auto& [key, _] : fn->properties) {
          if (key.rfind("__", 0) == 0) {
            continue;
          }
          pushKey(key);
        }
      } else {
        return false;
      }
      return true;
    };

    try {
      if (args[0].isObject()) {
        auto obj = args[0].getGC<Object>();
        auto importMetaIt = obj->properties.find("__import_meta__");
        if (importMetaIt != obj->properties.end() &&
            importMetaIt->second.isBool() &&
            importMetaIt->second.toBool()) {
          auto err = GarbageCollector::makeGC<Error>(ErrorType::TypeError, "Cannot convert object to primitive value");
          promise->reject(Value(err));
          return Value(promise);
        }
      }
      Value primitiveSpecifier = Value(Undefined{});
      Value abrupt = Value(Undefined{});
      if (!toPrimitiveImport(args[0], true, primitiveSpecifier, abrupt)) {
        promise->reject(abrupt);
        return Value(promise);
      }
      if (primitiveSpecifier.isSymbol()) {
        auto err = GarbageCollector::makeGC<Error>(ErrorType::TypeError, "Cannot convert a Symbol value to a string");
        promise->reject(Value(err));
        return Value(promise);
      }
      specifier = primitiveSpecifier.toString();

      if (importPhase == ImportPhase::Source) {
        auto err = GarbageCollector::makeGC<Error>(ErrorType::SyntaxError, "Source phase import is not available");
        promise->reject(Value(err));
        return Value(promise);
      }

      if (hasImportOptions && !importOptions.isUndefined()) {
        if (!isObjectLikeImport(importOptions)) {
          auto err = GarbageCollector::makeGC<Error>(ErrorType::TypeError, "import() options must be an object");
          promise->reject(Value(err));
          return Value(promise);
        }

        bool hasWith = false;
        Value withValue = Value(Undefined{});
        Value abrupt = Value(Undefined{});
        if (!getImportProperty(importOptions, "with", hasWith, withValue, abrupt)) {
          promise->reject(abrupt);
          return Value(promise);
        }

        if (hasWith && !withValue.isUndefined()) {
          if (!isObjectLikeImport(withValue)) {
            auto err = GarbageCollector::makeGC<Error>(ErrorType::TypeError, "import() options.with must be an object");
            promise->reject(Value(err));
            return Value(promise);
          }

          std::vector<std::string> keys;
          if (!enumerateImportAttributeKeys(withValue, keys, abrupt)) {
            promise->reject(abrupt);
            return Value(promise);
          }

          for (const auto& key : keys) {
            bool foundAttr = false;
            Value attrValue = Value(Undefined{});
            if (!getImportProperty(withValue, key, foundAttr, attrValue, abrupt)) {
              promise->reject(abrupt);
              return Value(promise);
            }
            if (!attrValue.isString()) {
              auto err = GarbageCollector::makeGC<Error>(ErrorType::TypeError, "import() options.with values must be strings");
              promise->reject(Value(err));
              return Value(promise);
            }
          }
        }
      }

      // Use global module loader if available
      if (g_moduleLoader && g_interpreter) {
        // Resolve the module path
        std::string resolvedPath = g_moduleLoader->resolvePath(specifier);

        // Load the module
        auto module = g_moduleLoader->loadModule(resolvedPath);
        if (!module) {
          rejectWith(ErrorType::Error, "Failed to load module: " + specifier, g_moduleLoader->getLastError());
          return Value(promise);
        }

        // Instantiate the module
        if (!module->instantiate(g_moduleLoader.get())) {
          rejectWith(ErrorType::SyntaxError, "Failed to instantiate module: " + specifier, module->getLastError());
          return Value(promise);
        }

        bool deferEvaluationUntilNamespaceAccess = false;
        if (importPhase == ImportPhase::Defer) {
          // Defer phase: eagerly evaluate only asynchronous transitive dependencies.
          std::unordered_set<std::string> visitedModules;
          std::unordered_set<std::string> queuedAsyncModules;
          std::vector<std::string> orderedAsyncModules;
          gatherAsyncTransitiveDependencies(
            resolvedPath, g_moduleLoader.get(), visitedModules, queuedAsyncModules, orderedAsyncModules);

          for (const auto& asyncModulePath : orderedAsyncModules) {
            auto asyncModule = g_moduleLoader->loadModule(asyncModulePath);
            if (!asyncModule) {
              rejectWith(
                ErrorType::Error,
                "Failed to load deferred async dependency: " + asyncModulePath,
                g_moduleLoader->getLastError());
              return Value(promise);
            }
            if (!asyncModule->instantiate(g_moduleLoader.get())) {
              rejectWith(
                ErrorType::SyntaxError,
                "Failed to instantiate deferred async dependency: " + asyncModulePath,
                asyncModule->getLastError());
              return Value(promise);
            }
            if (!asyncModule->evaluate(g_interpreter)) {
              rejectWith(
                ErrorType::Error,
                "Failed to evaluate deferred async dependency: " + asyncModulePath,
                asyncModule->getLastError());
              return Value(promise);
            }
          }

          deferEvaluationUntilNamespaceAccess = (module->getState() != Module::State::Evaluated);
        } else {
          // Normal dynamic import evaluates immediately.
          if (!module->evaluate(g_interpreter)) {
            rejectWith(ErrorType::Error, "Failed to evaluate module: " + specifier, module->getLastError());
            return Value(promise);
          }
        }

        if (module->getState() == Module::State::EvaluatingAsync) {
          auto evalPromise = module->getEvaluationPromise();
          if (evalPromise) {
            evalPromise->then(
              [promise, module](Value) -> Value {
                promise->resolve(Value(module->getNamespaceObject()));
                return Value(Undefined{});
              },
              [promise](Value reason) -> Value {
                promise->reject(reason);
                return Value(Undefined{});
              }
            );
            return Value(promise);
          }
        }

        auto moduleNamespace = module->getNamespaceObject();
        if (deferEvaluationUntilNamespaceAccess) {
          auto deferredEvalFn = GarbageCollector::makeGC<Function>();
          deferredEvalFn->isNative = true;
          deferredEvalFn->properties["__throw_on_new__"] = Value(true);
          auto deferredModule = module;
          deferredEvalFn->nativeFunc = [deferredModule](const std::vector<Value>&) -> Value {
            if (deferredModule->getState() == Module::State::Evaluated) {
              return Value(Undefined{});
            }
            Interpreter* interpreter = getGlobalInterpreter();
            if (!interpreter) {
              throw std::runtime_error("Error: Interpreter unavailable for deferred module evaluation");
            }
            if (!deferredModule->evaluate(interpreter)) {
              if (auto error = deferredModule->getLastError()) {
                throw std::runtime_error(error->toString());
              }
              throw std::runtime_error("Error: Failed to evaluate deferred module");
            }
            return Value(Undefined{});
          };
          moduleNamespace->properties["__deferred_pending__"] = Value(true);
          moduleNamespace->properties["__deferred_eval__"] = Value(deferredEvalFn);
        }

        promise->resolve(Value(moduleNamespace));
      } else {
        // Fallback: create a placeholder namespace (for cases where module loader isn't set up)
        auto moduleNamespace = GarbageCollector::makeGC<Object>();
        GarbageCollector::instance().reportAllocation(sizeof(Object));
        moduleNamespace->properties["__esModule"] = Value(true);
        moduleNamespace->properties["__moduleSpecifier"] = Value(specifier);
        promise->resolve(Value(moduleNamespace));
      }
    } catch (const std::exception& e) {
      std::string message = e.what();
      ErrorType errorType = ErrorType::Error;
      auto consumePrefix = [&](const std::string& prefix, ErrorType type) {
        if (message.rfind(prefix, 0) == 0) {
          errorType = type;
          message = message.substr(prefix.size());
          return true;
        }
        return false;
      };
      if (!consumePrefix("TypeError: ", ErrorType::TypeError) &&
          !consumePrefix("ReferenceError: ", ErrorType::ReferenceError) &&
          !consumePrefix("RangeError: ", ErrorType::RangeError) &&
          !consumePrefix("SyntaxError: ", ErrorType::SyntaxError) &&
          !consumePrefix("URIError: ", ErrorType::URIError) &&
          !consumePrefix("EvalError: ", ErrorType::EvalError) &&
          !consumePrefix("Error: ", ErrorType::Error)) {
        // Keep non-Error abrupt completions (e.g. thrown strings) as-is.
        promise->reject(Value(message));
        return Value(promise);
      }
      auto err = GarbageCollector::makeGC<Error>(errorType, message);
      promise->reject(Value(err));
    } catch (...) {
      auto err = GarbageCollector::makeGC<Error>(ErrorType::Error, "Failed to load module: " + specifier);
      promise->reject(Value(err));
    }

    return Value(promise);
  };
  env->define("import", Value(importFn));

  auto regExpPrototype = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  auto regExpMatchAll = GarbageCollector::makeGC<Function>();
  regExpMatchAll->isNative = true;
  regExpMatchAll->isConstructor = false;
  regExpMatchAll->properties["__uses_this_arg__"] = Value(true);
  regExpMatchAll->properties["__throw_on_new__"] = Value(true);
  regExpMatchAll->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isRegex()) {
      throw std::runtime_error("TypeError: RegExp.prototype[@@matchAll] called on non-RegExp");
    }

    auto regexPtr = args[0].getGC<Regex>();
    std::string input = args.size() > 1 ? args[1].toString() : "";
    bool global = regexPtr->flags.find('g') != std::string::npos;
    bool unicodeMode = regexPtr->flags.find('u') != std::string::npos ||
                       regexPtr->flags.find('v') != std::string::npos;
    auto utf16IndexFromEngineOffset = [&input](size_t rawOffset) -> double {
      auto accumulateUtf16ByCodePointCount = [&input](size_t codePointCountTarget) -> size_t {
        size_t cursor = 0;
        size_t codePointCount = 0;
        size_t utf16Units = 0;
        while (cursor < input.size() && codePointCount < codePointCountTarget) {
          uint32_t codePoint = unicode::decodeUTF8(input, cursor);
          utf16Units += (codePoint > 0xFFFF) ? 2 : 1;
          codePointCount++;
        }
        return utf16Units;
      };

      // Some regex backends report byte offsets, others effectively report code point offsets.
      // If the offset lands inside a UTF-8 sequence, treat it as a code point count.
      if (rawOffset < input.size() &&
          unicode::isContinuationByte(static_cast<uint8_t>(input[rawOffset]))) {
        return static_cast<double>(accumulateUtf16ByCodePointCount(rawOffset));
      }

      size_t cursor = 0;
      size_t utf16Units = 0;
      while (cursor < rawOffset && cursor < input.size()) {
        uint32_t codePoint = unicode::decodeUTF8(input, cursor);
        utf16Units += (codePoint > 0xFFFF) ? 2 : 1;
      }
      return static_cast<double>(utf16Units);
    };

    auto allMatches = GarbageCollector::makeGC<Array>();
    GarbageCollector::instance().reportAllocation(sizeof(Array));

#if USE_SIMPLE_REGEX
    std::string remaining = input;
    size_t offsetBytes = 0;
    std::vector<simple_regex::Regex::Match> matches;
    while (regexPtr->regex->search(remaining, matches)) {
      if (matches.empty()) break;
      auto matchArr = GarbageCollector::makeGC<Array>();
      GarbageCollector::instance().reportAllocation(sizeof(Array));
      for (size_t i = 0; i < matches.size(); ++i) {
        matchArr->elements.push_back(Value(matches[i].str));
      }
      size_t matchStartBytes = offsetBytes + matches[0].start;
      double matchIndex = utf16IndexFromEngineOffset(matchStartBytes);
      matchArr->properties["index"] = Value(matchIndex);
      matchArr->properties["input"] = Value(input);
      allMatches->elements.push_back(Value(matchArr));

      if (!global) break;

      size_t matchAdvance = matches[0].start + matches[0].str.length();
      if (matchAdvance == 0) {
        if (!remaining.empty()) {
          if (unicodeMode) {
            matchAdvance = unicode::utf8SequenceLength(static_cast<uint8_t>(remaining[0]));
          } else {
            matchAdvance = 1;
          }
        } else {
          break;
        }
      }
      offsetBytes += matchAdvance;
      remaining = remaining.substr(matchAdvance);
      matches.clear();
    }
#else
    std::string::const_iterator searchStart = input.cbegin();
    std::smatch match;
    while (std::regex_search(searchStart, input.cend(), match, regexPtr->regex)) {
      auto matchArr = GarbageCollector::makeGC<Array>();
      GarbageCollector::instance().reportAllocation(sizeof(Array));
      for (size_t i = 0; i < match.size(); ++i) {
        matchArr->elements.push_back(Value(match[i].str()));
      }
      size_t matchStartBytes = static_cast<size_t>(match.position() + (searchStart - input.cbegin()));
      double matchIndex = utf16IndexFromEngineOffset(matchStartBytes);
      matchArr->properties["index"] = Value(matchIndex);
      matchArr->properties["input"] = Value(input);
      allMatches->elements.push_back(Value(matchArr));

      if (!global) break;
      searchStart = match.suffix().first;
      if (match[0].length() == 0) {
        if (searchStart != input.cend()) {
          if (unicodeMode) {
            size_t byteOffset = static_cast<size_t>(searchStart - input.cbegin());
            size_t advance = unicode::utf8SequenceLength(static_cast<uint8_t>(input[byteOffset]));
            std::advance(searchStart, static_cast<std::ptrdiff_t>(advance));
          } else {
            ++searchStart;
          }
        } else {
          break;  // empty match at end of string, stop
        }
      }
    }
#endif

    // Return a RegExpStringIterator (object with .next() method)
    auto iterObj = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    auto idx = std::make_shared<size_t>(0);
    auto nextFn = GarbageCollector::makeGC<Function>();
    nextFn->isNative = true;
    nextFn->nativeFunc = [allMatches, idx](const std::vector<Value>& /*args*/) -> Value {
      auto result = GarbageCollector::makeGC<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      if (*idx < allMatches->elements.size()) {
        result->properties["value"] = allMatches->elements[*idx];
        result->properties["done"] = Value(false);
        (*idx)++;
      } else {
        result->properties["value"] = Value(Undefined{});
        result->properties["done"] = Value(true);
      }
      return Value(result);
    };
    iterObj->properties["next"] = Value(nextFn);
    return Value(iterObj);
  };
  regExpPrototype->properties[WellKnownSymbols::matchAllKey()] = Value(regExpMatchAll);
  regExpPrototype->properties["__non_enum_" + WellKnownSymbols::matchAllKey()] = Value(true);

  // RegExp.prototype.exec
  auto regExpExec = GarbageCollector::makeGC<Function>();
  regExpExec->isNative = true;
  regExpExec->isConstructor = false;
  regExpExec->properties["name"] = Value(std::string("exec"));
  regExpExec->properties["length"] = Value(1.0);
  regExpExec->properties["__non_writable_name"] = Value(true);
  regExpExec->properties["__non_enum_name"] = Value(true);
  regExpExec->properties["__non_writable_length"] = Value(true);
  regExpExec->properties["__non_enum_length"] = Value(true);
  regExpExec->properties["__uses_this_arg__"] = Value(true);
  regExpExec->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // Stub - actual exec is handled in evaluateMember/interpreter
    return Value(Null{});
  };
  regExpPrototype->properties["exec"] = Value(regExpExec);
  regExpPrototype->properties["__non_enum_exec"] = Value(true);

  // RegExp.prototype.test
  auto regExpTest = GarbageCollector::makeGC<Function>();
  regExpTest->isNative = true;
  regExpTest->isConstructor = false;
  regExpTest->properties["name"] = Value(std::string("test"));
  regExpTest->properties["length"] = Value(1.0);
  regExpTest->properties["__non_writable_name"] = Value(true);
  regExpTest->properties["__non_enum_name"] = Value(true);
  regExpTest->properties["__non_writable_length"] = Value(true);
  regExpTest->properties["__non_enum_length"] = Value(true);
  regExpTest->properties["__uses_this_arg__"] = Value(true);
  regExpTest->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // Stub - actual test is handled in evaluateMember/interpreter
    return Value(false);
  };
  regExpPrototype->properties["test"] = Value(regExpTest);
  regExpPrototype->properties["__non_enum_test"] = Value(true);

  // RegExp.prototype.toString
  auto regExpToString = GarbageCollector::makeGC<Function>();
  regExpToString->isNative = true;
  regExpToString->isConstructor = false;
  regExpToString->properties["name"] = Value(std::string("toString"));
  regExpToString->properties["length"] = Value(0.0);
  regExpToString->properties["__non_writable_name"] = Value(true);
  regExpToString->properties["__non_enum_name"] = Value(true);
  regExpToString->properties["__non_writable_length"] = Value(true);
  regExpToString->properties["__non_enum_length"] = Value(true);
  regExpToString->properties["__uses_this_arg__"] = Value(true);
  regExpToString->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (!args.empty() && args[0].isRegex()) {
      auto rx = args[0].getGC<Regex>();
      std::string source = rx->pattern.empty() ? "(?:)" : rx->pattern;
      return Value("/" + source + "/" + rx->flags);
    }
    return Value(std::string("/undefined/"));
  };
  regExpPrototype->properties["toString"] = Value(regExpToString);
  regExpPrototype->properties["__non_enum_toString"] = Value(true);

  // RegExp.prototype accessor properties: source, flags, global, ignoreCase, multiline, unicode, sticky, dotAll
  auto makeRegExpGetter = [&](const std::string& name, auto extractFn) {
    auto getter = GarbageCollector::makeGC<Function>();
    getter->isNative = true;
    getter->isConstructor = false;
    getter->properties["__uses_this_arg__"] = Value(true);
    getter->nativeFunc = [extractFn](const std::vector<Value>& args) -> Value {
      if (!args.empty() && args[0].isRegex()) {
        return extractFn(args[0].getGC<Regex>());
      }
      // RegExp.prototype itself: return undefined (except source returns "(?:)")
      return Value(Undefined{});
    };
    regExpPrototype->properties["__get_" + name] = Value(getter);
    regExpPrototype->properties["__non_enum_" + name] = Value(true);
  };

  makeRegExpGetter("source", [](GCPtr<Regex> rx) -> Value {
    return Value(rx->pattern.empty() ? std::string("(?:)") : rx->pattern);
  });
  makeRegExpGetter("flags", [](GCPtr<Regex> rx) -> Value {
    return Value(rx->flags);
  });
  makeRegExpGetter("global", [](GCPtr<Regex> rx) -> Value {
    return Value(rx->flags.find('g') != std::string::npos);
  });
  makeRegExpGetter("ignoreCase", [](GCPtr<Regex> rx) -> Value {
    return Value(rx->flags.find('i') != std::string::npos);
  });
  makeRegExpGetter("multiline", [](GCPtr<Regex> rx) -> Value {
    return Value(rx->flags.find('m') != std::string::npos);
  });
  makeRegExpGetter("unicode", [](GCPtr<Regex> rx) -> Value {
    return Value(rx->flags.find('u') != std::string::npos);
  });
  makeRegExpGetter("sticky", [](GCPtr<Regex> rx) -> Value {
    return Value(rx->flags.find('y') != std::string::npos);
  });
  makeRegExpGetter("dotAll", [](GCPtr<Regex> rx) -> Value {
    return Value(rx->flags.find('s') != std::string::npos);
  });

  auto regExpConstructor = GarbageCollector::makeGC<Function>();
  regExpConstructor->isNative = true;
  regExpConstructor->isConstructor = true;
  regExpConstructor->properties["name"] = Value(std::string("RegExp"));
  regExpConstructor->properties["length"] = Value(2.0);
  regExpConstructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    std::string pattern = args.empty() ? std::string("") : args[0].toString();
    std::string flags = args.size() > 1 ? args[1].toString() : "";
    auto rx = GarbageCollector::makeGC<Regex>(pattern, flags);
    // lastIndex: writable, non-enumerable, non-configurable
    rx->properties["lastIndex"] = Value(0.0);
    rx->properties["__non_enum_lastIndex"] = Value(true);
    rx->properties["__non_configurable_lastIndex"] = Value(true);
    return Value(rx);
  };
  regExpConstructor->properties["prototype"] = Value(regExpPrototype);
  regExpConstructor->properties["__non_writable_prototype"] = Value(true);
  regExpConstructor->properties["__non_enum_prototype"] = Value(true);
  regExpConstructor->properties["__non_configurable_prototype"] = Value(true);
  regExpPrototype->properties["constructor"] = Value(regExpConstructor);
  regExpPrototype->properties["__non_enum_constructor"] = Value(true);
  env->define("RegExp", Value(regExpConstructor));

  // Error constructors
  // Error.prototype.toString per ES spec 20.5.3.4
  auto errorToString = GarbageCollector::makeGC<Function>();
  errorToString->isNative = true;
  errorToString->properties["__uses_this_arg__"] = Value(true);
  errorToString->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || (!args[0].isObject() && !args[0].isError())) {
      throw std::runtime_error("TypeError: Error.prototype.toString requires that 'this' be an Object");
    }
    // Get name property
    std::string name = "Error";
    std::string msg = "";
    if (args[0].isError()) {
      auto err = args[0].getGC<Error>();
      // Look for 'name' in own properties first
      auto nameIt = err->properties.find("name");
      if (nameIt != err->properties.end()) {
        if (!nameIt->second.isUndefined()) name = nameIt->second.toString();
      } else {
        // Walk prototype chain for 'name'
        auto protoIt = err->properties.find("__proto__");
        bool foundName = false;
        int depth = 0;
        while (protoIt != err->properties.end() && protoIt->second.isObject() && depth < 16) {
          auto proto = protoIt->second.getGC<Object>();
          auto pNameIt = proto->properties.find("name");
          if (pNameIt != proto->properties.end() && !pNameIt->second.isUndefined()) {
            name = pNameIt->second.toString();
            foundName = true;
            break;
          }
          protoIt = proto->properties.find("__proto__");
          depth++;
        }
        if (!foundName) {
          name = err->getName();
        }
      }
      auto msgIt = err->properties.find("message");
      if (msgIt != err->properties.end()) {
        if (!msgIt->second.isUndefined()) msg = msgIt->second.toString();
      }
    } else {
      auto obj = args[0].getGC<Object>();
      auto nameIt = obj->properties.find("name");
      if (nameIt != obj->properties.end()) {
        if (!nameIt->second.isUndefined()) name = nameIt->second.toString();
      }
      auto msgIt = obj->properties.find("message");
      if (msgIt != obj->properties.end()) {
        if (!msgIt->second.isUndefined()) msg = msgIt->second.toString();
      }
    }
    if (name.empty() && msg.empty()) return Value(std::string(""));
    if (name.empty()) return Value(msg);
    if (msg.empty()) return Value(name);
    return Value(name + ": " + msg);
  };
  errorToString->properties["name"] = Value(std::string("toString"));
  errorToString->properties["__non_writable_name"] = Value(true);
  errorToString->properties["__non_enum_name"] = Value(true);
  errorToString->properties["length"] = Value(0.0);
  errorToString->properties["__non_writable_length"] = Value(true);
  errorToString->properties["__non_enum_length"] = Value(true);

  auto createErrorConstructor = [&errorToString](ErrorType type, const std::string& name) {
    auto prototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    auto func = GarbageCollector::makeGC<Function>();
    func->isNative = true;
    func->isConstructor = true;
    func->nativeFunc = [type, prototype](const std::vector<Value>& args) -> Value {
      bool hasMessage = args.size() >= 1 && !args[0].isUndefined();
      if (hasMessage && args[0].isSymbol()) {
        throw std::runtime_error("TypeError: Cannot convert a Symbol value to a string");
      }
      std::string message = hasMessage ? args[0].toString() : "";
      auto err = GarbageCollector::makeGC<Error>(type, message);
      if (hasMessage) {
        err->properties["message"] = Value(message);
        err->properties["__non_enum_message"] = Value(true);
      }
      // ES spec: if options is an Object with "cause" property, set it
      size_t optionsIdx = 1;
      if (args.size() > optionsIdx && args[optionsIdx].isObject()) {
        auto opts = args[optionsIdx].getGC<Object>();
        // Check for getter first
        auto getterIt = opts->properties.find("__get_cause");
        if (getterIt != opts->properties.end() && getterIt->second.isFunction()) {
          Interpreter* interp = getGlobalInterpreter();
          if (interp) {
            Value causeVal = interp->callForHarness(getterIt->second, {}, args[optionsIdx]);
            if (interp->hasError()) {
              Value errVal = interp->getError();
              interp->clearError();
              throw std::runtime_error(errVal.toString());
            }
            err->properties["cause"] = causeVal;
            err->properties["__non_enum_cause"] = Value(true);
          }
        } else {
          auto causeIt = opts->properties.find("cause");
          if (causeIt != opts->properties.end()) {
            err->properties["cause"] = causeIt->second;
            err->properties["__non_enum_cause"] = Value(true);
          }
        }
      }
      // Set __proto__ so Error() without new also gets correct prototype
      err->properties["__proto__"] = Value(prototype);
      return Value(err);
    };
    func->properties["__error_type__"] = Value(static_cast<double>(static_cast<int>(type)));
    func->properties["name"] = Value(name);
    func->properties["__non_writable_name"] = Value(true);
    func->properties["__non_enum_name"] = Value(true);
    func->properties["length"] = Value(1.0);
    func->properties["__non_writable_length"] = Value(true);
    func->properties["__non_enum_length"] = Value(true);
    func->properties["prototype"] = Value(prototype);
    func->properties["__non_writable_prototype"] = Value(true);
    func->properties["__non_enum_prototype"] = Value(true);
    func->properties["__non_configurable_prototype"] = Value(true);
    prototype->properties["constructor"] = Value(func);
    prototype->properties["__non_enum_constructor"] = Value(true);
    prototype->properties["name"] = Value(name);
    prototype->properties["__non_enum_name"] = Value(true);
    prototype->properties["message"] = Value(std::string(""));
    prototype->properties["__non_enum_message"] = Value(true);
    return func;
  };

  auto errorCtor = createErrorConstructor(ErrorType::Error, "Error");
  // Add toString only to Error.prototype (sub-errors inherit via proto chain)
  {
    auto errorProtoIt = errorCtor->properties.find("prototype");
    if (errorProtoIt != errorCtor->properties.end() && errorProtoIt->second.isObject()) {
      auto errorProto = errorProtoIt->second.getGC<Object>();
      errorProto->properties["toString"] = Value(errorToString);
      errorProto->properties["__non_enum_toString"] = Value(true);
    }
  }
  auto typeErrorCtor = createErrorConstructor(ErrorType::TypeError, "TypeError");
  auto referenceErrorCtor = createErrorConstructor(ErrorType::ReferenceError, "ReferenceError");
  auto rangeErrorCtor = createErrorConstructor(ErrorType::RangeError, "RangeError");
  auto syntaxErrorCtor = createErrorConstructor(ErrorType::SyntaxError, "SyntaxError");
  auto uriErrorCtor = createErrorConstructor(ErrorType::URIError, "URIError");
  auto evalErrorCtor = createErrorConstructor(ErrorType::EvalError, "EvalError");

  // Error.isError (ES2025)
  {
    auto isErrorFn = GarbageCollector::makeGC<Function>();
    isErrorFn->isNative = true;
    isErrorFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
      if (args.empty()) return Value(false);
      return Value(args[0].isError());
    };
    isErrorFn->properties["name"] = Value(std::string("isError"));
    isErrorFn->properties["__non_writable_name"] = Value(true);
    isErrorFn->properties["__non_enum_name"] = Value(true);
    isErrorFn->properties["length"] = Value(1.0);
    isErrorFn->properties["__non_writable_length"] = Value(true);
    isErrorFn->properties["__non_enum_length"] = Value(true);
    errorCtor->properties["isError"] = Value(isErrorFn);
    errorCtor->properties["__non_enum_isError"] = Value(true);
  }

  // Set Error.prototype.__proto__ = Object.prototype later (after objectPrototype is created)

  env->define("Error", Value(errorCtor));
  env->define("TypeError", Value(typeErrorCtor));
  env->define("ReferenceError", Value(referenceErrorCtor));
  env->define("RangeError", Value(rangeErrorCtor));
  env->define("SyntaxError", Value(syntaxErrorCtor));
  env->define("URIError", Value(uriErrorCtor));
  env->define("EvalError", Value(evalErrorCtor));

  // AggregateError (ES2021) - minimal constructor/prototype for subclassing tests.
  {
    auto aggregateErrorCtor = GarbageCollector::makeGC<Function>();
    aggregateErrorCtor->isNative = true;
    aggregateErrorCtor->isConstructor = true;
    aggregateErrorCtor->properties["name"] = Value(std::string("AggregateError"));
    aggregateErrorCtor->properties["length"] = Value(2.0);
    aggregateErrorCtor->nativeFunc = [](const std::vector<Value>& args) -> Value {
      std::string message;
      if (args.size() >= 2 && !args[1].isUndefined()) {
        message = args[1].toString();
      }
      return Value(GarbageCollector::makeGC<Error>(ErrorType::Error, message));
    };
    auto aggregateErrorProto = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    aggregateErrorCtor->properties["prototype"] = Value(aggregateErrorProto);
    aggregateErrorProto->properties["constructor"] = Value(aggregateErrorCtor);
    aggregateErrorProto->properties["name"] = Value(std::string("AggregateError"));
    auto errorProtoIt = errorCtor->properties.find("prototype");
    if (errorProtoIt != errorCtor->properties.end() && errorProtoIt->second.isObject()) {
      aggregateErrorProto->properties["__proto__"] = errorProtoIt->second;
    }
    env->define("AggregateError", Value(aggregateErrorCtor));
  }

  // Map constructor
  auto mapConstructor = GarbageCollector::makeGC<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  mapConstructor->isNative = true;
  mapConstructor->isConstructor = true;
  mapConstructor->properties["name"] = Value(std::string("Map"));
  mapConstructor->properties["__non_writable_name"] = Value(true);
  mapConstructor->properties["__non_enum_name"] = Value(true);
  mapConstructor->properties["length"] = Value(0.0);
  mapConstructor->properties["__non_writable_length"] = Value(true);
  mapConstructor->properties["__non_enum_length"] = Value(true);
  mapConstructor->properties["__require_new__"] = Value(true);
  mapConstructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto mapObj = GarbageCollector::makeGC<Map>();
    GarbageCollector::instance().reportAllocation(sizeof(Map));

    if (!args.empty() && args[0].isArray()) {
      auto entriesArr = args[0].getGC<Array>();
      for (const auto& entryVal : entriesArr->elements) {
        Value k(Undefined{});
        Value v(Undefined{});
        if (entryVal.isArray()) {
          auto entryArr = entryVal.getGC<Array>();
          if (entryArr->elements.size() >= 1) k = entryArr->elements[0];
          if (entryArr->elements.size() >= 2) v = entryArr->elements[1];
          mapObj->set(k, v);
          continue;
        }
        if (entryVal.isObject()) {
          auto entryObj = entryVal.getGC<Object>();
          if (auto it0 = entryObj->properties.find("0"); it0 != entryObj->properties.end()) {
            k = it0->second;
          }
          if (auto it1 = entryObj->properties.find("1"); it1 != entryObj->properties.end()) {
            v = it1->second;
          }
          mapObj->set(k, v);
        }
      }
    }

    return Value(mapObj);
  };
  auto mapPrototype = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  mapConstructor->properties["prototype"] = Value(mapPrototype);
  mapConstructor->properties["__non_writable_prototype"] = Value(true);
  mapConstructor->properties["__non_enum_prototype"] = Value(true);
  mapConstructor->properties["__non_configurable_prototype"] = Value(true);
  mapPrototype->properties["constructor"] = Value(mapConstructor);
  mapPrototype->properties["__non_enum_constructor"] = Value(true);

  // Map[Symbol.toStringTag] = "Map"
  mapPrototype->properties[WellKnownSymbols::toStringTagKey()] = Value(std::string("Map"));
  mapPrototype->properties["__non_writable_" + WellKnownSymbols::toStringTagKey()] = Value(true);
  mapPrototype->properties["__non_enum_" + WellKnownSymbols::toStringTagKey()] = Value(true);

  // Helper to validate Map `this`
  auto validateMapThis = [](const std::vector<Value>& args, const std::string& methodName) -> GCPtr<Map> {
    if (args.empty() || !args[0].isMap()) {
      throw std::runtime_error("TypeError: Method Map.prototype." + methodName + " called on incompatible receiver");
    }
    return args[0].getGC<Map>();
  };

  // Helper to define a Map prototype method
  auto defineMapMethod = [&](const std::string& name, int length, std::function<Value(const std::vector<Value>&)> impl) {
    auto fn = GarbageCollector::makeGC<Function>();
    fn->isNative = true;
    fn->properties["__uses_this_arg__"] = Value(true);
    fn->properties["name"] = Value(name);
    fn->properties["__non_writable_name"] = Value(true);
    fn->properties["__non_enum_name"] = Value(true);
    fn->properties["length"] = Value(static_cast<double>(length));
    fn->properties["__non_writable_length"] = Value(true);
    fn->properties["__non_enum_length"] = Value(true);
    fn->nativeFunc = impl;
    mapPrototype->properties[name] = Value(fn);
    mapPrototype->properties["__non_enum_" + name] = Value(true);
  };

  defineMapMethod("get", 1, [validateMapThis](const std::vector<Value>& args) -> Value {
    auto m = validateMapThis(args, "get");
    if (args.size() < 2) return Value(Undefined{});
    return m->get(args[1]);
  });

  defineMapMethod("set", 2, [validateMapThis](const std::vector<Value>& args) -> Value {
    auto m = validateMapThis(args, "set");
    Value key = args.size() > 1 ? args[1] : Value(Undefined{});
    Value val = args.size() > 2 ? args[2] : Value(Undefined{});
    m->set(key, val);
    return Value(m);
  });

  defineMapMethod("has", 1, [validateMapThis](const std::vector<Value>& args) -> Value {
    auto m = validateMapThis(args, "has");
    if (args.size() < 2) return Value(m->has(Value(Undefined{})));
    return Value(m->has(args[1]));
  });

  defineMapMethod("delete", 1, [validateMapThis](const std::vector<Value>& args) -> Value {
    auto m = validateMapThis(args, "delete");
    if (args.size() < 2) return Value(false);
    return Value(m->deleteKey(args[1]));
  });

  defineMapMethod("clear", 0, [validateMapThis](const std::vector<Value>& args) -> Value {
    auto m = validateMapThis(args, "clear");
    m->clear();
    return Value(Undefined{});
  });

  defineMapMethod("forEach", 1, [validateMapThis](const std::vector<Value>& args) -> Value {
    auto m = validateMapThis(args, "forEach");
    if (args.size() < 2 || !args[1].isFunction()) {
      throw std::runtime_error("TypeError: forEach requires a callback function");
    }
    // forEach implementation handled by interpreter's dynamic dispatch
    // This prototype method just validates the receiver
    auto* interp = getGlobalInterpreter();
    if (!interp) return Value(Undefined{});
    auto callback = args[1].getGC<Function>();
    Value thisArg = args.size() > 2 ? args[2] : Value(Undefined{});
    for (size_t i = 0; i < m->entries.size(); ++i) {
      auto& entry = m->entries[i];
      interp->callForHarness(Value(callback), {entry.second, entry.first, Value(m)}, thisArg);
    }
    return Value(Undefined{});
  });

  defineMapMethod("entries", 0, [validateMapThis](const std::vector<Value>& args) -> Value {
    auto m = validateMapThis(args, "entries");
    auto iterObj = GarbageCollector::makeGC<Object>();
    auto indexPtr = std::make_shared<size_t>(0);
    auto nextFn = GarbageCollector::makeGC<Function>();
    nextFn->isNative = true;
    nextFn->nativeFunc = [m, indexPtr](const std::vector<Value>&) -> Value {
      if (*indexPtr >= m->entries.size()) {
        return makeIteratorResultObject(Value(Undefined{}), true);
      }
      auto& entry = m->entries[*indexPtr];
      auto pair = GarbageCollector::makeGC<Array>();
      pair->elements.push_back(entry.first);
      pair->elements.push_back(entry.second);
      (*indexPtr)++;
      return makeIteratorResultObject(Value(pair), false);
    };
    iterObj->properties["next"] = Value(nextFn);
    return Value(iterObj);
  });

  defineMapMethod("keys", 0, [validateMapThis](const std::vector<Value>& args) -> Value {
    auto m = validateMapThis(args, "keys");
    auto iterObj = GarbageCollector::makeGC<Object>();
    auto indexPtr = std::make_shared<size_t>(0);
    auto nextFn = GarbageCollector::makeGC<Function>();
    nextFn->isNative = true;
    nextFn->nativeFunc = [m, indexPtr](const std::vector<Value>&) -> Value {
      if (*indexPtr >= m->entries.size()) {
        return makeIteratorResultObject(Value(Undefined{}), true);
      }
      auto& entry = m->entries[*indexPtr];
      (*indexPtr)++;
      return makeIteratorResultObject(entry.first, false);
    };
    iterObj->properties["next"] = Value(nextFn);
    return Value(iterObj);
  });

  defineMapMethod("values", 0, [validateMapThis](const std::vector<Value>& args) -> Value {
    auto m = validateMapThis(args, "values");
    auto iterObj = GarbageCollector::makeGC<Object>();
    auto indexPtr = std::make_shared<size_t>(0);
    auto nextFn = GarbageCollector::makeGC<Function>();
    nextFn->isNative = true;
    nextFn->nativeFunc = [m, indexPtr](const std::vector<Value>&) -> Value {
      if (*indexPtr >= m->entries.size()) {
        return makeIteratorResultObject(Value(Undefined{}), true);
      }
      auto& entry = m->entries[*indexPtr];
      (*indexPtr)++;
      return makeIteratorResultObject(entry.second, false);
    };
    iterObj->properties["next"] = Value(nextFn);
    return Value(iterObj);
  });

  // Map.prototype[Symbol.iterator] = Map.prototype.entries
  {
    auto it = mapPrototype->properties.find("entries");
    if (it != mapPrototype->properties.end()) {
      mapPrototype->properties[WellKnownSymbols::iteratorKey()] = it->second;
      mapPrototype->properties["__non_enum_" + WellKnownSymbols::iteratorKey()] = Value(true);
    }
  }

  // Map.prototype.size as getter
  {
    auto sizeGetter = GarbageCollector::makeGC<Function>();
    sizeGetter->isNative = true;
    sizeGetter->properties["__uses_this_arg__"] = Value(true);
    sizeGetter->nativeFunc = [validateMapThis](const std::vector<Value>& args) -> Value {
      auto m = validateMapThis(args, "size");
      return Value(static_cast<double>(m->size()));
    };
    sizeGetter->properties["name"] = Value(std::string("get size"));
    sizeGetter->properties["__non_writable_name"] = Value(true);
    sizeGetter->properties["__non_enum_name"] = Value(true);
    sizeGetter->properties["length"] = Value(0.0);
    sizeGetter->properties["__non_writable_length"] = Value(true);
    sizeGetter->properties["__non_enum_length"] = Value(true);
    mapPrototype->properties["__get_size"] = Value(sizeGetter);
    mapPrototype->properties["__non_enum_size"] = Value(true);
  }

  // Map.prototype.getOrInsert
  defineMapMethod("getOrInsert", 2, [validateMapThis](const std::vector<Value>& args) -> Value {
    auto m = validateMapThis(args, "getOrInsert");
    Value key = args.size() > 1 ? args[1] : Value(Undefined{});
    // Normalize -0 to +0
    if (key.isNumber() && key.toNumber() == 0.0) key = Value(0.0);
    if (m->has(key)) {
      return m->get(key);
    }
    Value val = args.size() > 2 ? args[2] : Value(Undefined{});
    m->set(key, val);
    return val;
  });

  // Map.prototype.getOrInsertComputed
  defineMapMethod("getOrInsertComputed", 2, [validateMapThis](const std::vector<Value>& args) -> Value {
    auto m = validateMapThis(args, "getOrInsertComputed");
    Value key = args.size() > 1 ? args[1] : Value(Undefined{});
    // Normalize -0 to +0
    if (key.isNumber() && key.toNumber() == 0.0) key = Value(0.0);
    if (m->has(key)) {
      return m->get(key);
    }
    if (args.size() < 3 || !args[2].isFunction()) {
      throw std::runtime_error("TypeError: callbackfn is not a function");
    }
    auto* interp = getGlobalInterpreter();
    if (!interp) return Value(Undefined{});
    Value val = interp->callForHarness(Value(args[2].getGC<Function>()), {key});
    m->set(key, val);
    return val;
  });

  // Map.groupBy(items, callbackfn)
  {
    auto groupByFn = GarbageCollector::makeGC<Function>();
    groupByFn->isNative = true;
    groupByFn->properties["name"] = Value(std::string("groupBy"));
    groupByFn->properties["__non_writable_name"] = Value(true);
    groupByFn->properties["__non_enum_name"] = Value(true);
    groupByFn->properties["length"] = Value(2.0);
    groupByFn->properties["__non_writable_length"] = Value(true);
    groupByFn->properties["__non_enum_length"] = Value(true);
    groupByFn->nativeFunc = [mapPrototype](const std::vector<Value>& args) -> Value {
      if (args.size() < 2 || !args[1].isFunction()) {
        throw std::runtime_error("TypeError: callbackfn is not a function");
      }
      auto items = args[0];
      auto callbackFn = args[1].getGC<Function>();
      auto* interp = getGlobalInterpreter();
      if (!interp) return Value(Undefined{});

      auto resultMap = GarbageCollector::makeGC<Map>();
      GarbageCollector::instance().reportAllocation(sizeof(Map));
      resultMap->properties["__proto__"] = Value(mapPrototype);

      // Helper to add an item to the group
      auto addToGroup = [&](const Value& item, size_t index) {
        Value key = interp->callForHarness(Value(callbackFn), {item, Value(static_cast<double>(index))});
        if (interp->hasError()) {
          Value err = interp->getError();
          interp->clearError();
          throw std::runtime_error(err.toString());
        }
        // Normalize -0 to +0
        if (key.isNumber() && key.toNumber() == 0.0) key = Value(0.0);
        if (resultMap->has(key)) {
          Value existing = resultMap->get(key);
          if (existing.isArray()) {
            existing.getGC<Array>()->elements.push_back(item);
          }
        } else {
          auto group = GarbageCollector::makeGC<Array>();
          group->elements.push_back(item);
          resultMap->set(key, Value(group));
        }
      };

      if (items.isArray()) {
        auto arr = items.getGC<Array>();
        for (size_t i = 0; i < arr->elements.size(); ++i) {
          addToGroup(arr->elements[i], i);
        }
      } else if (items.isString()) {
        // Iterate string by code points
        const std::string& str = std::get<std::string>(items.data);
        size_t bytePos = 0;
        size_t idx = 0;
        while (bytePos < str.size()) {
          unsigned char c = str[bytePos];
          size_t charLen = 1;
          if ((c & 0x80) == 0) charLen = 1;
          else if ((c & 0xE0) == 0xC0) charLen = 2;
          else if ((c & 0xF0) == 0xE0) charLen = 3;
          else if ((c & 0xF8) == 0xF0) charLen = 4;
          std::string ch = str.substr(bytePos, charLen);
          addToGroup(Value(ch), idx);
          bytePos += charLen;
          idx++;
        }
      } else {
        // Try iterator protocol
        auto [found, iterFn] = interp->getPropertyForExternal(items, WellKnownSymbols::iteratorKey());
        if (found && iterFn.isFunction()) {
          Value iter = interp->callForHarness(iterFn, {}, items);
          size_t idx = 0;
          int limit = 100000;
          while (limit-- > 0) {
            Value nextResult;
            if (iter.isObject()) {
              auto iterObj = iter.getGC<Object>();
              auto nextIt = iterObj->properties.find("next");
              if (nextIt != iterObj->properties.end() && nextIt->second.isFunction()) {
                nextResult = interp->callForHarness(nextIt->second, {}, iter);
              } else break;
            } else break;
            if (!nextResult.isObject()) break;
            auto resultObj = nextResult.getGC<Object>();
            auto doneIt = resultObj->properties.find("done");
            if (doneIt != resultObj->properties.end() && doneIt->second.toBool()) break;
            auto valueIt = resultObj->properties.find("value");
            Value item = (valueIt != resultObj->properties.end()) ? valueIt->second : Value(Undefined{});
            addToGroup(item, idx);
            idx++;
          }
        }
      }
      return Value(resultMap);
    };
    mapConstructor->properties["groupBy"] = Value(groupByFn);
  }

  env->define("Map", Value(mapConstructor));

  // Set constructor
  auto setConstructor = GarbageCollector::makeGC<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  setConstructor->isNative = true;
  setConstructor->isConstructor = true;
  setConstructor->properties["name"] = Value(std::string("Set"));
  setConstructor->properties["__non_writable_name"] = Value(true);
  setConstructor->properties["__non_enum_name"] = Value(true);
  setConstructor->properties["length"] = Value(0.0);
  setConstructor->properties["__non_writable_length"] = Value(true);
  setConstructor->properties["__non_enum_length"] = Value(true);
  setConstructor->properties["__require_new__"] = Value(true);
  setConstructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto setObj = GarbageCollector::makeGC<Set>();
    GarbageCollector::instance().reportAllocation(sizeof(Set));

    if (!args.empty() && args[0].isArray()) {
      auto entriesArr = args[0].getGC<Array>();
      for (const auto& entryVal : entriesArr->elements) {
        setObj->add(entryVal);
      }
    }

    return Value(setObj);
  };
  auto setPrototype = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  setConstructor->properties["prototype"] = Value(setPrototype);
  setConstructor->properties["__non_writable_prototype"] = Value(true);
  setConstructor->properties["__non_enum_prototype"] = Value(true);
  setConstructor->properties["__non_configurable_prototype"] = Value(true);
  setPrototype->properties["constructor"] = Value(setConstructor);
  setPrototype->properties["__non_enum_constructor"] = Value(true);

  // Set[Symbol.toStringTag] = "Set"
  setPrototype->properties[WellKnownSymbols::toStringTagKey()] = Value(std::string("Set"));
  setPrototype->properties["__non_writable_" + WellKnownSymbols::toStringTagKey()] = Value(true);
  setPrototype->properties["__non_enum_" + WellKnownSymbols::toStringTagKey()] = Value(true);

  auto validateSetThis = [](const std::vector<Value>& args, const std::string& methodName) -> GCPtr<Set> {
    if (args.empty() || !args[0].isSet()) {
      throw std::runtime_error("TypeError: Method Set.prototype." + methodName + " called on incompatible receiver");
    }
    return args[0].getGC<Set>();
  };

  auto defineSetMethod = [&](const std::string& name, int length, std::function<Value(const std::vector<Value>&)> impl) {
    auto fn = GarbageCollector::makeGC<Function>();
    fn->isNative = true;
    fn->properties["__uses_this_arg__"] = Value(true);
    fn->properties["name"] = Value(name);
    fn->properties["__non_writable_name"] = Value(true);
    fn->properties["__non_enum_name"] = Value(true);
    fn->properties["length"] = Value(static_cast<double>(length));
    fn->properties["__non_writable_length"] = Value(true);
    fn->properties["__non_enum_length"] = Value(true);
    fn->nativeFunc = impl;
    setPrototype->properties[name] = Value(fn);
    setPrototype->properties["__non_enum_" + name] = Value(true);
  };

  defineSetMethod("add", 1, [validateSetThis](const std::vector<Value>& args) -> Value {
    auto s = validateSetThis(args, "add");
    Value val = args.size() > 1 ? args[1] : Value(Undefined{});
    s->add(val);
    return Value(s);
  });

  defineSetMethod("has", 1, [validateSetThis](const std::vector<Value>& args) -> Value {
    auto s = validateSetThis(args, "has");
    if (args.size() < 2) return Value(s->has(Value(Undefined{})));
    return Value(s->has(args[1]));
  });

  defineSetMethod("delete", 1, [validateSetThis](const std::vector<Value>& args) -> Value {
    auto s = validateSetThis(args, "delete");
    if (args.size() < 2) return Value(false);
    return Value(s->deleteValue(args[1]));
  });

  defineSetMethod("clear", 0, [validateSetThis](const std::vector<Value>& args) -> Value {
    auto s = validateSetThis(args, "clear");
    s->clear();
    return Value(Undefined{});
  });

  defineSetMethod("forEach", 1, [validateSetThis](const std::vector<Value>& args) -> Value {
    auto s = validateSetThis(args, "forEach");
    if (args.size() < 2 || !args[1].isFunction()) {
      throw std::runtime_error("TypeError: forEach requires a callback function");
    }
    auto* interp = getGlobalInterpreter();
    if (!interp) return Value(Undefined{});
    auto callback = args[1].getGC<Function>();
    Value thisArg = args.size() > 2 ? args[2] : Value(Undefined{});
    for (size_t i = 0; i < s->values.size(); ++i) {
      interp->callForHarness(Value(callback), {s->values[i], s->values[i], Value(s)}, thisArg);
    }
    return Value(Undefined{});
  });

  defineSetMethod("entries", 0, [validateSetThis](const std::vector<Value>& args) -> Value {
    auto s = validateSetThis(args, "entries");
    auto iterObj = GarbageCollector::makeGC<Object>();
    auto indexPtr = std::make_shared<size_t>(0);
    auto nextFn = GarbageCollector::makeGC<Function>();
    nextFn->isNative = true;
    nextFn->nativeFunc = [s, indexPtr](const std::vector<Value>&) -> Value {
      if (*indexPtr >= s->values.size()) {
        return makeIteratorResultObject(Value(Undefined{}), true);
      }
      auto& elem = s->values[*indexPtr];
      auto pair = GarbageCollector::makeGC<Array>();
      pair->elements.push_back(elem);
      pair->elements.push_back(elem);
      (*indexPtr)++;
      return makeIteratorResultObject(Value(pair), false);
    };
    iterObj->properties["next"] = Value(nextFn);
    return Value(iterObj);
  });

  defineSetMethod("keys", 0, [validateSetThis](const std::vector<Value>& args) -> Value {
    auto s = validateSetThis(args, "keys");
    auto iterObj = GarbageCollector::makeGC<Object>();
    auto indexPtr = std::make_shared<size_t>(0);
    auto nextFn = GarbageCollector::makeGC<Function>();
    nextFn->isNative = true;
    nextFn->nativeFunc = [s, indexPtr](const std::vector<Value>&) -> Value {
      if (*indexPtr >= s->values.size()) {
        return makeIteratorResultObject(Value(Undefined{}), true);
      }
      auto& elem = s->values[*indexPtr];
      (*indexPtr)++;
      return makeIteratorResultObject(elem, false);
    };
    iterObj->properties["next"] = Value(nextFn);
    return Value(iterObj);
  });

  defineSetMethod("values", 0, [validateSetThis](const std::vector<Value>& args) -> Value {
    auto s = validateSetThis(args, "values");
    auto iterObj = GarbageCollector::makeGC<Object>();
    auto indexPtr = std::make_shared<size_t>(0);
    auto nextFn = GarbageCollector::makeGC<Function>();
    nextFn->isNative = true;
    nextFn->nativeFunc = [s, indexPtr](const std::vector<Value>&) -> Value {
      if (*indexPtr >= s->values.size()) {
        return makeIteratorResultObject(Value(Undefined{}), true);
      }
      auto& elem = s->values[*indexPtr];
      (*indexPtr)++;
      return makeIteratorResultObject(elem, false);
    };
    iterObj->properties["next"] = Value(nextFn);
    return Value(iterObj);
  });

  // Set.prototype.keys = Set.prototype.values (spec: they are the same function)
  {
    auto it = setPrototype->properties.find("values");
    if (it != setPrototype->properties.end()) {
      setPrototype->properties["keys"] = it->second;
      setPrototype->properties["__non_enum_keys"] = Value(true);
    }
  }

  // Set.prototype[Symbol.iterator] = Set.prototype.values
  {
    auto it = setPrototype->properties.find("values");
    if (it != setPrototype->properties.end()) {
      setPrototype->properties[WellKnownSymbols::iteratorKey()] = it->second;
      setPrototype->properties["__non_enum_" + WellKnownSymbols::iteratorKey()] = Value(true);
    }
  }

  // Set.prototype.size as getter
  {
    auto sizeGetter = GarbageCollector::makeGC<Function>();
    sizeGetter->isNative = true;
    sizeGetter->properties["__uses_this_arg__"] = Value(true);
    sizeGetter->nativeFunc = [validateSetThis](const std::vector<Value>& args) -> Value {
      auto s = validateSetThis(args, "size");
      return Value(static_cast<double>(s->values.size()));
    };
    sizeGetter->properties["name"] = Value(std::string("get size"));
    sizeGetter->properties["__non_writable_name"] = Value(true);
    sizeGetter->properties["__non_enum_name"] = Value(true);
    sizeGetter->properties["length"] = Value(0.0);
    sizeGetter->properties["__non_writable_length"] = Value(true);
    sizeGetter->properties["__non_enum_length"] = Value(true);
    setPrototype->properties["__get_size"] = Value(sizeGetter);
    setPrototype->properties["__non_enum_size"] = Value(true);
  }

  // SetRecord: holds the interface for set-like objects
  struct SetRecord {
    Value obj;
    double size;
    Value has;
    Value keys;
  };

  // Helper: GetSetRecord(obj)
  auto getSetRecord = [](const Value& other) -> SetRecord {
    // Step 1: If obj is not an Object, throw TypeError
    if (other.isNull() || other.isUndefined() || other.isNumber() ||
        other.isString() || other.isBool() || other.isBigInt() || other.isSymbol()) {
      throw std::runtime_error("TypeError: GetSetRecord called on non-object");
    }
    auto* interp = getGlobalInterpreter();

    // Get "size" property via property lookup (handles getters, prototype chain)
    double sizeVal = 0;
    if (interp) {
      auto [found, sizeRaw] = interp->getPropertyForExternal(other, "size");
      if (!found) {
        throw std::runtime_error("TypeError: property 'size' is not a function");
      }
      if (sizeRaw.isNumber()) {
        sizeVal = std::get<double>(sizeRaw.data);
      } else {
        sizeVal = sizeRaw.toNumber();
      }
      if (std::isnan(sizeVal)) {
        throw std::runtime_error("TypeError: 'size' is NaN");
      }
    }

    // Get "has" method via property lookup
    Value hasMethod;
    if (interp) {
      auto [found, val] = interp->getPropertyForExternal(other, "has");
      if (found) hasMethod = val;
    }
    if (!hasMethod.isFunction()) {
      throw std::runtime_error("TypeError: property 'has' is not a function");
    }

    // Get "keys" method via property lookup
    Value keysMethod;
    if (interp) {
      auto [found, val] = interp->getPropertyForExternal(other, "keys");
      if (found) keysMethod = val;
    }
    if (!keysMethod.isFunction()) {
      throw std::runtime_error("TypeError: property 'keys' is not a function");
    }

    return SetRecord{other, sizeVal, hasMethod, keysMethod};
  };

  // Helper: call has(value) on a SetRecord
  auto setRecordHas = [](const SetRecord& rec, const Value& value) -> bool {
    auto* interp = getGlobalInterpreter();
    if (!interp) return false;
    Value result = interp->callForHarness(rec.has, {value}, rec.obj);
    return result.toBool();
  };

  // Helper: iterate keys from a SetRecord
  auto setRecordKeys = [](const SetRecord& rec) -> std::vector<Value> {
    auto* interp = getGlobalInterpreter();
    if (!interp) return {};
    std::vector<Value> result;
    // If it's a Set, get values directly
    if (rec.obj.isSet()) {
      auto s = rec.obj.getGC<Set>();
      return s->values;
    }
    // Otherwise call keys() and iterate
    Value iter = interp->callForHarness(rec.keys, {}, rec.obj);
    if (interp->hasError()) {
      interp->clearError();
      return {};
    }
    // Iterate the returned iterator
    int limit = 10000;
    while (limit-- > 0) {
      Value nextResult;
      if (iter.isObject()) {
        auto iterObj = iter.getGC<Object>();
        auto nextIt = iterObj->properties.find("next");
        if (nextIt != iterObj->properties.end() && nextIt->second.isFunction()) {
          nextResult = interp->callForHarness(nextIt->second, {}, iter);
        } else break;
      } else break;
      if (interp->hasError()) { interp->clearError(); break; }
      if (!nextResult.isObject()) break;
      auto resultObj = nextResult.getGC<Object>();
      auto doneIt = resultObj->properties.find("done");
      if (doneIt != resultObj->properties.end() && doneIt->second.toBool()) break;
      auto valueIt = resultObj->properties.find("value");
      if (valueIt != resultObj->properties.end()) {
        result.push_back(valueIt->second);
      }
    }
    return result;
  };

  // Helper to create a new Set with proper prototype
  auto makeNewSet = [setPrototype]() -> GCPtr<Set> {
    auto result = GarbageCollector::makeGC<Set>();
    GarbageCollector::instance().reportAllocation(sizeof(Set));
    result->properties["__proto__"] = Value(setPrototype);
    return result;
  };

  // Set.prototype.union(other)
  defineSetMethod("union", 1, [validateSetThis, getSetRecord, setRecordKeys, makeNewSet](const std::vector<Value>& args) -> Value {
    auto s = validateSetThis(args, "union");
    if (args.size() < 2) throw std::runtime_error("TypeError: union requires an argument");
    auto rec = getSetRecord(args[1]);
    auto result = makeNewSet();
    for (const auto& v : s->values) result->add(v);
    auto otherKeys = setRecordKeys(rec);
    for (const auto& v : otherKeys) result->add(v);
    return Value(result);
  });

  // Set.prototype.intersection(other)
  defineSetMethod("intersection", 1, [validateSetThis, getSetRecord, setRecordHas, setRecordKeys, makeNewSet](const std::vector<Value>& args) -> Value {
    auto s = validateSetThis(args, "intersection");
    if (args.size() < 2) throw std::runtime_error("TypeError: intersection requires an argument");
    auto rec = getSetRecord(args[1]);
    auto result = makeNewSet();
    if (static_cast<double>(s->values.size()) <= rec.size) {
      for (const auto& v : s->values) {
        if (setRecordHas(rec, v)) result->add(v);
      }
    } else {
      auto otherKeys = setRecordKeys(rec);
      for (const auto& v : otherKeys) {
        if (s->has(v)) result->add(v);
      }
    }
    return Value(result);
  });

  // Set.prototype.difference(other)
  defineSetMethod("difference", 1, [validateSetThis, getSetRecord, setRecordHas, setRecordKeys, makeNewSet](const std::vector<Value>& args) -> Value {
    auto s = validateSetThis(args, "difference");
    if (args.size() < 2) throw std::runtime_error("TypeError: difference requires an argument");
    auto rec = getSetRecord(args[1]);
    auto result = makeNewSet();
    if (static_cast<double>(s->values.size()) <= rec.size) {
      for (const auto& v : s->values) {
        if (!setRecordHas(rec, v)) result->add(v);
      }
    } else {
      for (const auto& v : s->values) result->add(v);
      auto otherKeys = setRecordKeys(rec);
      for (const auto& v : otherKeys) {
        result->deleteValue(v);
      }
    }
    return Value(result);
  });

  // Set.prototype.symmetricDifference(other)
  defineSetMethod("symmetricDifference", 1, [validateSetThis, getSetRecord, setRecordKeys, makeNewSet](const std::vector<Value>& args) -> Value {
    auto s = validateSetThis(args, "symmetricDifference");
    if (args.size() < 2) throw std::runtime_error("TypeError: symmetricDifference requires an argument");
    auto rec = getSetRecord(args[1]);
    auto result = makeNewSet();
    for (const auto& v : s->values) result->add(v);
    auto otherKeys = setRecordKeys(rec);
    for (const auto& v : otherKeys) {
      if (s->has(v)) {
        result->deleteValue(v);
      } else {
        result->add(v);
      }
    }
    return Value(result);
  });

  // Set.prototype.isSubsetOf(other)
  defineSetMethod("isSubsetOf", 1, [validateSetThis, getSetRecord, setRecordHas](const std::vector<Value>& args) -> Value {
    auto s = validateSetThis(args, "isSubsetOf");
    if (args.size() < 2) throw std::runtime_error("TypeError: isSubsetOf requires an argument");
    auto rec = getSetRecord(args[1]);
    if (static_cast<double>(s->values.size()) > rec.size) return Value(false);
    for (const auto& v : s->values) {
      if (!setRecordHas(rec, v)) return Value(false);
    }
    return Value(true);
  });

  // Set.prototype.isSupersetOf(other)
  defineSetMethod("isSupersetOf", 1, [validateSetThis, getSetRecord, setRecordHas, setRecordKeys](const std::vector<Value>& args) -> Value {
    auto s = validateSetThis(args, "isSupersetOf");
    if (args.size() < 2) throw std::runtime_error("TypeError: isSupersetOf requires an argument");
    auto rec = getSetRecord(args[1]);
    auto otherKeys = setRecordKeys(rec);
    for (const auto& v : otherKeys) {
      if (!s->has(v)) return Value(false);
    }
    return Value(true);
  });

  // Set.prototype.isDisjointFrom(other)
  defineSetMethod("isDisjointFrom", 1, [validateSetThis, getSetRecord, setRecordHas, setRecordKeys](const std::vector<Value>& args) -> Value {
    auto s = validateSetThis(args, "isDisjointFrom");
    if (args.size() < 2) throw std::runtime_error("TypeError: isDisjointFrom requires an argument");
    auto rec = getSetRecord(args[1]);
    if (static_cast<double>(s->values.size()) <= rec.size) {
      for (const auto& v : s->values) {
        if (setRecordHas(rec, v)) return Value(false);
      }
    } else {
      auto otherKeys = setRecordKeys(rec);
      for (const auto& v : otherKeys) {
        if (s->has(v)) return Value(false);
      }
    }
    return Value(true);
  });

  env->define("Set", Value(setConstructor));

  // WeakMap constructor
  auto weakMapConstructor = GarbageCollector::makeGC<Function>();
  weakMapConstructor->isNative = true;
  weakMapConstructor->isConstructor = true;
  weakMapConstructor->properties["name"] = Value(std::string("WeakMap"));
  weakMapConstructor->properties["__non_writable_name"] = Value(true);
  weakMapConstructor->properties["__non_enum_name"] = Value(true);
  weakMapConstructor->properties["length"] = Value(0.0);
  weakMapConstructor->properties["__non_writable_length"] = Value(true);
  weakMapConstructor->properties["__non_enum_length"] = Value(true);
  weakMapConstructor->properties["__require_new__"] = Value(true);
  weakMapConstructor->nativeFunc = [](const std::vector<Value>&) -> Value {
    auto wm = GarbageCollector::makeGC<WeakMap>();
    GarbageCollector::instance().reportAllocation(sizeof(WeakMap));
    return Value(wm);
  };
  {
    auto weakMapPrototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    weakMapConstructor->properties["prototype"] = Value(weakMapPrototype);
    weakMapPrototype->properties["constructor"] = Value(weakMapConstructor);
    weakMapPrototype->properties["__non_enum_constructor"] = Value(true);
    weakMapPrototype->properties[WellKnownSymbols::toStringTagKey()] = Value(std::string("WeakMap"));
    weakMapPrototype->properties["__non_writable_" + WellKnownSymbols::toStringTagKey()] = Value(true);
    weakMapPrototype->properties["__non_enum_" + WellKnownSymbols::toStringTagKey()] = Value(true);

    auto validateWeakMapThis = [](const std::vector<Value>& args, const std::string& methodName) -> GCPtr<WeakMap> {
      if (args.empty() || !args[0].isWeakMap()) {
        throw std::runtime_error("TypeError: Method WeakMap.prototype." + methodName + " called on incompatible receiver");
      }
      return args[0].getGC<WeakMap>();
    };

    auto defineWMMethod = [&](const std::string& name, int length, std::function<Value(const std::vector<Value>&)> impl) {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->properties["__uses_this_arg__"] = Value(true);
      fn->properties["name"] = Value(name);
      fn->properties["__non_writable_name"] = Value(true);
      fn->properties["__non_enum_name"] = Value(true);
      fn->properties["length"] = Value(static_cast<double>(length));
      fn->properties["__non_writable_length"] = Value(true);
      fn->properties["__non_enum_length"] = Value(true);
      fn->nativeFunc = impl;
      weakMapPrototype->properties[name] = Value(fn);
      weakMapPrototype->properties["__non_enum_" + name] = Value(true);
    };

    defineWMMethod("get", 1, [validateWeakMapThis](const std::vector<Value>& args) -> Value {
      auto wm = validateWeakMapThis(args, "get");
      if (args.size() < 2) return Value(Undefined{});
      return wm->get(args[1]);
    });
    defineWMMethod("set", 2, [validateWeakMapThis](const std::vector<Value>& args) -> Value {
      auto wm = validateWeakMapThis(args, "set");
      Value key = args.size() > 1 ? args[1] : Value(Undefined{});
      Value val = args.size() > 2 ? args[2] : Value(Undefined{});
      wm->set(key, val);
      return Value(wm);
    });
    defineWMMethod("has", 1, [validateWeakMapThis](const std::vector<Value>& args) -> Value {
      auto wm = validateWeakMapThis(args, "has");
      if (args.size() < 2) return Value(false);
      return Value(wm->has(args[1]));
    });
    defineWMMethod("delete", 1, [validateWeakMapThis](const std::vector<Value>& args) -> Value {
      auto wm = validateWeakMapThis(args, "delete");
      if (args.size() < 2) return Value(false);
      return Value(wm->deleteKey(args[1]));
    });
  }
  env->define("WeakMap", Value(weakMapConstructor));

  // WeakSet constructor
  auto weakSetConstructor = GarbageCollector::makeGC<Function>();
  weakSetConstructor->isNative = true;
  weakSetConstructor->isConstructor = true;
  weakSetConstructor->properties["name"] = Value(std::string("WeakSet"));
  weakSetConstructor->properties["__non_writable_name"] = Value(true);
  weakSetConstructor->properties["__non_enum_name"] = Value(true);
  weakSetConstructor->properties["length"] = Value(0.0);
  weakSetConstructor->properties["__non_writable_length"] = Value(true);
  weakSetConstructor->properties["__non_enum_length"] = Value(true);
  weakSetConstructor->properties["__require_new__"] = Value(true);
  weakSetConstructor->nativeFunc = [](const std::vector<Value>&) -> Value {
    auto ws = GarbageCollector::makeGC<WeakSet>();
    GarbageCollector::instance().reportAllocation(sizeof(WeakSet));
    return Value(ws);
  };
  {
    auto weakSetPrototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    weakSetConstructor->properties["prototype"] = Value(weakSetPrototype);
    weakSetPrototype->properties["constructor"] = Value(weakSetConstructor);
    weakSetPrototype->properties["__non_enum_constructor"] = Value(true);
    weakSetPrototype->properties[WellKnownSymbols::toStringTagKey()] = Value(std::string("WeakSet"));
    weakSetPrototype->properties["__non_writable_" + WellKnownSymbols::toStringTagKey()] = Value(true);
    weakSetPrototype->properties["__non_enum_" + WellKnownSymbols::toStringTagKey()] = Value(true);

    auto validateWeakSetThis = [](const std::vector<Value>& args, const std::string& methodName) -> GCPtr<WeakSet> {
      if (args.empty() || !args[0].isWeakSet()) {
        throw std::runtime_error("TypeError: Method WeakSet.prototype." + methodName + " called on incompatible receiver");
      }
      return args[0].getGC<WeakSet>();
    };

    auto defineWSMethod = [&](const std::string& name, int length, std::function<Value(const std::vector<Value>&)> impl) {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->properties["__uses_this_arg__"] = Value(true);
      fn->properties["name"] = Value(name);
      fn->properties["__non_writable_name"] = Value(true);
      fn->properties["__non_enum_name"] = Value(true);
      fn->properties["length"] = Value(static_cast<double>(length));
      fn->properties["__non_writable_length"] = Value(true);
      fn->properties["__non_enum_length"] = Value(true);
      fn->nativeFunc = impl;
      weakSetPrototype->properties[name] = Value(fn);
      weakSetPrototype->properties["__non_enum_" + name] = Value(true);
    };

    defineWSMethod("add", 1, [validateWeakSetThis](const std::vector<Value>& args) -> Value {
      auto ws = validateWeakSetThis(args, "add");
      if (args.size() < 2) throw std::runtime_error("TypeError: Invalid value used in weak set");
      ws->add(args[1]);
      return Value(ws);
    });
    defineWSMethod("has", 1, [validateWeakSetThis](const std::vector<Value>& args) -> Value {
      auto ws = validateWeakSetThis(args, "has");
      if (args.size() < 2) return Value(false);
      return Value(ws->has(args[1]));
    });
    defineWSMethod("delete", 1, [validateWeakSetThis](const std::vector<Value>& args) -> Value {
      auto ws = validateWeakSetThis(args, "delete");
      if (args.size() < 2) return Value(false);
      return Value(ws->deleteValue(args[1]));
    });
  }
  env->define("WeakSet", Value(weakSetConstructor));

  // WeakRef (ES2021)
  auto weakRefConstructor = GarbageCollector::makeGC<Function>();
  weakRefConstructor->isNative = true;
  weakRefConstructor->isConstructor = true;
  weakRefConstructor->properties["name"] = Value(std::string("WeakRef"));
  weakRefConstructor->properties["__non_writable_name"] = Value(true);
  weakRefConstructor->properties["__non_enum_name"] = Value(true);
  weakRefConstructor->properties["length"] = Value(1.0);
  weakRefConstructor->properties["__non_writable_length"] = Value(true);
  weakRefConstructor->properties["__non_enum_length"] = Value(true);
  weakRefConstructor->properties["__require_new__"] = Value(true);
  weakRefConstructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto canBeHeldWeakly = [&globalSymbolRegistry](const Value& v) -> bool {
      if (v.isSymbol()) {
        // Registered symbols (from Symbol.for) can't be held weakly
        const auto& sym = std::get<Symbol>(v.data);
        for (const auto& [key, val] : globalSymbolRegistry) {
          if (val.isSymbol() && std::get<Symbol>(val.data).id == sym.id) {
            return false;
          }
        }
        return true;
      }
      return v.isObject() || v.isArray() || v.isFunction() ||
             v.isClass() || v.isMap() || v.isSet() || v.isError() || v.isRegex() ||
             v.isProxy() || v.isPromise() || v.isGenerator() || v.isTypedArray() ||
             v.isArrayBuffer() || v.isDataView();
    };
    if (args.empty() || !canBeHeldWeakly(args[0])) {
      throw std::runtime_error("TypeError: WeakRef target must be an object or symbol");
    }
    auto obj = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    obj->properties["__weakref_target__"] = args[0];
    return Value(obj);
  };
  {
    auto weakRefPrototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    weakRefConstructor->properties["prototype"] = Value(weakRefPrototype);
    weakRefConstructor->properties["__non_writable_prototype"] = Value(true);
    weakRefConstructor->properties["__non_enum_prototype"] = Value(true);
    weakRefConstructor->properties["__non_configurable_prototype"] = Value(true);
    weakRefPrototype->properties["constructor"] = Value(weakRefConstructor);
    weakRefPrototype->properties["__non_enum_constructor"] = Value(true);

    // WeakRef.prototype.deref()
    auto derefFn = GarbageCollector::makeGC<Function>();
    derefFn->isNative = true;
    derefFn->properties["__uses_this_arg__"] = Value(true);
    derefFn->properties["name"] = Value(std::string("deref"));
    derefFn->properties["__non_writable_name"] = Value(true);
    derefFn->properties["__non_enum_name"] = Value(true);
    derefFn->properties["length"] = Value(0.0);
    derefFn->properties["__non_writable_length"] = Value(true);
    derefFn->properties["__non_enum_length"] = Value(true);
    derefFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isObject()) {
        throw std::runtime_error("TypeError: WeakRef.prototype.deref called on incompatible receiver");
      }
      auto obj = args[0].getGC<Object>();
      auto it = obj->properties.find("__weakref_target__");
      if (it == obj->properties.end()) {
        throw std::runtime_error("TypeError: WeakRef.prototype.deref called on incompatible receiver");
      }
      return it->second;
    };
    weakRefPrototype->properties["deref"] = Value(derefFn);
    weakRefPrototype->properties["__non_enum_deref"] = Value(true);

    // WeakRef.prototype[Symbol.toStringTag] = "WeakRef"
    weakRefPrototype->properties[WellKnownSymbols::toStringTagKey()] = Value(std::string("WeakRef"));
    weakRefPrototype->properties["__non_writable_" + WellKnownSymbols::toStringTagKey()] = Value(true);
    weakRefPrototype->properties["__non_enum_" + WellKnownSymbols::toStringTagKey()] = Value(true);
  }
  env->define("WeakRef", Value(weakRefConstructor));

  // FinalizationRegistry (ES2021)
  auto finRegConstructor = GarbageCollector::makeGC<Function>();
  finRegConstructor->isNative = true;
  finRegConstructor->isConstructor = true;
  finRegConstructor->properties["name"] = Value(std::string("FinalizationRegistry"));
  finRegConstructor->properties["__non_writable_name"] = Value(true);
  finRegConstructor->properties["__non_enum_name"] = Value(true);
  finRegConstructor->properties["length"] = Value(1.0);
  finRegConstructor->properties["__non_writable_length"] = Value(true);
  finRegConstructor->properties["__non_enum_length"] = Value(true);
  finRegConstructor->properties["__require_new__"] = Value(true);
  finRegConstructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isFunction()) {
      throw std::runtime_error("TypeError: FinalizationRegistry requires a cleanup callback");
    }
    auto obj = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    obj->properties["__fr_callback__"] = args[0];
    obj->properties["__fr_cells__"] = Value(GarbageCollector::makeGC<Array>());
    return Value(obj);
  };
  {
    auto finRegPrototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    finRegConstructor->properties["prototype"] = Value(finRegPrototype);
    finRegConstructor->properties["__non_writable_prototype"] = Value(true);
    finRegConstructor->properties["__non_enum_prototype"] = Value(true);
    finRegConstructor->properties["__non_configurable_prototype"] = Value(true);
    finRegPrototype->properties["constructor"] = Value(finRegConstructor);
    finRegPrototype->properties["__non_enum_constructor"] = Value(true);

    auto isFinReg = [](const Value& v) -> bool {
      if (!v.isObject()) return false;
      auto obj = v.getGC<Object>();
      return obj->properties.find("__fr_callback__") != obj->properties.end();
    };

    auto canBeHeldWeakly = [&globalSymbolRegistry](const Value& v) -> bool {
      if (v.isSymbol()) {
        // Registered symbols (from Symbol.for) can't be held weakly
        const auto& sym = std::get<Symbol>(v.data);
        for (const auto& [key, val] : globalSymbolRegistry) {
          if (val.isSymbol() && std::get<Symbol>(val.data).id == sym.id) {
            return false;
          }
        }
        return true;
      }
      return v.isObject() || v.isArray() || v.isFunction() ||
             v.isClass() || v.isMap() || v.isSet() || v.isError() || v.isRegex() ||
             v.isProxy() || v.isPromise() || v.isGenerator() || v.isTypedArray() ||
             v.isArrayBuffer() || v.isDataView();
    };

    // FinalizationRegistry.prototype.register(target, heldValue [, unregisterToken])
    auto registerFn = GarbageCollector::makeGC<Function>();
    registerFn->isNative = true;
    registerFn->properties["__uses_this_arg__"] = Value(true);
    registerFn->properties["name"] = Value(std::string("register"));
    registerFn->properties["__non_writable_name"] = Value(true);
    registerFn->properties["__non_enum_name"] = Value(true);
    registerFn->properties["length"] = Value(2.0);
    registerFn->properties["__non_writable_length"] = Value(true);
    registerFn->properties["__non_enum_length"] = Value(true);
    registerFn->nativeFunc = [isFinReg, canBeHeldWeakly](const std::vector<Value>& args) -> Value {
      // args[0] = this, args[1] = target, args[2] = heldValue, args[3] = unregisterToken
      if (args.empty() || !isFinReg(args[0])) {
        throw std::runtime_error("TypeError: FinalizationRegistry.prototype.register called on incompatible receiver");
      }
      if (args.size() < 2 || !canBeHeldWeakly(args[1])) {
        throw std::runtime_error("TypeError: FinalizationRegistry.prototype.register: target must be an object or symbol");
      }
      Value heldValue = args.size() > 2 ? args[2] : Value(Undefined{});
      // ES spec: SameValue(target, heldValue) must be false
      if (args.size() > 2) {
        bool same = false;
        const auto& target = args[1];
        const auto& held = args[2];
        if (target.isSymbol() && held.isSymbol()) {
          same = std::get<Symbol>(target.data).id == std::get<Symbol>(held.data).id;
        } else if (target.isObject() && held.isObject()) {
          same = target.getGC<Object>().get() == held.getGC<Object>().get();
        } else if (target.isArray() && held.isArray()) {
          same = target.getGC<Array>().get() == held.getGC<Array>().get();
        } else if (target.isFunction() && held.isFunction()) {
          same = target.getGC<Function>().get() == held.getGC<Function>().get();
        } else if (target.isMap() && held.isMap()) {
          same = target.getGC<Map>().get() == held.getGC<Map>().get();
        } else if (target.isSet() && held.isSet()) {
          same = target.getGC<Set>().get() == held.getGC<Set>().get();
        }
        if (same) {
          throw std::runtime_error("TypeError: FinalizationRegistry.prototype.register: target and heldValue must not be the same");
        }
      }
      Value unregisterToken = args.size() > 3 ? args[3] : Value(Undefined{});
      // unregisterToken, if provided and not undefined, must be an object or symbol
      if (!unregisterToken.isUndefined() && !canBeHeldWeakly(unregisterToken)) {
        throw std::runtime_error("TypeError: FinalizationRegistry.prototype.register: unregisterToken must be an object, symbol, or undefined");
      }
      auto thisObj = args[0].getGC<Object>();
      auto cellsIt = thisObj->properties.find("__fr_cells__");
      if (cellsIt != thisObj->properties.end() && cellsIt->second.isArray()) {
        auto cells = cellsIt->second.getGC<Array>();
        auto entry = GarbageCollector::makeGC<Object>();
        entry->properties["target"] = args[1];
        entry->properties["heldValue"] = heldValue;
        if (!unregisterToken.isUndefined()) {
          entry->properties["unregisterToken"] = unregisterToken;
        }
        cells->elements.push_back(Value(entry));
      }
      return Value(Undefined{});
    };
    finRegPrototype->properties["register"] = Value(registerFn);
    finRegPrototype->properties["__non_enum_register"] = Value(true);

    // FinalizationRegistry.prototype.unregister(unregisterToken)
    auto unregisterFn = GarbageCollector::makeGC<Function>();
    unregisterFn->isNative = true;
    unregisterFn->properties["__uses_this_arg__"] = Value(true);
    unregisterFn->properties["name"] = Value(std::string("unregister"));
    unregisterFn->properties["__non_writable_name"] = Value(true);
    unregisterFn->properties["__non_enum_name"] = Value(true);
    unregisterFn->properties["length"] = Value(1.0);
    unregisterFn->properties["__non_writable_length"] = Value(true);
    unregisterFn->properties["__non_enum_length"] = Value(true);
    unregisterFn->nativeFunc = [isFinReg, canBeHeldWeakly](const std::vector<Value>& args) -> Value {
      // args[0] = this, args[1] = unregisterToken
      if (args.empty() || !isFinReg(args[0])) {
        throw std::runtime_error("TypeError: FinalizationRegistry.prototype.unregister called on incompatible receiver");
      }
      if (args.size() < 2 || !canBeHeldWeakly(args[1])) {
        throw std::runtime_error("TypeError: FinalizationRegistry.prototype.unregister: unregisterToken must be an object or symbol");
      }
      auto thisObj = args[0].getGC<Object>();
      auto cellsIt = thisObj->properties.find("__fr_cells__");
      bool removed = false;
      if (cellsIt != thisObj->properties.end() && cellsIt->second.isArray()) {
        auto cells = cellsIt->second.getGC<Array>();
        const auto& token = args[1];
        std::vector<Value> remaining;
        auto sameValue = [](const Value& a, const Value& b) -> bool {
          if (a.isSymbol() && b.isSymbol()) {
            return std::get<Symbol>(a.data).id == std::get<Symbol>(b.data).id;
          }
          if (a.isObject() && b.isObject()) return a.getGC<Object>().get() == b.getGC<Object>().get();
          if (a.isArray() && b.isArray()) return a.getGC<Array>().get() == b.getGC<Array>().get();
          if (a.isFunction() && b.isFunction()) return a.getGC<Function>().get() == b.getGC<Function>().get();
          if (a.isMap() && b.isMap()) return a.getGC<Map>().get() == b.getGC<Map>().get();
          if (a.isSet() && b.isSet()) return a.getGC<Set>().get() == b.getGC<Set>().get();
          return false;
        };
        for (auto& cell : cells->elements) {
          if (cell.isObject()) {
            auto entry = cell.getGC<Object>();
            auto tokenIt = entry->properties.find("unregisterToken");
            if (tokenIt != entry->properties.end() && sameValue(tokenIt->second, token)) {
              removed = true;
              continue;  // skip this entry
            }
          }
          remaining.push_back(cell);
        }
        cells->elements = remaining;
      }
      return Value(removed);
    };
    finRegPrototype->properties["unregister"] = Value(unregisterFn);
    finRegPrototype->properties["__non_enum_unregister"] = Value(true);

    // FinalizationRegistry.prototype.cleanupSome([callback])
    auto cleanupSomeFn = GarbageCollector::makeGC<Function>();
    cleanupSomeFn->isNative = true;
    cleanupSomeFn->properties["__uses_this_arg__"] = Value(true);
    cleanupSomeFn->properties["name"] = Value(std::string("cleanupSome"));
    cleanupSomeFn->properties["__non_writable_name"] = Value(true);
    cleanupSomeFn->properties["__non_enum_name"] = Value(true);
    cleanupSomeFn->properties["length"] = Value(0.0);
    cleanupSomeFn->properties["__non_writable_length"] = Value(true);
    cleanupSomeFn->properties["__non_enum_length"] = Value(true);
    cleanupSomeFn->nativeFunc = [isFinReg](const std::vector<Value>& args) -> Value {
      if (args.empty() || !isFinReg(args[0])) {
        throw std::runtime_error("TypeError: FinalizationRegistry.prototype.cleanupSome called on incompatible receiver");
      }
      return Value(Undefined{});
    };
    finRegPrototype->properties["cleanupSome"] = Value(cleanupSomeFn);
    finRegPrototype->properties["__non_enum_cleanupSome"] = Value(true);

    // FinalizationRegistry.prototype[Symbol.toStringTag]
    finRegPrototype->properties[WellKnownSymbols::toStringTagKey()] = Value(std::string("FinalizationRegistry"));
    finRegPrototype->properties["__non_writable_" + WellKnownSymbols::toStringTagKey()] = Value(true);
    finRegPrototype->properties["__non_enum_" + WellKnownSymbols::toStringTagKey()] = Value(true);
  }
  env->define("FinalizationRegistry", Value(finRegConstructor));

  // Proxy constructor
  auto proxyConstructor = GarbageCollector::makeGC<Function>();
  proxyConstructor->isNative = true;
  proxyConstructor->isConstructor = true;
  auto isCallableTarget = [](const Value& target) -> bool {
    if (target.isFunction() || target.isClass()) return true;
    if (target.isObject()) {
      auto obj = target.getGC<Object>();
      auto it = obj->properties.find("__callable_object__");
      return it != obj->properties.end() && it->second.isBool() && it->second.toBool();
    }
    if (target.isProxy()) {
      auto p = target.getGC<Proxy>();
      return p && p->isCallable;
    }
    return false;
  };
  proxyConstructor->nativeFunc = [isCallableTarget](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) {
      // Need target and handler
      return Value(Undefined{});
    }
    auto proxy = GarbageCollector::makeGC<Proxy>(args[0], args[1]);
    GarbageCollector::instance().reportAllocation(sizeof(Proxy));
    proxy->isCallable = isCallableTarget(args[0]);
    return Value(proxy);
  };

  // Proxy.revocable(target, handler)
  auto proxyRevocable = GarbageCollector::makeGC<Function>();
  proxyRevocable->isNative = true;
  proxyRevocable->nativeFunc = [isCallableTarget](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) {
      return Value(Undefined{});
    }
    auto proxy = GarbageCollector::makeGC<Proxy>(args[0], args[1]);
    GarbageCollector::instance().reportAllocation(sizeof(Proxy));
    proxy->isCallable = isCallableTarget(args[0]);

    auto revokeFn = GarbageCollector::makeGC<Function>();
    revokeFn->isNative = true;
    revokeFn->nativeFunc = [proxy](const std::vector<Value>&) -> Value {
      if (proxy) {
        proxy->revoked = true;
        if (proxy->target) *proxy->target = Value(Undefined{});
        if (proxy->handler) *proxy->handler = Value(Undefined{});
      }
      return Value(Undefined{});
    };

    auto result = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    result->properties["proxy"] = Value(proxy);
    result->properties["revoke"] = Value(revokeFn);
    return Value(result);
  };
  proxyConstructor->properties["revocable"] = Value(proxyRevocable);
  env->define("Proxy", Value(proxyConstructor));

  // Reflect object with static methods
  auto reflectObj = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // Reflect.get(target, property)
  auto reflectGet = GarbageCollector::makeGC<Function>();
  reflectGet->isNative = true;
  reflectGet->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(Undefined{});

    const Value& target = args[0];
    std::string prop = valueToPropertyKey(args[1]);

    if (target.isObject()) {
      auto obj = target.getGC<Object>();
      if (obj->isModuleNamespace) {
        auto getterIt = obj->properties.find("__get_" + prop);
        if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
          Interpreter* interpreter = getGlobalInterpreter();
          if (interpreter) {
            Value out = interpreter->callForHarness(getterIt->second, {}, target);
            if (interpreter->hasError()) {
              Value err = interpreter->getError();
              interpreter->clearError();
              throw std::runtime_error(err.toString());
            }
            return out;
          }
        }
      }
      auto it = obj->properties.find(prop);
      if (it != obj->properties.end()) {
        return it->second;
      }
    }
    return Value(Undefined{});
  };
  reflectObj->properties["get"] = Value(reflectGet);

  // Reflect.set(target, property, value)
  auto reflectSet = GarbageCollector::makeGC<Function>();
  reflectSet->isNative = true;
  reflectSet->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 3) return Value(false);

    const Value& target = args[0];
    std::string prop = valueToPropertyKey(args[1]);
    const Value& value = args[2];
    const Value& receiver = args.size() > 3 ? args[3] : target;

    if (isObjectLikeValue(target)) {
      if (target.isObject()) {
      auto obj = target.getGC<Object>();
      if (obj->isModuleNamespace) {
        return Value(false);
      }
      }
      if (receiver.isProxy()) {
        auto proxy = receiver.getGC<Proxy>();
        if (proxy->handler && proxy->handler->isObject()) {
          auto handlerObj = proxy->handler->getGC<Object>();
          Interpreter* interpreter = getGlobalInterpreter();

          auto getOwnPropertyDescriptorIt = handlerObj->properties.find("getOwnPropertyDescriptor");
          if (getOwnPropertyDescriptorIt != handlerObj->properties.end() &&
              getOwnPropertyDescriptorIt->second.isFunction() &&
              interpreter && proxy->target) {
            interpreter->callForHarness(
              getOwnPropertyDescriptorIt->second,
              {*proxy->target, Value(prop)},
              Value(handlerObj));
            if (interpreter->hasError()) {
              Value err = interpreter->getError();
              interpreter->clearError();
              throw std::runtime_error(err.toString());
            }
          }

          auto definePropertyIt = handlerObj->properties.find("defineProperty");
          if (definePropertyIt != handlerObj->properties.end() &&
              definePropertyIt->second.isFunction() &&
              interpreter && proxy->target) {
            auto descriptor = GarbageCollector::makeGC<Object>();
            descriptor->properties["value"] = value;
            Value result = interpreter->callForHarness(
              definePropertyIt->second,
              {*proxy->target, Value(prop), Value(descriptor)},
              Value(handlerObj));
            if (interpreter->hasError()) {
              Value err = interpreter->getError();
              interpreter->clearError();
              throw std::runtime_error(err.toString());
            }
            return Value(result.toBool());
          }
        }
      }
      return Value(setPropertyLike(receiver, prop, value));
    }
    return Value(false);
  };
  reflectObj->properties["set"] = Value(reflectSet);

  // Reflect.has(target, property)
  auto reflectHas = GarbageCollector::makeGC<Function>();
  reflectHas->isNative = true;
  reflectHas->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(false);

    const Value& target = args[0];
    std::string prop = valueToPropertyKey(args[1]);

    if (target.isObject()) {
      auto obj = target.getGC<Object>();
      if (obj->isModuleNamespace) {
        if (prop == WellKnownSymbols::toStringTagKey()) {
          return Value(true);
        }
        return Value(isModuleNamespaceExportKey(obj, prop));
      }
      return Value(obj->properties.find(prop) != obj->properties.end());
    }
    return Value(false);
  };
  reflectObj->properties["has"] = Value(reflectHas);

  // Reflect.deleteProperty(target, property)
  auto reflectDelete = GarbageCollector::makeGC<Function>();
  reflectDelete->isNative = true;
  reflectDelete->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(false);

    const Value& target = args[0];
    std::string prop = valueToPropertyKey(args[1]);

    if (target.isObject()) {
      auto obj = target.getGC<Object>();
      if (obj->isModuleNamespace) {
        if (prop == WellKnownSymbols::toStringTagKey()) {
          return Value(false);
        }
        return Value(!isModuleNamespaceExportKey(obj, prop));
      }
      return Value(obj->properties.erase(prop) > 0);
    }
    return Value(false);
  };
  reflectObj->properties["deleteProperty"] = Value(reflectDelete);

  // Reflect.apply(target, thisArg, argumentsList)
  auto reflectApply = GarbageCollector::makeGC<Function>();
  reflectApply->isNative = true;
  reflectApply->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 3 || !args[0].isFunction()) {
      throw std::runtime_error("TypeError: Reflect.apply target is not a function");
    }

    auto func = args[0].getGC<Function>();
    const Value& thisArg = args[1];

    std::vector<Value> callArgs;
    if (args[2].isArray()) {
      auto arr = args[2].getGC<Array>();
      callArgs = arr->elements;
    }

    if (func->isNative) {
      auto usesThisIt = func->properties.find("__uses_this_arg__");
      bool usesThis = usesThisIt != func->properties.end() &&
                      usesThisIt->second.isBool() &&
                      usesThisIt->second.toBool();
      if (usesThis) {
        std::vector<Value> nativeArgs;
        nativeArgs.reserve(callArgs.size() + 1);
        nativeArgs.push_back(thisArg);
        nativeArgs.insert(nativeArgs.end(), callArgs.begin(), callArgs.end());
        return func->nativeFunc(nativeArgs);
      }
      return func->nativeFunc(callArgs);
    }

    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) {
      throw std::runtime_error("TypeError: Interpreter unavailable");
    }
    Value out = interpreter->callForHarness(Value(func), callArgs, thisArg);
    if (interpreter->hasError()) {
      Value err = interpreter->getError();
      interpreter->clearError();
      throw JsValueException(err);
    }
    return out;
  };
  reflectObj->properties["apply"] = Value(reflectApply);

  // Reflect.construct(target, argumentsList, newTarget?)
  auto reflectConstruct = GarbageCollector::makeGC<Function>();
  reflectConstruct->isNative = true;
  reflectConstruct->properties["__reflect_construct__"] = Value(true);
  reflectConstruct->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2 || !args[0].isFunction()) {
      throw std::runtime_error("TypeError: Reflect.construct target is not a function");
    }

    auto func = args[0].getGC<Function>();
    if (!func->isConstructor) {
      throw std::runtime_error("TypeError: target is not a constructor");
    }

    if (args.size() >= 3) {
      if (!args[2].isFunction()) {
        throw std::runtime_error("TypeError: newTarget is not a constructor");
      }
      auto newTarget = args[2].getGC<Function>();
      if (!newTarget->isConstructor) {
        throw std::runtime_error("TypeError: newTarget is not a constructor");
      }
    }

    std::vector<Value> callArgs;
    if (args[1].isArray()) {
      auto arr = args[1].getGC<Array>();
      callArgs = arr->elements;
    }

    if (func->isNative) {
      return func->nativeFunc(callArgs);
    }
    // For non-native functions, we'd need interpreter access
    return Value(Undefined{});
  };
  reflectObj->properties["construct"] = Value(reflectConstruct);

  // Reflect.getPrototypeOf(target) - delegates to Object.getPrototypeOf logic
  auto reflectGetPrototypeOf = GarbageCollector::makeGC<Function>();
  reflectGetPrototypeOf->isNative = true;
  reflectGetPrototypeOf->nativeFunc = [env](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(Null{});
    if (args[0].isFunction()) {
      auto fn = args[0].getGC<Function>();
      auto protoIt = fn->properties.find("__proto__");
      if (protoIt != fn->properties.end()) return protoIt->second;
      if (auto funcCtor = env->get("Function"); funcCtor && funcCtor->isFunction()) {
        auto funcFn = std::get<GCPtr<Function>>(funcCtor->data);
        auto fpIt = funcFn->properties.find("prototype");
        if (fpIt != funcFn->properties.end()) return fpIt->second;
      }
    }
    if (args[0].isObject()) {
      auto obj = args[0].getGC<Object>();
      auto it = obj->properties.find("__proto__");
      if (it != obj->properties.end()) return it->second;
    }
    return Value(Null{});
  };
  reflectObj->properties["getPrototypeOf"] = Value(reflectGetPrototypeOf);

  // Reflect.setPrototypeOf(target, proto) - returns false (not supported)
  auto reflectSetPrototypeOf = GarbageCollector::makeGC<Function>();
  reflectSetPrototypeOf->isNative = true;
  reflectSetPrototypeOf->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // LightJS doesn't support dynamic prototype modification
    return Value(false);
  };
  reflectObj->properties["setPrototypeOf"] = Value(reflectSetPrototypeOf);

  // Reflect.isExtensible(target) - check if object can be extended
  auto reflectIsExtensible = GarbageCollector::makeGC<Function>();
  reflectIsExtensible->isNative = true;
  reflectIsExtensible->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(false);
    if (args[0].isClass()) {
      auto cls = args[0].getGC<Class>();
      auto it = cls->properties.find("__non_extensible__");
      bool nonExtensible = it != cls->properties.end() &&
                           it->second.isBool() &&
                           it->second.toBool();
      return Value(!nonExtensible);
    }
    if (args[0].isFunction() || args[0].isArray()) {
      return Value(true);  // Functions and arrays are always extensible
    }
    if (args[0].isTypedArray()) {
      auto ta = args[0].getGC<TypedArray>();
      return Value(ta->properties.find("__non_extensible__") == ta->properties.end());
    }
    if (args[0].isArrayBuffer()) {
      auto ab = args[0].getGC<ArrayBuffer>();
      return Value(ab->properties.find("__non_extensible__") == ab->properties.end());
    }
    if (args[0].isDataView()) {
      auto dv = args[0].getGC<DataView>();
      return Value(dv->properties.find("__non_extensible__") == dv->properties.end());
    }
    if (args[0].isObject()) {
      auto obj = args[0].getGC<Object>();
      if (obj->isModuleNamespace) {
        return Value(false);
      }
      return Value(!obj->sealed && !obj->frozen && !obj->nonExtensible);
    }
    return Value(false);
  };
  reflectObj->properties["isExtensible"] = Value(reflectIsExtensible);

  // Reflect.preventExtensions(target) - prevent adding new properties
  auto reflectPreventExtensions = GarbageCollector::makeGC<Function>();
  reflectPreventExtensions->isNative = true;
  reflectPreventExtensions->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(false);
    }
    if (args[0].isClass()) {
      auto cls = args[0].getGC<Class>();
      cls->properties["__non_extensible__"] = Value(true);
      return Value(true);
    }
    if (args[0].isTypedArray()) {
      args[0].getGC<TypedArray>()->properties["__non_extensible__"] = Value(true);
      return Value(true);
    }
    if (args[0].isArrayBuffer()) {
      args[0].getGC<ArrayBuffer>()->properties["__non_extensible__"] = Value(true);
      return Value(true);
    }
    if (args[0].isDataView()) {
      args[0].getGC<DataView>()->properties["__non_extensible__"] = Value(true);
      return Value(true);
    }
    if (!args[0].isObject()) {
      return Value(false);
    }
    auto obj = args[0].getGC<Object>();
    if (obj->isModuleNamespace) {
      return Value(true);
    }
    obj->nonExtensible = true;
    return Value(true);
  };
  reflectObj->properties["preventExtensions"] = Value(reflectPreventExtensions);

  // Reflect.ownKeys(target) - return array of own property keys
  auto reflectOwnKeys = GarbageCollector::makeGC<Function>();
  reflectOwnKeys->isNative = true;
  reflectOwnKeys->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto result = GarbageCollector::makeGC<Array>();
    auto appendPropertyKey = [&](const std::string& key) {
      Symbol symbolValue;
      if (propertyKeyToSymbol(key, symbolValue)) {
        result->elements.push_back(Value(symbolValue));
      } else {
        result->elements.push_back(Value(key));
      }
    };

    if (args.empty()) return Value(result);

    if (args[0].isObject()) {
      auto obj = args[0].getGC<Object>();
      if (obj->isModuleNamespace) {
        for (const auto& key : obj->moduleExportNames) {
          result->elements.push_back(Value(key));
        }
        result->elements.push_back(WellKnownSymbols::toStringTag());
        return Value(result);
      }
      for (const auto& key : obj->properties.orderedKeys()) {
        // Skip internal properties (__*__ and __get_/__set_/__non_enum_/etc.)
        if (key.size() >= 4 && key.substr(0, 2) == "__" &&
            key.substr(key.size() - 2) == "__") continue;
        if (key.size() >= 6 && (key.substr(0, 6) == "__get_" || key.substr(0, 6) == "__set_")) continue;
        if (key.size() >= 7 && key.substr(0, 7) == "__enum_") continue;
        if (key.size() >= 11 && key.substr(0, 11) == "__non_enum_") continue;
        if (key.size() >= 15 && key.substr(0, 15) == "__non_writable_") continue;
        if (key.size() >= 19 && key.substr(0, 19) == "__non_configurable_") continue;
        appendPropertyKey(key);
      }
    } else if (args[0].isArray()) {
      auto arr = args[0].getGC<Array>();
      for (size_t i = 0; i < arr->elements.size(); ++i) {
        result->elements.push_back(Value(std::to_string(i)));
      }
      result->elements.push_back(Value("length"));
    }

    return Value(result);
  };
  reflectObj->properties["ownKeys"] = Value(reflectOwnKeys);

  // Reflect.getOwnPropertyDescriptor(target, propertyKey)
  auto reflectGetOwnPropertyDescriptor = GarbageCollector::makeGC<Function>();
  reflectGetOwnPropertyDescriptor->isNative = true;
  reflectGetOwnPropertyDescriptor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(Undefined{});

    std::string prop = valueToPropertyKey(args[1]);

    if (args[0].isObject()) {
      auto obj = args[0].getGC<Object>();
      if (obj->isModuleNamespace) {
        auto descriptor = GarbageCollector::makeGC<Object>();
        if (prop == WellKnownSymbols::toStringTagKey()) {
          descriptor->properties["value"] = Value(std::string("Module"));
          descriptor->properties["writable"] = Value(false);
          descriptor->properties["enumerable"] = Value(false);
          descriptor->properties["configurable"] = Value(false);
          return Value(descriptor);
        }
        if (!isModuleNamespaceExportKey(obj, prop)) {
          return Value(Undefined{});
        }
        auto getterIt = obj->properties.find("__get_" + prop);
        if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
          Interpreter* interpreter = getGlobalInterpreter();
          if (interpreter) {
            Value out = interpreter->callForHarness(getterIt->second, {}, args[0]);
            if (interpreter->hasError()) {
              Value err = interpreter->getError();
              interpreter->clearError();
              throw std::runtime_error(err.toString());
            }
            descriptor->properties["value"] = out;
          } else {
            descriptor->properties["value"] = Value(Undefined{});
          }
        } else if (auto it = obj->properties.find(prop); it != obj->properties.end()) {
          descriptor->properties["value"] = it->second;
        } else {
          descriptor->properties["value"] = Value(Undefined{});
        }
        descriptor->properties["writable"] = Value(true);
        descriptor->properties["enumerable"] = Value(true);
        descriptor->properties["configurable"] = Value(false);
        return Value(descriptor);
      }
      auto it = obj->properties.find(prop);
      if (it == obj->properties.end()) {
        return Value(Undefined{});
      }
      // Create a simplified property descriptor
      auto descriptor = GarbageCollector::makeGC<Object>();
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
  auto reflectDefineProperty = GarbageCollector::makeGC<Function>();
  reflectDefineProperty->isNative = true;
  reflectDefineProperty->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 3) return Value(false);
    if (!args[0].isObject()) return Value(false);

    auto obj = args[0].getGC<Object>();
    std::string prop = valueToPropertyKey(args[1]);
    if (obj->isModuleNamespace) {
      if (!args[2].isObject()) {
        return Value(false);
      }
      auto descriptor = args[2].getGC<Object>();
      return Value(defineModuleNamespaceProperty(obj, prop, descriptor));
    }

    // Check if object is sealed/frozen
    bool isNewProp = obj->properties.find(prop) == obj->properties.end();
    if (((obj->sealed || obj->nonExtensible) && isNewProp) || obj->frozen) {
      return Value(false);
    }

    // Get value from descriptor
    if (args[2].isObject()) {
      auto descriptor = args[2].getGC<Object>();
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
  auto numberObj = GarbageCollector::makeGC<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  numberObj->isNative = true;
  numberObj->isConstructor = true;
  numberObj->properties["__wrap_primitive__"] = Value(true);
  numberObj->properties["name"] = Value(std::string("Number"));
  numberObj->properties["length"] = Value(1.0);
  numberObj->nativeFunc = [toPrimitive](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(0.0);
    }
    Value primitive = toPrimitive(args[0], false);
    if (primitive.isSymbol()) {
      throw std::runtime_error("TypeError: Cannot convert Symbol to number");
    }
    return Value(primitive.toNumber());
  };

  // Number.parseInt
  auto parseIntFn = GarbageCollector::makeGC<Function>();
  parseIntFn->isNative = true;
  parseIntFn->nativeFunc = [toPrimitive](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    // 1. Let inputString be ? ToString(string)
    Value arg0 = args[0];
    if (isObjectLikeValue(arg0)) {
      arg0 = toPrimitive(arg0, true); // hint "string" per spec
    }
    std::string str = arg0.toString();

    // 2. Let S be trimmed leading whitespace (including Unicode whitespace)
    str = stripLeadingESWhitespace(str);
    if (str.empty()) return Value(std::numeric_limits<double>::quiet_NaN());

    // 3. Handle sign
    bool negative = false;
    if (str[0] == '+' || str[0] == '-') {
      negative = (str[0] == '-');
      str = str.substr(1);
    }

    // 4. Let R be ? ToInt32(radix)
    int radix = 0;
    if (args.size() > 1 && !args[1].isUndefined()) {
      Value radixArg = args[1];
      if (isObjectLikeValue(radixArg)) {
        radixArg = toPrimitive(radixArg, false);
      }
      double radixNum = radixArg.toNumber();
      if (std::isnan(radixNum) || std::isinf(radixNum)) {
        radix = 0;
      } else {
        // ToInt32: wrap to 32-bit
        double d = std::trunc(radixNum);
        double two32 = 4294967296.0;
        d = std::fmod(d, two32);
        if (d < 0) d += two32;
        if (d >= 2147483648.0) d -= two32;
        radix = static_cast<int>(d);
      }
    }

    bool stripPrefix = true;
    if (radix != 0) {
      if (radix < 2 || radix > 36) {
        return Value(std::numeric_limits<double>::quiet_NaN());
      }
      if (radix != 16) stripPrefix = false;
    } else {
      radix = 10;
    }

    // Handle 0x prefix for hex
    if (stripPrefix && str.size() >= 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
      radix = 16;
      str = str.substr(2);
    }

    // Parse digits manually (handles arbitrary precision)
    double result = 0.0;
    size_t parsed = 0;
    for (char c : str) {
      int digit = -1;
      if (c >= '0' && c <= '9') digit = c - '0';
      else if (c >= 'a' && c <= 'z') digit = c - 'a' + 10;
      else if (c >= 'A' && c <= 'Z') digit = c - 'A' + 10;
      if (digit < 0 || digit >= radix) break;
      result = result * radix + digit;
      parsed++;
    }
    if (parsed == 0) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(negative ? -result : result);
  };
  parseIntFn->properties["name"] = Value(std::string("parseInt"));
  parseIntFn->properties["length"] = Value(2.0);
  parseIntFn->properties["__non_writable_name"] = Value(true);
  parseIntFn->properties["__non_enum_name"] = Value(true);
  parseIntFn->properties["__non_writable_length"] = Value(true);
  parseIntFn->properties["__non_enum_length"] = Value(true);
  numberObj->properties["parseInt"] = Value(parseIntFn);
  numberObj->properties["__non_enum_parseInt"] = Value(true);

  // Number.parseFloat
  auto parseFloatFn = GarbageCollector::makeGC<Function>();
  parseFloatFn->isNative = true;
  parseFloatFn->nativeFunc = [toPrimitive](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    Value arg0 = args[0];
    if (isObjectLikeValue(arg0)) {
      arg0 = toPrimitive(arg0, true); // hint "string" per spec
    }
    std::string str = arg0.toString();

    // Use ES whitespace trimming
    str = stripLeadingESWhitespace(str);
    if (str.empty()) return Value(std::numeric_limits<double>::quiet_NaN());

    // Handle Infinity/+Infinity/-Infinity explicitly (case-sensitive per spec)
    if (str.substr(0, 8) == "Infinity" || str.substr(0, 9) == "+Infinity") {
      return Value(std::numeric_limits<double>::infinity());
    }
    if (str.substr(0, 9) == "-Infinity") {
      return Value(-std::numeric_limits<double>::infinity());
    }
    // Reject non-spec infinity variants (inf, INFINITY, etc.)
    if ((str[0] == 'i' || str[0] == 'I') && str.substr(0, 3) != "Inf") {
      return Value(std::numeric_limits<double>::quiet_NaN());
    }

    // parseFloat does NOT parse hex - "0x1" should return 0 (parse only "0")
    // Replace 0x/0X prefix to prevent stod from parsing hex
    std::string parseStr = str;
    if (parseStr.size() >= 2 && parseStr[0] == '0' && (parseStr[1] == 'x' || parseStr[1] == 'X')) {
      parseStr = "0"; // Only parse the leading "0"
    }

    try {
      size_t idx;
      double result = std::stod(parseStr, &idx);
      if (idx == 0) {
        return Value(std::numeric_limits<double>::quiet_NaN());
      }
      // std::stod may parse "inf"/"INF" etc - reject non-spec Infinity
      if (std::isinf(result) && parseStr.substr(0, 8) != "Infinity" &&
          parseStr.substr(0, 9) != "+Infinity" && parseStr.substr(0, 9) != "-Infinity") {
        return Value(std::numeric_limits<double>::quiet_NaN());
      }
      return Value(result);
    } catch (...) {
      return Value(std::numeric_limits<double>::quiet_NaN());
    }
  };
  parseFloatFn->properties["name"] = Value(std::string("parseFloat"));
  parseFloatFn->properties["length"] = Value(1.0);
  parseFloatFn->properties["__non_writable_name"] = Value(true);
  parseFloatFn->properties["__non_enum_name"] = Value(true);
  parseFloatFn->properties["__non_writable_length"] = Value(true);
  parseFloatFn->properties["__non_enum_length"] = Value(true);
  numberObj->properties["parseFloat"] = Value(parseFloatFn);
  numberObj->properties["__non_enum_parseFloat"] = Value(true);

  // Number.isNaN
  auto isNaNFn = GarbageCollector::makeGC<Function>();
  isNaNFn->isNative = true;
  isNaNFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isNumber()) return Value(false);
    double num = std::get<double>(args[0].data);
    return Value(std::isnan(num));
  };
  isNaNFn->properties["name"] = Value(std::string("isNaN"));
  isNaNFn->properties["length"] = Value(1.0);
  numberObj->properties["isNaN"] = Value(isNaNFn);
  numberObj->properties["__non_enum_isNaN"] = Value(true);

  // Number.isFinite
  auto isFiniteFn = GarbageCollector::makeGC<Function>();
  isFiniteFn->isNative = true;
  isFiniteFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isNumber()) return Value(false);
    double num = std::get<double>(args[0].data);
    return Value(std::isfinite(num));
  };
  isFiniteFn->properties["name"] = Value(std::string("isFinite"));
  isFiniteFn->properties["length"] = Value(1.0);
  numberObj->properties["isFinite"] = Value(isFiniteFn);
  numberObj->properties["__non_enum_isFinite"] = Value(true);

  // Number.isInteger
  auto isIntegerFn = GarbageCollector::makeGC<Function>();
  isIntegerFn->isNative = true;
  isIntegerFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isNumber()) return Value(false);
    double num = std::get<double>(args[0].data);
    return Value(std::isfinite(num) && std::floor(num) == num);
  };
  isIntegerFn->properties["name"] = Value(std::string("isInteger"));
  isIntegerFn->properties["length"] = Value(1.0);
  numberObj->properties["isInteger"] = Value(isIntegerFn);
  numberObj->properties["__non_enum_isInteger"] = Value(true);

  // Number.isSafeInteger
  auto isSafeIntegerFn = GarbageCollector::makeGC<Function>();
  isSafeIntegerFn->isNative = true;
  isSafeIntegerFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isNumber()) return Value(false);
    double num = std::get<double>(args[0].data);
    const double MAX_SAFE_INTEGER = 9007199254740991.0;
    return Value(std::isfinite(num) && std::floor(num) == num &&
                 num >= -MAX_SAFE_INTEGER && num <= MAX_SAFE_INTEGER);
  };
  isSafeIntegerFn->properties["name"] = Value(std::string("isSafeInteger"));
  isSafeIntegerFn->properties["length"] = Value(1.0);
  numberObj->properties["isSafeInteger"] = Value(isSafeIntegerFn);
  numberObj->properties["__non_enum_isSafeInteger"] = Value(true);

  // Number constants (non-writable, non-enumerable, non-configurable per spec)
  auto defineNumberConst = [&](const std::string& name, Value val) {
    numberObj->properties[name] = val;
    numberObj->properties["__non_writable_" + name] = Value(true);
    numberObj->properties["__non_enum_" + name] = Value(true);
    numberObj->properties["__non_configurable_" + name] = Value(true);
  };
  defineNumberConst("MAX_VALUE", Value(std::numeric_limits<double>::max()));
  defineNumberConst("MIN_VALUE", Value(std::numeric_limits<double>::denorm_min()));
  defineNumberConst("POSITIVE_INFINITY", Value(std::numeric_limits<double>::infinity()));
  defineNumberConst("NEGATIVE_INFINITY", Value(-std::numeric_limits<double>::infinity()));
  defineNumberConst("NaN", Value(std::numeric_limits<double>::quiet_NaN()));
  defineNumberConst("MAX_SAFE_INTEGER", Value(9007199254740991.0));
  defineNumberConst("MIN_SAFE_INTEGER", Value(-9007199254740991.0));
  defineNumberConst("EPSILON", Value(2.220446049250313e-16));

  {
    auto numberPrototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    numberObj->properties["prototype"] = Value(numberPrototype);
    numberObj->properties["__non_writable_prototype"] = Value(true);
    numberObj->properties["__non_enum_prototype"] = Value(true);
    numberObj->properties["__non_configurable_prototype"] = Value(true);
    numberPrototype->properties["constructor"] = Value(numberObj);
    numberPrototype->properties["__non_enum_constructor"] = Value(true);
    numberPrototype->properties["__primitive_value__"] = Value(0.0);
    numberPrototype->properties["name"] = Value(std::string("Number"));

    // Helper: ES ToNumber that calls ToPrimitive for objects
    auto toNumberFromArg = [toPrimitive, isObjectLike](const Value& arg) -> double {
      Value prim = isObjectLike(arg) ? toPrimitive(arg, false) : arg;
      if (prim.isSymbol()) {
        throw std::runtime_error("TypeError: Cannot convert a Symbol value to a number");
      }
      if (prim.isBigInt()) {
        throw std::runtime_error("TypeError: Cannot convert a BigInt value to a number");
      }
      return prim.toNumber();
    };

    auto numberThisValue = [](const Value& thisArg, const char* methodName) -> double {
      if (thisArg.isNumber()) {
        return thisArg.toNumber();
      }
      if (thisArg.isObject()) {
        auto obj = thisArg.getGC<Object>();
        auto primIt = obj->properties.find("__primitive_value__");
        if (primIt != obj->properties.end() && primIt->second.isNumber()) {
          return primIt->second.toNumber();
        }
      }
      throw std::runtime_error(std::string("TypeError: Number.prototype.") + methodName +
                               " requires Number");
    };

    auto numberProtoValueOf = GarbageCollector::makeGC<Function>();
    GarbageCollector::instance().reportAllocation(sizeof(Function));
    numberProtoValueOf->isNative = true;
    numberProtoValueOf->properties["__uses_this_arg__"] = Value(true);
    numberProtoValueOf->properties["__throw_on_new__"] = Value(true);
    numberProtoValueOf->properties["name"] = Value(std::string("valueOf"));
    numberProtoValueOf->properties["length"] = Value(0.0);
    numberProtoValueOf->properties["__non_writable_name"] = Value(true);
    numberProtoValueOf->properties["__non_enum_name"] = Value(true);
    numberProtoValueOf->properties["__non_writable_length"] = Value(true);
    numberProtoValueOf->properties["__non_enum_length"] = Value(true);
    numberProtoValueOf->nativeFunc = [numberThisValue](const std::vector<Value>& callArgs) -> Value {
      if (callArgs.empty()) {
        throw std::runtime_error("TypeError: Number.prototype.valueOf requires Number");
      }
      return Value(numberThisValue(callArgs[0], "valueOf"));
    };
    numberPrototype->properties["valueOf"] = Value(numberProtoValueOf);
    numberPrototype->properties["__non_enum_valueOf"] = Value(true);

    auto numberProtoToString = GarbageCollector::makeGC<Function>();
    GarbageCollector::instance().reportAllocation(sizeof(Function));
    numberProtoToString->isNative = true;
    numberProtoToString->properties["__uses_this_arg__"] = Value(true);
    numberProtoToString->properties["__throw_on_new__"] = Value(true);
    numberProtoToString->properties["name"] = Value(std::string("toString"));
    numberProtoToString->properties["length"] = Value(1.0);
    numberProtoToString->properties["__non_writable_name"] = Value(true);
    numberProtoToString->properties["__non_enum_name"] = Value(true);
    numberProtoToString->properties["__non_writable_length"] = Value(true);
    numberProtoToString->properties["__non_enum_length"] = Value(true);
    numberProtoToString->nativeFunc = [numberThisValue](const std::vector<Value>& callArgs) -> Value {
      if (callArgs.empty()) {
        throw std::runtime_error("TypeError: Number.prototype.toString requires Number");
      }
      double num = numberThisValue(callArgs[0], "toString");
      int radix = 10;
      if (callArgs.size() > 1 && !callArgs[1].isUndefined()) {
        radix = static_cast<int>(callArgs[1].toNumber());
        if (radix < 2 || radix > 36) {
          throw std::runtime_error("RangeError: toString() radix must be between 2 and 36");
        }
      }
      if (std::isnan(num)) return Value(std::string("NaN"));
      if (std::isinf(num)) return Value(num < 0 ? std::string("-Infinity") : std::string("Infinity"));
      if (radix == 10) return Value(ecmaNumberToString(num));
      // Radix conversion
      bool negative = num < 0;
      double absNum = negative ? -num : num;
      std::string result;
      // Integer part
      double intPart = std::floor(absNum);
      double fracPart = absNum - intPart;
      if (intPart == 0) {
        result = "0";
      } else {
        while (intPart > 0) {
          int digit = static_cast<int>(std::fmod(intPart, radix));
          result = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10) + result;
          intPart = std::floor(intPart / radix);
        }
      }
      // Fractional part
      if (fracPart > 0) {
        result += '.';
        for (int i = 0; i < 20 && fracPart > 0; ++i) {
          fracPart *= radix;
          int digit = static_cast<int>(fracPart);
          result += (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
          fracPart -= digit;
        }
        // Remove trailing zeros
        while (!result.empty() && result.back() == '0') result.pop_back();
        if (!result.empty() && result.back() == '.') result.pop_back();
      }
      if (negative) result = "-" + result;
      return Value(result);
    };
    numberPrototype->properties["toString"] = Value(numberProtoToString);
    numberPrototype->properties["__non_enum_toString"] = Value(true);

    auto numberProtoToLocaleString = GarbageCollector::makeGC<Function>();
    GarbageCollector::instance().reportAllocation(sizeof(Function));
    numberProtoToLocaleString->isNative = true;
    numberProtoToLocaleString->properties["__uses_this_arg__"] = Value(true);
    numberProtoToLocaleString->properties["__throw_on_new__"] = Value(true);
    numberProtoToLocaleString->properties["name"] = Value(std::string("toLocaleString"));
    numberProtoToLocaleString->properties["length"] = Value(0.0);
    numberProtoToLocaleString->properties["__non_writable_name"] = Value(true);
    numberProtoToLocaleString->properties["__non_enum_name"] = Value(true);
    numberProtoToLocaleString->properties["__non_writable_length"] = Value(true);
    numberProtoToLocaleString->properties["__non_enum_length"] = Value(true);
    numberProtoToLocaleString->nativeFunc = [numberThisValue](const std::vector<Value>& callArgs) -> Value {
      if (callArgs.empty()) {
        throw std::runtime_error("TypeError: Number.prototype.toLocaleString requires Number");
      }
      return Value(Value(numberThisValue(callArgs[0], "toLocaleString")).toString());
    };
    numberPrototype->properties["toLocaleString"] = Value(numberProtoToLocaleString);
    numberPrototype->properties["__non_enum_toLocaleString"] = Value(true);

    // Number.prototype.toFixed
    auto numberProtoToFixed = GarbageCollector::makeGC<Function>();
    GarbageCollector::instance().reportAllocation(sizeof(Function));
    numberProtoToFixed->isNative = true;
    numberProtoToFixed->properties["__uses_this_arg__"] = Value(true);
    numberProtoToFixed->properties["__throw_on_new__"] = Value(true);
    numberProtoToFixed->properties["name"] = Value(std::string("toFixed"));
    numberProtoToFixed->properties["length"] = Value(1.0);
    numberProtoToFixed->properties["__non_writable_name"] = Value(true);
    numberProtoToFixed->properties["__non_enum_name"] = Value(true);
    numberProtoToFixed->properties["__non_writable_length"] = Value(true);
    numberProtoToFixed->properties["__non_enum_length"] = Value(true);
    numberProtoToFixed->nativeFunc = [numberThisValue, toNumberFromArg](const std::vector<Value>& callArgs) -> Value {
      if (callArgs.empty()) {
        throw std::runtime_error("TypeError: Number.prototype.toFixed requires Number");
      }
      double num = numberThisValue(callArgs[0], "toFixed");
      // Step 2: Let f be ToInteger(fractionDigits) - use raw double for range check
      double fRaw = (callArgs.size() > 1 && !callArgs[1].isUndefined()) ? toNumberFromArg(callArgs[1]) : 0.0;
      double fDouble = std::isnan(fRaw) ? 0.0 : std::trunc(fRaw);
      // Step 3: Range check on the double value (before casting to int)
      if (fDouble < 0 || fDouble > 100) {
        throw std::runtime_error("RangeError: toFixed() digits argument must be between 0 and 100");
      }
      int f = static_cast<int>(fDouble);
      // Step 4: NaN check
      if (std::isnan(num)) return Value(std::string("NaN"));
      // Step 9: If x >= 10^21, return ToString(x)
      if (std::isinf(num) || std::fabs(num) >= 1e21) {
        return Value(ecmaNumberToString(num));
      }
      char buf[350];
      snprintf(buf, sizeof(buf), "%.*f", f, num);
      return Value(std::string(buf));
    };
    numberPrototype->properties["toFixed"] = Value(numberProtoToFixed);
    numberPrototype->properties["__non_enum_toFixed"] = Value(true);

    // Number.prototype.toPrecision
    auto numberProtoToPrecision = GarbageCollector::makeGC<Function>();
    GarbageCollector::instance().reportAllocation(sizeof(Function));
    numberProtoToPrecision->isNative = true;
    numberProtoToPrecision->properties["__uses_this_arg__"] = Value(true);
    numberProtoToPrecision->properties["__throw_on_new__"] = Value(true);
    numberProtoToPrecision->properties["name"] = Value(std::string("toPrecision"));
    numberProtoToPrecision->properties["length"] = Value(1.0);
    numberProtoToPrecision->properties["__non_writable_name"] = Value(true);
    numberProtoToPrecision->properties["__non_enum_name"] = Value(true);
    numberProtoToPrecision->properties["__non_writable_length"] = Value(true);
    numberProtoToPrecision->properties["__non_enum_length"] = Value(true);
    numberProtoToPrecision->nativeFunc = [numberThisValue, toNumberFromArg](const std::vector<Value>& callArgs) -> Value {
      if (callArgs.empty()) {
        throw std::runtime_error("TypeError: Number.prototype.toPrecision requires Number");
      }
      double num = numberThisValue(callArgs[0], "toPrecision");
      // Step 2: If precision is undefined, return ToString(x)
      if (callArgs.size() <= 1 || callArgs[1].isUndefined()) {
        return Value(ecmaNumberToString(num));
      }
      // Step 3: Let p be ToInteger(precision) - BEFORE NaN/Infinity checks
      double pRaw = toNumberFromArg(callArgs[1]);
      // Step 4-5: NaN/Infinity checks AFTER ToInteger
      if (std::isnan(num)) return Value(std::string("NaN"));
      if (std::isinf(num)) return Value(num < 0 ? std::string("-Infinity") : std::string("Infinity"));
      int p = std::isnan(pRaw) ? 0 : (std::isinf(pRaw) ? (pRaw > 0 ? 101 : -1) : static_cast<int>(std::trunc(pRaw)));
      // Step 6: Range check
      if (p < 1 || p > 100) {
        throw std::runtime_error("RangeError: toPrecision() argument must be between 1 and 100");
      }
      // Step 7-8: sign handling
      std::string s;
      double x = num;
      if (x < 0) { s = "-"; x = -x; }
      // Step 9: x == 0 case
      if (x == 0.0) {
        std::string m(p, '0');
        if (p == 1) return Value(s + m);
        return Value(s + m.substr(0, 1) + "." + m.substr(1));
      }
      // Step 10: Find e and m using snprintf with scientific notation
      char buf[128];
      snprintf(buf, sizeof(buf), "%.*e", p - 1, x);
      // Parse mantissa digits and exponent from scientific output
      std::string sci(buf);
      auto epos = sci.find('e');
      std::string mantPart = sci.substr(0, epos);
      int e = std::stoi(sci.substr(epos + 1));
      // Extract digits (remove sign and decimal point)
      std::string m;
      for (char c : mantPart) {
        if (c >= '0' && c <= '9') m += c;
      }
      // Ensure m has exactly p digits
      while (static_cast<int>(m.size()) < p) m += '0';
      m = m.substr(0, p);
      // Step 11: exponential notation if e < -6 or e >= p
      if (e < -6 || e >= p) {
        std::string result = s;
        if (p == 1) {
          result += m;
        } else {
          result += m.substr(0, 1) + "." + m.substr(1);
        }
        result += "e";
        result += (e >= 0) ? "+" : "-";
        result += std::to_string(std::abs(e));
        return Value(result);
      }
      // Step 12: e == p-1 → no decimal point needed
      if (e == p - 1) {
        return Value(s + m);
      }
      // Step 13: e >= 0 → insert decimal point
      if (e >= 0) {
        return Value(s + m.substr(0, e + 1) + "." + m.substr(e + 1));
      }
      // Step 14: e < 0 → prepend zeros
      return Value(s + "0." + std::string(-(e + 1), '0') + m);
    };
    numberPrototype->properties["toPrecision"] = Value(numberProtoToPrecision);
    numberPrototype->properties["__non_enum_toPrecision"] = Value(true);

    // Number.prototype.toExponential
    auto numberProtoToExponential = GarbageCollector::makeGC<Function>();
    GarbageCollector::instance().reportAllocation(sizeof(Function));
    numberProtoToExponential->isNative = true;
    numberProtoToExponential->properties["__uses_this_arg__"] = Value(true);
    numberProtoToExponential->properties["__throw_on_new__"] = Value(true);
    numberProtoToExponential->properties["name"] = Value(std::string("toExponential"));
    numberProtoToExponential->properties["length"] = Value(1.0);
    numberProtoToExponential->properties["__non_writable_name"] = Value(true);
    numberProtoToExponential->properties["__non_enum_name"] = Value(true);
    numberProtoToExponential->properties["__non_writable_length"] = Value(true);
    numberProtoToExponential->properties["__non_enum_length"] = Value(true);
    numberProtoToExponential->nativeFunc = [numberThisValue, toNumberFromArg](const std::vector<Value>& callArgs) -> Value {
      if (callArgs.empty()) {
        throw std::runtime_error("TypeError: Number.prototype.toExponential requires Number");
      }
      double num = numberThisValue(callArgs[0], "toExponential");
      // Step 2: Let f be ToInteger(fractionDigits) - BEFORE NaN/Infinity checks
      bool hasDigits = callArgs.size() > 1 && !callArgs[1].isUndefined();
      double fRaw = hasDigits ? toNumberFromArg(callArgs[1]) : 0.0;
      // Step 3-4: NaN/Infinity checks AFTER ToInteger
      if (std::isnan(num)) return Value(std::string("NaN"));
      if (std::isinf(num)) return Value(num < 0 ? std::string("-Infinity") : std::string("Infinity"));
      int f = hasDigits ? (std::isnan(fRaw) ? 0 : (std::isinf(fRaw) ? (fRaw > 0 ? 101 : -1) : static_cast<int>(std::trunc(fRaw)))) : -1;
      // Step 5: Range check (only when fractionDigits specified)
      if (hasDigits && (f < 0 || f > 100)) {
        throw std::runtime_error("RangeError: toExponential() argument must be between 0 and 100");
      }
      // Step 6-7: sign handling
      std::string s;
      double x = num;
      if (x < 0) { s = "-"; x = -x; }
      else if (x == 0.0 && std::signbit(num)) { x = 0.0; } // handle -0
      // Step 8: x == 0 case
      if (x == 0.0) {
        std::string m(hasDigits ? f + 1 : 1, '0');
        std::string result = s;
        if (hasDigits && f != 0) {
          result += m.substr(0, 1) + "." + m.substr(1);
        } else if (!hasDigits) {
          result += "0";
        } else {
          result += m;
        }
        result += "e+0";
        return Value(result);
      }
      // Step 9-10: Non-zero case
      if (!hasDigits) {
        // Use ecmaNumberToString for shortest representation, then convert to exponential
        std::string shortest = ecmaNumberToString(x);
        // Parse the shortest representation to extract digits and exponent
        std::string digits;
        int e = 0;
        auto ep = shortest.find_first_of("eE");
        if (ep != std::string::npos) {
          // Already in exponential notation (e.g., "1e+21")
          std::string mPart = shortest.substr(0, ep);
          e = std::stoi(shortest.substr(ep + 1));
          for (char c : mPart) {
            if (c >= '0' && c <= '9') digits += c;
          }
        } else {
          auto dotPos = shortest.find('.');
          if (dotPos != std::string::npos) {
            // Has decimal point (e.g., "123.456" or "0.001")
            std::string intPart = shortest.substr(0, dotPos);
            std::string fracPart = shortest.substr(dotPos + 1);
            if (intPart == "0") {
              // e.g., "0.001" → digits="1", e=-3
              int leadingZeros = 0;
              for (char c : fracPart) {
                if (c == '0') leadingZeros++;
                else break;
              }
              for (char c : fracPart) {
                if (c >= '0' && c <= '9') digits += c;
              }
              // Remove leading zeros from digits
              size_t firstNonZero = digits.find_first_not_of('0');
              if (firstNonZero != std::string::npos) digits = digits.substr(firstNonZero);
              e = -(leadingZeros + 1);
            } else {
              // e.g., "123.456"
              digits = intPart + fracPart;
              e = static_cast<int>(intPart.size()) - 1;
            }
          } else {
            // Pure integer (e.g., "100")
            digits = shortest;
            // Remove trailing zeros to find significant digits
            e = static_cast<int>(digits.size()) - 1;
          }
        }
        // Remove trailing zeros from digits for shortest form
        while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
        // Format result
        std::string result = s;
        if (digits.size() == 1) {
          result += digits;
        } else {
          result += digits.substr(0, 1) + "." + digits.substr(1);
        }
        result += "e";
        result += (e >= 0) ? "+" : "-";
        result += std::to_string(std::abs(e));
        return Value(result);
      }
      // hasDigits case: use snprintf with manual round-half-up
      char buf[350];
      // Get high-precision representation for manual rounding
      snprintf(buf, sizeof(buf), "%.*e", f + 5, x);  // extra precision for rounding
      std::string sci(buf);
      auto epos = sci.find('e');
      std::string mantPart = sci.substr(0, epos);
      int e = std::stoi(sci.substr(epos + 1));
      // Extract all digits from mantissa
      std::string allDigits;
      for (char c : mantPart) {
        if (c >= '0' && c <= '9') allDigits += c;
      }
      // Round to f+1 digits using round-half-up
      int needed = f + 1;
      if (static_cast<int>(allDigits.size()) > needed) {
        // Check the digit after the cutoff
        int roundDigit = allDigits[needed] - '0';
        allDigits = allDigits.substr(0, needed);
        if (roundDigit >= 5) {
          // Round up
          int carry = 1;
          for (int i = needed - 1; i >= 0 && carry; --i) {
            int d = (allDigits[i] - '0') + carry;
            allDigits[i] = '0' + (d % 10);
            carry = d / 10;
          }
          if (carry) {
            allDigits = "1" + allDigits.substr(0, needed - 1);
            e++;
          }
        }
      }
      while (static_cast<int>(allDigits.size()) < needed) allDigits += '0';
      // Format mantissa
      std::string mantissa;
      if (f == 0) {
        mantissa = allDigits.substr(0, 1);
      } else {
        mantissa = allDigits.substr(0, 1) + "." + allDigits.substr(1, f);
      }
      std::string result = s + mantissa + "e";
      result += (e >= 0) ? "+" : "-";
      result += std::to_string(std::abs(e));
      return Value(result);
    };
    numberPrototype->properties["toExponential"] = Value(numberProtoToExponential);
    numberPrototype->properties["__non_enum_toExponential"] = Value(true);
  }

  env->define("Number", Value(numberObj));

  // Boolean constructor
  auto booleanObj = GarbageCollector::makeGC<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  booleanObj->isNative = true;
  booleanObj->isConstructor = true;
  booleanObj->properties["__wrap_primitive__"] = Value(true);
  booleanObj->properties["name"] = Value(std::string("Boolean"));
  booleanObj->properties["length"] = Value(1.0);
  booleanObj->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(false);
    }
    // ToBoolean does NOT call ToPrimitive - objects are always truthy
    return Value(args[0].toBool());
  };

  {
    auto booleanPrototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    booleanObj->properties["prototype"] = Value(booleanPrototype);
    booleanObj->properties["__non_writable_prototype"] = Value(true);
    booleanObj->properties["__non_enum_prototype"] = Value(true);
    booleanObj->properties["__non_configurable_prototype"] = Value(true);
    booleanPrototype->properties["constructor"] = Value(booleanObj);
    booleanPrototype->properties["__non_enum_constructor"] = Value(true);
    booleanPrototype->properties["name"] = Value(std::string("Boolean"));
    booleanPrototype->properties["__primitive_value__"] = Value(false);

    auto boolProtoValueOf = GarbageCollector::makeGC<Function>();
    GarbageCollector::instance().reportAllocation(sizeof(Function));
    boolProtoValueOf->isNative = true;
    boolProtoValueOf->properties["__uses_this_arg__"] = Value(true);
    boolProtoValueOf->properties["name"] = Value(std::string("valueOf"));
    boolProtoValueOf->properties["length"] = Value(0.0);
    boolProtoValueOf->nativeFunc = [](const std::vector<Value>& callArgs) -> Value {
      if (callArgs.empty()) {
        throw std::runtime_error("TypeError: Boolean.prototype.valueOf requires Boolean");
      }
      Value thisArg = callArgs[0];
      if (thisArg.isBool()) return thisArg;
      if (thisArg.isObject()) {
        auto obj = thisArg.getGC<Object>();
        auto primIt = obj->properties.find("__primitive_value__");
        if (primIt != obj->properties.end() && primIt->second.isBool()) {
          return primIt->second;
        }
      }
      throw std::runtime_error("TypeError: Boolean.prototype.valueOf requires Boolean");
    };
    booleanPrototype->properties["valueOf"] = Value(boolProtoValueOf);
    booleanPrototype->properties["__non_enum_valueOf"] = Value(true);

    auto boolProtoToString = GarbageCollector::makeGC<Function>();
    GarbageCollector::instance().reportAllocation(sizeof(Function));
    boolProtoToString->isNative = true;
    boolProtoToString->properties["__uses_this_arg__"] = Value(true);
    boolProtoToString->properties["name"] = Value(std::string("toString"));
    boolProtoToString->properties["length"] = Value(0.0);
    boolProtoToString->nativeFunc = [](const std::vector<Value>& callArgs) -> Value {
      if (callArgs.empty()) {
        throw std::runtime_error("TypeError: Boolean.prototype.toString requires Boolean");
      }
      Value thisArg = callArgs[0];
      bool b = false;
      if (thisArg.isBool()) {
        b = thisArg.toBool();
      } else if (thisArg.isObject()) {
        auto obj = thisArg.getGC<Object>();
        auto primIt = obj->properties.find("__primitive_value__");
        if (primIt != obj->properties.end() && primIt->second.isBool()) {
          b = primIt->second.toBool();
        } else {
          throw std::runtime_error("TypeError: Boolean.prototype.toString requires Boolean");
        }
      } else {
        throw std::runtime_error("TypeError: Boolean.prototype.toString requires Boolean");
      }
      return Value(std::string(b ? "true" : "false"));
    };
    booleanPrototype->properties["toString"] = Value(boolProtoToString);
    booleanPrototype->properties["__non_enum_toString"] = Value(true);
  }
  env->define("Boolean", Value(booleanObj));

  // Global parseInt and parseFloat (aliases)
  env->define("parseInt", Value(parseIntFn));
  env->define("parseFloat", Value(parseFloatFn));

  // Global isNaN (different from Number.isNaN - coerces via ToNumber which calls ToPrimitive)
  auto globalIsNaNFn = GarbageCollector::makeGC<Function>();
  globalIsNaNFn->isNative = true;
  globalIsNaNFn->properties["name"] = Value(std::string("isNaN"));
  globalIsNaNFn->properties["length"] = Value(1.0);
  globalIsNaNFn->properties["__non_writable_name"] = Value(true);
  globalIsNaNFn->properties["__non_enum_name"] = Value(true);
  globalIsNaNFn->properties["__non_writable_length"] = Value(true);
  globalIsNaNFn->properties["__non_enum_length"] = Value(true);
  globalIsNaNFn->nativeFunc = [toPrimitive](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(true);
    Value arg = args[0];
    if (arg.isSymbol()) {
      throw std::runtime_error("TypeError: Cannot convert a Symbol value to a number");
    }
    if (isObjectLikeValue(arg)) {
      arg = toPrimitive(arg, false);
      if (arg.isSymbol()) {
        throw std::runtime_error("TypeError: Cannot convert a Symbol value to a number");
      }
    }
    double num = arg.toNumber();
    return Value(std::isnan(num));
  };
  env->define("isNaN", Value(globalIsNaNFn));

  // Global isFinite (coerces via ToNumber which calls ToPrimitive)
  auto globalIsFiniteFn = GarbageCollector::makeGC<Function>();
  globalIsFiniteFn->isNative = true;
  globalIsFiniteFn->properties["name"] = Value(std::string("isFinite"));
  globalIsFiniteFn->properties["length"] = Value(1.0);
  globalIsFiniteFn->properties["__non_writable_name"] = Value(true);
  globalIsFiniteFn->properties["__non_enum_name"] = Value(true);
  globalIsFiniteFn->properties["__non_writable_length"] = Value(true);
  globalIsFiniteFn->properties["__non_enum_length"] = Value(true);
  globalIsFiniteFn->nativeFunc = [toPrimitive](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(false);
    Value arg = args[0];
    if (arg.isSymbol()) {
      throw std::runtime_error("TypeError: Cannot convert a Symbol value to a number");
    }
    if (isObjectLikeValue(arg)) {
      arg = toPrimitive(arg, false);
      if (arg.isSymbol()) {
        throw std::runtime_error("TypeError: Cannot convert a Symbol value to a number");
      }
    }
    double num = arg.toNumber();
    return Value(std::isfinite(num));
  };
  env->define("isFinite", Value(globalIsFiniteFn));

  // Array constructor with static methods
  auto arrayConstructorFn = GarbageCollector::makeGC<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  arrayConstructorFn->isNative = true;
  arrayConstructorFn->isConstructor = true;
  arrayConstructorFn->properties["name"] = Value(std::string("Array"));
  arrayConstructorFn->properties["length"] = Value(1.0);
  arrayConstructorFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto result = GarbageCollector::makeGC<Array>();
    GarbageCollector::instance().reportAllocation(sizeof(Array));

    if (args.empty()) {
      return Value(result);
    }

    if (args.size() == 1 && args[0].isNumber()) {
      double lengthNum = args[0].toNumber();
      if (!std::isfinite(lengthNum) || lengthNum < 0 || std::floor(lengthNum) != lengthNum) {
        throw std::runtime_error("RangeError: Invalid array length");
      }
      result->elements.resize(static_cast<size_t>(lengthNum), Value(Undefined{}));
      return Value(result);
    }

    result->elements = args;
    return Value(result);
  };

  auto arrayConstructorObj = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  arrayConstructorObj->properties["__callable_object__"] = Value(true);
  arrayConstructorObj->properties["__constructor_wrapper__"] = Value(true);
  arrayConstructorObj->properties["constructor"] = Value(arrayConstructorFn);
  arrayConstructorObj->properties["length"] = Value(1.0);

  auto arrayPrototype = GarbageCollector::makeGC<Object>();
  arrayConstructorObj->properties["prototype"] = Value(arrayPrototype);
  arrayConstructorObj->properties["__non_writable_prototype"] = Value(true);
  arrayConstructorObj->properties["__non_enum_prototype"] = Value(true);
  arrayConstructorObj->properties["__non_configurable_prototype"] = Value(true);
  arrayConstructorFn->properties["prototype"] = Value(arrayPrototype);
  arrayPrototype->properties["constructor"] = Value(arrayConstructorObj);
  arrayPrototype->properties["__non_enum_constructor"] = Value(true);
  env->define("__array_prototype__", Value(arrayPrototype));
  setGlobalArrayPrototype(Value(arrayPrototype));

  // Array.prototype.push - generic (works with array-like objects)
  auto arrayProtoPush = GarbageCollector::makeGC<Function>();
  arrayProtoPush->isNative = true;
  arrayProtoPush->properties["__uses_this_arg__"] = Value(true);
  arrayProtoPush->properties["name"] = Value(std::string("push"));
  arrayProtoPush->properties["length"] = Value(1.0);
  arrayProtoPush->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      throw std::runtime_error("TypeError: Array.prototype.push called on null or undefined");
    }
    Value thisVal = args[0];
    if (thisVal.isArray()) {
      auto arr = thisVal.getGC<Array>();
      for (size_t i = 1; i < args.size(); ++i) {
        arr->elements.push_back(args[i]);
      }
      return Value(static_cast<double>(arr->elements.size()));
    }
    // Generic object: get length, set properties, update length
    Interpreter* interp = getGlobalInterpreter();
    size_t len = 0;
    if (interp) {
      auto [found, lenVal] = interp->getPropertyForExternal(thisVal, "length");
      if (found) {
        double d = lenVal.toNumber();
        if (!std::isnan(d) && d >= 0) len = static_cast<size_t>(d);
      }
    }
    // Set each argument at increasing indices
    for (size_t i = 1; i < args.size(); ++i) {
      if (thisVal.isObject()) {
        thisVal.getGC<Object>()->properties[std::to_string(len)] = args[i];
      } else if (thisVal.isFunction()) {
        thisVal.getGC<Function>()->properties[std::to_string(len)] = args[i];
      }
      len++;
    }
    Value newLen = Value(static_cast<double>(len));
    if (thisVal.isObject()) {
      thisVal.getGC<Object>()->properties["length"] = newLen;
    } else if (thisVal.isFunction()) {
      thisVal.getGC<Function>()->properties["length"] = newLen;
    }
    return newLen;
  };
  arrayProtoPush->properties["__non_writable_name"] = Value(true);
  arrayProtoPush->properties["__non_enum_name"] = Value(true);
  arrayProtoPush->properties["__non_writable_length"] = Value(true);
  arrayProtoPush->properties["__non_enum_length"] = Value(true);
  arrayPrototype->properties["push"] = Value(arrayProtoPush);
  arrayPrototype->properties["__non_enum_push"] = Value(true);

  // Array.prototype.join
  auto arrayProtoJoin = GarbageCollector::makeGC<Function>();
  arrayProtoJoin->isNative = true;
  arrayProtoJoin->properties["__uses_this_arg__"] = Value(true);
  arrayProtoJoin->properties["name"] = Value(std::string("join"));
  arrayProtoJoin->properties["length"] = Value(1.0);

  arrayProtoJoin->properties["__non_writable_name"] = Value(true);
  arrayProtoJoin->properties["__non_enum_name"] = Value(true);
  arrayProtoJoin->properties["__non_writable_length"] = Value(true);
  arrayProtoJoin->properties["__non_enum_length"] = Value(true);
  arrayProtoJoin->nativeFunc = [toPrimitive](const std::vector<Value>& args) -> Value {
    auto toStringForJoin = [toPrimitive](const Value& input) -> std::string {
      Value primitive = isObjectLikeValue(input) ? toPrimitive(input, true) : input;
      if (primitive.isSymbol()) {
        throw std::runtime_error("TypeError: Cannot convert Symbol to string");
      }
      return primitive.toString();
    };

    if (args.empty() || (args[0].isNull() || args[0].isUndefined())) {
      throw std::runtime_error("TypeError: Array.prototype.join called on null or undefined");
    }
    Value thisVal = args[0];
    std::string separator = args.size() > 1 && !args[1].isUndefined() ? toStringForJoin(args[1]) : ",";
    std::string result;
    if (thisVal.isArray()) {
      auto arr = thisVal.getGC<Array>();
      for (size_t i = 0; i < arr->elements.size(); ++i) {
        if (i > 0) result += separator;
        if (!arr->elements[i].isUndefined() && !arr->elements[i].isNull()) {
          result += toStringForJoin(arr->elements[i]);
        }
      }
    } else {
      // Generic: read .length, iterate
      Interpreter* interp = getGlobalInterpreter();
      size_t len = 0;
      if (interp) {
        auto [found, lenVal] = interp->getPropertyForExternal(thisVal, "length");
        if (found) {
          double d = lenVal.toNumber();
          if (!std::isnan(d) && d >= 0) len = static_cast<size_t>(d);
        }
      }
      for (size_t i = 0; i < len; ++i) {
        if (i > 0) result += separator;
        if (interp) {
          auto [found, elem] = interp->getPropertyForExternal(thisVal, std::to_string(i));
          if (found && !elem.isUndefined() && !elem.isNull()) {
            result += toStringForJoin(elem);
          }
        }
      }
    }
    return Value(result);
  };
  arrayPrototype->properties["join"] = Value(arrayProtoJoin);
  arrayPrototype->properties["__non_enum_join"] = Value(true);

  auto arrayProtoReverse = GarbageCollector::makeGC<Function>();
  arrayProtoReverse->isNative = true;
  arrayProtoReverse->properties["__uses_this_arg__"] = Value(true);
  arrayProtoReverse->properties["name"] = Value(std::string("reverse"));
  arrayProtoReverse->properties["length"] = Value(0.0);

  arrayProtoReverse->properties["__non_writable_name"] = Value(true);
  arrayProtoReverse->properties["__non_enum_name"] = Value(true);
  arrayProtoReverse->properties["__non_writable_length"] = Value(true);
  arrayProtoReverse->properties["__non_enum_length"] = Value(true);
  arrayProtoReverse->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || args[0].isNull() || args[0].isUndefined()) {
      throw std::runtime_error("TypeError: Array.prototype.reverse called on null or undefined");
    }
    if (!args[0].isArray()) {
      // Generic object: reverse by swapping properties
      Interpreter* interp = getGlobalInterpreter();
      if (interp && args[0].isObject()) {
        auto obj = args[0].getGC<Object>();
        auto [found, lenVal] = interp->getPropertyForExternal(args[0], "length");
        if (found) {
          size_t len = static_cast<size_t>(lenVal.toNumber());
          for (size_t i = 0; i < len / 2; ++i) {
            size_t j = len - 1 - i;
            std::string ki = std::to_string(i), kj = std::to_string(j);
            bool hasI = obj->properties.count(ki) > 0;
            bool hasJ = obj->properties.count(kj) > 0;
            if (hasI && hasJ) {
              std::swap(obj->properties[ki], obj->properties[kj]);
            } else if (hasI) {
              obj->properties[kj] = obj->properties[ki];
              obj->properties.erase(ki);
            } else if (hasJ) {
              obj->properties[ki] = obj->properties[kj];
              obj->properties.erase(kj);
            }
          }
        }
      }
      return args[0];
    }
    auto arr = args[0].getGC<Array>();
    std::reverse(arr->elements.begin(), arr->elements.end());
    return args[0];
  };
  arrayPrototype->properties["reverse"] = Value(arrayProtoReverse);
  arrayPrototype->properties["__non_enum_reverse"] = Value(true);

  auto arrayProtoSort = GarbageCollector::makeGC<Function>();
  arrayProtoSort->isNative = true;
  arrayProtoSort->properties["__uses_this_arg__"] = Value(true);
  arrayProtoSort->properties["name"] = Value(std::string("sort"));
  arrayProtoSort->properties["length"] = Value(1.0);

  arrayProtoSort->properties["__non_writable_name"] = Value(true);
  arrayProtoSort->properties["__non_enum_name"] = Value(true);
  arrayProtoSort->properties["__non_writable_length"] = Value(true);
  arrayProtoSort->properties["__non_enum_length"] = Value(true);
  arrayProtoSort->nativeFunc = [toPrimitive](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      throw std::runtime_error("TypeError: Array.prototype.sort called on non-array");
    }

    auto arr = args[0].getGC<Array>();
    if (arr->elements.size() <= 1) {
      return args[0];
    }

    Value compareFn = args.size() > 1 ? args[1] : Value(Undefined{});
    if (!compareFn.isUndefined() && !compareFn.isFunction()) {
      throw std::runtime_error("TypeError: Array.prototype.sort comparator must be a function");
    }

    auto elementToString = [toPrimitive](const Value& value) -> std::string {
      if (value.isUndefined()) return std::string("undefined");
      Value primitive = isObjectLikeValue(value) ? toPrimitive(value, true) : value;
      if (primitive.isSymbol()) {
        throw std::runtime_error("TypeError: Cannot convert Symbol to string");
      }
      return primitive.toString();
    };

    Interpreter* interpreter = getGlobalInterpreter();
    std::stable_sort(arr->elements.begin(), arr->elements.end(),
      [&](const Value& lhs, const Value& rhs) {
        if (compareFn.isFunction()) {
          if (!interpreter) return false;
          Value result = interpreter->callForHarness(compareFn, {lhs, rhs}, Value(Undefined{}));
          if (interpreter->hasError()) {
            Value err = interpreter->getError();
            interpreter->clearError();
            throw JsValueException(err);
          }
          double number = result.toNumber();
          if (std::isnan(number) || number == 0.0) return false;
          return number < 0.0;
        }
        return elementToString(lhs) < elementToString(rhs);
      });

    return args[0];
  };
  arrayPrototype->properties["sort"] = Value(arrayProtoSort);
  arrayPrototype->properties["__non_enum_sort"] = Value(true);

  // Array.prototype.toString - calls join()
  auto arrayProtoToString = GarbageCollector::makeGC<Function>();
  arrayProtoToString->isNative = true;
  arrayProtoToString->properties["__uses_this_arg__"] = Value(true);
  arrayProtoToString->properties["name"] = Value(std::string("toString"));
  arrayProtoToString->properties["length"] = Value(0.0);

  arrayProtoToString->properties["__non_writable_name"] = Value(true);
  arrayProtoToString->properties["__non_enum_name"] = Value(true);
  arrayProtoToString->properties["__non_writable_length"] = Value(true);
  arrayProtoToString->properties["__non_enum_length"] = Value(true);
  arrayProtoToString->nativeFunc = [env, callChecked](const std::vector<Value>& args) -> Value {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
      throw std::runtime_error("TypeError: Array.prototype.toString called on null or undefined");
    }

    auto [foundJoin, join] = getPropertyLike(args[0], "join", args[0]);
    if (foundJoin && join.isFunction()) {
      return callChecked(join, {}, args[0]);
    }

    if (auto objectProto = env->get("__object_prototype__"); objectProto.has_value()) {
      auto [foundToString, objectToString] = getPropertyLike(*objectProto, "toString", *objectProto);
      if (foundToString && objectToString.isFunction()) {
        return callChecked(objectToString, {}, args[0]);
      }
    }

    return Value(std::string("[object Object]"));
  };
  arrayPrototype->properties["toString"] = Value(arrayProtoToString);
  arrayPrototype->properties["__non_enum_toString"] = Value(true);

  auto arrayProtoToLocaleString = GarbageCollector::makeGC<Function>();
  arrayProtoToLocaleString->isNative = true;
  arrayProtoToLocaleString->properties["__uses_this_arg__"] = Value(true);
  arrayProtoToLocaleString->properties["name"] = Value(std::string("toLocaleString"));
  arrayProtoToLocaleString->properties["length"] = Value(0.0);

  arrayProtoToLocaleString->properties["__non_writable_name"] = Value(true);
  arrayProtoToLocaleString->properties["__non_enum_name"] = Value(true);
  arrayProtoToLocaleString->properties["__non_writable_length"] = Value(true);
  arrayProtoToLocaleString->properties["__non_enum_length"] = Value(true);
  arrayProtoToLocaleString->nativeFunc = [env, toPrimitive, callChecked](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      throw std::runtime_error("TypeError: Array.prototype.toLocaleString called on non-array");
    }

    auto lookupPrimitiveLocaleMethod = [&](const Value& element) -> Value {
      auto lookupFromCtor = [&](const char* ctorName) -> Value {
        if (auto ctor = env->get(ctorName); ctor.has_value()) {
          auto [foundProto, proto] = getPropertyLike(*ctor, "prototype", element);
          if (foundProto) {
            auto [foundMethod, method] = getPropertyLike(proto, "toLocaleString", element);
            if (foundMethod) {
              return method;
            }
          }
        }
        return Value(Undefined{});
      };

      if (element.isString()) return lookupFromCtor("String");
      if (element.isNumber()) return lookupFromCtor("Number");
      if (element.isBigInt()) return lookupFromCtor("BigInt");
      if (element.isBool()) return lookupFromCtor("Boolean");
      return Value(Undefined{});
    };

    auto elementToLocaleString = [&](const Value& element) -> std::string {
      if (element.isUndefined() || element.isNull()) {
        return std::string("");
      }

      Value method = isObjectLikeValue(element)
        ? [&]() {
            auto [foundMethod, value] = getPropertyLike(element, "toLocaleString", element);
            return foundMethod ? value : Value(Undefined{});
          }()
        : lookupPrimitiveLocaleMethod(element);

      if (!method.isFunction()) {
        throw std::runtime_error("TypeError: undefined is not a function");
      }

      Value result = callChecked(method, {}, element);
      Value primitive = isObjectLikeValue(result) ? toPrimitive(result, true) : result;
      return primitive.toString();
    };

    auto arr = args[0].getGC<Array>();
    std::string result;
    for (size_t i = 0; i < arr->elements.size(); ++i) {
      if (i > 0) result += ",";
      result += elementToLocaleString(arr->elements[i]);
    }
    return Value(result);
  };
  arrayPrototype->properties["toLocaleString"] = Value(arrayProtoToLocaleString);
  arrayPrototype->properties["__non_enum_toLocaleString"] = Value(true);

  if (auto typedArrayCtor = env->get("TypedArray"); typedArrayCtor.has_value()) {
    auto [foundProto, typedArrayProto] = getPropertyLike(*typedArrayCtor, "prototype", *typedArrayCtor);
    if (foundProto && typedArrayProto.isObject()) {
      auto typedArrayPrototype = typedArrayProto.getGC<Object>();
      typedArrayPrototype->properties["toString"] = Value(arrayProtoToString);
      typedArrayPrototype->properties["__non_enum_toString"] = Value(true);
    }
  }

  // Array.prototype.reduce - placeholder (installed below via installArrayMethod)

  // Array.prototype.indexOf
  auto arrayProtoIndexOf = GarbageCollector::makeGC<Function>();
  arrayProtoIndexOf->isNative = true;
  arrayProtoIndexOf->properties["__uses_this_arg__"] = Value(true);
  arrayProtoIndexOf->properties["name"] = Value(std::string("indexOf"));
  arrayProtoIndexOf->properties["length"] = Value(1.0);
  arrayProtoIndexOf->properties["__non_writable_name"] = Value(true);
  arrayProtoIndexOf->properties["__non_enum_name"] = Value(true);
  arrayProtoIndexOf->properties["__non_writable_length"] = Value(true);
  arrayProtoIndexOf->properties["__non_enum_length"] = Value(true);
  arrayProtoIndexOf->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
      throw std::runtime_error("TypeError: Array.prototype.indexOf called on null or undefined");
    }

    auto strictEqual = [](const Value& lhs, const Value& rhs) -> bool {
      if (lhs.data.index() != rhs.data.index()) return false;
      if (lhs.isSymbol() && rhs.isSymbol()) return std::get<Symbol>(lhs.data).id == std::get<Symbol>(rhs.data).id;
      if (lhs.isBigInt() && rhs.isBigInt()) return lhs.toBigInt() == rhs.toBigInt();
      if (lhs.isNumber() && rhs.isNumber()) return lhs.toNumber() == rhs.toNumber();
      if (lhs.isString() && rhs.isString()) return lhs.toString() == rhs.toString();
      if (lhs.isBool() && rhs.isBool()) return lhs.toBool() == rhs.toBool();
      if ((lhs.isNull() && rhs.isNull()) || (lhs.isUndefined() && rhs.isUndefined())) return true;
      if (lhs.isObject() && rhs.isObject()) return lhs.getGC<Object>().get() == rhs.getGC<Object>().get();
      if (lhs.isArray() && rhs.isArray()) return lhs.getGC<Array>().get() == rhs.getGC<Array>().get();
      if (lhs.isFunction() && rhs.isFunction()) return lhs.getGC<Function>().get() == rhs.getGC<Function>().get();
      if (lhs.isTypedArray() && rhs.isTypedArray()) return lhs.getGC<TypedArray>().get() == rhs.getGC<TypedArray>().get();
      if (lhs.isPromise() && rhs.isPromise()) return lhs.getGC<Promise>().get() == rhs.getGC<Promise>().get();
      if (lhs.isRegex() && rhs.isRegex()) return lhs.getGC<Regex>().get() == rhs.getGC<Regex>().get();
      if (lhs.isMap() && rhs.isMap()) return lhs.getGC<Map>().get() == rhs.getGC<Map>().get();
      if (lhs.isSet() && rhs.isSet()) return lhs.getGC<Set>().get() == rhs.getGC<Set>().get();
      if (lhs.isError() && rhs.isError()) return lhs.getGC<Error>().get() == rhs.getGC<Error>().get();
      if (lhs.isGenerator() && rhs.isGenerator()) return lhs.getGC<Generator>().get() == rhs.getGC<Generator>().get();
      if (lhs.isProxy() && rhs.isProxy()) return lhs.getGC<Proxy>().get() == rhs.getGC<Proxy>().get();
      if (lhs.isWeakMap() && rhs.isWeakMap()) return lhs.getGC<WeakMap>().get() == rhs.getGC<WeakMap>().get();
      if (lhs.isWeakSet() && rhs.isWeakSet()) return lhs.getGC<WeakSet>().get() == rhs.getGC<WeakSet>().get();
      if (lhs.isArrayBuffer() && rhs.isArrayBuffer()) return lhs.getGC<ArrayBuffer>().get() == rhs.getGC<ArrayBuffer>().get();
      if (lhs.isDataView() && rhs.isDataView()) return lhs.getGC<DataView>().get() == rhs.getGC<DataView>().get();
      if (lhs.isClass() && rhs.isClass()) return lhs.getGC<Class>().get() == rhs.getGC<Class>().get();
      if (lhs.isWasmInstance() && rhs.isWasmInstance()) return lhs.getGC<WasmInstanceJS>().get() == rhs.getGC<WasmInstanceJS>().get();
      if (lhs.isWasmMemory() && rhs.isWasmMemory()) return lhs.getGC<WasmMemoryJS>().get() == rhs.getGC<WasmMemoryJS>().get();
      if (lhs.isReadableStream() && rhs.isReadableStream()) return lhs.getGC<ReadableStream>().get() == rhs.getGC<ReadableStream>().get();
      if (lhs.isWritableStream() && rhs.isWritableStream()) return lhs.getGC<WritableStream>().get() == rhs.getGC<WritableStream>().get();
      if (lhs.isTransformStream() && rhs.isTransformStream()) return lhs.getGC<TransformStream>().get() == rhs.getGC<TransformStream>().get();
      return false;
    };

    auto toArrayLikeObject = [](const Value& input) -> Value {
      if (isObjectLikeValue(input)) {
        return input;
      }
      if (input.isString()) {
        const std::string& text = input.toString();
        auto obj = GarbageCollector::makeGC<Object>();
        GarbageCollector::instance().reportAllocation(sizeof(Object));
        obj->properties["length"] = Value(static_cast<double>(text.size()));
        for (size_t i = 0; i < text.size(); ++i) {
          obj->properties[std::to_string(i)] = Value(std::string(1, text[i]));
        }
        return Value(obj);
      }
      auto obj = GarbageCollector::makeGC<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      return Value(obj);
    };

    auto toLength = [](const Value& value) -> size_t {
      double number = value.toNumber();
      if (std::isnan(number) || number <= 0.0) {
        return 0;
      }
      if (!std::isfinite(number)) {
        return std::numeric_limits<size_t>::max();
      }
      double truncated = std::floor(number);
      if (truncated >= static_cast<double>(std::numeric_limits<size_t>::max())) {
        return std::numeric_limits<size_t>::max();
      }
      return static_cast<size_t>(truncated);
    };

    auto toIntegerOrInfinity = [](const Value& value) -> double {
      double number = value.toNumber();
      if (std::isnan(number) || number == 0.0) {
        return 0.0;
      }
      if (!std::isfinite(number)) {
        return number;
      }
      return std::trunc(number);
    };

    Value receiver = args[0];
    Value searchElement = args.size() > 1 ? args[1] : Value(Undefined{});
    Value arrayLike = toArrayLikeObject(receiver);
    auto [foundLength, lengthValue] = getPropertyLike(arrayLike, "length", arrayLike);
    if (auto* interpreter = getGlobalInterpreter(); interpreter && interpreter->hasError()) {
      Value err = interpreter->getError();
      interpreter->clearError();
      throw JsValueException(err);
    }
    size_t length = foundLength ? toLength(lengthValue) : 0;
    if (length == 0) {
      return Value(-1.0);
    }

    double fromIndex = args.size() > 2 ? toIntegerOrInfinity(args[2]) : 0.0;
    if (fromIndex == std::numeric_limits<double>::infinity()) {
      return Value(-1.0);
    }

    size_t start = 0;
    if (fromIndex >= 0.0) {
      start = fromIndex >= static_cast<double>(length) ? length : static_cast<size_t>(fromIndex);
    } else if (fromIndex != -std::numeric_limits<double>::infinity()) {
      double relative = static_cast<double>(length) + fromIndex;
      start = relative <= 0.0 ? 0 : static_cast<size_t>(relative);
    }

    for (size_t i = start; i < length; ++i) {
      std::string key = std::to_string(i);
      if (!hasPropertyLike(arrayLike, key)) {
        if (auto* interpreter = getGlobalInterpreter(); interpreter && interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw JsValueException(err);
        }
        continue;
      }
      auto [found, element] = getPropertyLike(arrayLike, key, arrayLike);
      if (auto* interpreter = getGlobalInterpreter(); interpreter && interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw JsValueException(err);
      }
      if (found && strictEqual(element, searchElement)) {
        return Value(static_cast<double>(i));
      }
    }
    return Value(-1.0);
  };
  arrayPrototype->properties["indexOf"] = Value(arrayProtoIndexOf);
  arrayPrototype->properties["__non_enum_indexOf"] = Value(true);

  // --- Generic array-like helpers for ES spec Array prototype methods ---
  // These allow Array.prototype.map.call(obj, fn) etc. to work on any object with .length

  // Strict equality for search methods
  auto arrayStrictEqual = [](const Value& lhs, const Value& rhs) -> bool {
    if (lhs.data.index() != rhs.data.index()) return false;
    if (lhs.isNumber() && rhs.isNumber()) return lhs.toNumber() == rhs.toNumber();
    if (lhs.isString() && rhs.isString()) return lhs.toString() == rhs.toString();
    if (lhs.isBool() && rhs.isBool()) return lhs.toBool() == rhs.toBool();
    if (lhs.isSymbol() && rhs.isSymbol()) return std::get<Symbol>(lhs.data).id == std::get<Symbol>(rhs.data).id;
    if (lhs.isBigInt() && rhs.isBigInt()) return lhs.toBigInt() == rhs.toBigInt();
    if ((lhs.isNull() && rhs.isNull()) || (lhs.isUndefined() && rhs.isUndefined())) return true;
    if (lhs.isObject() && rhs.isObject()) return lhs.getGC<Object>().get() == rhs.getGC<Object>().get();
    if (lhs.isArray() && rhs.isArray()) return lhs.getGC<Array>().get() == rhs.getGC<Array>().get();
    if (lhs.isFunction() && rhs.isFunction()) return lhs.getGC<Function>().get() == rhs.getGC<Function>().get();
    if (lhs.isClass() && rhs.isClass()) return lhs.getGC<Class>().get() == rhs.getGC<Class>().get();
    if (lhs.isError() && rhs.isError()) return lhs.getGC<Error>().get() == rhs.getGC<Error>().get();
    if (lhs.isRegex() && rhs.isRegex()) return lhs.getGC<Regex>().get() == rhs.getGC<Regex>().get();
    if (lhs.isMap() && rhs.isMap()) return lhs.getGC<Map>().get() == rhs.getGC<Map>().get();
    if (lhs.isSet() && rhs.isSet()) return lhs.getGC<Set>().get() == rhs.getGC<Set>().get();
    if (lhs.isPromise() && rhs.isPromise()) return lhs.getGC<Promise>().get() == rhs.getGC<Promise>().get();
    if (lhs.isTypedArray() && rhs.isTypedArray()) return lhs.getGC<TypedArray>().get() == rhs.getGC<TypedArray>().get();
    if (lhs.isArrayBuffer() && rhs.isArrayBuffer()) return lhs.getGC<ArrayBuffer>().get() == rhs.getGC<ArrayBuffer>().get();
    if (lhs.isProxy() && rhs.isProxy()) return lhs.getGC<Proxy>().get() == rhs.getGC<Proxy>().get();
    if (lhs.isGenerator() && rhs.isGenerator()) return lhs.getGC<Generator>().get() == rhs.getGC<Generator>().get();
    if (lhs.isWeakMap() && rhs.isWeakMap()) return lhs.getGC<WeakMap>().get() == rhs.getGC<WeakMap>().get();
    if (lhs.isWeakSet() && rhs.isWeakSet()) return lhs.getGC<WeakSet>().get() == rhs.getGC<WeakSet>().get();
    if (lhs.isDataView() && rhs.isDataView()) return lhs.getGC<DataView>().get() == rhs.getGC<DataView>().get();
    return false;
  };

  // ToObject: throw TypeError on null/undefined, return this otherwise
  auto toObjectChecked = [](const Value& thisVal, const std::string& methodName) -> Value {
    if (thisVal.isNull() || thisVal.isUndefined()) {
      throw std::runtime_error("TypeError: Array.prototype." + methodName + " called on null or undefined");
    }
    return thisVal;
  };

  // Get length from any array-like value
  auto getArrayLikeLength = [](const Value& obj) -> size_t {
    if (obj.isArray()) {
      return obj.getGC<Array>()->elements.size();
    }
    if (obj.isString()) {
      // String length = code point count
      return obj.toString().size(); // byte count for simple impl; proper would be utf8 length
    }
    // Generic object: read .length property
    Interpreter* interp = getGlobalInterpreter();
    if (interp) {
      auto [found, lenVal] = interp->getPropertyForExternal(obj, "length");
      if (found) {
        double d = lenVal.toNumber();
        if (std::isnan(d) || d < 0) return 0;
        return static_cast<size_t>(d);
      }
    }
    return 0;
  };

  // Get element at index from array-like value
  auto getArrayLikeElement = [](const Value& obj, size_t idx) -> std::pair<bool, Value> {
    if (obj.isArray()) {
      auto arr = obj.getGC<Array>();
      if (idx >= arr->elements.size()) return {false, Value(Undefined{})};
      auto iStr = std::to_string(idx);
      if (arr->properties.count("__hole_" + iStr + "__")) return {false, Value(Undefined{})};
      if (arr->properties.count("__deleted_" + iStr + "__")) return {false, Value(Undefined{})};
      return {true, arr->elements[idx]};
    }
    Interpreter* interp = getGlobalInterpreter();
    if (interp) {
      auto [found, val] = interp->getPropertyForExternal(obj, std::to_string(idx));
      return {found, val};
    }
    return {false, Value(Undefined{})};
  };

  // Helper lambda for installing Array prototype methods that need callback support
  auto installArrayMethod = [&](const std::string& name, int length,
      std::function<Value(const std::vector<Value>&)> impl) {
    auto fn = GarbageCollector::makeGC<Function>();
    fn->isNative = true;
    fn->properties["__uses_this_arg__"] = Value(true);
    fn->properties["name"] = Value(name);
    fn->properties["length"] = Value(static_cast<double>(length));
    fn->properties["__non_writable_name"] = Value(true);
    fn->properties["__non_enum_name"] = Value(true);
    fn->properties["__non_writable_length"] = Value(true);
    fn->properties["__non_enum_length"] = Value(true);
    fn->nativeFunc = impl;
    arrayPrototype->properties[name] = Value(fn);
    arrayPrototype->properties["__non_enum_" + name] = Value(true);
  };

  // Array.prototype.map - generic (works on any array-like)
  installArrayMethod("map", 1, [toObjectChecked, getArrayLikeLength, getArrayLikeElement](const std::vector<Value>& args) -> Value {
    Value thisVal = toObjectChecked(args.empty() ? Value(Undefined{}) : args[0], "map");
    if (args.size() < 2 || !args[1].isFunction()) {
      throw std::runtime_error("TypeError: undefined is not a function");
    }
    Value callback = args[1];
    Value thisArg = args.size() > 2 ? args[2] : Value(Undefined{});
    size_t len = getArrayLikeLength(thisVal);
    auto result = makeArrayWithPrototype();
    result->elements.resize(len, Value(Undefined{}));
    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) throw std::runtime_error("TypeError: Interpreter unavailable");
    for (size_t i = 0; i < len; ++i) {
      auto [exists, elem] = getArrayLikeElement(thisVal, i);
      if (!exists) continue;
      std::vector<Value> callArgs = {elem, Value(static_cast<double>(i)), thisVal};
      Value mapped = interpreter->callForHarness(callback, callArgs, thisArg);
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw std::runtime_error(err.toString());
      }
      result->elements[i] = mapped;
    }
    return Value(result);
  });

  // Array.prototype.filter - generic
  installArrayMethod("filter", 1, [toObjectChecked, getArrayLikeLength, getArrayLikeElement](const std::vector<Value>& args) -> Value {
    Value thisVal = toObjectChecked(args.empty() ? Value(Undefined{}) : args[0], "filter");
    if (args.size() < 2 || !args[1].isFunction()) {
      throw std::runtime_error("TypeError: undefined is not a function");
    }
    Value callback = args[1];
    Value thisArg = args.size() > 2 ? args[2] : Value(Undefined{});
    size_t len = getArrayLikeLength(thisVal);
    auto result = makeArrayWithPrototype();
    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) throw std::runtime_error("TypeError: Interpreter unavailable");
    for (size_t i = 0; i < len; ++i) {
      auto [exists, elem] = getArrayLikeElement(thisVal, i);
      if (!exists) continue;
      std::vector<Value> callArgs = {elem, Value(static_cast<double>(i)), thisVal};
      Value keep = interpreter->callForHarness(callback, callArgs, thisArg);
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw std::runtime_error(err.toString());
      }
      if (keep.toBool()) {
        result->elements.push_back(elem);
      }
    }
    return Value(result);
  });

  // Array.prototype.forEach - generic
  installArrayMethod("forEach", 1, [toObjectChecked, getArrayLikeLength, getArrayLikeElement](const std::vector<Value>& args) -> Value {
    Value thisVal = toObjectChecked(args.empty() ? Value(Undefined{}) : args[0], "forEach");
    if (args.size() < 2 || !args[1].isFunction()) {
      throw std::runtime_error("TypeError: undefined is not a function");
    }
    Value callback = args[1];
    Value thisArg = args.size() > 2 ? args[2] : Value(Undefined{});
    size_t len = getArrayLikeLength(thisVal);
    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) throw std::runtime_error("TypeError: Interpreter unavailable");
    for (size_t i = 0; i < len; ++i) {
      auto [exists, elem] = getArrayLikeElement(thisVal, i);
      if (!exists) continue;
      std::vector<Value> callArgs = {elem, Value(static_cast<double>(i)), thisVal};
      interpreter->callForHarness(callback, callArgs, thisArg);
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw std::runtime_error(err.toString());
      }
    }
    return Value(Undefined{});
  });

  // Array.prototype.some - generic
  installArrayMethod("some", 1, [toObjectChecked, getArrayLikeLength, getArrayLikeElement](const std::vector<Value>& args) -> Value {
    Value thisVal = toObjectChecked(args.empty() ? Value(Undefined{}) : args[0], "some");
    if (args.size() < 2 || !args[1].isFunction()) {
      throw std::runtime_error("TypeError: undefined is not a function");
    }
    Value callback = args[1];
    Value thisArg = args.size() > 2 ? args[2] : Value(Undefined{});
    size_t len = getArrayLikeLength(thisVal);
    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) throw std::runtime_error("TypeError: Interpreter unavailable");
    for (size_t i = 0; i < len; ++i) {
      auto [exists, elem] = getArrayLikeElement(thisVal, i);
      if (!exists) continue;
      std::vector<Value> callArgs = {elem, Value(static_cast<double>(i)), thisVal};
      Value result = interpreter->callForHarness(callback, callArgs, thisArg);
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw std::runtime_error(err.toString());
      }
      if (result.toBool()) return Value(true);
    }
    return Value(false);
  });

  // Array.prototype.every - generic
  installArrayMethod("every", 1, [toObjectChecked, getArrayLikeLength, getArrayLikeElement](const std::vector<Value>& args) -> Value {
    Value thisVal = toObjectChecked(args.empty() ? Value(Undefined{}) : args[0], "every");
    if (args.size() < 2 || !args[1].isFunction()) {
      throw std::runtime_error("TypeError: undefined is not a function");
    }
    Value callback = args[1];
    Value thisArg = args.size() > 2 ? args[2] : Value(Undefined{});
    size_t len = getArrayLikeLength(thisVal);
    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) throw std::runtime_error("TypeError: Interpreter unavailable");
    for (size_t i = 0; i < len; ++i) {
      auto [exists, elem] = getArrayLikeElement(thisVal, i);
      if (!exists) continue;
      std::vector<Value> callArgs = {elem, Value(static_cast<double>(i)), thisVal};
      Value result = interpreter->callForHarness(callback, callArgs, thisArg);
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw std::runtime_error(err.toString());
      }
      if (!result.toBool()) return Value(false);
    }
    return Value(true);
  });

  // Array.prototype.find - generic
  installArrayMethod("find", 1, [toObjectChecked, getArrayLikeLength, getArrayLikeElement](const std::vector<Value>& args) -> Value {
    Value thisVal = toObjectChecked(args.empty() ? Value(Undefined{}) : args[0], "find");
    if (args.size() < 2 || !args[1].isFunction()) {
      throw std::runtime_error("TypeError: undefined is not a function");
    }
    Value callback = args[1];
    Value thisArg = args.size() > 2 ? args[2] : Value(Undefined{});
    size_t len = getArrayLikeLength(thisVal);
    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) throw std::runtime_error("TypeError: Interpreter unavailable");
    for (size_t i = 0; i < len; ++i) {
      auto [exists, elem] = getArrayLikeElement(thisVal, i);
      Value kValue = exists ? elem : Value(Undefined{});
      std::vector<Value> callArgs = {kValue, Value(static_cast<double>(i)), thisVal};
      Value result = interpreter->callForHarness(callback, callArgs, thisArg);
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw std::runtime_error(err.toString());
      }
      if (result.toBool()) return kValue;
    }
    return Value(Undefined{});
  });

  // Array.prototype.findIndex - generic
  installArrayMethod("findIndex", 1, [toObjectChecked, getArrayLikeLength, getArrayLikeElement](const std::vector<Value>& args) -> Value {
    Value thisVal = toObjectChecked(args.empty() ? Value(Undefined{}) : args[0], "findIndex");
    if (args.size() < 2 || !args[1].isFunction()) {
      throw std::runtime_error("TypeError: undefined is not a function");
    }
    Value callback = args[1];
    Value thisArg = args.size() > 2 ? args[2] : Value(Undefined{});
    size_t len = getArrayLikeLength(thisVal);
    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) throw std::runtime_error("TypeError: Interpreter unavailable");
    for (size_t i = 0; i < len; ++i) {
      auto [exists, elem] = getArrayLikeElement(thisVal, i);
      Value kValue = exists ? elem : Value(Undefined{});
      std::vector<Value> callArgs = {kValue, Value(static_cast<double>(i)), thisVal};
      Value result = interpreter->callForHarness(callback, callArgs, thisArg);
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw std::runtime_error(err.toString());
      }
      if (result.toBool()) return Value(static_cast<double>(i));
    }
    return Value(-1.0);
  });

  // Array.prototype.findLast - generic
  installArrayMethod("findLast", 1, [toObjectChecked, getArrayLikeLength, getArrayLikeElement](const std::vector<Value>& args) -> Value {
    Value thisVal = toObjectChecked(args.empty() ? Value(Undefined{}) : args[0], "findLast");
    if (args.size() < 2 || !args[1].isFunction()) {
      throw std::runtime_error("TypeError: undefined is not a function");
    }
    Value callback = args[1];
    Value thisArg = args.size() > 2 ? args[2] : Value(Undefined{});
    size_t len = getArrayLikeLength(thisVal);
    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) throw std::runtime_error("TypeError: Interpreter unavailable");
    for (int i = static_cast<int>(len) - 1; i >= 0; --i) {
      auto [exists, elem] = getArrayLikeElement(thisVal, static_cast<size_t>(i));
      Value kValue = exists ? elem : Value(Undefined{});
      std::vector<Value> callArgs = {kValue, Value(static_cast<double>(i)), thisVal};
      Value result = interpreter->callForHarness(callback, callArgs, thisArg);
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw std::runtime_error(err.toString());
      }
      if (result.toBool()) return kValue;
    }
    return Value(Undefined{});
  });

  // Array.prototype.findLastIndex - generic
  installArrayMethod("findLastIndex", 1, [toObjectChecked, getArrayLikeLength, getArrayLikeElement](const std::vector<Value>& args) -> Value {
    Value thisVal = toObjectChecked(args.empty() ? Value(Undefined{}) : args[0], "findLastIndex");
    if (args.size() < 2 || !args[1].isFunction()) {
      throw std::runtime_error("TypeError: undefined is not a function");
    }
    Value callback = args[1];
    Value thisArg = args.size() > 2 ? args[2] : Value(Undefined{});
    size_t len = getArrayLikeLength(thisVal);
    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) throw std::runtime_error("TypeError: Interpreter unavailable");
    for (int i = static_cast<int>(len) - 1; i >= 0; --i) {
      auto [exists, elem] = getArrayLikeElement(thisVal, static_cast<size_t>(i));
      Value kValue = exists ? elem : Value(Undefined{});
      std::vector<Value> callArgs = {kValue, Value(static_cast<double>(i)), thisVal};
      Value result = interpreter->callForHarness(callback, callArgs, thisArg);
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw std::runtime_error(err.toString());
      }
      if (result.toBool()) return Value(static_cast<double>(i));
    }
    return Value(-1.0);
  });

  // Array.prototype.includes - generic
  installArrayMethod("includes", 1, [toObjectChecked, getArrayLikeLength, getArrayLikeElement](const std::vector<Value>& args) -> Value {
    Value thisVal = toObjectChecked(args.empty() ? Value(Undefined{}) : args[0], "includes");
    Value searchElement = args.size() > 1 ? args[1] : Value(Undefined{});
    size_t len = getArrayLikeLength(thisVal);
    if (len == 0) return Value(false);
    // ToIntegerOrInfinity for fromIndex
    double n = 0;
    if (args.size() > 2) {
      double fi = args[2].toNumber();
      if (std::isnan(fi)) n = 0;
      else n = std::trunc(fi);
    }
    size_t k = 0;
    if (n >= 0) {
      if (n >= static_cast<double>(len)) return Value(false);
      k = static_cast<size_t>(n);
    } else {
      double relative = static_cast<double>(len) + n;
      k = relative < 0 ? 0 : static_cast<size_t>(relative);
    }
    auto sameValueZero = [](const Value& a, const Value& b) -> bool {
      if (a.isNumber() && b.isNumber()) {
        double x = std::get<double>(a.data), y = std::get<double>(b.data);
        if (std::isnan(x) && std::isnan(y)) return true;
        return x == y;
      }
      if (a.data.index() != b.data.index()) return false;
      if (a.isString()) return std::get<std::string>(a.data) == std::get<std::string>(b.data);
      if (a.isBool()) return std::get<bool>(a.data) == std::get<bool>(b.data);
      if (a.isNull() || a.isUndefined()) return true;
      if (a.isObject()) return a.getGC<Object>().get() == b.getGC<Object>().get();
      if (a.isArray()) return a.getGC<Array>().get() == b.getGC<Array>().get();
      if (a.isFunction()) return a.getGC<Function>().get() == b.getGC<Function>().get();
      if (a.isSymbol()) return std::get<Symbol>(a.data).id == std::get<Symbol>(b.data).id;
      return false;
    };
    for (size_t i = k; i < len; ++i) {
      auto [exists, elem] = getArrayLikeElement(thisVal, i);
      if (!exists) {
        if (searchElement.isUndefined()) return Value(true);
        continue;
      }
      if (sameValueZero(elem, searchElement)) return Value(true);
    }
    return Value(false);
  });

  // Array.prototype.reduceRight - generic
  installArrayMethod("reduceRight", 1, [toObjectChecked, getArrayLikeLength, getArrayLikeElement](const std::vector<Value>& args) -> Value {
    Value thisVal = toObjectChecked(args.empty() ? Value(Undefined{}) : args[0], "reduceRight");
    if (args.size() < 2 || !args[1].isFunction()) {
      throw std::runtime_error("TypeError: undefined is not a function");
    }
    Value callback = args[1];
    size_t len = getArrayLikeLength(thisVal);
    bool hasInitial = args.size() >= 3;
    Value accumulator(Undefined{});
    int k = static_cast<int>(len) - 1;
    if (hasInitial) {
      accumulator = args[2];
    } else {
      bool found = false;
      while (k >= 0 && !found) {
        auto [exists, elem] = getArrayLikeElement(thisVal, static_cast<size_t>(k));
        if (exists) { accumulator = elem; found = true; }
        k--;
      }
      if (!found) throw std::runtime_error("TypeError: Reduce of empty array with no initial value");
    }
    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) throw std::runtime_error("TypeError: Interpreter unavailable");
    for (; k >= 0; --k) {
      auto [exists, elem] = getArrayLikeElement(thisVal, static_cast<size_t>(k));
      if (!exists) continue;
      std::vector<Value> callArgs = {accumulator, elem, Value(static_cast<double>(k)), thisVal};
      accumulator = interpreter->callForHarness(callback, callArgs, Value(Undefined{}));
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw std::runtime_error(err.toString());
      }
    }
    return accumulator;
  });

  // Array.prototype.reduce - generic
  installArrayMethod("reduce", 1, [toObjectChecked, getArrayLikeLength, getArrayLikeElement](const std::vector<Value>& args) -> Value {
    Value thisVal = toObjectChecked(args.empty() ? Value(Undefined{}) : args[0], "reduce");
    if (args.size() < 2 || !args[1].isFunction()) {
      throw std::runtime_error("TypeError: undefined is not a function");
    }
    Value callback = args[1];
    size_t len = getArrayLikeLength(thisVal);
    bool hasInitial = args.size() >= 3;
    Value accumulator(Undefined{});
    size_t k = 0;
    if (hasInitial) {
      accumulator = args[2];
    } else {
      bool found = false;
      while (k < len && !found) {
        auto [exists, elem] = getArrayLikeElement(thisVal, k);
        if (exists) { accumulator = elem; found = true; }
        k++;
      }
      if (!found) throw std::runtime_error("TypeError: Reduce of empty array with no initial value");
    }
    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) throw std::runtime_error("TypeError: Interpreter unavailable");
    for (; k < len; ++k) {
      auto [exists, elem] = getArrayLikeElement(thisVal, k);
      if (!exists) continue;
      std::vector<Value> callArgs = {accumulator, elem, Value(static_cast<double>(k)), thisVal};
      accumulator = interpreter->callForHarness(callback, callArgs, Value(Undefined{}));
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw std::runtime_error(err.toString());
      }
    }
    return accumulator;
  });

  // Array.prototype.lastIndexOf - generic
  installArrayMethod("lastIndexOf", 1, [toObjectChecked, getArrayLikeLength, getArrayLikeElement, arrayStrictEqual](const std::vector<Value>& args) -> Value {
    Value thisVal = toObjectChecked(args.empty() ? Value(Undefined{}) : args[0], "lastIndexOf");
    Value searchElement = args.size() > 1 ? args[1] : Value(Undefined{});
    int len = static_cast<int>(getArrayLikeLength(thisVal));
    if (len == 0) return Value(-1.0);
    int fromIndex = len - 1;
    if (args.size() > 2) {
      double fi = args[2].toNumber();
      if (std::isnan(fi)) return Value(-1.0);
      fromIndex = static_cast<int>(fi);
      if (fromIndex < 0) fromIndex = len + fromIndex;
    }
    if (fromIndex >= len) fromIndex = len - 1;
    for (int i = fromIndex; i >= 0; --i) {
      auto [exists, elem] = getArrayLikeElement(thisVal, static_cast<size_t>(i));
      if (!exists) continue;
      if (arrayStrictEqual(elem, searchElement)) return Value(static_cast<double>(i));
    }
    return Value(-1.0);
  });

  // Array.prototype.flat
  installArrayMethod("flat", 0, [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      throw std::runtime_error("TypeError: Array.prototype.flat called on non-array");
    }
    auto arr = args[0].getGC<Array>();
    double depth = 1;
    if (args.size() > 1 && !args[1].isUndefined()) {
      depth = args[1].toNumber();
      if (std::isnan(depth) || depth < 0) depth = 0;
      depth = std::floor(depth);
    }
    auto result = GarbageCollector::makeGC<Array>();
    std::function<void(const GCPtr<Array>&, double)> flatten;
    flatten = [&](const GCPtr<Array>& src, double d) {
      for (const auto& elem : src->elements) {
        if (d > 0 && elem.isArray()) {
          flatten(elem.getGC<Array>(), d - 1);
        } else {
          result->elements.push_back(elem);
        }
      }
    };
    flatten(arr, depth);
    return Value(result);
  });

  // Array.prototype.flatMap
  installArrayMethod("flatMap", 1, [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      throw std::runtime_error("TypeError: Array.prototype.flatMap called on non-array");
    }
    if (args.size() < 2 || !args[1].isFunction()) {
      throw std::runtime_error("TypeError: Array.prototype.flatMap requires a callback function");
    }
    auto arr = args[0].getGC<Array>();
    Value callback = args[1];
    Value thisArg = args.size() > 2 ? args[2] : Value(Undefined{});
    auto result = GarbageCollector::makeGC<Array>();
    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) throw std::runtime_error("TypeError: Interpreter unavailable");
    for (size_t i = 0; i < arr->elements.size(); ++i) {
      std::vector<Value> callArgs = {arr->elements[i], Value(static_cast<double>(i)), args[0]};
      Value mapped = interpreter->callForHarness(callback, callArgs, thisArg);
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw std::runtime_error(err.toString());
      }
      if (mapped.isArray()) {
        auto mappedArr = mapped.getGC<Array>();
        for (const auto& elem : mappedArr->elements) {
          result->elements.push_back(elem);
        }
      } else {
        result->elements.push_back(mapped);
      }
    }
    return Value(result);
  });

  // Array.prototype.fill
  installArrayMethod("fill", 1, [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      throw std::runtime_error("TypeError: Array.prototype.fill called on non-array");
    }
    auto arr = args[0].getGC<Array>();
    Value fillValue = args.size() > 1 ? args[1] : Value(Undefined{});
    int len = static_cast<int>(arr->elements.size());
    int start = 0, end = len;
    if (args.size() > 2 && !args[2].isUndefined()) {
      start = static_cast<int>(args[2].toNumber());
      if (start < 0) start = std::max(0, len + start);
    }
    if (args.size() > 3 && !args[3].isUndefined()) {
      end = static_cast<int>(args[3].toNumber());
      if (end < 0) end = std::max(0, len + end);
    }
    start = std::min(start, len);
    end = std::min(end, len);
    for (int i = start; i < end; ++i) {
      arr->elements[i] = fillValue;
    }
    return args[0]; // Return the array
  });

  // Array.prototype.copyWithin
  installArrayMethod("copyWithin", 2, [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      throw std::runtime_error("TypeError: Array.prototype.copyWithin called on non-array");
    }
    auto arr = args[0].getGC<Array>();
    int len = static_cast<int>(arr->elements.size());
    int target = args.size() > 1 ? static_cast<int>(args[1].toNumber()) : 0;
    int start = args.size() > 2 ? static_cast<int>(args[2].toNumber()) : 0;
    int end = args.size() > 3 && !args[3].isUndefined() ? static_cast<int>(args[3].toNumber()) : len;
    if (target < 0) target = std::max(0, len + target);
    if (start < 0) start = std::max(0, len + start);
    if (end < 0) end = std::max(0, len + end);
    target = std::min(target, len);
    start = std::min(start, len);
    end = std::min(end, len);
    int count = std::min(end - start, len - target);
    if (count <= 0) return args[0];
    // Use temporary copy to handle overlapping
    std::vector<Value> temp(arr->elements.begin() + start, arr->elements.begin() + start + count);
    for (int i = 0; i < count; ++i) {
      arr->elements[target + i] = temp[i];
    }
    return args[0];
  });

  // Array.prototype.at
  installArrayMethod("at", 1, [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      throw std::runtime_error("TypeError: Array.prototype.at called on non-array");
    }
    auto arr = args[0].getGC<Array>();
    double index = args.size() > 1 ? args[1].toNumber() : 0;
    int len = static_cast<int>(arr->elements.size());
    int idx = static_cast<int>(index);
    if (idx < 0) idx = len + idx;
    if (idx < 0 || idx >= len) return Value(Undefined{});
    return arr->elements[idx];
  });

  // Array.prototype.with
  installArrayMethod("with", 2, [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      throw std::runtime_error("TypeError: Array.prototype.with called on non-array");
    }
    auto arr = args[0].getGC<Array>();
    double index = args.size() > 1 ? args[1].toNumber() : 0;
    Value value = args.size() > 2 ? args[2] : Value(Undefined{});
    int len = static_cast<int>(arr->elements.size());
    int idx = static_cast<int>(index);
    if (idx < 0) idx = len + idx;
    if (idx < 0 || idx >= len) {
      throw std::runtime_error("RangeError: Invalid index");
    }
    auto result = GarbageCollector::makeGC<Array>();
    result->elements = arr->elements;
    result->elements[idx] = value;
    return Value(result);
  });

  // Array.prototype.splice
  installArrayMethod("splice", 2, [toObjectChecked, getArrayLikeLength, getArrayLikeElement](const std::vector<Value>& args) -> Value {
    Value thisVal = toObjectChecked(args.empty() ? Value(Undefined{}) : args[0], "splice");
    if (thisVal.isArray()) {
      auto arr = thisVal.getGC<Array>();
      int len = static_cast<int>(arr->elements.size());
      auto result = makeArrayWithPrototype();
      if (args.size() < 2) return Value(result);
      int start = static_cast<int>(args[1].toNumber());
      if (start < 0) start = std::max(0, len + start);
      if (start > len) start = len;
      int deleteCount = 0;
      if (args.size() >= 3) {
        deleteCount = static_cast<int>(args[2].toNumber());
        if (deleteCount < 0) deleteCount = 0;
        if (deleteCount > len - start) deleteCount = len - start;
      } else {
        deleteCount = len - start;
      }
      for (int i = 0; i < deleteCount; ++i) {
        result->elements.push_back(arr->elements[start + i]);
      }
      std::vector<Value> newItems;
      for (size_t i = 3; i < args.size(); ++i) {
        newItems.push_back(args[i]);
      }
      arr->elements.erase(arr->elements.begin() + start, arr->elements.begin() + start + deleteCount);
      arr->elements.insert(arr->elements.begin() + start, newItems.begin(), newItems.end());
      return Value(result);
    }
    // Generic: work with object properties
    int len = static_cast<int>(getArrayLikeLength(thisVal));
    auto result = makeArrayWithPrototype();
    if (args.size() < 2) return Value(result);
    int start = static_cast<int>(args[1].toNumber());
    if (start < 0) start = std::max(0, len + start);
    if (start > len) start = len;
    int deleteCount = (args.size() >= 3) ? std::max(0, std::min(static_cast<int>(args[2].toNumber()), len - start)) : len - start;
    for (int i = 0; i < deleteCount; ++i) {
      auto [exists, elem] = getArrayLikeElement(thisVal, start + i);
      result->elements.push_back(exists ? elem : Value(Undefined{}));
    }
    // For generic objects, modifying properties is complex; for now return deleted
    return Value(result);
  });

  // Array.prototype.slice
  installArrayMethod("slice", 2, [toObjectChecked, getArrayLikeLength, getArrayLikeElement](const std::vector<Value>& args) -> Value {
    Value thisVal = toObjectChecked(args.empty() ? Value(Undefined{}) : args[0], "slice");
    int len = static_cast<int>(getArrayLikeLength(thisVal));
    int start = 0, end = len;
    if (args.size() > 1 && !args[1].isUndefined()) {
      double s = args[1].toNumber();
      start = std::isnan(s) ? 0 : static_cast<int>(s);
      if (start < 0) start = std::max(0, len + start);
    }
    if (args.size() > 2 && !args[2].isUndefined()) {
      double e = args[2].toNumber();
      end = std::isnan(e) ? 0 : static_cast<int>(e);
      if (end < 0) end = std::max(0, len + end);
    }
    start = std::min(start, len);
    end = std::min(end, len);
    auto result = makeArrayWithPrototype();
    for (int i = start; i < end; ++i) {
      auto [exists, elem] = getArrayLikeElement(thisVal, static_cast<size_t>(i));
      if (exists) {
        result->elements.push_back(elem);
      } else {
        result->elements.push_back(Value(Undefined{}));
        result->properties["__hole_" + std::to_string(result->elements.size() - 1) + "__"] = Value(true);
      }
    }
    return Value(result);
  });

  // Array.prototype.pop - generic
  installArrayMethod("pop", 0, [toObjectChecked, getArrayLikeLength](const std::vector<Value>& args) -> Value {
    Value thisVal = toObjectChecked(args.empty() ? Value(Undefined{}) : args[0], "pop");
    if (thisVal.isArray()) {
      auto arr = thisVal.getGC<Array>();
      if (arr->elements.empty()) return Value(Undefined{});
      Value last = arr->elements.back();
      arr->elements.pop_back();
      return last;
    }
    // Generic object
    size_t len = getArrayLikeLength(thisVal);
    if (len == 0) {
      if (thisVal.isObject()) thisVal.getGC<Object>()->properties["length"] = Value(0.0);
      return Value(Undefined{});
    }
    Interpreter* interp = getGlobalInterpreter();
    std::string lastKey = std::to_string(len - 1);
    Value result(Undefined{});
    if (interp) {
      auto [found, val] = interp->getPropertyForExternal(thisVal, lastKey);
      if (found) result = val;
    }
    if (thisVal.isObject()) {
      thisVal.getGC<Object>()->properties.erase(lastKey);
      thisVal.getGC<Object>()->properties["length"] = Value(static_cast<double>(len - 1));
    }
    return result;
  });

  // Array.prototype.shift - generic
  installArrayMethod("shift", 0, [toObjectChecked, getArrayLikeLength, getArrayLikeElement](const std::vector<Value>& args) -> Value {
    Value thisVal = toObjectChecked(args.empty() ? Value(Undefined{}) : args[0], "shift");
    if (thisVal.isArray()) {
      auto arr = thisVal.getGC<Array>();
      if (arr->elements.empty()) return Value(Undefined{});
      Value first = arr->elements.front();
      arr->elements.erase(arr->elements.begin());
      return first;
    }
    size_t len = getArrayLikeLength(thisVal);
    if (len == 0) {
      if (thisVal.isObject()) thisVal.getGC<Object>()->properties["length"] = Value(0.0);
      return Value(Undefined{});
    }
    Value first(Undefined{});
    Interpreter* interp = getGlobalInterpreter();
    if (interp) {
      auto [found, val] = interp->getPropertyForExternal(thisVal, "0");
      if (found) first = val;
    }
    if (thisVal.isObject()) {
      auto obj = thisVal.getGC<Object>();
      for (size_t i = 1; i < len; ++i) {
        std::string from = std::to_string(i), to = std::to_string(i - 1);
        if (obj->properties.count(from)) {
          obj->properties[to] = obj->properties[from];
        } else {
          obj->properties.erase(to);
        }
      }
      obj->properties.erase(std::to_string(len - 1));
      obj->properties["length"] = Value(static_cast<double>(len - 1));
    }
    return first;
  });

  // Array.prototype.unshift - generic
  installArrayMethod("unshift", 1, [toObjectChecked, getArrayLikeLength](const std::vector<Value>& args) -> Value {
    Value thisVal = toObjectChecked(args.empty() ? Value(Undefined{}) : args[0], "unshift");
    if (thisVal.isArray()) {
      auto arr = thisVal.getGC<Array>();
      for (size_t i = args.size() - 1; i >= 1; --i) {
        arr->elements.insert(arr->elements.begin(), args[i]);
      }
      return Value(static_cast<double>(arr->elements.size()));
    }
    size_t len = getArrayLikeLength(thisVal);
    size_t argCount = args.size() - 1;
    if (thisVal.isObject()) {
      auto obj = thisVal.getGC<Object>();
      // Shift existing elements up
      for (int i = static_cast<int>(len) - 1; i >= 0; --i) {
        std::string from = std::to_string(i), to = std::to_string(i + argCount);
        if (obj->properties.count(from)) {
          obj->properties[to] = obj->properties[from];
        } else {
          obj->properties.erase(to);
        }
      }
      // Insert new elements
      for (size_t i = 0; i < argCount; ++i) {
        obj->properties[std::to_string(i)] = args[i + 1];
      }
      size_t newLen = len + argCount;
      obj->properties["length"] = Value(static_cast<double>(newLen));
      return Value(static_cast<double>(newLen));
    }
    return Value(static_cast<double>(len + argCount));
  });

  // Array.prototype.concat
  installArrayMethod("concat", 1, [toObjectChecked, getArrayLikeLength, getArrayLikeElement](const std::vector<Value>& args) -> Value {
    Value thisVal = toObjectChecked(args.empty() ? Value(Undefined{}) : args[0], "concat");
    auto result = makeArrayWithPrototype();
    // Helper: spread an array-like into result
    auto spreadInto = [&](const Value& val) {
      // Check Symbol.isConcatSpreadable or isArray
      bool spreadable = val.isArray();
      if (val.isObject()) {
        Interpreter* interp = getGlobalInterpreter();
        if (interp) {
          auto [found, iCS] = interp->getPropertyForExternal(val, "__Symbol.isConcatSpreadable__");
          if (found && !iCS.isUndefined()) {
            spreadable = iCS.toBool();
          }
        }
      }
      if (spreadable) {
        size_t len = getArrayLikeLength(val);
        for (size_t i = 0; i < len; ++i) {
          auto [exists, elem] = getArrayLikeElement(val, i);
          if (exists) {
            // Ensure result has enough elements
            while (result->elements.size() <= result->elements.size()) {
              result->elements.push_back(exists ? elem : Value(Undefined{}));
              break;
            }
          } else {
            result->elements.push_back(Value(Undefined{}));
            // Mark as hole
            result->properties["__hole_" + std::to_string(result->elements.size() - 1) + "__"] = Value(true);
          }
        }
      } else {
        result->elements.push_back(val);
      }
    };
    spreadInto(thisVal);
    for (size_t i = 1; i < args.size(); ++i) {
      spreadInto(args[i]);
    }
    return Value(result);
  });

  // Array.prototype.toReversed
  installArrayMethod("toReversed", 0, [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      throw std::runtime_error("TypeError: Array.prototype.toReversed called on non-array");
    }
    auto arr = args[0].getGC<Array>();
    auto result = GarbageCollector::makeGC<Array>();
    result->elements = arr->elements;
    std::reverse(result->elements.begin(), result->elements.end());
    return Value(result);
  });

  // Array.prototype.toSorted
  installArrayMethod("toSorted", 1, [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      throw std::runtime_error("TypeError: Array.prototype.toSorted called on non-array");
    }
    auto arr = args[0].getGC<Array>();
    auto result = GarbageCollector::makeGC<Array>();
    result->elements = arr->elements;
    bool hasCompareFn = args.size() > 1 && args[1].isFunction();
    Value compareFn = hasCompareFn ? args[1] : Value(Undefined{});
    Interpreter* interpreter = hasCompareFn ? getGlobalInterpreter() : nullptr;
    std::sort(result->elements.begin(), result->elements.end(),
      [&](const Value& a, const Value& b) -> bool {
        if (a.isUndefined() && b.isUndefined()) return false;
        if (a.isUndefined()) return false;
        if (b.isUndefined()) return true;
        if (hasCompareFn && interpreter) {
          Value cmpResult = interpreter->callForHarness(compareFn, {a, b}, Value(Undefined{}));
          if (interpreter->hasError()) {
            interpreter->clearError();
            return false;
          }
          return cmpResult.toNumber() < 0;
        }
        return a.toString() < b.toString();
      });
    return Value(result);
  });

  // Array.prototype.toSpliced
  installArrayMethod("toSpliced", 2, [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      throw std::runtime_error("TypeError: Array.prototype.toSpliced called on non-array");
    }
    auto arr = args[0].getGC<Array>();
    auto result = GarbageCollector::makeGC<Array>();
    result->elements = arr->elements;
    int len = static_cast<int>(result->elements.size());
    if (args.size() < 2) return Value(result);
    int start = static_cast<int>(args[1].toNumber());
    if (start < 0) start = std::max(0, len + start);
    if (start > len) start = len;
    int deleteCount = 0;
    if (args.size() >= 3) {
      deleteCount = static_cast<int>(args[2].toNumber());
      if (deleteCount < 0) deleteCount = 0;
      if (deleteCount > len - start) deleteCount = len - start;
    } else {
      deleteCount = len - start;
    }
    std::vector<Value> newItems;
    for (size_t i = 3; i < args.size(); ++i) {
      newItems.push_back(args[i]);
    }
    result->elements.erase(result->elements.begin() + start, result->elements.begin() + start + deleteCount);
    result->elements.insert(result->elements.begin() + start, newItems.begin(), newItems.end());
    return Value(result);
  });

  // Array.prototype[Symbol.iterator] - values iterator
  {
    const auto& iterKey = WellKnownSymbols::iteratorKey();
    auto arrayProtoIterator = GarbageCollector::makeGC<Function>();
    arrayProtoIterator->isNative = true;
    arrayProtoIterator->properties["__uses_this_arg__"] = Value(true);
    arrayProtoIterator->properties["__builtin_array_iterator__"] = Value(true);
    arrayProtoIterator->nativeFunc = [](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isArray()) {
        return Value(Undefined{});
      }
      Value arrayValue = args[0];
      auto iterObj = GarbageCollector::makeGC<Object>();
      auto indexPtr = std::make_shared<size_t>(0);
      auto nextFn = GarbageCollector::makeGC<Function>();
      nextFn->isNative = true;
      nextFn->nativeFunc = [arrayValue, indexPtr](const std::vector<Value>&) -> Value {
        auto arr = arrayValue.getGC<Array>();
        auto result = GarbageCollector::makeGC<Object>();
        if (*indexPtr >= arr->elements.size()) {
          result->properties["value"] = Value(Undefined{});
          result->properties["done"] = Value(true);
        } else {
          std::string indexKey = std::to_string(*indexPtr);
          auto [found, value] = getOwnPropertyLike(arrayValue, indexKey, arrayValue);
          result->properties["value"] = found ? value : Value(Undefined{});
          result->properties["done"] = Value(false);
          (*indexPtr)++;
        }
        return Value(result);
      };
      iterObj->properties["next"] = Value(nextFn);
      return Value(iterObj);
    };
    arrayPrototype->properties[iterKey] = Value(arrayProtoIterator);
  }

  // Array.isArray
  auto isArrayFn = GarbageCollector::makeGC<Function>();
  isArrayFn->isNative = true;
  isArrayFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(false);
    return Value(args[0].isArray());
  };
  isArrayFn->properties["name"] = Value(std::string("isArray"));
  isArrayFn->properties["length"] = Value(1.0);
  isArrayFn->properties["__non_writable_name"] = Value(true);
  isArrayFn->properties["__non_enum_name"] = Value(true);
  isArrayFn->properties["__non_writable_length"] = Value(true);
  isArrayFn->properties["__non_enum_length"] = Value(true);
  arrayConstructorObj->properties["isArray"] = Value(isArrayFn);
  arrayConstructorObj->properties["__non_enum_isArray"] = Value(true);

  // Array.from - creates array from array-like or iterable object
  auto fromFn = GarbageCollector::makeGC<Function>();
  fromFn->isNative = true;
  fromFn->nativeFunc = [toPrimitive](const std::vector<Value>& args) -> Value {
    auto result = GarbageCollector::makeGC<Array>();

    if (args.empty()) {
      return Value(result);
    }

    const Value& arrayLike = args[0];
    Value mapFn = args.size() > 1 ? args[1] : Value(Undefined{});
    Value thisArg = args.size() > 2 ? args[2] : Value(Undefined{});
    bool hasMapFn = !mapFn.isUndefined();

    if (hasMapFn && !mapFn.isFunction()) {
      throw std::runtime_error("TypeError: Array.from mapper must be a function");
    }

    auto applyMap = [&](const Value& value, size_t index) -> Value {
      if (!hasMapFn) {
        return value;
      }

      std::vector<Value> mapperArgs = {value, Value(static_cast<double>(index))};
      Interpreter* interpreter = getGlobalInterpreter();
      if (interpreter) {
        Value mapped = interpreter->callForHarness(mapFn, mapperArgs, thisArg);
        if (interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw std::runtime_error(err.toString());
        }
        return mapped;
      }

      auto mapper = mapFn.getGC<Function>();
      if (mapper->isNative) {
        auto itUsesThis = mapper->properties.find("__uses_this_arg__");
        if (itUsesThis != mapper->properties.end() && itUsesThis->second.isBool() && itUsesThis->second.toBool()) {
          std::vector<Value> nativeArgs;
          nativeArgs.reserve(mapperArgs.size() + 1);
          nativeArgs.push_back(thisArg);
          nativeArgs.insert(nativeArgs.end(), mapperArgs.begin(), mapperArgs.end());
          return mapper->nativeFunc(nativeArgs);
        }
        return mapper->nativeFunc(mapperArgs);
      }

      return value;
    };

    // If it's already an array, copy it
    if (arrayLike.isArray()) {
      auto srcArray = arrayLike.getGC<Array>();
      size_t index = 0;
      for (const auto& elem : srcArray->elements) {
        result->elements.push_back(applyMap(elem, index++));
      }
      return Value(result);
    }

    // If it's a string, convert each character to array element
    if (arrayLike.isString()) {
      std::string str = std::get<std::string>(arrayLike.data);
      size_t index = 0;
      for (char c : str) {
        result->elements.push_back(applyMap(Value(std::string(1, c)), index++));
      }
      return Value(result);
    }

    // If it's an iterator object, consume it
    if (arrayLike.isObject()) {
      Value iteratorValue = arrayLike;
      auto srcObj = arrayLike.getGC<Object>();
      const auto& iteratorKey = WellKnownSymbols::iteratorKey();

      auto iteratorMethodIt = srcObj->properties.find(iteratorKey);
      if (iteratorMethodIt != srcObj->properties.end() && iteratorMethodIt->second.isFunction()) {
        Interpreter* interpreter = getGlobalInterpreter();
        if (interpreter) {
          iteratorValue = interpreter->callForHarness(iteratorMethodIt->second, {}, arrayLike);
          if (interpreter->hasError()) {
            Value err = interpreter->getError();
            interpreter->clearError();
            throw std::runtime_error(err.toString());
          }
        } else {
          auto iterMethod = iteratorMethodIt->second.getGC<Function>();
          iteratorValue = iterMethod->isNative ? iterMethod->nativeFunc({}) : Value(Undefined{});
        }
      }

      if (iteratorValue.isObject()) {
        auto iterObj = iteratorValue.getGC<Object>();
        auto nextIt = iterObj->properties.find("next");
        if (nextIt != iterObj->properties.end() && nextIt->second.isFunction()) {
          size_t index = 0;
          while (true) {
            Value stepResult;
            Interpreter* interpreter = getGlobalInterpreter();
            if (interpreter) {
              stepResult = interpreter->callForHarness(nextIt->second, {}, iteratorValue);
              if (interpreter->hasError()) {
                Value err = interpreter->getError();
                interpreter->clearError();
                throw std::runtime_error(err.toString());
              }
            } else {
              auto nextFn = nextIt->second.getGC<Function>();
              stepResult = nextFn->isNative ? nextFn->nativeFunc({}) : Value(Undefined{});
            }

            if (!stepResult.isObject()) break;
            auto stepObj = stepResult.getGC<Object>();
            bool done = false;
            if (auto doneIt = stepObj->properties.find("done"); doneIt != stepObj->properties.end()) {
              done = doneIt->second.toBool();
            }
            if (done) break;

            Value element = Value(Undefined{});
            if (auto valueIt = stepObj->properties.find("value"); valueIt != stepObj->properties.end()) {
              element = valueIt->second;
            }
            result->elements.push_back(applyMap(element, index++));
          }
          return Value(result);
        }
      }

      auto [foundLength, lengthValue] = getPropertyLike(arrayLike, "length", arrayLike);
      if (foundLength) {
        Value numericLength = isObjectLikeValue(lengthValue) ? toPrimitive(lengthValue, false) : lengthValue;
        if (numericLength.isBigInt() || numericLength.isSymbol()) {
          throw std::runtime_error("TypeError: Invalid array-like length");
        }
        double rawLength = numericLength.toNumber();
        size_t length = 0;
        if (!std::isnan(rawLength) && rawLength > 0.0) {
          if (std::isinf(rawLength)) {
            length = 9007199254740991ull;
          } else {
            double integer = std::floor(rawLength);
            if (integer > 9007199254740991.0) {
              integer = 9007199254740991.0;
            }
            length = static_cast<size_t>(integer);
          }
        }

        for (size_t i = 0; i < length; ++i) {
          auto [foundValue, element] = getPropertyLike(arrayLike, std::to_string(i), arrayLike);
          result->elements.push_back(applyMap(foundValue ? element : Value(Undefined{}), i));
        }
        return Value(result);
      }
    }

    // Otherwise return empty array
    return Value(result);
  };
  fromFn->properties["name"] = Value(std::string("from"));
  fromFn->properties["length"] = Value(1.0);
  fromFn->properties["__non_writable_name"] = Value(true);
  fromFn->properties["__non_enum_name"] = Value(true);
  fromFn->properties["__non_writable_length"] = Value(true);
  fromFn->properties["__non_enum_length"] = Value(true);
  arrayConstructorObj->properties["from"] = Value(fromFn);
  arrayConstructorObj->properties["__non_enum_from"] = Value(true);

  // Array.of - creates array from arguments
  auto ofFn = GarbageCollector::makeGC<Function>();
  ofFn->isNative = true;
  ofFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto result = GarbageCollector::makeGC<Array>();
    result->elements = args;
    return Value(result);
  };
  ofFn->properties["name"] = Value(std::string("of"));
  ofFn->properties["length"] = Value(0.0);
  ofFn->properties["__non_writable_name"] = Value(true);
  ofFn->properties["__non_enum_name"] = Value(true);
  ofFn->properties["__non_writable_length"] = Value(true);
  ofFn->properties["__non_enum_length"] = Value(true);
  arrayConstructorObj->properties["of"] = Value(ofFn);
  arrayConstructorObj->properties["__non_enum_of"] = Value(true);

  env->define("Array", Value(arrayConstructorObj));

  // Promise constructor
  auto promiseFunc = GarbageCollector::makeGC<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  promiseFunc->isNative = true;
  promiseFunc->isConstructor = true;
  promiseFunc->properties["__require_new__"] = Value(true);
  promiseFunc->properties["name"] = Value(std::string("Promise"));
  promiseFunc->properties["length"] = Value(1.0);
  promiseFunc->properties["__non_writable_name"] = Value(true);
  promiseFunc->properties["__non_enum_name"] = Value(true);
  promiseFunc->properties["__non_writable_length"] = Value(true);
  promiseFunc->properties["__non_enum_length"] = Value(true);
  {
    auto speciesGetter = GarbageCollector::makeGC<Function>();
    speciesGetter->isNative = true;
    speciesGetter->isConstructor = false;
    speciesGetter->properties["name"] = Value(std::string("get [Symbol.species]"));
    speciesGetter->properties["length"] = Value(0.0);
    speciesGetter->properties["__non_writable_name"] = Value(true);
    speciesGetter->properties["__non_enum_name"] = Value(true);
    speciesGetter->properties["__non_writable_length"] = Value(true);
    speciesGetter->properties["__non_enum_length"] = Value(true);
    speciesGetter->properties["__uses_this_arg__"] = Value(true);
    speciesGetter->properties["__throw_on_new__"] = Value(true);
    speciesGetter->nativeFunc = [](const std::vector<Value>& args) -> Value {
      return args.empty() ? Value(Undefined{}) : args[0];
    };
    const auto& speciesKey = WellKnownSymbols::speciesKey();
    promiseFunc->properties["__get_" + speciesKey] = Value(speciesGetter);
    promiseFunc->properties["__non_enum_" + speciesKey] = Value(true);
  }

  auto promiseConstructor = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  auto promisePrototype = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  promiseConstructor->properties["prototype"] = Value(promisePrototype);
  promiseConstructor->properties["__non_writable_prototype"] = Value(true);
  promiseConstructor->properties["__non_enum_prototype"] = Value(true);
  promiseConstructor->properties["__non_configurable_prototype"] = Value(true);
  promisePrototype->properties["constructor"] = Value(promiseFunc);
  promisePrototype->properties["__non_enum_constructor"] = Value(true);
  promisePrototype->properties[WellKnownSymbols::toStringTagKey()] = Value(std::string("Promise"));
  promisePrototype->properties["__non_writable_" + WellKnownSymbols::toStringTagKey()] = Value(true);
  promisePrototype->properties["__non_enum_" + WellKnownSymbols::toStringTagKey()] = Value(true);
  if (auto objectCtor = env->get("Object"); objectCtor && objectCtor->isFunction()) {
    auto objectFunc = objectCtor->getGC<Function>();
    auto protoIt = objectFunc->properties.find("prototype");
    if (protoIt != objectFunc->properties.end() && protoIt->second.isObject()) {
      promisePrototype->properties["__proto__"] = protoIt->second;
    }
  }

  auto invokePromiseCallback = [](const GCPtr<Function>& callback, const Value& arg) -> Value {
    if (!callback) {
      return arg;
    }
    if (callback->isNative) {
      return callback->nativeFunc({arg});
    }
    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) {
      throw std::runtime_error("TypeError: Interpreter unavailable");
    }
    interpreter->clearError();
    Value out = interpreter->callForHarness(Value(callback), {arg}, Value(Undefined{}));
    if (interpreter->hasError()) {
      Value err = interpreter->getError();
      interpreter->clearError();
      throw std::runtime_error(err.toString());
    }
    return out;
  };

  auto getPropertyOnObjectLikeChain = [callChecked](const Value& target,
                                                    const std::string& key,
                                                    const Value& receiverForGetter) -> std::pair<bool, Value> {
    Value current = target;
    int depth = 0;
    while (isObjectLikeValue(current) && depth <= 16) {
      depth++;
      OrderedMap<std::string, Value>* bag = nullptr;
      if (current.isObject()) {
        bag = &current.getGC<Object>()->properties;
      } else if (current.isFunction()) {
        bag = &current.getGC<Function>()->properties;
      } else if (current.isClass()) {
        bag = &current.getGC<Class>()->properties;
      } else if (current.isPromise()) {
        bag = &current.getGC<Promise>()->properties;
      } else if (current.isArray()) {
        bag = &current.getGC<Array>()->properties;
      } else if (current.isProxy()) {
        auto proxy = current.getGC<Proxy>();
        if (!proxy->target) {
          break;
        }
        current = *proxy->target;
        continue;
      } else {
        break;
      }

      auto valueIt = bag->find(key);
      if (valueIt != bag->end()) {
        return {true, valueIt->second};
      }

      auto getterIt = bag->find("__get_" + key);
      if (getterIt != bag->end()) {
        if (getterIt->second.isFunction()) {
          return {true, callChecked(getterIt->second, {}, receiverForGetter)};
        }
        return {true, Value(Undefined{})};
      }

      auto protoIt = bag->find("__proto__");
      if (protoIt == bag->end()) {
        break;
      }
      current = protoIt->second;
    }
    return {false, Value(Undefined{})};
  };

  auto promiseProtoThen = GarbageCollector::makeGC<Function>();
  promiseProtoThen->isNative = true;
  promiseProtoThen->properties["__uses_this_arg__"] = Value(true);
  promiseProtoThen->properties["name"] = Value(std::string("then"));
  promiseProtoThen->properties["length"] = Value(2.0);
  promiseProtoThen->nativeFunc = [callChecked, getProperty, getPropertyOnObjectLikeChain, invokePromiseCallback, promiseFunc](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isPromise()) {
      throw std::runtime_error("TypeError: Promise.prototype.then called on non-Promise");
    }

    auto promise = args[0].getGC<Promise>();
    Value receiver = args[0];

    Value constructor = Value(promiseFunc);
    {
      auto [hasCtor, ctorValue] = getProperty(receiver, "constructor");
      if (hasCtor) {
        if (!ctorValue.isUndefined()) {
          if (!isObjectLikeValue(ctorValue)) {
            throw std::runtime_error("TypeError: Promise constructor is not an object");
          }
          const auto& speciesKey = WellKnownSymbols::speciesKey();
          auto [hasSpecies, speciesValue] = getPropertyOnObjectLikeChain(ctorValue, speciesKey, ctorValue);
          if (!hasSpecies || speciesValue.isUndefined() || speciesValue.isNull()) {
            constructor = Value(promiseFunc);
          } else if ((speciesValue.isFunction() && speciesValue.getGC<Function>()->isConstructor) ||
                     speciesValue.isClass()) {
            constructor = speciesValue;
          } else {
            throw std::runtime_error("TypeError: Promise @@species is not a constructor");
          }
        }
      }
    }

    auto capResolve = std::make_shared<Value>(Value(Undefined{}));
    auto capReject = std::make_shared<Value>(Value(Undefined{}));

    auto executor = GarbageCollector::makeGC<Function>();
    executor->isNative = true;
    executor->isConstructor = false;
    executor->properties["name"] = Value(std::string(""));
    executor->properties["length"] = Value(2.0);
    executor->properties["__non_writable_name"] = Value(true);
    executor->properties["__non_enum_name"] = Value(true);
    executor->properties["__non_writable_length"] = Value(true);
    executor->properties["__non_enum_length"] = Value(true);
    executor->nativeFunc = [capResolve, capReject](const std::vector<Value>& executorArgs) -> Value {
      if (!capResolve->isUndefined()) {
        throw std::runtime_error("TypeError: Promise resolve function already set");
      }
      if (!capReject->isUndefined()) {
        throw std::runtime_error("TypeError: Promise reject function already set");
      }
      *capResolve = executorArgs.size() > 0 ? executorArgs[0] : Value(Undefined{});
      *capReject = executorArgs.size() > 1 ? executorArgs[1] : Value(Undefined{});
      return Value(Undefined{});
    };

    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) {
      throw std::runtime_error("TypeError: Interpreter unavailable");
    }
    interpreter->clearError();
    Value resultPromise = interpreter->constructFromNative(constructor, {Value(executor)});
    if (interpreter->hasError()) {
      Value err = interpreter->getError();
      interpreter->clearError();
      throw JsValueException(err);
    }
    if (!capResolve->isFunction() || !capReject->isFunction()) {
      throw std::runtime_error("TypeError: Promise resolve or reject function is not callable");
    }

    Value onFulfilledValue = args.size() > 1 ? args[1] : Value(Undefined{});
    Value onRejectedValue = args.size() > 2 ? args[2] : Value(Undefined{});

    std::function<Value(Value)> onFulfilled = [callChecked, capResolve, capReject, onFulfilledValue](Value v) -> Value {
      try {
        if (onFulfilledValue.isFunction()) {
          Value callbackResult = callChecked(onFulfilledValue, {v}, Value(Undefined{}));
          if (callbackResult.isPromise()) {
            auto returnedPromise = callbackResult.getGC<Promise>();
            returnedPromise->then(
              [callChecked, capResolve](Value settled) -> Value {
                callChecked(*capResolve, {settled}, Value(Undefined{}));
                return settled;
              },
              [callChecked, capReject](Value reason) -> Value {
                callChecked(*capReject, {reason}, Value(Undefined{}));
                return reason;
              });
          } else {
            callChecked(*capResolve, {callbackResult}, Value(Undefined{}));
          }
        } else {
          callChecked(*capResolve, {v}, Value(Undefined{}));
        }
      } catch (const JsValueException& e) {
        callChecked(*capReject, {e.value()}, Value(Undefined{}));
      } catch (const std::exception& e) {
        callChecked(*capReject, {Value(std::string(e.what()))}, Value(Undefined{}));
      }
      return Value(Undefined{});
    };

    std::function<Value(Value)> onRejected = [callChecked, capResolve, capReject, onRejectedValue](Value v) -> Value {
      try {
        if (onRejectedValue.isFunction()) {
          Value callbackResult = callChecked(onRejectedValue, {v}, Value(Undefined{}));
          if (callbackResult.isPromise()) {
            auto returnedPromise = callbackResult.getGC<Promise>();
            returnedPromise->then(
              [callChecked, capResolve](Value settled) -> Value {
                callChecked(*capResolve, {settled}, Value(Undefined{}));
                return settled;
              },
              [callChecked, capReject](Value reason) -> Value {
                callChecked(*capReject, {reason}, Value(Undefined{}));
                return reason;
              });
          } else {
            callChecked(*capResolve, {callbackResult}, Value(Undefined{}));
          }
        } else {
          callChecked(*capReject, {v}, Value(Undefined{}));
        }
      } catch (const JsValueException& e) {
        callChecked(*capReject, {e.value()}, Value(Undefined{}));
      } catch (const std::exception& e) {
        callChecked(*capReject, {Value(std::string(e.what()))}, Value(Undefined{}));
      }
      return Value(Undefined{});
    };

    promise->then(onFulfilled, onRejected);
    return resultPromise;
  };
  promisePrototype->properties["then"] = Value(promiseProtoThen);
  promisePrototype->properties["__non_enum_then"] = Value(true);

  auto promiseProtoCatch = GarbageCollector::makeGC<Function>();
  promiseProtoCatch->isNative = true;
  promiseProtoCatch->properties["__uses_this_arg__"] = Value(true);
  promiseProtoCatch->properties["name"] = Value(std::string("catch"));
  promiseProtoCatch->properties["length"] = Value(1.0);
  promiseProtoCatch->nativeFunc = [callChecked, getPropertyOnObjectLikeChain](const std::vector<Value>& args) -> Value {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
      throw std::runtime_error("TypeError: Promise.prototype.catch called on null or undefined");
    }

    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) {
      throw std::runtime_error("TypeError: Interpreter unavailable");
    }
    interpreter->clearError();
    auto [hasThen, thenValue] = isObjectLikeValue(args[0])
      ? getPropertyOnObjectLikeChain(args[0], "then", args[0])
      : interpreter->getPropertyForExternal(args[0], "then");
    if (interpreter->hasError()) {
      Value err = interpreter->getError();
      interpreter->clearError();
      throw JsValueException(err);
    }
    if (!hasThen || !thenValue.isFunction()) {
      throw std::runtime_error("TypeError: Promise.prototype.catch requires callable then");
    }
    Value onRejected = args.size() > 1 ? args[1] : Value(Undefined{});
    return callChecked(thenValue, {Value(Undefined{}), onRejected}, args[0]);
  };
  promisePrototype->properties["catch"] = Value(promiseProtoCatch);
  promisePrototype->properties["__non_enum_catch"] = Value(true);

  auto promiseProtoFinally = GarbageCollector::makeGC<Function>();
  promiseProtoFinally->isNative = true;
  promiseProtoFinally->properties["__uses_this_arg__"] = Value(true);
  promiseProtoFinally->properties["name"] = Value(std::string("finally"));
  promiseProtoFinally->properties["length"] = Value(1.0);
  promiseProtoFinally->nativeFunc = [callChecked, getPropertyOnObjectLikeChain, promiseFunc](const std::vector<Value>& args) -> Value {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
      throw std::runtime_error("TypeError: Promise.prototype.finally called on null or undefined");
    }

    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) {
      throw std::runtime_error("TypeError: Interpreter unavailable");
    }

    Value receiver = args[0];
    Value onFinally = args.size() > 1 ? args[1] : Value(Undefined{});

    interpreter->clearError();
    auto [hasThen, thenValue] = isObjectLikeValue(receiver)
      ? getPropertyOnObjectLikeChain(receiver, "then", receiver)
      : interpreter->getPropertyForExternal(receiver, "then");
    if (interpreter->hasError()) {
      Value err = interpreter->getError();
      interpreter->clearError();
      throw JsValueException(err);
    }
    if (!hasThen || !thenValue.isFunction()) {
      throw std::runtime_error("TypeError: Promise.prototype.finally requires callable then");
    }

    if (!onFinally.isFunction()) {
      return callChecked(thenValue, {onFinally, onFinally}, receiver);
    }

    Value constructor = Value(promiseFunc);
    interpreter->clearError();
    auto [hasCtor, ctorValue] = isObjectLikeValue(receiver)
      ? getPropertyOnObjectLikeChain(receiver, "constructor", receiver)
      : interpreter->getPropertyForExternal(receiver, "constructor");
    if (interpreter->hasError()) {
      Value err = interpreter->getError();
      interpreter->clearError();
      throw JsValueException(err);
    }
    if (hasCtor && !ctorValue.isUndefined()) {
      if (!isObjectLikeValue(ctorValue)) {
        throw std::runtime_error("TypeError: Promise constructor is not an object");
      }
      const auto& speciesKey = WellKnownSymbols::speciesKey();
      auto [hasSpecies, speciesValue] = getPropertyOnObjectLikeChain(ctorValue, speciesKey, ctorValue);
      if (!hasSpecies || speciesValue.isUndefined() || speciesValue.isNull()) {
        constructor = Value(promiseFunc);
      } else if ((speciesValue.isFunction() && speciesValue.getGC<Function>()->isConstructor) ||
                 speciesValue.isClass()) {
        constructor = speciesValue;
      } else {
        throw std::runtime_error("TypeError: Promise @@species is not a constructor");
      }
    }

    auto resolveThunk = GarbageCollector::makeGC<Function>();
    resolveThunk->isNative = true;
    resolveThunk->isConstructor = false;
    resolveThunk->properties["name"] = Value(std::string(""));
    resolveThunk->properties["length"] = Value(1.0);
    resolveThunk->properties["__non_writable_name"] = Value(true);
    resolveThunk->properties["__non_enum_name"] = Value(true);
    resolveThunk->properties["__non_writable_length"] = Value(true);
    resolveThunk->properties["__non_enum_length"] = Value(true);

    auto rejectThunk = GarbageCollector::makeGC<Function>();
    rejectThunk->isNative = true;
    rejectThunk->isConstructor = false;
    rejectThunk->properties["name"] = Value(std::string(""));
    rejectThunk->properties["length"] = Value(1.0);
    rejectThunk->properties["__non_writable_name"] = Value(true);
    rejectThunk->properties["__non_enum_name"] = Value(true);
    rejectThunk->properties["__non_writable_length"] = Value(true);
    rejectThunk->properties["__non_enum_length"] = Value(true);

    resolveThunk->nativeFunc = [callChecked, getPropertyOnObjectLikeChain, constructor, onFinally](const std::vector<Value>& callbackArgs) -> Value {
      Value originalValue = callbackArgs.empty() ? Value(Undefined{}) : callbackArgs[0];
      Value finallyResult = callChecked(onFinally, {}, Value(Undefined{}));

      auto [hasResolve, resolveValue] = getPropertyOnObjectLikeChain(constructor, "resolve", constructor);
      if (!hasResolve || !resolveValue.isFunction()) {
        throw std::runtime_error("TypeError: Promise resolve is not callable");
      }
      Value promise = callChecked(resolveValue, {finallyResult}, constructor);

      auto valueThunk = GarbageCollector::makeGC<Function>();
      valueThunk->isNative = true;
      valueThunk->isConstructor = false;
      valueThunk->properties["name"] = Value(std::string(""));
      valueThunk->properties["length"] = Value(1.0);
      valueThunk->properties["__non_writable_name"] = Value(true);
      valueThunk->properties["__non_enum_name"] = Value(true);
      valueThunk->properties["__non_writable_length"] = Value(true);
      valueThunk->properties["__non_enum_length"] = Value(true);
      valueThunk->nativeFunc = [originalValue](const std::vector<Value>&) -> Value {
        return originalValue;
      };

      auto [hasThenInner, thenInner] = getPropertyOnObjectLikeChain(promise, "then", promise);
      if (!hasThenInner || !thenInner.isFunction()) {
        throw std::runtime_error("TypeError: Promise.prototype.finally inner then is not callable");
      }
      return callChecked(thenInner, {Value(valueThunk)}, promise);
    };

    rejectThunk->nativeFunc = [callChecked, getPropertyOnObjectLikeChain, constructor, onFinally](const std::vector<Value>& callbackArgs) -> Value {
      Value originalReason = callbackArgs.empty() ? Value(Undefined{}) : callbackArgs[0];
      Value finallyResult = callChecked(onFinally, {}, Value(Undefined{}));

      auto [hasResolve, resolveValue] = getPropertyOnObjectLikeChain(constructor, "resolve", constructor);
      if (!hasResolve || !resolveValue.isFunction()) {
        throw std::runtime_error("TypeError: Promise resolve is not callable");
      }
      Value promise = callChecked(resolveValue, {finallyResult}, constructor);

      auto thrower = GarbageCollector::makeGC<Function>();
      thrower->isNative = true;
      thrower->isConstructor = false;
      thrower->properties["name"] = Value(std::string(""));
      thrower->properties["length"] = Value(1.0);
      thrower->properties["__non_writable_name"] = Value(true);
      thrower->properties["__non_enum_name"] = Value(true);
      thrower->properties["__non_writable_length"] = Value(true);
      thrower->properties["__non_enum_length"] = Value(true);
      thrower->nativeFunc = [originalReason](const std::vector<Value>&) -> Value {
        throw JsValueException(originalReason);
      };

      auto [hasThenInner, thenInner] = getPropertyOnObjectLikeChain(promise, "then", promise);
      if (!hasThenInner || !thenInner.isFunction()) {
        throw std::runtime_error("TypeError: Promise.prototype.finally inner then is not callable");
      }
      return callChecked(thenInner, {Value(thrower)}, promise);
    };

    return callChecked(thenValue, {Value(resolveThunk), Value(rejectThunk)}, receiver);
  };
  promisePrototype->properties["finally"] = Value(promiseProtoFinally);
  promisePrototype->properties["__non_enum_finally"] = Value(true);

  auto getThenProperty = [callChecked, getProperty, env](const Value& candidate) -> std::pair<bool, Value> {
    if (candidate.isArray()) {
      auto arr = candidate.getGC<Array>();
      auto getterIt = arr->properties.find("__get_then");
      if (getterIt != arr->properties.end()) {
        if (getterIt->second.isFunction()) {
          return {true, callChecked(getterIt->second, {}, candidate)};
        }
        return {true, Value(Undefined{})};
      }
      auto ownIt = arr->properties.find("then");
      if (ownIt != arr->properties.end()) {
        return {true, ownIt->second};
      }
      if (auto arrayCtor = env->get("Array"); arrayCtor && arrayCtor->isObject()) {
        auto arrayObjPtr = std::get<GCPtr<Object>>(arrayCtor->data);
        auto protoIt = arrayObjPtr->properties.find("prototype");
        if (protoIt != arrayObjPtr->properties.end() && protoIt->second.isObject()) {
          auto protoObj = protoIt->second.getGC<Object>();
          auto protoGetterIt = protoObj->properties.find("__get_then");
          if (protoGetterIt != protoObj->properties.end()) {
            if (protoGetterIt->second.isFunction()) {
              return {true, callChecked(protoGetterIt->second, {}, candidate)};
            }
            return {true, Value(Undefined{})};
          }
          auto protoThenIt = protoObj->properties.find("then");
          if (protoThenIt != protoObj->properties.end()) {
            return {true, protoThenIt->second};
          }
        }
      }
      return {false, Value(Undefined{})};
    }

    return getProperty(candidate, "then");
  };

  auto isConstructorValue = [](const Value& constructor) -> bool {
    if (constructor.isFunction()) {
      return constructor.getGC<Function>()->isConstructor;
    }
    if (constructor.isClass()) {
      return true;
    }
    return false;
  };

  auto newPromiseCapability = [isConstructorValue](const Value& constructor) -> std::tuple<Value, Value, Value> {
    if (!isObjectLikeValue(constructor)) {
      throw std::runtime_error("TypeError: Promise capability requires object constructor");
    }
    if (!isConstructorValue(constructor)) {
      throw std::runtime_error("TypeError: Promise capability requires constructor");
    }

    auto capResolve = std::make_shared<Value>(Value(Undefined{}));
    auto capReject = std::make_shared<Value>(Value(Undefined{}));

    auto executor = GarbageCollector::makeGC<Function>();
    executor->isNative = true;
    executor->isConstructor = false;
    executor->properties["length"] = Value(2.0);
    executor->properties["name"] = Value(std::string(""));
    executor->properties["__non_writable_name"] = Value(true);
    executor->properties["__non_enum_name"] = Value(true);
    executor->properties["__non_writable_length"] = Value(true);
    executor->properties["__non_enum_length"] = Value(true);
    executor->nativeFunc = [capResolve, capReject](const std::vector<Value>& executorArgs) -> Value {
      if (!capResolve->isUndefined()) {
        throw std::runtime_error("TypeError: Promise resolve function already set");
      }
      if (!capReject->isUndefined()) {
        throw std::runtime_error("TypeError: Promise reject function already set");
      }
      *capResolve = executorArgs.size() > 0 ? executorArgs[0] : Value(Undefined{});
      *capReject = executorArgs.size() > 1 ? executorArgs[1] : Value(Undefined{});
      return Value(Undefined{});
    };

    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) {
      throw std::runtime_error("TypeError: Interpreter unavailable");
    }
    interpreter->clearError();
    Value promise = interpreter->constructFromNative(constructor, {Value(executor)});
    if (interpreter->hasError()) {
      Value err = interpreter->getError();
      interpreter->clearError();
      throw JsValueException(err);
    }

    if (!capResolve->isFunction() || !capReject->isFunction()) {
      throw std::runtime_error("TypeError: Promise resolve or reject function is not callable");
    }
    return {promise, *capResolve, *capReject};
  };

  // Promise.resolve
  auto promiseResolve = GarbageCollector::makeGC<Function>();
  promiseResolve->isNative = true;
  promiseResolve->properties["__uses_this_arg__"] = Value(true);
  promiseResolve->properties["name"] = Value(std::string("resolve"));
  promiseResolve->properties["length"] = Value(1.0);
  promiseResolve->nativeFunc = [callChecked, getProperty, newPromiseCapability, promiseFunc](const std::vector<Value>& args) -> Value {
    Value constructor = args.empty() ? Value(Undefined{}) : args[0];
    Value resolution = args.size() > 1 ? args[1] : Value(Undefined{});

    if (resolution.isPromise()) {
      try {
        auto [hasConstructor, resolutionConstructor] = getProperty(resolution, "constructor");
        bool sameConstructor =
          hasConstructor &&
          ((constructor.isFunction() && resolutionConstructor.isFunction() &&
            constructor.getGC<Function>().get() == resolutionConstructor.getGC<Function>().get()) ||
           (constructor.isClass() && resolutionConstructor.isClass() &&
            constructor.getGC<Class>().get() == resolutionConstructor.getGC<Class>().get()));
        if (sameConstructor) {
          return resolution;
        }
      } catch (...) {
      }
    }

    auto [promise, resolve, reject] = newPromiseCapability(constructor);
    (void)reject;
    callChecked(resolve, {resolution}, Value(Undefined{}));
    return promise;
  };
  promiseConstructor->properties["resolve"] = Value(promiseResolve);

  // Promise.reject
  auto promiseReject = GarbageCollector::makeGC<Function>();
  promiseReject->isNative = true;
  promiseReject->properties["__uses_this_arg__"] = Value(true);
  promiseReject->properties["name"] = Value(std::string("reject"));
  promiseReject->properties["length"] = Value(1.0);
  promiseReject->nativeFunc = [callChecked, newPromiseCapability](const std::vector<Value>& args) -> Value {
    Value constructor = args.empty() ? Value(Undefined{}) : args[0];
    Value reason = args.size() > 1 ? args[1] : Value(Undefined{});
    auto [promise, resolve, reject] = newPromiseCapability(constructor);
    callChecked(reject, {reason}, Value(Undefined{}));
    return promise;
  };
  promiseConstructor->properties["reject"] = Value(promiseReject);

  // Promise.try
  auto promiseTry = GarbageCollector::makeGC<Function>();
  promiseTry->isNative = true;
  promiseTry->isConstructor = false;
  promiseTry->properties["__uses_this_arg__"] = Value(true);
  promiseTry->properties["__throw_on_new__"] = Value(true);
  promiseTry->properties["name"] = Value(std::string("try"));
  promiseTry->properties["length"] = Value(1.0);
  promiseTry->properties["__non_writable_name"] = Value(true);
  promiseTry->properties["__non_enum_name"] = Value(true);
  promiseTry->properties["__non_writable_length"] = Value(true);
  promiseTry->properties["__non_enum_length"] = Value(true);
  promiseTry->nativeFunc = [](const std::vector<Value>& args) -> Value {
    Value constructor = args.empty() ? Value(Undefined{}) : args[0];
    if (!isObjectLikeValue(constructor)) {
      throw std::runtime_error("TypeError: Promise.try called on non-object");
    }
    bool isConstructor = false;
    if (constructor.isFunction()) {
      isConstructor = constructor.getGC<Function>()->isConstructor;
    } else if (constructor.isClass()) {
      isConstructor = true;
    }
    if (!isConstructor) {
      throw std::runtime_error("TypeError: Promise.try called on non-constructor");
    }

    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) {
      throw std::runtime_error("TypeError: Interpreter unavailable");
    }

    auto capResolve = std::make_shared<Value>(Value(Undefined{}));
    auto capReject = std::make_shared<Value>(Value(Undefined{}));

    auto executor = GarbageCollector::makeGC<Function>();
    executor->isNative = true;
    executor->isConstructor = false;
    executor->nativeFunc = [capResolve, capReject](const std::vector<Value>& executorArgs) -> Value {
      if (!capResolve->isUndefined() || !capReject->isUndefined()) {
        throw std::runtime_error("TypeError: Promise capability already initialized");
      }
      *capResolve = executorArgs.size() > 0 ? executorArgs[0] : Value(Undefined{});
      *capReject = executorArgs.size() > 1 ? executorArgs[1] : Value(Undefined{});
      return Value(Undefined{});
    };

    interpreter->clearError();
    Value resultPromise = interpreter->constructFromNative(constructor, {Value(executor)});
    if (interpreter->hasError()) {
      Value err = interpreter->getError();
      interpreter->clearError();
      throw JsValueException(err);
    }
    if (!capResolve->isFunction() || !capReject->isFunction()) {
      throw std::runtime_error("TypeError: Promise capability functions are not callable");
    }

    auto callWithThis = [interpreter](const Value& callee,
                                      const std::vector<Value>& callArgs,
                                      const Value& thisArg,
                                      Value& out,
                                      Value& thrown) -> bool {
      if (callee.isFunction()) {
        auto fn = callee.getGC<Function>();
        if (fn->isNative) {
          try {
            auto itUsesThis = fn->properties.find("__uses_this_arg__");
            if (itUsesThis != fn->properties.end() &&
                itUsesThis->second.isBool() &&
                itUsesThis->second.toBool()) {
              std::vector<Value> nativeArgs;
              nativeArgs.reserve(callArgs.size() + 1);
              nativeArgs.push_back(thisArg);
              nativeArgs.insert(nativeArgs.end(), callArgs.begin(), callArgs.end());
              out = fn->nativeFunc(nativeArgs);
            } else {
              out = fn->nativeFunc(callArgs);
            }
            return true;
          } catch (const JsValueException& e) {
            thrown = e.value();
            return false;
          } catch (const std::exception& e) {
            thrown = Value(std::string(e.what()));
            return false;
          }
        }
      }

      interpreter->clearError();
      out = interpreter->callForHarness(callee, callArgs, thisArg);
      if (interpreter->hasError()) {
        thrown = interpreter->getError();
        interpreter->clearError();
        return false;
      }
      return true;
    };

    Value callback = args.size() > 1 ? args[1] : Value(Undefined{});
    std::vector<Value> callbackArgs;
    if (args.size() > 2) {
      callbackArgs.insert(callbackArgs.end(), args.begin() + 2, args.end());
    }

    Value callbackResult, thrown;
    if (callWithThis(callback, callbackArgs, Value(Undefined{}), callbackResult, thrown)) {
      Value ignored, resolveThrown;
      callWithThis(*capResolve, {callbackResult}, Value(Undefined{}), ignored, resolveThrown);
    } else {
      Value ignored, rejectThrown;
      callWithThis(*capReject, {thrown}, Value(Undefined{}), ignored, rejectThrown);
    }

    return resultPromise;
  };
  promiseConstructor->properties["try"] = Value(promiseTry);
  promiseConstructor->properties["__non_enum_try"] = Value(true);

  // Promise.withResolvers
  auto promiseWithResolvers = GarbageCollector::makeGC<Function>();
  promiseWithResolvers->isNative = true;
  promiseWithResolvers->properties["__uses_this_arg__"] = Value(true);
  promiseWithResolvers->nativeFunc = [newPromiseCapability](const std::vector<Value>& args) -> Value {
    Value constructor = args.empty() ? Value(Undefined{}) : args[0];
    auto [promise, resolve, reject] = newPromiseCapability(constructor);
    auto result = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    result->properties["promise"] = promise;
    result->properties["resolve"] = resolve;
    result->properties["reject"] = reject;
    return Value(result);
  };
  promiseConstructor->properties["withResolvers"] = Value(promiseWithResolvers);

  // Promise.all
  auto promiseAll = GarbageCollector::makeGC<Function>();
  promiseAll->isNative = true;
  promiseAll->properties["__uses_this_arg__"] = Value(true);
  promiseAll->properties["name"] = Value(std::string("all"));
  promiseAll->properties["length"] = Value(1.0);
  promiseAll->nativeFunc = [callChecked, getProperty, env, promiseFunc](const std::vector<Value>& args) -> Value {
    Value constructor = args.empty() ? Value(Undefined{}) : args[0];
    Value iterable = args.size() > 1 ? args[1] : Value(Undefined{});

    auto typeErrorValue = [](const std::string& msg) -> Value {
      return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, msg));
    };

    auto callWithThis = [](const Value& callee,
                           const std::vector<Value>& callArgs,
                           const Value& thisArg,
                           Value& out,
                           Value& thrown) -> bool {
      if (!callee.isFunction()) {
        thrown = Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "Value is not callable"));
        return false;
      }

      auto fn = callee.getGC<Function>();
      if (fn->isNative) {
        try {
          auto itUsesThis = fn->properties.find("__uses_this_arg__");
          if (itUsesThis != fn->properties.end() &&
              itUsesThis->second.isBool() &&
              itUsesThis->second.toBool()) {
            std::vector<Value> nativeArgs;
            nativeArgs.reserve(callArgs.size() + 1);
            nativeArgs.push_back(thisArg);
            nativeArgs.insert(nativeArgs.end(), callArgs.begin(), callArgs.end());
            out = fn->nativeFunc(nativeArgs);
          } else {
            out = fn->nativeFunc(callArgs);
          }
          return true;
        } catch (const JsValueException& e) {
          thrown = e.value();
          return false;
        } catch (const std::exception& e) {
          thrown = Value(std::string(e.what()));
          return false;
        }
      }

      Interpreter* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        thrown = Value(std::string("TypeError: Interpreter unavailable"));
        return false;
      }
      interpreter->clearError();
      out = interpreter->callForHarness(callee, callArgs, thisArg);
      if (interpreter->hasError()) {
        thrown = interpreter->getError();
        interpreter->clearError();
        return false;
      }
      return true;
    };

    auto getPropertyWithThrow = [&callWithThis, &env](const Value& receiver,
                                                      const std::string& key,
                                                      Value& out,
                                                      bool& found,
                                                      Value& thrown) -> bool {
      auto resolveFromObject = [&](const GCPtr<Object>& obj,
                                   const Value& originalReceiver) -> bool {
        auto current = obj;
        int depth = 0;
        while (current && depth <= 32) {
          depth++;

          auto getterIt = current->properties.find("__get_" + key);
          if (getterIt != current->properties.end()) {
            found = true;
            if (!getterIt->second.isFunction()) {
              out = Value(Undefined{});
              return true;
            }
            Value getterOut;
            if (!callWithThis(getterIt->second, {}, originalReceiver, getterOut, thrown)) {
              return false;
            }
            out = getterOut;
            return true;
          }

          auto it = current->properties.find(key);
          if (it != current->properties.end()) {
            found = true;
            out = it->second;
            return true;
          }

          auto protoIt = current->properties.find("__proto__");
          if (protoIt == current->properties.end() || !protoIt->second.isObject()) {
            break;
          }
          current = protoIt->second.getGC<Object>();
        }
        return true;
      };

      found = false;
      out = Value(Undefined{});

      if (receiver.isObject()) {
        return resolveFromObject(receiver.getGC<Object>(), receiver);
      }
      if (receiver.isFunction()) {
        auto fn = receiver.getGC<Function>();
        auto getterIt = fn->properties.find("__get_" + key);
        if (getterIt != fn->properties.end()) {
          found = true;
          if (!getterIt->second.isFunction()) {
            out = Value(Undefined{});
            return true;
          }
          return callWithThis(getterIt->second, {}, receiver, out, thrown);
        }
        auto it = fn->properties.find(key);
        if (it != fn->properties.end()) {
          found = true;
          out = it->second;
        }
        return true;
      }
      if (receiver.isArray()) {
        auto arr = receiver.getGC<Array>();
        auto getterIt = arr->properties.find("__get_" + key);
        if (getterIt != arr->properties.end()) {
          found = true;
          if (!getterIt->second.isFunction()) {
            out = Value(Undefined{});
            return true;
          }
          return callWithThis(getterIt->second, {}, receiver, out, thrown);
        }
        auto it = arr->properties.find(key);
        if (it != arr->properties.end()) {
          found = true;
          out = it->second;
          return true;
        }
        if (key == "length") {
          found = true;
          out = Value(static_cast<double>(arr->elements.size()));
          return true;
        }
        if (auto arrayCtor = env->get("Array"); arrayCtor && arrayCtor->isObject()) {
          auto arrayObjPtr = std::get<GCPtr<Object>>(arrayCtor->data);
          auto protoIt = arrayObjPtr->properties.find("prototype");
          if (protoIt != arrayObjPtr->properties.end() && protoIt->second.isObject()) {
            return resolveFromObject(protoIt->second.getGC<Object>(), receiver);
          }
        }
        return true;
      }
      if (receiver.isClass()) {
        auto cls = receiver.getGC<Class>();
        auto getterIt = cls->properties.find("__get_" + key);
        if (getterIt != cls->properties.end()) {
          found = true;
          if (!getterIt->second.isFunction()) {
            out = Value(Undefined{});
            return true;
          }
          return callWithThis(getterIt->second, {}, receiver, out, thrown);
        }
        auto it = cls->properties.find(key);
        if (it != cls->properties.end()) {
          found = true;
          out = it->second;
        }
        return true;
      }

      return true;
    };

    if (!constructor.isFunction() && !constructor.isClass()) {
      throw std::runtime_error("TypeError: Promise.all called on non-constructor");
    }

    auto capResolve = std::make_shared<Value>(Value(Undefined{}));
    auto capReject = std::make_shared<Value>(Value(Undefined{}));

    auto executor = GarbageCollector::makeGC<Function>();
    executor->isNative = true;
    executor->isConstructor = false;
    executor->nativeFunc = [capResolve, capReject](const std::vector<Value>& executorArgs) -> Value {
      if (!capResolve->isUndefined()) {
        throw std::runtime_error("TypeError: Promise resolve function already set");
      }
      if (!capReject->isUndefined()) {
        throw std::runtime_error("TypeError: Promise reject function already set");
      }
      *capResolve = executorArgs.size() > 0 ? executorArgs[0] : Value(Undefined{});
      *capReject = executorArgs.size() > 1 ? executorArgs[1] : Value(Undefined{});
      return Value(Undefined{});
    };

    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) {
      throw std::runtime_error("TypeError: Interpreter unavailable");
    }
    interpreter->clearError();
    Value resultPromiseValue = interpreter->constructFromNative(constructor, {Value(executor)});
    if (interpreter->hasError()) {
      Value err = interpreter->getError();
      interpreter->clearError();
      if (err.isError()) {
        auto errPtr = err.getGC<Error>();
        throw std::runtime_error(errPtr->message);
      }
      throw std::runtime_error("TypeError: Failed to construct promise");
    }

    if (!capResolve->isFunction() || !capReject->isFunction()) {
      throw std::runtime_error("TypeError: Promise resolve or reject function is not callable");
    }

    auto rejectCapability = [&callWithThis, capReject](const Value& reason) {
      Value out, thrown;
      callWithThis(*capReject, {reason}, Value(Undefined{}), out, thrown);
    };
    auto rejectAndReturn = [&rejectCapability, &resultPromiseValue](const Value& reason) -> Value {
      rejectCapability(reason);
      return resultPromiseValue;
    };

    Value promiseResolveMethod = Value(Undefined{});
    bool hasResolve = false;
    Value resolveThrown;
    if (!getPropertyWithThrow(constructor, "resolve", promiseResolveMethod, hasResolve, resolveThrown)) {
      return rejectAndReturn(resolveThrown);
    }
    if (!hasResolve || !promiseResolveMethod.isFunction()) {
      return rejectAndReturn(typeErrorValue("Promise.resolve is not callable"));
    }

    auto values = GarbageCollector::makeGC<Array>();
    auto remaining = std::make_shared<size_t>(1);
    auto alreadyResolved = std::make_shared<bool>(false);

    auto fulfillResult = [alreadyResolved, capResolve, capReject, callWithThis](const Value& finalValue) {
      if (*alreadyResolved) {
        return;
      }
      *alreadyResolved = true;
      Value out, thrown;
      if (!callWithThis(*capResolve, {finalValue}, Value(Undefined{}), out, thrown)) {
        callWithThis(*capReject, {thrown}, Value(Undefined{}), out, thrown);
      }
    };

    auto finalizeIfDone = [env, remaining, values, fulfillResult]() {
      if (*remaining == 0) {
        auto resultArray = GarbageCollector::makeGC<Array>();
        resultArray->elements = values->elements;
        if (auto arrayCtor = env->get("Array"); arrayCtor && arrayCtor->isObject()) {
          auto arrayObjPtr = std::get<GCPtr<Object>>(arrayCtor->data);
          auto protoIt = arrayObjPtr->properties.find("prototype");
          if (protoIt != arrayObjPtr->properties.end() && protoIt->second.isObject()) {
            resultArray->properties["__proto__"] = protoIt->second;
          }
        }
        fulfillResult(Value(resultArray));
      }
    };

    auto processElement = [&](size_t index, const Value& nextValue, Value& failureReason) -> bool {
      if (values->elements.size() <= index) {
        values->elements.resize(index + 1, Value(Undefined{}));
      }

      (*remaining)++;
      Value nextPromise = Value(Undefined{});
      Value resolveCallThrown;
      if (!callWithThis(promiseResolveMethod, {nextValue}, constructor, nextPromise, resolveCallThrown)) {
        failureReason = resolveCallThrown;
        return false;
      }

      auto alreadyCalled = std::make_shared<bool>(false);
      auto resolveElement = GarbageCollector::makeGC<Function>();
      resolveElement->isNative = true;
      resolveElement->properties["name"] = Value(std::string(""));
      resolveElement->properties["length"] = Value(1.0);
      resolveElement->nativeFunc = [values, remaining, index, alreadyCalled, finalizeIfDone](const std::vector<Value>& innerArgs) -> Value {
        if (*alreadyCalled) {
          return Value(Undefined{});
        }
        *alreadyCalled = true;
        values->elements[index] = innerArgs.empty() ? Value(Undefined{}) : innerArgs[0];
        (*remaining)--;
        finalizeIfDone();
        return Value(Undefined{});
      };

      Value rejectHandler = *capReject;
      if (nextPromise.isPromise()) {
        auto promisePtr = nextPromise.getGC<Promise>();
        Value overriddenThen = Value(Undefined{});
        bool hasOverriddenThen = false;

        auto getterIt = promisePtr->properties.find("__get_then");
        if (getterIt != promisePtr->properties.end()) {
          hasOverriddenThen = true;
          if (getterIt->second.isFunction()) {
            Value getterOut = Value(Undefined{});
            Value getterThrown;
            if (!callWithThis(getterIt->second, {}, nextPromise, getterOut, getterThrown)) {
              failureReason = getterThrown;
              return false;
            }
            overriddenThen = getterOut;
          }
        } else if (auto thenIt = promisePtr->properties.find("then");
                   thenIt != promisePtr->properties.end()) {
          hasOverriddenThen = true;
          overriddenThen = thenIt->second;
        }

        if (hasOverriddenThen) {
          if (!overriddenThen.isFunction()) {
            failureReason = typeErrorValue("Promise resolve result is not thenable");
            return false;
          }
          Value ignored;
          Value thenInvokeThrown;
          if (!callWithThis(overriddenThen, {Value(resolveElement), rejectHandler},
                            nextPromise, ignored, thenInvokeThrown) &&
              !*alreadyCalled) {
            failureReason = thenInvokeThrown;
            return false;
          }
          return true;
        }

        promisePtr->then(
          [resolveElement](Value v) -> Value {
            return resolveElement->nativeFunc({v});
          },
          [rejectHandler](Value reason) -> Value {
            if (rejectHandler.isFunction()) {
              return rejectHandler.getGC<Function>()->nativeFunc({reason});
            }
            return Value(Undefined{});
          });
        return true;
      }

      Value thenMethod = Value(Undefined{});
      bool hasThenMethod = false;
      Value thenLookupThrown;
      if (!getPropertyWithThrow(nextPromise, "then", thenMethod, hasThenMethod, thenLookupThrown)) {
        failureReason = thenLookupThrown;
        return false;
      }
      if (!hasThenMethod || !thenMethod.isFunction()) {
        failureReason = typeErrorValue("Promise resolve result is not thenable");
        return false;
      }

      Value ignored;
      Value thenInvokeThrown;
      if (!callWithThis(thenMethod, {Value(resolveElement), rejectHandler}, nextPromise, ignored, thenInvokeThrown) &&
          !*alreadyCalled) {
        failureReason = thenInvokeThrown;
        return false;
      }
      return true;
    };

    auto closeIterator = [&](const Value& iteratorValue, Value& closeFailure) -> bool {
      Value returnMethod = Value(Undefined{});
      bool hasReturn = false;
      Value returnLookupThrown;
      if (!getPropertyWithThrow(iteratorValue, "return", returnMethod, hasReturn, returnLookupThrown)) {
        closeFailure = returnLookupThrown;
        return false;
      }
      if (!hasReturn || returnMethod.isUndefined() || returnMethod.isNull()) {
        return true;
      }
      if (!returnMethod.isFunction()) {
        closeFailure = typeErrorValue("Iterator return is not callable");
        return false;
      }
      Value ignored;
      Value returnThrown;
      if (!callWithThis(returnMethod, {}, iteratorValue, ignored, returnThrown)) {
        closeFailure = returnThrown;
        return false;
      }
      return true;
    };

    const auto& iteratorKey = WellKnownSymbols::iteratorKey();
    size_t nextIndex = 0;
    bool useArrayFastPath = false;
    if (iterable.isArray()) {
      auto arr = iterable.getGC<Array>();
      auto getterIt = arr->properties.find("__get_" + iteratorKey);
      auto propIt = arr->properties.find(iteratorKey);
      if (getterIt == arr->properties.end() && propIt == arr->properties.end()) {
        useArrayFastPath = true;
      }
    }
    if (useArrayFastPath) {
      auto arr = iterable.getGC<Array>();
      for (const auto& value : arr->elements) {
        Value failureReason;
        if (!processElement(nextIndex++, value, failureReason)) {
          return rejectAndReturn(failureReason);
        }
      }
    } else if (iterable.isString()) {
      const auto& str = std::get<std::string>(iterable.data);
      for (char c : str) {
        Value failureReason;
        if (!processElement(nextIndex++, Value(std::string(1, c)), failureReason)) {
          return rejectAndReturn(failureReason);
        }
      }
    } else {
      Value iteratorMethod = Value(Undefined{});
      bool hasIteratorMethod = false;
      Value iteratorLookupThrown;
      if (!getPropertyWithThrow(iterable, iteratorKey, iteratorMethod, hasIteratorMethod, iteratorLookupThrown)) {
        return rejectAndReturn(iteratorLookupThrown);
      }
      if (!hasIteratorMethod || iteratorMethod.isUndefined() || iteratorMethod.isNull()) {
        return rejectAndReturn(typeErrorValue("Value is not iterable"));
      }
      if (!iteratorMethod.isFunction()) {
        return rejectAndReturn(typeErrorValue("Symbol.iterator is not callable"));
      }

      Value iteratorValue = Value(Undefined{});
      Value iteratorThrown;
      if (!callWithThis(iteratorMethod, {}, iterable, iteratorValue, iteratorThrown)) {
        return rejectAndReturn(iteratorThrown);
      }
      if (!iteratorValue.isObject()) {
        return rejectAndReturn(typeErrorValue("Iterator must be an object"));
      }

      while (true) {
        Value nextMethod = Value(Undefined{});
        bool hasNext = false;
        Value nextLookupThrown;
        if (!getPropertyWithThrow(iteratorValue, "next", nextMethod, hasNext, nextLookupThrown)) {
          return rejectAndReturn(nextLookupThrown);
        }
        if (!hasNext || !nextMethod.isFunction()) {
          return rejectAndReturn(typeErrorValue("Iterator next is not callable"));
        }

        Value stepResult = Value(Undefined{});
        Value nextThrown;
        if (!callWithThis(nextMethod, {}, iteratorValue, stepResult, nextThrown)) {
          return rejectAndReturn(nextThrown);
        }
        if (!stepResult.isObject()) {
          return rejectAndReturn(typeErrorValue("Iterator result is not an object"));
        }

        Value doneValue = Value(false);
        bool hasDone = false;
        Value doneThrown;
        if (!getPropertyWithThrow(stepResult, "done", doneValue, hasDone, doneThrown)) {
          return rejectAndReturn(doneThrown);
        }
        if (hasDone && doneValue.toBool()) {
          break;
        }

        Value itemValue = Value(Undefined{});
        bool hasItem = false;
        Value itemThrown;
        if (!getPropertyWithThrow(stepResult, "value", itemValue, hasItem, itemThrown)) {
          return rejectAndReturn(itemThrown);
        }

        Value failureReason;
        if (!processElement(nextIndex++, hasItem ? itemValue : Value(Undefined{}), failureReason)) {
          Value closeFailure;
          if (!closeIterator(iteratorValue, closeFailure)) {
            return rejectAndReturn(closeFailure);
          }
          return rejectAndReturn(failureReason);
        }
      }
    }

    (*remaining)--;
    finalizeIfDone();
    return resultPromiseValue;
  };
  promiseConstructor->properties["all"] = Value(promiseAll);

  // Promise.allSettled - waits for all promises to settle (resolve or reject)
  auto promiseAllSettled = GarbageCollector::makeGC<Function>();
  promiseAllSettled->isNative = true;
  promiseAllSettled->properties["__uses_this_arg__"] = Value(true);
  promiseAllSettled->properties["name"] = Value(std::string("allSettled"));
  promiseAllSettled->properties["length"] = Value(1.0);
  promiseAllSettled->nativeFunc = [callChecked, getProperty, env, promiseFunc](const std::vector<Value>& args) -> Value {
    Value constructor = args.empty() ? Value(Undefined{}) : args[0];
    Value iterable = args.size() > 1 ? args[1] : Value(Undefined{});

    auto typeErrorValue = [](const std::string& msg) -> Value {
      return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, msg));
    };

    auto callWithThis = [](const Value& callee,
                           const std::vector<Value>& callArgs,
                           const Value& thisArg,
                           Value& out,
                           Value& thrown) -> bool {
      if (!callee.isFunction()) {
        thrown = Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "Value is not callable"));
        return false;
      }

      auto fn = callee.getGC<Function>();
      if (fn->isNative) {
        try {
          auto itUsesThis = fn->properties.find("__uses_this_arg__");
          if (itUsesThis != fn->properties.end() &&
              itUsesThis->second.isBool() &&
              itUsesThis->second.toBool()) {
            std::vector<Value> nativeArgs;
            nativeArgs.reserve(callArgs.size() + 1);
            nativeArgs.push_back(thisArg);
            nativeArgs.insert(nativeArgs.end(), callArgs.begin(), callArgs.end());
            out = fn->nativeFunc(nativeArgs);
          } else {
            out = fn->nativeFunc(callArgs);
          }
          return true;
        } catch (const std::exception& e) {
          thrown = Value(std::string(e.what()));
          return false;
        }
      }

      Interpreter* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        thrown = Value(std::string("TypeError: Interpreter unavailable"));
        return false;
      }
      interpreter->clearError();
      out = interpreter->callForHarness(callee, callArgs, thisArg);
      if (interpreter->hasError()) {
        thrown = interpreter->getError();
        interpreter->clearError();
        return false;
      }
      return true;
    };

    // === NewPromiseCapability(C) ===
    // Validate constructor is callable/constructable
    if (!constructor.isFunction() && !constructor.isClass()) {
      throw std::runtime_error("TypeError: Promise.allSettled called on non-constructor");
    }

    auto capResolve = std::make_shared<Value>(Value(Undefined{}));
    auto capReject = std::make_shared<Value>(Value(Undefined{}));

    auto executor = GarbageCollector::makeGC<Function>();
    executor->isNative = true;
    executor->isConstructor = false;
    executor->nativeFunc = [capResolve, capReject](const std::vector<Value>& executorArgs) -> Value {
      // GetCapabilitiesExecutor: spec steps 4-7
      if (!capResolve->isUndefined()) {
        throw std::runtime_error("TypeError: Promise resolve function already set");
      }
      if (!capReject->isUndefined()) {
        throw std::runtime_error("TypeError: Promise reject function already set");
      }
      *capResolve = executorArgs.size() > 0 ? executorArgs[0] : Value(Undefined{});
      *capReject = executorArgs.size() > 1 ? executorArgs[1] : Value(Undefined{});
      return Value(Undefined{});
    };

    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) {
      throw std::runtime_error("TypeError: Interpreter unavailable");
    }
    interpreter->clearError();
    Value resultPromiseValue = interpreter->constructFromNative(constructor, {Value(executor)});
    if (interpreter->hasError()) {
      Value err = interpreter->getError();
      interpreter->clearError();
      if (err.isError()) {
        auto errPtr = err.getGC<Error>();
        throw std::runtime_error(errPtr->message);
      }
      throw std::runtime_error("TypeError: Failed to construct promise");
    }

    // Validate resolve and reject are callable
    if (!capResolve->isFunction() || !capReject->isFunction()) {
      throw std::runtime_error("TypeError: Promise resolve or reject function is not callable");
    }

    auto alreadyResolved = std::make_shared<bool>(false);
    auto resultPromiseVal = std::make_shared<Value>(resultPromiseValue);

    // Helper to reject the capability and return the result promise
    auto rejectCapability = [&callWithThis, capReject](const Value& reason) {
      Value out, thrown;
      callWithThis(*capReject, {reason}, Value(Undefined{}), out, thrown);
    };

    auto rejectAndReturn = [&rejectCapability, &resultPromiseValue](const Value& reason) -> Value {
      rejectCapability(reason);
      return resultPromiseValue;
    };

    auto getPropertyWithThrow = [&callWithThis, &env](const Value& receiver,
                                    const std::string& key,
                                    Value& out,
                                    bool& found,
                                    Value& thrown) -> bool {
      auto resolveFromObject = [&](const GCPtr<Object>& obj,
                                   const Value& originalReceiver) -> bool {
        auto current = obj;
        int depth = 0;
        while (current && depth <= 32) {
          depth++;

          auto getterIt = current->properties.find("__get_" + key);
          if (getterIt != current->properties.end()) {
            found = true;
            if (!getterIt->second.isFunction()) {
              out = Value(Undefined{});
              return true;
            }
            Value getterOut;
            if (!callWithThis(getterIt->second, {}, originalReceiver, getterOut, thrown)) {
              return false;
            }
            out = getterOut;
            return true;
          }

          auto it = current->properties.find(key);
          if (it != current->properties.end()) {
            found = true;
            out = it->second;
            return true;
          }

          auto protoIt = current->properties.find("__proto__");
          if (protoIt == current->properties.end() || !protoIt->second.isObject()) {
            break;
          }
          current = protoIt->second.getGC<Object>();
        }
        return true;
      };

      found = false;
      out = Value(Undefined{});

      if (receiver.isObject()) {
        return resolveFromObject(receiver.getGC<Object>(), receiver);
      }
      if (receiver.isFunction()) {
        auto fn = receiver.getGC<Function>();
        auto getterIt = fn->properties.find("__get_" + key);
        if (getterIt != fn->properties.end()) {
          found = true;
          if (!getterIt->second.isFunction()) {
            out = Value(Undefined{});
            return true;
          }
          return callWithThis(getterIt->second, {}, receiver, out, thrown);
        }
        auto it = fn->properties.find(key);
        if (it != fn->properties.end()) {
          found = true;
          out = it->second;
        }
        return true;
      }
      if (receiver.isArray()) {
        auto arr = receiver.getGC<Array>();
        auto getterIt = arr->properties.find("__get_" + key);
        if (getterIt != arr->properties.end()) {
          found = true;
          if (!getterIt->second.isFunction()) {
            out = Value(Undefined{});
            return true;
          }
          return callWithThis(getterIt->second, {}, receiver, out, thrown);
        }
        auto it = arr->properties.find(key);
        if (it != arr->properties.end()) {
          found = true;
          out = it->second;
          return true;
        }
        if (key == "length") {
          found = true;
          out = Value(static_cast<double>(arr->elements.size()));
          return true;
        }
        if (auto arrayCtor = env->get("Array"); arrayCtor && arrayCtor->isObject()) {
          auto arrayObjPtr = std::get<GCPtr<Object>>(arrayCtor->data);
          auto protoIt = arrayObjPtr->properties.find("prototype");
          if (protoIt != arrayObjPtr->properties.end() && protoIt->second.isObject()) {
            return resolveFromObject(protoIt->second.getGC<Object>(), receiver);
          }
        }
        return true;
      }
      if (receiver.isClass()) {
        auto cls = receiver.getGC<Class>();
        auto getterIt = cls->properties.find("__get_" + key);
        if (getterIt != cls->properties.end()) {
          found = true;
          if (!getterIt->second.isFunction()) {
            out = Value(Undefined{});
            return true;
          }
          return callWithThis(getterIt->second, {}, receiver, out, thrown);
        }
        auto it = cls->properties.find(key);
        if (it != cls->properties.end()) {
          found = true;
          out = it->second;
        }
        return true;
      }

      return true;
    };

    Value promiseResolveMethod = Value(Undefined{});
    bool hasResolve = false;
    Value resolveThrown;
    if (!getPropertyWithThrow(constructor, "resolve", promiseResolveMethod, hasResolve, resolveThrown)) {
      return rejectAndReturn(resolveThrown);
    }
    if (!hasResolve || !promiseResolveMethod.isFunction()) {
      return rejectAndReturn(typeErrorValue("Promise.resolve is not callable"));
    }

    auto results = makeArrayWithPrototype();
    auto remaining = std::make_shared<size_t>(1);

    auto resolveResultPromise = std::make_shared<std::function<void(const Value&)>>();
    *resolveResultPromise = [alreadyResolved, capResolve, capReject, resultPromiseVal,
                             resolveResultPromise, getPropertyWithThrow, callWithThis](const Value& finalValue) {
      if (*alreadyResolved) {
        return;
      }

      if (finalValue.isPromise()) {
        auto nested = finalValue.getGC<Promise>();
        // Check self-reference
        if (resultPromiseVal->isPromise() &&
            nested.get() == std::get<GCPtr<Promise>>(resultPromiseVal->data).get()) {
          *alreadyResolved = true;
          Value out, thrown;
          callWithThis(*capReject, {Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "Cannot resolve promise with itself"))},
                       Value(Undefined{}), out, thrown);
          return;
        }
        if (nested->state == PromiseState::Fulfilled) {
          (*resolveResultPromise)(nested->result);
          return;
        }
        if (nested->state == PromiseState::Rejected) {
          *alreadyResolved = true;
          Value out, thrown;
          callWithThis(*capReject, {nested->result}, Value(Undefined{}), out, thrown);
          return;
        }
        nested->then(
          [resolveResultPromise](Value fulfilled) -> Value {
            (*resolveResultPromise)(fulfilled);
            return fulfilled;
          },
          [alreadyResolved, capReject, callWithThis](Value reason) -> Value {
            if (!*alreadyResolved) {
              *alreadyResolved = true;
              Value out, thrown;
              callWithThis(*capReject, {reason}, Value(Undefined{}), out, thrown);
            }
            return reason;
          });
        return;
      }

      if (finalValue.isObject() || finalValue.isArray() || finalValue.isFunction() ||
          finalValue.isRegex() || finalValue.isProxy()) {
        Value thenValue = Value(Undefined{});
        bool hasThen = false;
        Value thenThrown;
        if (!getPropertyWithThrow(finalValue, "then", thenValue, hasThen, thenThrown)) {
          *alreadyResolved = true;
          Value out, thrown;
          callWithThis(*capReject, {thenThrown}, Value(Undefined{}), out, thrown);
          return;
        }
        if (hasThen && thenValue.isFunction()) {
          auto alreadyCalled = std::make_shared<bool>(false);
          auto resolveFn = GarbageCollector::makeGC<Function>();
          resolveFn->isNative = true;
          resolveFn->nativeFunc = [resolveResultPromise, alreadyCalled](const std::vector<Value>& innerArgs) -> Value {
            if (*alreadyCalled) {
              return Value(Undefined{});
            }
            *alreadyCalled = true;
            Value next = innerArgs.empty() ? Value(Undefined{}) : innerArgs[0];
            (*resolveResultPromise)(next);
            return Value(Undefined{});
          };
          auto rejectFn = GarbageCollector::makeGC<Function>();
          rejectFn->isNative = true;
          rejectFn->nativeFunc = [alreadyResolved, capReject, callWithThis, alreadyCalled](const std::vector<Value>& innerArgs) -> Value {
            if (*alreadyCalled) {
              return Value(Undefined{});
            }
            *alreadyCalled = true;
            if (!*alreadyResolved) {
              *alreadyResolved = true;
              Value reason = innerArgs.empty() ? Value(Undefined{}) : innerArgs[0];
              Value out, thrown;
              callWithThis(*capReject, {reason}, Value(Undefined{}), out, thrown);
            }
            return Value(Undefined{});
          };

          Value ignored;
          Value thenCallThrown;
          if (!callWithThis(thenValue, {Value(resolveFn), Value(rejectFn)}, finalValue, ignored, thenCallThrown) &&
              !*alreadyCalled) {
            *alreadyResolved = true;
            Value out, thrown;
            callWithThis(*capReject, {thenCallThrown}, Value(Undefined{}), out, thrown);
          }
          return;
        }
      }

      *alreadyResolved = true;
      Value out, thrown;
      if (!callWithThis(*capResolve, {finalValue}, Value(Undefined{}), out, thrown)) {
        // If resolve throws, reject the capability with the thrown error
        Value out2, thrown2;
        callWithThis(*capReject, {thrown}, Value(Undefined{}), out2, thrown2);
      }
    };

    auto finalizeIfDone = [remaining, results, resolveResultPromise]() {
      if (*remaining == 0) {
        auto valuesArray = makeArrayWithPrototype();
        valuesArray->elements = results->elements;
        (*resolveResultPromise)(Value(valuesArray));
      }
    };

    auto processElement = [&](size_t index, const Value& nextValue, Value& failureReason) -> bool {
      if (results->elements.size() <= index) {
        results->elements.resize(index + 1, Value(Undefined{}));
      }

      (*remaining)++;
      Value nextPromise = Value(Undefined{});
      Value resolveCallThrown;
      if (!callWithThis(promiseResolveMethod, {nextValue}, constructor, nextPromise, resolveCallThrown)) {
        failureReason = resolveCallThrown;
        return false;
      }

      auto alreadyCalled = std::make_shared<bool>(false);
      auto resolveElement = GarbageCollector::makeGC<Function>();
      resolveElement->isNative = true;
      resolveElement->properties["length"] = Value(1.0);
      resolveElement->properties["name"] = Value(std::string(""));
      resolveElement->nativeFunc = [results, remaining, index, alreadyCalled, finalizeIfDone](const std::vector<Value>& innerArgs) -> Value {
        if (*alreadyCalled) {
          return Value(Undefined{});
        }
        *alreadyCalled = true;
        Value settledValue = innerArgs.empty() ? Value(Undefined{}) : innerArgs[0];
        auto entry = GarbageCollector::makeGC<Object>();
        entry->properties["status"] = Value(std::string("fulfilled"));
        entry->properties["value"] = settledValue;
        results->elements[index] = Value(entry);
        (*remaining)--;
        finalizeIfDone();
        return Value(Undefined{});
      };

      auto rejectElement = GarbageCollector::makeGC<Function>();
      rejectElement->isNative = true;
      rejectElement->properties["length"] = Value(1.0);
      rejectElement->properties["name"] = Value(std::string(""));
      rejectElement->nativeFunc = [results, remaining, index, alreadyCalled, finalizeIfDone](const std::vector<Value>& innerArgs) -> Value {
        if (*alreadyCalled) {
          return Value(Undefined{});
        }
        *alreadyCalled = true;
        Value settledReason = innerArgs.empty() ? Value(Undefined{}) : innerArgs[0];
        auto entry = GarbageCollector::makeGC<Object>();
        entry->properties["status"] = Value(std::string("rejected"));
        entry->properties["reason"] = settledReason;
        results->elements[index] = Value(entry);
        (*remaining)--;
        finalizeIfDone();
        return Value(Undefined{});
      };

      if (nextPromise.isPromise()) {
        auto promisePtr = nextPromise.getGC<Promise>();
        Value overriddenThen = Value(Undefined{});
        bool hasOverriddenThen = false;

        auto getterIt = promisePtr->properties.find("__get_then");
        if (getterIt != promisePtr->properties.end()) {
          hasOverriddenThen = true;
          if (getterIt->second.isFunction()) {
            Value getterOut = Value(Undefined{});
            Value getterThrown;
            if (!callWithThis(getterIt->second, {}, nextPromise, getterOut, getterThrown)) {
              failureReason = getterThrown;
              return false;
            }
            overriddenThen = getterOut;
          }
        } else if (auto thenIt = promisePtr->properties.find("then");
                   thenIt != promisePtr->properties.end()) {
          hasOverriddenThen = true;
          overriddenThen = thenIt->second;
        }

        if (hasOverriddenThen) {
          if (!overriddenThen.isFunction()) {
            failureReason = typeErrorValue("Promise resolve result is not thenable");
            return false;
          }
          Value ignored;
          Value thenInvokeThrown;
          if (!callWithThis(overriddenThen, {Value(resolveElement), Value(rejectElement)},
                            nextPromise, ignored, thenInvokeThrown) &&
              !*alreadyCalled) {
            failureReason = thenInvokeThrown;
            return false;
          }
          return true;
        }

        promisePtr->then(
          [resolveElement](Value v) -> Value {
            return resolveElement->nativeFunc({v});
          },
          [rejectElement](Value reason) -> Value {
            return rejectElement->nativeFunc({reason});
          });
        return true;
      }

      Value thenMethod = Value(Undefined{});
      bool hasThenMethod = false;
      Value thenLookupThrown;
      if (!getPropertyWithThrow(nextPromise, "then", thenMethod, hasThenMethod, thenLookupThrown)) {
        failureReason = thenLookupThrown;
        return false;
      }
      if (!hasThenMethod || !thenMethod.isFunction()) {
        failureReason = typeErrorValue("Promise resolve result is not thenable");
        return false;
      }

      Value ignored;
      Value thenInvokeThrown;
      if (!callWithThis(thenMethod, {Value(resolveElement), Value(rejectElement)}, nextPromise, ignored, thenInvokeThrown) &&
          !*alreadyCalled) {
        failureReason = thenInvokeThrown;
        return false;
      }
      return true;
    };

    auto closeIterator = [&](const Value& iteratorValue, Value& closeFailure) -> bool {
      Value returnMethod = Value(Undefined{});
      bool hasReturn = false;
      Value returnLookupThrown;
      if (!getPropertyWithThrow(iteratorValue, "return", returnMethod, hasReturn, returnLookupThrown)) {
        closeFailure = returnLookupThrown;
        return false;
      }
      if (!hasReturn || returnMethod.isUndefined() || returnMethod.isNull()) {
        return true;
      }
      if (!returnMethod.isFunction()) {
        closeFailure = typeErrorValue("Iterator return is not callable");
        return false;
      }
      Value ignored;
      Value returnThrown;
      if (!callWithThis(returnMethod, {}, iteratorValue, ignored, returnThrown)) {
        closeFailure = returnThrown;
        return false;
      }
      return true;
    };

    const auto& iteratorKey = WellKnownSymbols::iteratorKey();
    size_t nextIndex = 0;
    // Check if array has a custom/poisoned Symbol.iterator getter before using fast path
    bool useArrayFastPath = false;
    if (iterable.isArray()) {
      auto arr = iterable.getGC<Array>();
      auto getterIt = arr->properties.find("__get_" + iteratorKey);
      auto propIt = arr->properties.find(iteratorKey);
      if (getterIt == arr->properties.end() && propIt == arr->properties.end()) {
        useArrayFastPath = true;
      }
    }
    if (useArrayFastPath) {
      auto arr = iterable.getGC<Array>();
      for (const auto& value : arr->elements) {
        Value failureReason;
        if (!processElement(nextIndex++, value, failureReason)) {
          return rejectAndReturn(failureReason);
        }
      }
    } else if (iterable.isString()) {
      const auto& str = std::get<std::string>(iterable.data);
      for (char c : str) {
        Value failureReason;
        if (!processElement(nextIndex++, Value(std::string(1, c)), failureReason)) {
          return rejectAndReturn(failureReason);
        }
      }
    } else {
      Value iteratorMethod = Value(Undefined{});
      bool hasIteratorMethod = false;
      Value iteratorLookupThrown;
      if (!getPropertyWithThrow(iterable, iteratorKey, iteratorMethod, hasIteratorMethod, iteratorLookupThrown)) {
        return rejectAndReturn(iteratorLookupThrown);
      }
      if (!hasIteratorMethod || iteratorMethod.isUndefined() || iteratorMethod.isNull()) {
        return rejectAndReturn(typeErrorValue("Value is not iterable"));
      }
      if (!iteratorMethod.isFunction()) {
        return rejectAndReturn(typeErrorValue("Symbol.iterator is not callable"));
      }

      Value iteratorValue = Value(Undefined{});
      Value iteratorThrown;
      if (!callWithThis(iteratorMethod, {}, iterable, iteratorValue, iteratorThrown)) {
        return rejectAndReturn(iteratorThrown);
      }
      if (!iteratorValue.isObject()) {
        return rejectAndReturn(typeErrorValue("Iterator must be an object"));
      }

      while (true) {
        Value nextMethod = Value(Undefined{});
        bool hasNext = false;
        Value nextLookupThrown;
        if (!getPropertyWithThrow(iteratorValue, "next", nextMethod, hasNext, nextLookupThrown)) {
          return rejectAndReturn(nextLookupThrown);
        }
        if (!hasNext || !nextMethod.isFunction()) {
          return rejectAndReturn(typeErrorValue("Iterator next is not callable"));
        }

        Value stepResult = Value(Undefined{});
        Value nextThrown;
        if (!callWithThis(nextMethod, {}, iteratorValue, stepResult, nextThrown)) {
          return rejectAndReturn(nextThrown);
        }
        if (!stepResult.isObject()) {
          return rejectAndReturn(typeErrorValue("Iterator result is not an object"));
        }

        Value doneValue = Value(false);
        bool hasDone = false;
        Value doneThrown;
        if (!getPropertyWithThrow(stepResult, "done", doneValue, hasDone, doneThrown)) {
          return rejectAndReturn(doneThrown);
        }
        if (hasDone && doneValue.toBool()) {
          break;
        }

        Value itemValue = Value(Undefined{});
        bool hasItem = false;
        Value itemThrown;
        if (!getPropertyWithThrow(stepResult, "value", itemValue, hasItem, itemThrown)) {
          return rejectAndReturn(itemThrown);
        }

        Value failureReason;
        if (!processElement(nextIndex++, hasItem ? itemValue : Value(Undefined{}), failureReason)) {
          Value closeFailure;
          if (!closeIterator(iteratorValue, closeFailure)) {
            return rejectAndReturn(closeFailure);
          }
          return rejectAndReturn(failureReason);
        }
      }
    }

    (*remaining)--;
    finalizeIfDone();
    return resultPromiseValue;
  };
  promiseConstructor->properties["allSettled"] = Value(promiseAllSettled);

  // Promise.any - resolves when any promise fulfills, rejects if all reject
  auto promiseAny = GarbageCollector::makeGC<Function>();
  promiseAny->isNative = true;
  promiseAny->properties["__uses_this_arg__"] = Value(true);
  promiseAny->properties["name"] = Value(std::string("any"));
  promiseAny->properties["length"] = Value(1.0);
  promiseAny->nativeFunc = [callChecked, env, promiseFunc](const std::vector<Value>& args) -> Value {
    Value constructor = args.empty() ? Value(Undefined{}) : args[0];
    Value iterable = args.size() > 1 ? args[1] : Value(Undefined{});

    auto typeErrorValue = [](const std::string& msg) -> Value {
      return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, msg));
    };

    auto callWithThis = [](const Value& callee,
                           const std::vector<Value>& callArgs,
                           const Value& thisArg,
                           Value& out,
                           Value& thrown) -> bool {
      if (!callee.isFunction()) {
        thrown = Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "Value is not callable"));
        return false;
      }

      auto fn = callee.getGC<Function>();
      if (fn->isNative) {
        try {
          auto itUsesThis = fn->properties.find("__uses_this_arg__");
          if (itUsesThis != fn->properties.end() &&
              itUsesThis->second.isBool() &&
              itUsesThis->second.toBool()) {
            std::vector<Value> nativeArgs;
            nativeArgs.reserve(callArgs.size() + 1);
            nativeArgs.push_back(thisArg);
            nativeArgs.insert(nativeArgs.end(), callArgs.begin(), callArgs.end());
            out = fn->nativeFunc(nativeArgs);
          } else {
            out = fn->nativeFunc(callArgs);
          }
          return true;
        } catch (const JsValueException& e) {
          thrown = e.value();
          return false;
        } catch (const std::exception& e) {
          thrown = Value(std::string(e.what()));
          return false;
        }
      }

      Interpreter* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        thrown = Value(std::string("TypeError: Interpreter unavailable"));
        return false;
      }
      interpreter->clearError();
      out = interpreter->callForHarness(callee, callArgs, thisArg);
      if (interpreter->hasError()) {
        thrown = interpreter->getError();
        interpreter->clearError();
        return false;
      }
      return true;
    };

    auto getPropertyWithThrow = [&callWithThis, &env](const Value& receiver,
                                                      const std::string& key,
                                                      Value& out,
                                                      bool& found,
                                                      Value& thrown) -> bool {
      auto resolveFromObject = [&](const GCPtr<Object>& obj,
                                   const Value& originalReceiver) -> bool {
        auto current = obj;
        int depth = 0;
        while (current && depth <= 32) {
          depth++;

          auto getterIt = current->properties.find("__get_" + key);
          if (getterIt != current->properties.end()) {
            found = true;
            if (!getterIt->second.isFunction()) {
              out = Value(Undefined{});
              return true;
            }
            Value getterOut;
            if (!callWithThis(getterIt->second, {}, originalReceiver, getterOut, thrown)) {
              return false;
            }
            out = getterOut;
            return true;
          }

          auto it = current->properties.find(key);
          if (it != current->properties.end()) {
            found = true;
            out = it->second;
            return true;
          }

          auto protoIt = current->properties.find("__proto__");
          if (protoIt == current->properties.end() || !protoIt->second.isObject()) {
            break;
          }
          current = protoIt->second.getGC<Object>();
        }
        return true;
      };

      found = false;
      out = Value(Undefined{});

      if (receiver.isObject()) {
        return resolveFromObject(receiver.getGC<Object>(), receiver);
      }
      if (receiver.isFunction()) {
        auto fn = receiver.getGC<Function>();
        auto getterIt = fn->properties.find("__get_" + key);
        if (getterIt != fn->properties.end()) {
          found = true;
          if (!getterIt->second.isFunction()) {
            out = Value(Undefined{});
            return true;
          }
          return callWithThis(getterIt->second, {}, receiver, out, thrown);
        }
        auto it = fn->properties.find(key);
        if (it != fn->properties.end()) {
          found = true;
          out = it->second;
        }
        return true;
      }
      if (receiver.isArray()) {
        auto arr = receiver.getGC<Array>();
        auto getterIt = arr->properties.find("__get_" + key);
        if (getterIt != arr->properties.end()) {
          found = true;
          if (!getterIt->second.isFunction()) {
            out = Value(Undefined{});
            return true;
          }
          return callWithThis(getterIt->second, {}, receiver, out, thrown);
        }
        auto it = arr->properties.find(key);
        if (it != arr->properties.end()) {
          found = true;
          out = it->second;
          return true;
        }
        if (key == "length") {
          found = true;
          out = Value(static_cast<double>(arr->elements.size()));
          return true;
        }
        if (auto arrayCtor = env->get("Array"); arrayCtor && arrayCtor->isObject()) {
          auto arrayObjPtr = std::get<GCPtr<Object>>(arrayCtor->data);
          auto protoIt = arrayObjPtr->properties.find("prototype");
          if (protoIt != arrayObjPtr->properties.end() && protoIt->second.isObject()) {
            return resolveFromObject(protoIt->second.getGC<Object>(), receiver);
          }
        }
        return true;
      }
      if (receiver.isClass()) {
        auto cls = receiver.getGC<Class>();
        auto getterIt = cls->properties.find("__get_" + key);
        if (getterIt != cls->properties.end()) {
          found = true;
          if (!getterIt->second.isFunction()) {
            out = Value(Undefined{});
            return true;
          }
          return callWithThis(getterIt->second, {}, receiver, out, thrown);
        }
        auto it = cls->properties.find(key);
        if (it != cls->properties.end()) {
          found = true;
          out = it->second;
        }
        return true;
      }

      return true;
    };

    auto makeArrayWithProto = [env](const std::vector<Value>& elements) -> Value {
      auto array = GarbageCollector::makeGC<Array>();
      array->elements = elements;
      if (auto arrayCtor = env->get("Array"); arrayCtor && arrayCtor->isObject()) {
        auto arrayObjPtr = std::get<GCPtr<Object>>(arrayCtor->data);
        auto protoIt = arrayObjPtr->properties.find("prototype");
        if (protoIt != arrayObjPtr->properties.end() && protoIt->second.isObject()) {
          array->properties["__proto__"] = protoIt->second;
        }
      }
      return Value(array);
    };

    auto makeAggregateError = [env, makeArrayWithProto](const std::vector<Value>& reasons) -> Value {
      auto error = GarbageCollector::makeGC<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      if (auto aggregateCtor = env->get("AggregateError"); aggregateCtor && aggregateCtor->isFunction()) {
        auto ctor = aggregateCtor->getGC<Function>();
        auto protoIt = ctor->properties.find("prototype");
        if (protoIt != ctor->properties.end()) {
          error->properties["__proto__"] = protoIt->second;
        }
      }
      error->properties["message"] = Value(std::string("All promises were rejected"));
      error->properties["errors"] = makeArrayWithProto(reasons);
      error->properties["__non_enum_errors"] = Value(true);
      return Value(error);
    };

    if (!constructor.isFunction() && !constructor.isClass()) {
      throw std::runtime_error("TypeError: Promise.any called on non-constructor");
    }

    auto capResolve = std::make_shared<Value>(Value(Undefined{}));
    auto capReject = std::make_shared<Value>(Value(Undefined{}));

    auto executor = GarbageCollector::makeGC<Function>();
    executor->isNative = true;
    executor->isConstructor = false;
    executor->nativeFunc = [capResolve, capReject](const std::vector<Value>& executorArgs) -> Value {
      if (!capResolve->isUndefined()) {
        throw std::runtime_error("TypeError: Promise resolve function already set");
      }
      if (!capReject->isUndefined()) {
        throw std::runtime_error("TypeError: Promise reject function already set");
      }
      *capResolve = executorArgs.size() > 0 ? executorArgs[0] : Value(Undefined{});
      *capReject = executorArgs.size() > 1 ? executorArgs[1] : Value(Undefined{});
      return Value(Undefined{});
    };

    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) {
      throw std::runtime_error("TypeError: Interpreter unavailable");
    }
    interpreter->clearError();
    Value resultPromiseValue = interpreter->constructFromNative(constructor, {Value(executor)});
    if (interpreter->hasError()) {
      Value err = interpreter->getError();
      interpreter->clearError();
      if (err.isError()) {
        auto errPtr = err.getGC<Error>();
        throw std::runtime_error(errPtr->message);
      }
      throw std::runtime_error("TypeError: Failed to construct promise");
    }

    if (!capResolve->isFunction() || !capReject->isFunction()) {
      throw std::runtime_error("TypeError: Promise resolve or reject function is not callable");
    }

    auto rejectAndReturn = [&callWithThis, &resultPromiseValue, capReject](const Value& reason) -> Value {
      Value out, thrown;
      callWithThis(*capReject, {reason}, Value(Undefined{}), out, thrown);
      return resultPromiseValue;
    };

    Value promiseResolveMethod = Value(Undefined{});
    bool hasResolve = false;
    Value resolveThrown;
    if (!getPropertyWithThrow(constructor, "resolve", promiseResolveMethod, hasResolve, resolveThrown)) {
      return rejectAndReturn(resolveThrown);
    }
    if (!hasResolve || !promiseResolveMethod.isFunction()) {
      return rejectAndReturn(typeErrorValue("Promise.resolve is not callable"));
    }

    auto reasons = std::make_shared<std::vector<Value>>();
    auto remaining = std::make_shared<size_t>(1);
    auto alreadySettled = std::make_shared<bool>(false);

    auto rejectIfDone = [alreadySettled, capReject, callWithThis, reasons, remaining, makeAggregateError]() {
      if (*alreadySettled || *remaining != 0) {
        return;
      }
      *alreadySettled = true;
      Value aggregate = makeAggregateError(*reasons);
      Value out, thrown;
      callWithThis(*capReject, {aggregate}, Value(Undefined{}), out, thrown);
    };

    auto processElement = [&](size_t index, const Value& nextValue, Value& failureReason) -> bool {
      if (reasons->size() <= index) {
        reasons->resize(index + 1, Value(Undefined{}));
      }

      (*remaining)++;
      Value nextPromise = Value(Undefined{});
      Value resolveCallThrown;
      if (!callWithThis(promiseResolveMethod, {nextValue}, constructor, nextPromise, resolveCallThrown)) {
        failureReason = resolveCallThrown;
        return false;
      }

      auto alreadyCalled = std::make_shared<bool>(false);
      auto rejectElement = GarbageCollector::makeGC<Function>();
      rejectElement->isNative = true;
      rejectElement->properties["name"] = Value(std::string(""));
      rejectElement->properties["length"] = Value(1.0);
      rejectElement->properties["__non_writable_name"] = Value(true);
      rejectElement->properties["__non_enum_name"] = Value(true);
      rejectElement->properties["__non_writable_length"] = Value(true);
      rejectElement->properties["__non_enum_length"] = Value(true);
      rejectElement->nativeFunc = [reasons, remaining, index, alreadyCalled, rejectIfDone](const std::vector<Value>& innerArgs) -> Value {
        if (*alreadyCalled) {
          return Value(Undefined{});
        }
        *alreadyCalled = true;
        (*reasons)[index] = innerArgs.empty() ? Value(Undefined{}) : innerArgs[0];
        (*remaining)--;
        rejectIfDone();
        return Value(Undefined{});
      };

      Value resolveHandler = *capResolve;
      if (nextPromise.isPromise()) {
        auto promisePtr = nextPromise.getGC<Promise>();
        Value overriddenThen = Value(Undefined{});
        bool hasOverriddenThen = false;

        auto getterIt = promisePtr->properties.find("__get_then");
        if (getterIt != promisePtr->properties.end()) {
          hasOverriddenThen = true;
          if (getterIt->second.isFunction()) {
            Value getterOut = Value(Undefined{});
            Value getterThrown;
            if (!callWithThis(getterIt->second, {}, nextPromise, getterOut, getterThrown)) {
              failureReason = getterThrown;
              return false;
            }
            overriddenThen = getterOut;
          }
        } else if (auto thenIt = promisePtr->properties.find("then");
                   thenIt != promisePtr->properties.end()) {
          hasOverriddenThen = true;
          overriddenThen = thenIt->second;
        }

        if (hasOverriddenThen) {
          if (!overriddenThen.isFunction()) {
            failureReason = typeErrorValue("Promise resolve result is not thenable");
            return false;
          }
          Value ignored;
          Value thenInvokeThrown;
          if (!callWithThis(overriddenThen, {resolveHandler, Value(rejectElement)},
                            nextPromise, ignored, thenInvokeThrown) &&
              !*alreadyCalled) {
            failureReason = thenInvokeThrown;
            return false;
          }
          return true;
        }

        promisePtr->then(
          [resolveHandler, callWithThis](Value v) -> Value {
            Value out, thrown;
            callWithThis(resolveHandler, {v}, Value(Undefined{}), out, thrown);
            return v;
          },
          [rejectElement](Value reason) -> Value {
            return rejectElement->nativeFunc({reason});
          });
        return true;
      }

      Value thenMethod = Value(Undefined{});
      bool hasThenMethod = false;
      Value thenLookupThrown;
      if (!getPropertyWithThrow(nextPromise, "then", thenMethod, hasThenMethod, thenLookupThrown)) {
        failureReason = thenLookupThrown;
        return false;
      }
      if (!hasThenMethod || !thenMethod.isFunction()) {
        failureReason = typeErrorValue("Promise resolve result is not thenable");
        return false;
      }

      Value ignored;
      Value thenInvokeThrown;
      if (!callWithThis(thenMethod, {resolveHandler, Value(rejectElement)}, nextPromise, ignored, thenInvokeThrown) &&
          !*alreadyCalled) {
        failureReason = thenInvokeThrown;
        return false;
      }
      return true;
    };

    auto closeIterator = [&](const Value& iteratorValue, Value& closeFailure) -> bool {
      Value returnMethod = Value(Undefined{});
      bool hasReturn = false;
      Value returnLookupThrown;
      if (!getPropertyWithThrow(iteratorValue, "return", returnMethod, hasReturn, returnLookupThrown)) {
        closeFailure = returnLookupThrown;
        return false;
      }
      if (!hasReturn || returnMethod.isUndefined() || returnMethod.isNull()) {
        return true;
      }
      if (!returnMethod.isFunction()) {
        closeFailure = typeErrorValue("Iterator return is not callable");
        return false;
      }
      Value ignored;
      Value returnThrown;
      if (!callWithThis(returnMethod, {}, iteratorValue, ignored, returnThrown)) {
        closeFailure = returnThrown;
        return false;
      }
      return true;
    };

    const auto& iteratorKey = WellKnownSymbols::iteratorKey();
    size_t nextIndex = 0;
    bool useArrayFastPath = false;
    if (iterable.isArray()) {
      auto arr = iterable.getGC<Array>();
      auto getterIt = arr->properties.find("__get_" + iteratorKey);
      auto propIt = arr->properties.find(iteratorKey);
      if (getterIt == arr->properties.end() && propIt == arr->properties.end()) {
        useArrayFastPath = true;
      }
    }
    if (useArrayFastPath) {
      auto arr = iterable.getGC<Array>();
      for (const auto& value : arr->elements) {
        Value failureReason;
        if (!processElement(nextIndex++, value, failureReason)) {
          return rejectAndReturn(failureReason);
        }
      }
    } else if (iterable.isString()) {
      const auto& str = std::get<std::string>(iterable.data);
      for (char c : str) {
        Value failureReason;
        if (!processElement(nextIndex++, Value(std::string(1, c)), failureReason)) {
          return rejectAndReturn(failureReason);
        }
      }
    } else {
      Value iteratorMethod = Value(Undefined{});
      bool hasIteratorMethod = false;
      Value iteratorLookupThrown;
      if (!getPropertyWithThrow(iterable, iteratorKey, iteratorMethod, hasIteratorMethod, iteratorLookupThrown)) {
        return rejectAndReturn(iteratorLookupThrown);
      }
      if (!hasIteratorMethod || iteratorMethod.isUndefined() || iteratorMethod.isNull()) {
        return rejectAndReturn(typeErrorValue("Value is not iterable"));
      }
      if (!iteratorMethod.isFunction()) {
        return rejectAndReturn(typeErrorValue("Symbol.iterator is not callable"));
      }

      Value iteratorValue = Value(Undefined{});
      Value iteratorThrown;
      if (!callWithThis(iteratorMethod, {}, iterable, iteratorValue, iteratorThrown)) {
        return rejectAndReturn(iteratorThrown);
      }
      if (!iteratorValue.isObject()) {
        return rejectAndReturn(typeErrorValue("Iterator must be an object"));
      }

      while (true) {
        Value nextMethod = Value(Undefined{});
        bool hasNext = false;
        Value nextLookupThrown;
        if (!getPropertyWithThrow(iteratorValue, "next", nextMethod, hasNext, nextLookupThrown)) {
          return rejectAndReturn(nextLookupThrown);
        }
        if (!hasNext || !nextMethod.isFunction()) {
          return rejectAndReturn(typeErrorValue("Iterator next is not callable"));
        }

        Value stepResult = Value(Undefined{});
        Value nextThrown;
        if (!callWithThis(nextMethod, {}, iteratorValue, stepResult, nextThrown)) {
          return rejectAndReturn(nextThrown);
        }
        if (!stepResult.isObject()) {
          return rejectAndReturn(typeErrorValue("Iterator result is not an object"));
        }

        Value doneValue = Value(false);
        bool hasDone = false;
        Value doneThrown;
        if (!getPropertyWithThrow(stepResult, "done", doneValue, hasDone, doneThrown)) {
          return rejectAndReturn(doneThrown);
        }
        if (hasDone && doneValue.toBool()) {
          break;
        }

        Value itemValue = Value(Undefined{});
        bool hasItem = false;
        Value itemThrown;
        if (!getPropertyWithThrow(stepResult, "value", itemValue, hasItem, itemThrown)) {
          return rejectAndReturn(itemThrown);
        }

        Value failureReason;
        if (!processElement(nextIndex++, hasItem ? itemValue : Value(Undefined{}), failureReason)) {
          Value closeFailure;
          if (!closeIterator(iteratorValue, closeFailure)) {
            return rejectAndReturn(closeFailure);
          }
          return rejectAndReturn(failureReason);
        }
      }
    }

    (*remaining)--;
    rejectIfDone();
    return resultPromiseValue;
  };
  promiseConstructor->properties["any"] = Value(promiseAny);

  // Promise.race - resolves or rejects with the first settled promise
  auto promiseRace = GarbageCollector::makeGC<Function>();
  promiseRace->isNative = true;
  promiseRace->properties["__uses_this_arg__"] = Value(true);
  promiseRace->properties["name"] = Value(std::string("race"));
  promiseRace->properties["length"] = Value(1.0);
  promiseRace->nativeFunc = [callChecked, getProperty, env, promiseFunc](const std::vector<Value>& args) -> Value {
    Value constructor = args.empty() ? Value(Undefined{}) : args[0];
    Value iterable = args.size() > 1 ? args[1] : Value(Undefined{});

    auto typeErrorValue = [](const std::string& msg) -> Value {
      return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, msg));
    };

    auto callWithThis = [](const Value& callee,
                           const std::vector<Value>& callArgs,
                           const Value& thisArg,
                           Value& out,
                           Value& thrown) -> bool {
      if (!callee.isFunction()) {
        thrown = Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "Value is not callable"));
        return false;
      }

      auto fn = callee.getGC<Function>();
      if (fn->isNative) {
        try {
          auto itUsesThis = fn->properties.find("__uses_this_arg__");
          if (itUsesThis != fn->properties.end() &&
              itUsesThis->second.isBool() &&
              itUsesThis->second.toBool()) {
            std::vector<Value> nativeArgs;
            nativeArgs.reserve(callArgs.size() + 1);
            nativeArgs.push_back(thisArg);
            nativeArgs.insert(nativeArgs.end(), callArgs.begin(), callArgs.end());
            out = fn->nativeFunc(nativeArgs);
          } else {
            out = fn->nativeFunc(callArgs);
          }
          return true;
        } catch (const JsValueException& e) {
          thrown = e.value();
          return false;
        } catch (const std::exception& e) {
          thrown = Value(std::string(e.what()));
          return false;
        }
      }

      Interpreter* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        thrown = Value(std::string("TypeError: Interpreter unavailable"));
        return false;
      }
      interpreter->clearError();
      out = interpreter->callForHarness(callee, callArgs, thisArg);
      if (interpreter->hasError()) {
        thrown = interpreter->getError();
        interpreter->clearError();
        return false;
      }
      return true;
    };

    auto getPropertyWithThrow = [&callWithThis, &env](const Value& receiver,
                                                      const std::string& key,
                                                      Value& out,
                                                      bool& found,
                                                      Value& thrown) -> bool {
      auto resolveFromObject = [&](const GCPtr<Object>& obj,
                                   const Value& originalReceiver) -> bool {
        auto current = obj;
        int depth = 0;
        while (current && depth <= 32) {
          depth++;

          auto getterIt = current->properties.find("__get_" + key);
          if (getterIt != current->properties.end()) {
            found = true;
            if (!getterIt->second.isFunction()) {
              out = Value(Undefined{});
              return true;
            }
            Value getterOut;
            if (!callWithThis(getterIt->second, {}, originalReceiver, getterOut, thrown)) {
              return false;
            }
            out = getterOut;
            return true;
          }

          auto it = current->properties.find(key);
          if (it != current->properties.end()) {
            found = true;
            out = it->second;
            return true;
          }

          auto protoIt = current->properties.find("__proto__");
          if (protoIt == current->properties.end() || !protoIt->second.isObject()) {
            break;
          }
          current = protoIt->second.getGC<Object>();
        }
        return true;
      };

      found = false;
      out = Value(Undefined{});

      if (receiver.isObject()) {
        return resolveFromObject(receiver.getGC<Object>(), receiver);
      }
      if (receiver.isFunction()) {
        auto fn = receiver.getGC<Function>();
        auto getterIt = fn->properties.find("__get_" + key);
        if (getterIt != fn->properties.end()) {
          found = true;
          if (!getterIt->second.isFunction()) {
            out = Value(Undefined{});
            return true;
          }
          return callWithThis(getterIt->second, {}, receiver, out, thrown);
        }
        auto it = fn->properties.find(key);
        if (it != fn->properties.end()) {
          found = true;
          out = it->second;
        }
        return true;
      }
      if (receiver.isArray()) {
        auto arr = receiver.getGC<Array>();
        auto getterIt = arr->properties.find("__get_" + key);
        if (getterIt != arr->properties.end()) {
          found = true;
          if (!getterIt->second.isFunction()) {
            out = Value(Undefined{});
            return true;
          }
          return callWithThis(getterIt->second, {}, receiver, out, thrown);
        }
        auto it = arr->properties.find(key);
        if (it != arr->properties.end()) {
          found = true;
          out = it->second;
          return true;
        }
        if (key == "length") {
          found = true;
          out = Value(static_cast<double>(arr->elements.size()));
          return true;
        }
        if (auto arrayCtor = env->get("Array"); arrayCtor && arrayCtor->isObject()) {
          auto arrayObjPtr = std::get<GCPtr<Object>>(arrayCtor->data);
          auto protoIt = arrayObjPtr->properties.find("prototype");
          if (protoIt != arrayObjPtr->properties.end() && protoIt->second.isObject()) {
            return resolveFromObject(protoIt->second.getGC<Object>(), receiver);
          }
        }
        return true;
      }
      if (receiver.isClass()) {
        auto cls = receiver.getGC<Class>();
        auto getterIt = cls->properties.find("__get_" + key);
        if (getterIt != cls->properties.end()) {
          found = true;
          if (!getterIt->second.isFunction()) {
            out = Value(Undefined{});
            return true;
          }
          return callWithThis(getterIt->second, {}, receiver, out, thrown);
        }
        auto it = cls->properties.find(key);
        if (it != cls->properties.end()) {
          found = true;
          out = it->second;
        }
        return true;
      }

      return true;
    };

    if (!constructor.isFunction() && !constructor.isClass()) {
      throw std::runtime_error("TypeError: Promise.race called on non-constructor");
    }

    auto capResolve = std::make_shared<Value>(Value(Undefined{}));
    auto capReject = std::make_shared<Value>(Value(Undefined{}));

    auto executor = GarbageCollector::makeGC<Function>();
    executor->isNative = true;
    executor->isConstructor = false;
    executor->nativeFunc = [capResolve, capReject](const std::vector<Value>& executorArgs) -> Value {
      if (!capResolve->isUndefined()) {
        throw std::runtime_error("TypeError: Promise resolve function already set");
      }
      if (!capReject->isUndefined()) {
        throw std::runtime_error("TypeError: Promise reject function already set");
      }
      *capResolve = executorArgs.size() > 0 ? executorArgs[0] : Value(Undefined{});
      *capReject = executorArgs.size() > 1 ? executorArgs[1] : Value(Undefined{});
      return Value(Undefined{});
    };

    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) {
      throw std::runtime_error("TypeError: Interpreter unavailable");
    }
    interpreter->clearError();
    Value resultPromiseValue = interpreter->constructFromNative(constructor, {Value(executor)});
    if (interpreter->hasError()) {
      Value err = interpreter->getError();
      interpreter->clearError();
      if (err.isError()) {
        auto errPtr = err.getGC<Error>();
        throw std::runtime_error(errPtr->message);
      }
      throw std::runtime_error("TypeError: Failed to construct promise");
    }

    if (!capResolve->isFunction() || !capReject->isFunction()) {
      throw std::runtime_error("TypeError: Promise resolve or reject function is not callable");
    }

    auto rejectAndReturn = [&callWithThis, &resultPromiseValue, capReject](const Value& reason) -> Value {
      Value out, thrown;
      callWithThis(*capReject, {reason}, Value(Undefined{}), out, thrown);
      return resultPromiseValue;
    };

    Value promiseResolveMethod = Value(Undefined{});
    bool hasResolve = false;
    Value resolveThrown;
    if (!getPropertyWithThrow(constructor, "resolve", promiseResolveMethod, hasResolve, resolveThrown)) {
      return rejectAndReturn(resolveThrown);
    }
    if (!hasResolve || !promiseResolveMethod.isFunction()) {
      return rejectAndReturn(typeErrorValue("Promise.resolve is not callable"));
    }

    auto processElement = [&](const Value& nextValue, Value& failureReason) -> bool {
      Value nextPromise = Value(Undefined{});
      Value resolveCallThrown;
      if (!callWithThis(promiseResolveMethod, {nextValue}, constructor, nextPromise, resolveCallThrown)) {
        failureReason = resolveCallThrown;
        return false;
      }

      if (nextPromise.isPromise()) {
        auto promisePtr = nextPromise.getGC<Promise>();
        Value overriddenThen = Value(Undefined{});
        bool hasOverriddenThen = false;

        auto getterIt = promisePtr->properties.find("__get_then");
        if (getterIt != promisePtr->properties.end()) {
          hasOverriddenThen = true;
          if (getterIt->second.isFunction()) {
            Value getterOut = Value(Undefined{});
            Value getterThrown;
            if (!callWithThis(getterIt->second, {}, nextPromise, getterOut, getterThrown)) {
              failureReason = getterThrown;
              return false;
            }
            overriddenThen = getterOut;
          }
        } else if (auto thenIt = promisePtr->properties.find("then");
                   thenIt != promisePtr->properties.end()) {
          hasOverriddenThen = true;
          overriddenThen = thenIt->second;
        }

        if (hasOverriddenThen) {
          if (!overriddenThen.isFunction()) {
            failureReason = typeErrorValue("Promise resolve result is not thenable");
            return false;
          }
          Value ignored;
          Value thenInvokeThrown;
          if (!callWithThis(overriddenThen, {*capResolve, *capReject},
                            nextPromise, ignored, thenInvokeThrown)) {
            failureReason = thenInvokeThrown;
            return false;
          }
          return true;
        }

        promisePtr->then(
          [capResolve, callWithThis](Value v) -> Value {
            Value out, thrown;
            callWithThis(*capResolve, {v}, Value(Undefined{}), out, thrown);
            return v;
          },
          [capReject, callWithThis](Value reason) -> Value {
            Value out, thrown;
            callWithThis(*capReject, {reason}, Value(Undefined{}), out, thrown);
            return reason;
          });
        return true;
      }

      Value thenMethod = Value(Undefined{});
      bool hasThenMethod = false;
      Value thenLookupThrown;
      if (!getPropertyWithThrow(nextPromise, "then", thenMethod, hasThenMethod, thenLookupThrown)) {
        failureReason = thenLookupThrown;
        return false;
      }
      if (!hasThenMethod || !thenMethod.isFunction()) {
        failureReason = typeErrorValue("Promise resolve result is not thenable");
        return false;
      }

      Value ignored;
      Value thenInvokeThrown;
      if (!callWithThis(thenMethod, {*capResolve, *capReject}, nextPromise, ignored, thenInvokeThrown)) {
        failureReason = thenInvokeThrown;
        return false;
      }
      return true;
    };

    auto closeIterator = [&](const Value& iteratorValue, Value& closeFailure) -> bool {
      Value returnMethod = Value(Undefined{});
      bool hasReturn = false;
      Value returnLookupThrown;
      if (!getPropertyWithThrow(iteratorValue, "return", returnMethod, hasReturn, returnLookupThrown)) {
        closeFailure = returnLookupThrown;
        return false;
      }
      if (!hasReturn || returnMethod.isUndefined() || returnMethod.isNull()) {
        return true;
      }
      if (!returnMethod.isFunction()) {
        closeFailure = typeErrorValue("Iterator return is not callable");
        return false;
      }
      Value ignored;
      Value returnThrown;
      if (!callWithThis(returnMethod, {}, iteratorValue, ignored, returnThrown)) {
        closeFailure = returnThrown;
        return false;
      }
      return true;
    };

    const auto& iteratorKey = WellKnownSymbols::iteratorKey();
    bool useArrayFastPath = false;
    if (iterable.isArray()) {
      auto arr = iterable.getGC<Array>();
      auto getterIt = arr->properties.find("__get_" + iteratorKey);
      auto propIt = arr->properties.find(iteratorKey);
      if (getterIt == arr->properties.end() && propIt == arr->properties.end()) {
        useArrayFastPath = true;
      }
    }
    if (useArrayFastPath) {
      auto arr = iterable.getGC<Array>();
      for (const auto& value : arr->elements) {
        Value failureReason;
        if (!processElement(value, failureReason)) {
          return rejectAndReturn(failureReason);
        }
      }
      return resultPromiseValue;
    }

    if (iterable.isString()) {
      const auto& str = std::get<std::string>(iterable.data);
      for (char c : str) {
        Value failureReason;
        if (!processElement(Value(std::string(1, c)), failureReason)) {
          return rejectAndReturn(failureReason);
        }
      }
      return resultPromiseValue;
    }

    Value iteratorMethod = Value(Undefined{});
    bool hasIteratorMethod = false;
    Value iteratorLookupThrown;
    if (!getPropertyWithThrow(iterable, iteratorKey, iteratorMethod, hasIteratorMethod, iteratorLookupThrown)) {
      return rejectAndReturn(iteratorLookupThrown);
    }
    if (!hasIteratorMethod || iteratorMethod.isUndefined() || iteratorMethod.isNull()) {
      return rejectAndReturn(typeErrorValue("Value is not iterable"));
    }
    if (!iteratorMethod.isFunction()) {
      return rejectAndReturn(typeErrorValue("Symbol.iterator is not callable"));
    }

    Value iteratorValue = Value(Undefined{});
    Value iteratorThrown;
    if (!callWithThis(iteratorMethod, {}, iterable, iteratorValue, iteratorThrown)) {
      return rejectAndReturn(iteratorThrown);
    }
    if (!iteratorValue.isObject()) {
      return rejectAndReturn(typeErrorValue("Iterator must be an object"));
    }

    while (true) {
      Value nextMethod = Value(Undefined{});
      bool hasNext = false;
      Value nextLookupThrown;
      if (!getPropertyWithThrow(iteratorValue, "next", nextMethod, hasNext, nextLookupThrown)) {
        return rejectAndReturn(nextLookupThrown);
      }
      if (!hasNext || !nextMethod.isFunction()) {
        return rejectAndReturn(typeErrorValue("Iterator next is not callable"));
      }

      Value stepResult = Value(Undefined{});
      Value nextThrown;
      if (!callWithThis(nextMethod, {}, iteratorValue, stepResult, nextThrown)) {
        return rejectAndReturn(nextThrown);
      }
      if (!stepResult.isObject()) {
        return rejectAndReturn(typeErrorValue("Iterator result is not an object"));
      }

      Value doneValue = Value(false);
      bool hasDone = false;
      Value doneThrown;
      if (!getPropertyWithThrow(stepResult, "done", doneValue, hasDone, doneThrown)) {
        return rejectAndReturn(doneThrown);
      }
      if (hasDone && doneValue.toBool()) {
        break;
      }

      Value itemValue = Value(Undefined{});
      bool hasItem = false;
      Value itemThrown;
      if (!getPropertyWithThrow(stepResult, "value", itemValue, hasItem, itemThrown)) {
        return rejectAndReturn(itemThrown);
      }

      Value failureReason;
      if (!processElement(hasItem ? itemValue : Value(Undefined{}), failureReason)) {
        Value closeFailure;
        if (!closeIterator(iteratorValue, closeFailure)) {
          return rejectAndReturn(closeFailure);
        }
        return rejectAndReturn(failureReason);
      }
    }

    return resultPromiseValue;
  };
  promiseConstructor->properties["race"] = Value(promiseRace);

  // Promise constructor function
  promiseFunc->nativeFunc = [callChecked, getThenProperty, promiseFunc](const std::vector<Value>& args) -> Value {
    auto promise = GarbageCollector::makeGC<Promise>();
    GarbageCollector::instance().reportAllocation(sizeof(Promise));
    promise->properties["__constructor__"] = Value(promiseFunc);
    auto promiseProtoIt = promiseFunc->properties.find("prototype");
    if (promiseProtoIt != promiseFunc->properties.end() && promiseProtoIt->second.isObject()) {
      promise->properties["__proto__"] = promiseProtoIt->second;
    }

    if (args.empty() || !args[0].isFunction()) {
      throw std::runtime_error("TypeError: Promise resolver is not a function");
    }
    auto executor = args[0].getGC<Function>();
    auto alreadyResolved = std::make_shared<bool>(false);

    // Create resolve and reject functions
    auto resolveFunc = GarbageCollector::makeGC<Function>();
    resolveFunc->isNative = true;
    resolveFunc->properties["length"] = Value(1.0);
    resolveFunc->properties["name"] = Value(std::string(""));
    resolveFunc->properties["__non_writable_name"] = Value(true);
    resolveFunc->properties["__non_enum_name"] = Value(true);
    resolveFunc->properties["__non_writable_length"] = Value(true);
    resolveFunc->properties["__non_enum_length"] = Value(true);
    auto promisePtr = promise;
    resolveFunc->nativeFunc = [callChecked, getThenProperty, promisePtr, alreadyResolved](const std::vector<Value>& args) -> Value {
      if (*alreadyResolved) {
        return Value(Undefined{});
      }
      *alreadyResolved = true;
      auto resolveSelf = std::make_shared<std::function<void(const Value&)>>();
      *resolveSelf = [callChecked, getThenProperty, promisePtr, resolveSelf](const Value& value) {
        if (!promisePtr || promisePtr->state != PromiseState::Pending) {
          return;
        }

        if (value.isPromise() && value.getGC<Promise>().get() == promisePtr.get()) {
          promisePtr->reject(Value(GarbageCollector::makeGC<Error>(
            ErrorType::TypeError, "Cannot resolve promise with itself")));
          return;
        }

        if (isObjectLikeValue(value)) {
          try {
            auto [hasThen, thenValue] = getThenProperty(value);
            if (hasThen && thenValue.isFunction()) {
              auto alreadyCalled = std::make_shared<bool>(false);
              auto nestedResolve = GarbageCollector::makeGC<Function>();
              nestedResolve->isNative = true;
              nestedResolve->nativeFunc = [resolveSelf, alreadyCalled](const std::vector<Value>& innerArgs) -> Value {
                if (*alreadyCalled) {
                  return Value(Undefined{});
                }
                *alreadyCalled = true;
                (*resolveSelf)(innerArgs.empty() ? Value(Undefined{}) : innerArgs[0]);
                return Value(Undefined{});
              };
              auto nestedReject = GarbageCollector::makeGC<Function>();
              nestedReject->isNative = true;
              nestedReject->nativeFunc = [promisePtr, alreadyCalled](const std::vector<Value>& innerArgs) -> Value {
                if (*alreadyCalled) {
                  return Value(Undefined{});
                }
                *alreadyCalled = true;
                promisePtr->reject(innerArgs.empty() ? Value(Undefined{}) : innerArgs[0]);
                return Value(Undefined{});
              };
              EventLoopContext::instance().getLoop().queueMicrotask([callChecked, thenValue, value, nestedResolve, nestedReject]() {
                try {
                  callChecked(thenValue, {Value(nestedResolve), Value(nestedReject)}, value);
                } catch (const JsValueException& e) {
                  nestedReject->nativeFunc({e.value()});
                } catch (const std::exception& e) {
                  nestedReject->nativeFunc({Value(std::string(e.what()))});
                }
              });
              return;
            }
          } catch (const JsValueException& e) {
            promisePtr->reject(e.value());
            return;
          } catch (const std::exception& e) {
            promisePtr->reject(Value(std::string(e.what())));
            return;
          }
        }

        promisePtr->resolve(value);
      };

      (*resolveSelf)(args.empty() ? Value(Undefined{}) : args[0]);
      return Value(Undefined{});
    };

    auto rejectFunc = GarbageCollector::makeGC<Function>();
    rejectFunc->isNative = true;
    rejectFunc->properties["length"] = Value(1.0);
    rejectFunc->properties["name"] = Value(std::string(""));
    rejectFunc->properties["__non_writable_name"] = Value(true);
    rejectFunc->properties["__non_enum_name"] = Value(true);
    rejectFunc->properties["__non_writable_length"] = Value(true);
    rejectFunc->properties["__non_enum_length"] = Value(true);
    rejectFunc->nativeFunc = [promisePtr, alreadyResolved](const std::vector<Value>& args) -> Value {
      if (*alreadyResolved) {
        return Value(Undefined{});
      }
      *alreadyResolved = true;
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
      } catch (const JsValueException& e) {
        rejectFunc->nativeFunc({e.value()});
      } catch (const std::exception& e) {
        rejectFunc->nativeFunc({Value(std::string(e.what()))});
      }
    } else {
      Interpreter* interpreter = getGlobalInterpreter();
      if (interpreter) {
        interpreter->clearError();
        interpreter->callForHarness(Value(executor), {Value(resolveFunc), Value(rejectFunc)}, Value(Undefined{}));
        if (interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          rejectFunc->nativeFunc({err});
        }
      } else {
        rejectFunc->nativeFunc({Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "Interpreter unavailable"))});
      }
    }

    return Value(promise);
  };

  // Expose Promise as a callable constructor with static methods.
  for (const auto& [k, v] : promiseConstructor->properties) {
    promiseFunc->properties[k] = v;
  }
  env->define("Promise", Value(promiseFunc));
  // Keep intrinsic Promise reachable even if global Promise is overwritten.
  env->define("__intrinsic_Promise__", Value(promiseFunc));

  // JSON object
  auto jsonObj = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // JSON.parse
  auto jsonParse = GarbageCollector::makeGC<Function>();
  jsonParse->isNative = true;
  jsonParse->nativeFunc = JSON_parse;
  jsonParse->properties["name"] = Value(std::string("parse"));
  jsonParse->properties["length"] = Value(2.0);
  jsonParse->properties["__non_writable_name"] = Value(true);
  jsonParse->properties["__non_enum_name"] = Value(true);
  jsonParse->properties["__non_writable_length"] = Value(true);
  jsonParse->properties["__non_enum_length"] = Value(true);
  jsonObj->properties["parse"] = Value(jsonParse);

  // JSON.stringify
  auto jsonStringify = GarbageCollector::makeGC<Function>();
  jsonStringify->isNative = true;
  jsonStringify->nativeFunc = JSON_stringify;
  jsonStringify->properties["name"] = Value(std::string("stringify"));
  jsonStringify->properties["length"] = Value(3.0);
  jsonStringify->properties["__non_writable_name"] = Value(true);
  jsonStringify->properties["__non_enum_name"] = Value(true);
  jsonStringify->properties["__non_writable_length"] = Value(true);
  jsonStringify->properties["__non_enum_length"] = Value(true);
  jsonObj->properties["stringify"] = Value(jsonStringify);

  // JSON[Symbol.toStringTag] = "JSON"
  jsonObj->properties[WellKnownSymbols::toStringTagKey()] = Value(std::string("JSON"));
  jsonObj->properties["__non_writable_" + WellKnownSymbols::toStringTagKey()] = Value(true);
  jsonObj->properties["__non_enum_" + WellKnownSymbols::toStringTagKey()] = Value(true);

  // Make parse/stringify non-enumerable
  jsonObj->properties["__non_enum_parse"] = Value(true);
  jsonObj->properties["__non_enum_stringify"] = Value(true);

  // JSON's [[Prototype]] is Object.prototype
  if (auto objProtoVal = env->get("__object_prototype__"); objProtoVal) {
    jsonObj->properties["__proto__"] = *objProtoVal;
  }

  // Keep intrinsic JSON reachable even if global JSON is deleted/overwritten.
  env->define("__intrinsic_JSON__", Value(jsonObj));

  // Object static methods
  auto objectConstructor = GarbageCollector::makeGC<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  objectConstructor->isNative = true;
  objectConstructor->isConstructor = true;
  objectConstructor->properties["name"] = Value(std::string("Object"));
  objectConstructor->properties["length"] = Value(1.0);
  objectConstructor->nativeFunc = [env](const std::vector<Value>& args) -> Value {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
      auto obj = GarbageCollector::makeGC<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      return Value(obj);
    }

    const Value& value = args[0];
    if (!value.isBool() && !value.isNumber() && !value.isString() &&
        !value.isBigInt() && !value.isSymbol()) {
      return value;
    }

    auto wrapped = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    wrapped->properties["__primitive_value__"] = value;

    if (!value.isBigInt()) {
      auto valueOf = GarbageCollector::makeGC<Function>();
      valueOf->isNative = true;
      valueOf->properties["__uses_this_arg__"] = Value(true);
      valueOf->nativeFunc = [](const std::vector<Value>& callArgs) -> Value {
        if (!callArgs.empty() && callArgs[0].isObject()) {
          auto obj = callArgs[0].getGC<Object>();
          auto it = obj->properties.find("__primitive_value__");
          if (it != obj->properties.end()) {
            return it->second;
          }
        }
        return callArgs.empty() ? Value(Undefined{}) : callArgs[0];
      };
      wrapped->properties["valueOf"] = Value(valueOf);

      auto toString = GarbageCollector::makeGC<Function>();
      toString->isNative = true;
      toString->properties["__uses_this_arg__"] = Value(true);
      toString->nativeFunc = [](const std::vector<Value>& callArgs) -> Value {
        if (!callArgs.empty() && callArgs[0].isObject()) {
          auto obj = callArgs[0].getGC<Object>();
          auto it = obj->properties.find("__primitive_value__");
          if (it != obj->properties.end()) {
            return Value(it->second.toString());
          }
        }
        return Value(callArgs.empty() ? std::string("undefined") : callArgs[0].toString());
      };
      wrapped->properties["toString"] = Value(toString);
    }

    // Set __proto__ based on the primitive type
    std::string ctorName;
    if (value.isBigInt()) ctorName = "BigInt";
    else if (value.isSymbol()) ctorName = "Symbol";
    else if (value.isString()) ctorName = "String";
    else if (value.isNumber()) ctorName = "Number";
    else if (value.isBool()) ctorName = "Boolean";

    if (!ctorName.empty()) {
      if (auto ctor = env->get(ctorName)) {
        if (ctor->isFunction()) {
          auto ctorFn = std::get<GCPtr<Function>>(ctor->data);
          auto protoIt = ctorFn->properties.find("prototype");
          if (protoIt != ctorFn->properties.end() && protoIt->second.isObject()) {
            wrapped->properties["__proto__"] = protoIt->second;
          }
        }
      }
    }

    return Value(wrapped);
  };

  auto objectPrototype = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  objectConstructor->properties["prototype"] = Value(objectPrototype);
  objectConstructor->properties["__non_writable_prototype"] = Value(true);
  objectConstructor->properties["__non_enum_prototype"] = Value(true);
  objectConstructor->properties["__non_configurable_prototype"] = Value(true);
  objectPrototype->properties["constructor"] = Value(objectConstructor);
  objectPrototype->properties["__non_enum_constructor"] = Value(true);
  env->define("__object_prototype__", Value(objectPrototype));
  // Set JSON/Reflect [[Prototype]] to Object.prototype (defined earlier but before objectPrototype)
  if (auto jsonVal = env->get("__intrinsic_JSON__"); jsonVal && jsonVal->isObject()) {
    jsonVal->getGC<Object>()->properties["__proto__"] = Value(objectPrototype);
  }
  if (auto reflectVal = env->get("Reflect"); reflectVal && reflectVal->isObject()) {
    reflectVal->getGC<Object>()->properties["__proto__"] = Value(objectPrototype);
  }
  // Set Error.prototype.__proto__ = Object.prototype
  // SubError.prototype.__proto__ = Error.prototype
  {
    auto setErrorProtoChain = [&](const std::string& name) {
      auto val = env->get(name);
      if (!val || !val->isFunction()) return;
      auto fn = val->getGC<Function>();
      auto protoIt = fn->properties.find("prototype");
      if (protoIt == fn->properties.end() || !protoIt->second.isObject()) return;
      auto proto = protoIt->second.getGC<Object>();
      if (name == "Error") {
        proto->properties["__proto__"] = Value(objectPrototype);
      } else {
        // Sub-error prototypes chain to Error.prototype
        auto errVal = env->get("Error");
        if (errVal && errVal->isFunction()) {
          auto errFn = errVal->getGC<Function>();
          auto errProtoIt = errFn->properties.find("prototype");
          if (errProtoIt != errFn->properties.end()) {
            proto->properties["__proto__"] = errProtoIt->second;
          }
        }
      }
    };
    setErrorProtoChain("Error");
    setErrorProtoChain("TypeError");
    setErrorProtoChain("ReferenceError");
    setErrorProtoChain("RangeError");
    setErrorProtoChain("SyntaxError");
    setErrorProtoChain("URIError");
    setErrorProtoChain("EvalError");
  }
  if (auto hiddenArrayProto = env->get("__array_prototype__");
      hiddenArrayProto.has_value() && hiddenArrayProto->isObject()) {
    hiddenArrayProto->getGC<Object>()->properties["__proto__"] = Value(objectPrototype);
  }
  if (auto typedArrayCtor = env->get("TypedArray");
      typedArrayCtor.has_value() && typedArrayCtor->isFunction()) {
    auto typedArrayFn = typedArrayCtor->getGC<Function>();
    auto typedArrayPrototypeIt = typedArrayFn->properties.find("prototype");
    if (typedArrayPrototypeIt != typedArrayFn->properties.end() &&
        typedArrayPrototypeIt->second.isObject()) {
      typedArrayPrototypeIt->second.getGC<Object>()->properties["__proto__"] = Value(objectPrototype);
      for (const char* ctorName : {"Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array",
                                   "Uint16Array", "Float16Array", "Int32Array", "Uint32Array",
                                   "Float32Array", "Float64Array", "BigInt64Array", "BigUint64Array"}) {
        auto ctorValue = env->get(ctorName);
        if (!ctorValue || !ctorValue->isFunction()) {
          continue;
        }
        auto ctor = ctorValue->getGC<Function>();
        auto prototypeIt = ctor->properties.find("prototype");
        if (prototypeIt != ctor->properties.end() && prototypeIt->second.isObject()) {
          prototypeIt->second.getGC<Object>()->properties["__proto__"] =
            typedArrayPrototypeIt->second;
        }
      }
    }
  }
  if (auto sharedArrayBufferCtor = env->get("SharedArrayBuffer");
      sharedArrayBufferCtor.has_value() && sharedArrayBufferCtor->isFunction()) {
    auto prototypeIt = sharedArrayBufferCtor->getGC<Function>()->properties.find("prototype");
    if (prototypeIt != sharedArrayBufferCtor->getGC<Function>()->properties.end() &&
        prototypeIt->second.isObject()) {
      prototypeIt->second.getGC<Object>()->properties["__proto__"] = Value(objectPrototype);
    }
  }
  if (auto dataViewCtor = env->get("DataView");
      dataViewCtor.has_value() && dataViewCtor->isFunction()) {
    auto prototypeIt = dataViewCtor->getGC<Function>()->properties.find("prototype");
    if (prototypeIt != dataViewCtor->getGC<Function>()->properties.end() &&
        prototypeIt->second.isObject()) {
      prototypeIt->second.getGC<Object>()->properties["__proto__"] = Value(objectPrototype);
    }
  }
  if (auto promiseCtor = env->get("Promise");
      promiseCtor.has_value() && promiseCtor->isFunction()) {
    auto prototypeIt = promiseCtor->getGC<Function>()->properties.find("prototype");
    if (prototypeIt != promiseCtor->getGC<Function>()->properties.end() &&
        prototypeIt->second.isObject()) {
      prototypeIt->second.getGC<Object>()->properties["__proto__"] = Value(objectPrototype);
    }
  }

  // Set __proto__ of Map/Set/WeakMap/WeakSet prototypes to Object.prototype
  for (const char* ctorName : {"Map", "Set", "WeakMap", "WeakSet", "WeakRef", "FinalizationRegistry", "Symbol"}) {
    auto ctorVal = env->get(ctorName);
    if (ctorVal && ctorVal->isFunction()) {
      auto protoIt = ctorVal->getGC<Function>()->properties.find("prototype");
      if (protoIt != ctorVal->getGC<Function>()->properties.end() && protoIt->second.isObject()) {
        protoIt->second.getGC<Object>()->properties["__proto__"] = Value(objectPrototype);
      }
    }
  }

  // Link Number.prototype.__proto__ to Object.prototype
  if (auto numberCtor = env->get("Number");
      numberCtor.has_value() && numberCtor->isFunction()) {
    auto prototypeIt = numberCtor->getGC<Function>()->properties.find("prototype");
    if (prototypeIt != numberCtor->getGC<Function>()->properties.end() &&
        prototypeIt->second.isObject()) {
      prototypeIt->second.getGC<Object>()->properties["__proto__"] = Value(objectPrototype);
    }
  }
  // Link Boolean.prototype.__proto__ to Object.prototype
  if (auto booleanCtor = env->get("Boolean");
      booleanCtor.has_value() && booleanCtor->isFunction()) {
    auto prototypeIt = booleanCtor->getGC<Function>()->properties.find("prototype");
    if (prototypeIt != booleanCtor->getGC<Function>()->properties.end() &&
        prototypeIt->second.isObject()) {
      prototypeIt->second.getGC<Object>()->properties["__proto__"] = Value(objectPrototype);
    }
  }

  auto objectProtoHasOwnProperty = GarbageCollector::makeGC<Function>();
  objectProtoHasOwnProperty->isNative = true;
  objectProtoHasOwnProperty->properties["__uses_this_arg__"] = Value(true);
  objectProtoHasOwnProperty->nativeFunc = Object_hasOwnProperty;
  objectPrototype->properties["hasOwnProperty"] = Value(objectProtoHasOwnProperty);
  objectPrototype->properties["__non_enum_hasOwnProperty"] = Value(true);

  auto objectProtoIsPrototypeOf = GarbageCollector::makeGC<Function>();
  objectProtoIsPrototypeOf->isNative = true;
  objectProtoIsPrototypeOf->properties["__uses_this_arg__"] = Value(true);
  objectProtoIsPrototypeOf->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto isObjectLikeValue = [](const Value& value) -> bool {
      return value.isObject() || value.isArray() || value.isFunction() || value.isRegex() ||
             value.isPromise() || value.isGenerator() || value.isClass() ||
             value.isMap() || value.isSet() || value.isWeakMap() ||
             value.isWeakSet() || value.isTypedArray() || value.isArrayBuffer() ||
             value.isDataView() || value.isError() || value.isProxy();
    };
    auto sameReference = [](const Value& a, const Value& b) -> bool {
      if (a.data.index() != b.data.index()) return false;
      if (a.isObject()) return a.getGC<Object>().get() == b.getGC<Object>().get();
      if (a.isArray()) return a.getGC<Array>().get() == b.getGC<Array>().get();
      if (a.isFunction()) return a.getGC<Function>().get() == b.getGC<Function>().get();
      if (a.isRegex()) return a.getGC<Regex>().get() == b.getGC<Regex>().get();
      if (a.isProxy()) return a.getGC<Proxy>().get() == b.getGC<Proxy>().get();
      if (a.isPromise()) return a.getGC<Promise>().get() == b.getGC<Promise>().get();
      if (a.isGenerator()) return a.getGC<Generator>().get() == b.getGC<Generator>().get();
      if (a.isClass()) return a.getGC<Class>().get() == b.getGC<Class>().get();
      if (a.isMap()) return a.getGC<Map>().get() == b.getGC<Map>().get();
      if (a.isSet()) return a.getGC<Set>().get() == b.getGC<Set>().get();
      if (a.isWeakMap()) return a.getGC<WeakMap>().get() == b.getGC<WeakMap>().get();
      if (a.isWeakSet()) return a.getGC<WeakSet>().get() == b.getGC<WeakSet>().get();
      if (a.isTypedArray()) return a.getGC<TypedArray>().get() == b.getGC<TypedArray>().get();
      if (a.isArrayBuffer()) return a.getGC<ArrayBuffer>().get() == b.getGC<ArrayBuffer>().get();
      if (a.isDataView()) return a.getGC<DataView>().get() == b.getGC<DataView>().get();
      if (a.isError()) return a.getGC<Error>().get() == b.getGC<Error>().get();
      return false;
    };

    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
      throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
    }
    if (args.size() < 2) {
      return Value(false);
    }
    Value protoVal = args[0];
    if (!isObjectLikeValue(protoVal)) {
      return Value(false);
    }

    Value v = args[1];
    if (v.isUndefined() || v.isNull()) {
      throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
    }

    auto nextProtoValue = [&](const Value& cur) -> Value {
      if (cur.isObject()) {
        auto o = cur.getGC<Object>();
        auto it = o->properties.find("__proto__");
        return it != o->properties.end() ? it->second : Value(Undefined{});
      }
      if (cur.isFunction()) {
        auto f = cur.getGC<Function>();
        auto it = f->properties.find("__proto__");
        return it != f->properties.end() ? it->second : Value(Undefined{});
      }
      if (cur.isArray()) {
        auto a = cur.getGC<Array>();
        auto it = a->properties.find("__proto__");
        return it != a->properties.end() ? it->second : Value(Undefined{});
      }
      if (cur.isClass()) {
        auto c = cur.getGC<Class>();
        auto it = c->properties.find("__proto__");
        return it != c->properties.end() ? it->second : Value(Undefined{});
      }
      if (cur.isError()) {
        auto e = cur.getGC<Error>();
        auto it = e->properties.find("__proto__");
        return it != e->properties.end() ? it->second : Value(Undefined{});
      }
      if (cur.isPromise()) {
        auto p = cur.getGC<Promise>();
        auto it = p->properties.find("__proto__");
        return it != p->properties.end() ? it->second : Value(Undefined{});
      }
      if (cur.isMap()) {
        auto m = cur.getGC<Map>();
        auto it = m->properties.find("__proto__");
        return it != m->properties.end() ? it->second : Value(Undefined{});
      }
      if (cur.isSet()) {
        auto s = cur.getGC<Set>();
        auto it = s->properties.find("__proto__");
        return it != s->properties.end() ? it->second : Value(Undefined{});
      }
      if (cur.isRegex()) {
        auto r = cur.getGC<Regex>();
        auto it = r->properties.find("__proto__");
        return it != r->properties.end() ? it->second : Value(Undefined{});
      }
      if (cur.isTypedArray()) {
        auto t = cur.getGC<TypedArray>();
        auto it = t->properties.find("__proto__");
        return it != t->properties.end() ? it->second : Value(Undefined{});
      }
      if (cur.isGenerator()) {
        auto g = cur.getGC<Generator>();
        auto it = g->properties.find("__proto__");
        return it != g->properties.end() ? it->second : Value(Undefined{});
      }
      return Value(Undefined{});
    };

    Value cur = v;
    int depth = 0;
    while (depth < 100) {
      Value p = nextProtoValue(cur);
      if (p.isUndefined() || p.isNull()) {
        return Value(false);
      }
      if (sameReference(p, protoVal)) {
        return Value(true);
      }
      cur = p;
      depth++;
    }
    return Value(false);
  };
  objectPrototype->properties["isPrototypeOf"] = Value(objectProtoIsPrototypeOf);
  objectPrototype->properties["__non_enum_isPrototypeOf"] = Value(true);

  auto objectProtoValueOf = GarbageCollector::makeGC<Function>();
  objectProtoValueOf->isNative = true;
  objectProtoValueOf->properties["__uses_this_arg__"] = Value(true);
  objectProtoValueOf->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
      throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
    }
    return args[0];
  };
  objectPrototype->properties["valueOf"] = Value(objectProtoValueOf);
  objectPrototype->properties["__non_enum_valueOf"] = Value(true);

  auto objectProtoToString = GarbageCollector::makeGC<Function>();
  objectProtoToString->isNative = true;
  objectProtoToString->properties["__uses_this_arg__"] = Value(true);
  objectProtoToString->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || args[0].isUndefined()) {
      return Value(std::string("[object Undefined]"));
    }
    if (args[0].isNull()) {
      return Value(std::string("[object Null]"));
    }

    std::string tag = "Object";
    if (args[0].isObject()) {
      auto obj = args[0].getGC<Object>();
      auto toStringTagIt = obj->properties.find(WellKnownSymbols::toStringTagKey());
      if (toStringTagIt != obj->properties.end()) {
        tag = toStringTagIt->second.toString();
      } else {
        // Check for primitive wrapper objects
        auto primIt = obj->properties.find("__primitive_value__");
        if (primIt != obj->properties.end()) {
          if (primIt->second.isString()) {
            tag = "String";
          } else if (primIt->second.isNumber()) {
            tag = "Number";
          } else if (primIt->second.isBool()) {
            tag = "Boolean";
          }
        }
      }
    } else if (args[0].isArray()) {
      tag = "Array";
    } else if (args[0].isFunction()) {
      tag = "Function";
    } else if (args[0].isTypedArray()) {
      auto ta = args[0].getGC<TypedArray>();
      switch (ta->type) {
        case TypedArrayType::Int8: tag = "Int8Array"; break;
        case TypedArrayType::Uint8: tag = "Uint8Array"; break;
        case TypedArrayType::Uint8Clamped: tag = "Uint8ClampedArray"; break;
        case TypedArrayType::Int16: tag = "Int16Array"; break;
        case TypedArrayType::Uint16: tag = "Uint16Array"; break;
        case TypedArrayType::Int32: tag = "Int32Array"; break;
        case TypedArrayType::Uint32: tag = "Uint32Array"; break;
        case TypedArrayType::Float16: tag = "Float16Array"; break;
        case TypedArrayType::Float32: tag = "Float32Array"; break;
        case TypedArrayType::Float64: tag = "Float64Array"; break;
        case TypedArrayType::BigInt64: tag = "BigInt64Array"; break;
        case TypedArrayType::BigUint64: tag = "BigUint64Array"; break;
      }
    } else if (args[0].isArrayBuffer()) {
      tag = "ArrayBuffer";
    } else if (args[0].isDataView()) {
      tag = "DataView";
    } else if (args[0].isRegex()) {
      tag = "RegExp";
    } else if (args[0].isError()) {
      tag = "Error";
    }
    return Value(std::string("[object ") + tag + "]");
  };
  objectPrototype->properties["toString"] = Value(objectProtoToString);
  objectPrototype->properties["__non_enum_toString"] = Value(true);

  // Object.prototype.toLocaleString - calls this.toString()
  auto objectProtoToLocaleString = GarbageCollector::makeGC<Function>();
  objectProtoToLocaleString->isNative = true;
  objectProtoToLocaleString->isConstructor = false;
  objectProtoToLocaleString->properties["name"] = Value(std::string("toLocaleString"));
  objectProtoToLocaleString->properties["length"] = Value(0.0);
  objectProtoToLocaleString->properties["__non_writable_name"] = Value(true);
  objectProtoToLocaleString->properties["__non_enum_name"] = Value(true);
  objectProtoToLocaleString->properties["__non_writable_length"] = Value(true);
  objectProtoToLocaleString->properties["__non_enum_length"] = Value(true);
  objectProtoToLocaleString->properties["__uses_this_arg__"] = Value(true);
  objectProtoToLocaleString->nativeFunc = [objectProtoToString](const std::vector<Value>& args) -> Value {
    // toLocaleString calls toString
    return objectProtoToString->nativeFunc(args);
  };
  objectPrototype->properties["toLocaleString"] = Value(objectProtoToLocaleString);
  objectPrototype->properties["__non_enum_toLocaleString"] = Value(true);

  // Object.prototype.propertyIsEnumerable
  auto objectProtoPropertyIsEnumerable = GarbageCollector::makeGC<Function>();
  objectProtoPropertyIsEnumerable->isNative = true;
  objectProtoPropertyIsEnumerable->properties["__uses_this_arg__"] = Value(true);
  objectProtoPropertyIsEnumerable->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(false);
    Value thisVal = args[0];
    std::string key = valueToPropertyKey(args[1]);

    if (thisVal.isRegex()) {
      auto rx = thisVal.getGC<Regex>();
      if (key == "source" || key == "flags") return Value(true);
      if (rx->properties.find(key) == rx->properties.end() &&
          rx->properties.find("__get_" + key) == rx->properties.end()) {
        return Value(false);
      }
      auto neIt = rx->properties.find("__non_enum_" + key);
      return Value(neIt == rx->properties.end());
    }

    if (thisVal.isError()) {
      auto e = thisVal.getGC<Error>();
      if (e->properties.find(key) == e->properties.end() &&
          e->properties.find("__get_" + key) == e->properties.end()) {
        return Value(false);
      }
      auto neIt = e->properties.find("__non_enum_" + key);
      return Value(neIt == e->properties.end());
    }

    if (thisVal.isTypedArray()) {
      auto ta = thisVal.getGC<TypedArray>();
      if (key == "length" || key == "byteLength" || key == "buffer" || key == "byteOffset") return Value(false);
      if (!key.empty() && std::all_of(key.begin(), key.end(), ::isdigit)) {
        try {
          size_t idx = std::stoull(key);
          if (idx < ta->currentLength()) return Value(true);
        } catch (...) {
        }
      }
      if (ta->properties.find(key) == ta->properties.end() &&
          ta->properties.find("__get_" + key) == ta->properties.end()) {
        return Value(false);
      }
      auto neIt = ta->properties.find("__non_enum_" + key);
      return Value(neIt == ta->properties.end());
    }

    if (thisVal.isDataView()) {
      auto dv = thisVal.getGC<DataView>();
      if (dv->properties.find(key) == dv->properties.end() &&
          dv->properties.find("__get_" + key) == dv->properties.end()) {
        return Value(false);
      }
      auto neIt = dv->properties.find("__non_enum_" + key);
      return Value(neIt == dv->properties.end());
    }

    if (thisVal.isFunction()) {
      auto fn = thisVal.getGC<Function>();
      // name, length, prototype are non-enumerable on functions
      if (key == "name" || key == "length" || key == "prototype") return Value(false);
      // Internal properties
      if (key.size() >= 4 && key.substr(0, 2) == "__" &&
          key.substr(key.size() - 2) == "__") return Value(false);
      auto it = fn->properties.find(key);
      if (it == fn->properties.end()) return Value(false);
      // Check enum marker: built-in function props default non-enumerable
      auto enumIt = fn->properties.find("__enum_" + key);
      return Value(enumIt != fn->properties.end());
    }
    if (thisVal.isClass()) {
      auto cls = thisVal.getGC<Class>();
      // Internal properties
      if (key.size() >= 4 && key.substr(0, 2) == "__" &&
          key.substr(key.size() - 2) == "__") return Value(false);
      if (key.size() > 6 && (key.substr(0, 6) == "__get_" || key.substr(0, 6) == "__set_")) return Value(false);
      if (key.size() > 10 && key.substr(0, 10) == "__non_enum") return Value(false);
      if (key.size() > 14 && key.substr(0, 14) == "__non_writable") return Value(false);
      if (key.size() > 18 && key.substr(0, 18) == "__non_configurable") return Value(false);
      auto it = cls->properties.find(key);
      if (it == cls->properties.end()) return Value(false);
      auto enumIt = cls->properties.find("__enum_" + key);
      return Value(enumIt != cls->properties.end());
    }
    if (thisVal.isObject()) {
      auto obj = thisVal.getGC<Object>();
      if (obj->isModuleNamespace) {
        if (key == WellKnownSymbols::toStringTagKey()) {
          return Value(false);
        }
        bool isExport = isModuleNamespaceExportKey(obj, key);
        if (!isExport) {
          return Value(false);
        }
        auto getterIt = obj->properties.find("__get_" + key);
        if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
          auto getter = getterIt->second.getGC<Function>();
          if (getter && getter->isNative) {
            getter->nativeFunc({});
          }
        }
        return Value(true);
      }
      // Internal properties
      if (key.size() >= 4 && key.substr(0, 2) == "__" &&
          key.substr(key.size() - 2) == "__") return Value(false);
      auto it = obj->properties.find(key);
      if (it == obj->properties.end()) return Value(false);
      auto neIt = obj->properties.find("__non_enum_" + key);
      return Value(neIt == obj->properties.end());
    }
    if (thisVal.isArray()) {
      auto arr = thisVal.getGC<Array>();
      auto isCanonicalArrayIndex = [&](const std::string& s, size_t& outIdx) -> bool {
        if (s.empty()) return false;
        if (s.size() > 1 && s[0] == '0') return false;
        for (unsigned char c : s) {
          if (std::isdigit(c) == 0) return false;
        }
        try {
          outIdx = static_cast<size_t>(std::stoull(s));
        } catch (...) {
          return false;
        }
        return true;
      };
      size_t idx = 0;
      if (isCanonicalArrayIndex(key, idx)) {
        bool exists = idx < arr->elements.size() ||
                      arr->properties.find(key) != arr->properties.end() ||
                      arr->properties.find("__get_" + key) != arr->properties.end() ||
                      arr->properties.find("__set_" + key) != arr->properties.end();
        if (!exists) return Value(false);
        auto neIt = arr->properties.find("__non_enum_" + key);
        return Value(neIt == arr->properties.end());
      }
      auto it = arr->properties.find(key);
      if (it == arr->properties.end()) return Value(false);
      auto neIt = arr->properties.find("__non_enum_" + key);
      return Value(neIt == arr->properties.end());
    }
    return Value(false);
  };
  objectPrototype->properties["propertyIsEnumerable"] = Value(objectProtoPropertyIsEnumerable);
  objectPrototype->properties["__non_enum_propertyIsEnumerable"] = Value(true);

  // Annex B: Object.prototype.__lookupGetter__
  auto objectProtoLookupGetter = GarbageCollector::makeGC<Function>();
  objectProtoLookupGetter->isNative = true;
  objectProtoLookupGetter->properties["__uses_this_arg__"] = Value(true);
  objectProtoLookupGetter->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(Undefined{});
    Value current = args[0];
    std::string key = valueToPropertyKey(args[1]);

    auto getProps = [](const Value& v) -> OrderedMap<std::string, Value>* {
      if (v.isObject()) return &v.getGC<Object>()->properties;
      if (v.isArray()) return &v.getGC<Array>()->properties;
      if (v.isFunction()) return &v.getGC<Function>()->properties;
      if (v.isClass()) return &v.getGC<Class>()->properties;
      if (v.isPromise()) return &v.getGC<Promise>()->properties;
      if (v.isRegex()) return &v.getGC<Regex>()->properties;
      return nullptr;
    };

    for (int depth = 0; depth < 64; ++depth) {
      auto* props = getProps(current);
      if (!props) break;

      auto getterIt = props->find("__get_" + key);
      if (getterIt != props->end() && getterIt->second.isFunction()) {
        return getterIt->second;
      }
      if (props->find(key) != props->end()) {
        return Value(Undefined{});
      }

      auto protoIt = props->find("__proto__");
      if (protoIt == props->end() || protoIt->second.isNull()) break;
      current = protoIt->second;
    }
    return Value(Undefined{});
  };
  objectPrototype->properties["__lookupGetter__"] = Value(objectProtoLookupGetter);
  objectPrototype->properties["__non_enum___lookupGetter__"] = Value(true);

  // Annex B: Object.prototype.__lookupSetter__
  auto objectProtoLookupSetter = GarbageCollector::makeGC<Function>();
  objectProtoLookupSetter->isNative = true;
  objectProtoLookupSetter->properties["__uses_this_arg__"] = Value(true);
  objectProtoLookupSetter->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(Undefined{});
    Value current = args[0];
    std::string key = valueToPropertyKey(args[1]);

    auto getProps = [](const Value& v) -> OrderedMap<std::string, Value>* {
      if (v.isObject()) return &v.getGC<Object>()->properties;
      if (v.isArray()) return &v.getGC<Array>()->properties;
      if (v.isFunction()) return &v.getGC<Function>()->properties;
      if (v.isClass()) return &v.getGC<Class>()->properties;
      if (v.isPromise()) return &v.getGC<Promise>()->properties;
      if (v.isRegex()) return &v.getGC<Regex>()->properties;
      return nullptr;
    };

    for (int depth = 0; depth < 64; ++depth) {
      auto* props = getProps(current);
      if (!props) break;

      auto setterIt = props->find("__set_" + key);
      if (setterIt != props->end() && setterIt->second.isFunction()) {
        return setterIt->second;
      }
      if (props->find(key) != props->end()) {
        return Value(Undefined{});
      }

      auto protoIt = props->find("__proto__");
      if (protoIt == props->end() || protoIt->second.isNull()) break;
      current = protoIt->second;
    }
    return Value(Undefined{});
  };
  objectPrototype->properties["__lookupSetter__"] = Value(objectProtoLookupSetter);
  objectPrototype->properties["__non_enum___lookupSetter__"] = Value(true);

  // Object.keys
  auto objectKeys = GarbageCollector::makeGC<Function>();
  objectKeys->isNative = true;
  objectKeys->nativeFunc = Object_keys;
  objectKeys->properties["length"] = Value(1.0);
  objectKeys->properties["__non_writable_length"] = Value(true);
  objectKeys->properties["__non_enum_length"] = Value(true);
  objectKeys->properties["name"] = Value(std::string("keys"));
  objectKeys->properties["__non_writable_name"] = Value(true);
  objectKeys->properties["__non_enum_name"] = Value(true);
  objectConstructor->properties["keys"] = Value(objectKeys);

  // Object.values
  auto objectValues = GarbageCollector::makeGC<Function>();
  objectValues->isNative = true;
  objectValues->nativeFunc = Object_values;
  objectValues->properties["length"] = Value(1.0);
  objectValues->properties["__non_writable_length"] = Value(true);
  objectValues->properties["__non_enum_length"] = Value(true);
  objectValues->properties["name"] = Value(std::string("values"));
  objectValues->properties["__non_writable_name"] = Value(true);
  objectValues->properties["__non_enum_name"] = Value(true);
  objectConstructor->properties["values"] = Value(objectValues);

  // Object.entries
  auto objectEntries = GarbageCollector::makeGC<Function>();
  objectEntries->isNative = true;
  objectEntries->nativeFunc = Object_entries;
  objectEntries->properties["length"] = Value(1.0);
  objectEntries->properties["__non_writable_length"] = Value(true);
  objectEntries->properties["__non_enum_length"] = Value(true);
  objectEntries->properties["name"] = Value(std::string("entries"));
  objectEntries->properties["__non_writable_name"] = Value(true);
  objectEntries->properties["__non_enum_name"] = Value(true);
  objectConstructor->properties["entries"] = Value(objectEntries);

  // Object.assign
  auto objectAssign = GarbageCollector::makeGC<Function>();
  objectAssign->isNative = true;
  objectAssign->nativeFunc = Object_assign;
  objectAssign->properties["length"] = Value(2.0);
  objectAssign->properties["__non_writable_length"] = Value(true);
  objectAssign->properties["__non_enum_length"] = Value(true);
  objectAssign->properties["name"] = Value(std::string("assign"));
  objectAssign->properties["__non_writable_name"] = Value(true);
  objectAssign->properties["__non_enum_name"] = Value(true);
  objectConstructor->properties["assign"] = Value(objectAssign);

  // Object.hasOwnProperty (for prototypal access)
  auto objectHasOwnProperty = GarbageCollector::makeGC<Function>();
  objectHasOwnProperty->isNative = true;
  objectHasOwnProperty->nativeFunc = Object_hasOwnProperty;
  objectConstructor->properties["hasOwnProperty"] = Value(objectHasOwnProperty);

  // Object.getOwnPropertyNames
  auto objectGetOwnPropertyNames = GarbageCollector::makeGC<Function>();
  objectGetOwnPropertyNames->isNative = true;
  objectGetOwnPropertyNames->nativeFunc = Object_getOwnPropertyNames;
  objectGetOwnPropertyNames->properties["length"] = Value(1.0);
  objectGetOwnPropertyNames->properties["__non_writable_length"] = Value(true);
  objectGetOwnPropertyNames->properties["__non_enum_length"] = Value(true);
  objectGetOwnPropertyNames->properties["name"] = Value(std::string("getOwnPropertyNames"));
  objectGetOwnPropertyNames->properties["__non_writable_name"] = Value(true);
  objectGetOwnPropertyNames->properties["__non_enum_name"] = Value(true);
  objectConstructor->properties["getOwnPropertyNames"] = Value(objectGetOwnPropertyNames);

  auto objectGetOwnPropertySymbols = GarbageCollector::makeGC<Function>();
  objectGetOwnPropertySymbols->isNative = true;
  objectGetOwnPropertySymbols->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto result = makeArrayWithPrototype();
    if (args.empty()) {
      return Value(result);
    }
    Value target = args[0];
    OrderedMap<std::string, Value>* props = nullptr;
    GCPtr<Object> obj;
    if (target.isObject()) {
      obj = target.getGC<Object>();
      props = &obj->properties;
    } else if (target.isFunction()) {
      props = &target.getGC<Function>()->properties;
    } else if (target.isClass()) {
      props = &target.getGC<Class>()->properties;
    } else if (target.isArray()) {
      props = &target.getGC<Array>()->properties;
    } else if (target.isRegex()) {
      props = &target.getGC<Regex>()->properties;
    } else if (target.isPromise()) {
      props = &target.getGC<Promise>()->properties;
    } else if (target.isError()) {
      props = &target.getGC<Error>()->properties;
    }
    if (!props) {
      return Value(result);
    }
    auto appendSymbolForKey = [&](const std::string& key) {
      if (key == WellKnownSymbols::iteratorKey()) {
        result->elements.push_back(WellKnownSymbols::iterator());
      } else if (key == WellKnownSymbols::asyncIteratorKey()) {
        result->elements.push_back(WellKnownSymbols::asyncIterator());
      } else if (key == WellKnownSymbols::toStringTagKey()) {
        result->elements.push_back(WellKnownSymbols::toStringTag());
      } else if (key == WellKnownSymbols::toPrimitiveKey()) {
        result->elements.push_back(WellKnownSymbols::toPrimitive());
      } else if (key == WellKnownSymbols::matchAllKey()) {
        result->elements.push_back(WellKnownSymbols::matchAll());
      } else {
        Symbol symbolValue;
        if (propertyKeyToSymbol(key, symbolValue)) {
          result->elements.push_back(Value(symbolValue));
        }
      }
    };

    if (obj && obj->isModuleNamespace) {
      appendSymbolForKey(WellKnownSymbols::toStringTagKey());
      return Value(result);
    }

    for (const auto& key : props->orderedKeys()) {
      if (!isSymbolPropertyKey(key)) continue;
      appendSymbolForKey(key);
    }
    return Value(result);
  };
  objectConstructor->properties["getOwnPropertySymbols"] = Value(objectGetOwnPropertySymbols);

  // Object.create - deferred until after defineProperty is defined
  // (see below, after objectDefineProperty)

  // Object.fromEntries - converts array of [key, value] pairs to object
  auto objectFromEntries = GarbageCollector::makeGC<Function>();
  objectFromEntries->isNative = true;
  objectFromEntries->nativeFunc = Object_fromEntries;
  objectConstructor->properties["fromEntries"] = Value(objectFromEntries);

  // Object.hasOwn - checks if object has own property
  auto objectHasOwn = GarbageCollector::makeGC<Function>();
  objectHasOwn->isNative = true;
  objectHasOwn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || args[0].isNull() || args[0].isUndefined()) {
      throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
    }
    std::string key = args.size() > 1 ? valueToPropertyKey(args[1]) : "undefined";
    if (args[0].isObject()) {
      auto obj = args[0].getGC<Object>();
      if (obj->isModuleNamespace) {
        if (key == WellKnownSymbols::toStringTagKey()) {
          return Value(true);
        }
        if (!isModuleNamespaceExportKey(obj, key)) {
          return Value(false);
        }
        auto getterIt = obj->properties.find("__get_" + key);
        if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
          auto getter = getterIt->second.getGC<Function>();
          if (getter && getter->isNative) {
            getter->nativeFunc({});
          }
        }
        return Value(true);
      }
      if (obj->properties.find(key) != obj->properties.end()) return Value(true);
      if (obj->properties.find("__get_" + key) != obj->properties.end()) return Value(true);
      if (obj->properties.find("__set_" + key) != obj->properties.end()) return Value(true);
      return Value(false);
    }
    if (args[0].isArray()) {
      auto arr = args[0].getGC<Array>();
      // Check numeric index
      bool isNum = !key.empty() && std::all_of(key.begin(), key.end(), ::isdigit);
      if (isNum) {
        try {
          size_t idx = std::stoul(key);
          if (idx < arr->elements.size()) return Value(true);
        } catch (...) {}
      }
      // Check named properties (including symbol keys)
      if (arr->properties.find(key) != arr->properties.end()) return Value(true);
      if (arr->properties.find("__get_" + key) != arr->properties.end()) return Value(true);
      if (arr->properties.find("__set_" + key) != arr->properties.end()) return Value(true);
      return Value(false);
    }
    if (args[0].isFunction()) {
      auto fn = args[0].getGC<Function>();
      if (fn->properties.find(key) != fn->properties.end()) return Value(true);
      if (fn->properties.find("__get_" + key) != fn->properties.end()) return Value(true);
      if (fn->properties.find("__set_" + key) != fn->properties.end()) return Value(true);
      return Value(false);
    }
    if (args[0].isClass()) {
      auto cls = args[0].getGC<Class>();
      if (cls->properties.find(key) != cls->properties.end()) return Value(true);
      return Value(false);
    }
    if (args[0].isTypedArray()) {
      auto ta = args[0].getGC<TypedArray>();
      bool isNum = !key.empty() && std::all_of(key.begin(), key.end(), ::isdigit);
      if (isNum) {
        try {
          size_t idx = std::stoul(key);
          if (idx < ta->currentLength()) return Value(true);
        } catch (...) {}
      }
      if (ta->properties.find(key) != ta->properties.end()) return Value(true);
      if (ta->properties.find("__get_" + key) != ta->properties.end()) return Value(true);
      if (ta->properties.find("__set_" + key) != ta->properties.end()) return Value(true);
      return Value(false);
    }
    return Value(false);
  };
  objectConstructor->properties["hasOwn"] = Value(objectHasOwn);

  auto objectGetPrototypeOf = GarbageCollector::makeGC<Function>();
  objectGetPrototypeOf->isNative = true;
  objectGetPrototypeOf->nativeFunc = [promisePrototype, env](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(Null{});
    }
    if (args[0].isPromise()) {
      auto p = args[0].getGC<Promise>();
      if (auto it = p->properties.find("__proto__"); it != p->properties.end()) {
        return it->second;
      }
      return Value(promisePrototype);
    }
    if (args[0].isTypedArray()) {
      auto ta = args[0].getGC<TypedArray>();
      if (auto it = ta->properties.find("__proto__"); it != ta->properties.end()) {
        return it->second;
      }
      return Value(Null{});
    }
    if (args[0].isArrayBuffer()) {
      auto b = args[0].getGC<ArrayBuffer>();
      if (auto it = b->properties.find("__proto__"); it != b->properties.end()) {
        return it->second;
      }
      return Value(Null{});
    }
    if (args[0].isDataView()) {
      auto v = args[0].getGC<DataView>();
      if (auto it = v->properties.find("__proto__"); it != v->properties.end()) {
        return it->second;
      }
      return Value(Null{});
    }
    if (args[0].isMap()) {
      auto m = args[0].getGC<Map>();
      if (auto it = m->properties.find("__proto__"); it != m->properties.end()) {
        return it->second;
      }
      return Value(Null{});
    }
    if (args[0].isSet()) {
      auto s = args[0].getGC<Set>();
      if (auto it = s->properties.find("__proto__"); it != s->properties.end()) {
        return it->second;
      }
      return Value(Null{});
    }
    if (args[0].isWeakMap()) {
      auto wm = args[0].getGC<WeakMap>();
      if (auto it = wm->properties.find("__proto__"); it != wm->properties.end()) {
        return it->second;
      }
      return Value(Null{});
    }
    if (args[0].isWeakSet()) {
      auto ws = args[0].getGC<WeakSet>();
      if (auto it = ws->properties.find("__proto__"); it != ws->properties.end()) {
        return it->second;
      }
      return Value(Null{});
    }
    if (args[0].isRegex()) {
      auto rx = args[0].getGC<Regex>();
      if (auto it = rx->properties.find("__proto__"); it != rx->properties.end()) {
        return it->second;
      }
      return Value(Null{});
    }
    if (args[0].isArray()) {
      auto arr = args[0].getGC<Array>();
      // Arguments objects have __proto__ set explicitly to Object.prototype
      auto protoOverride = arr->properties.find("__proto__");
      if (protoOverride != arr->properties.end()) {
        return protoOverride->second;
      }
      if (auto arrayCtor = env->get("Array"); arrayCtor && arrayCtor->isObject()) {
        auto arrayObj = std::get<GCPtr<Object>>(arrayCtor->data);
        auto protoIt = arrayObj->properties.find("prototype");
        if (protoIt != arrayObj->properties.end()) {
          return protoIt->second;
        }
      }
      if (auto hiddenProto = env->get("__array_prototype__"); hiddenProto.has_value()) {
        return *hiddenProto;
      }
      return Value(Null{});
    }
    if (args[0].isError()) {
      auto err = args[0].getGC<Error>();
      if (auto it = err->properties.find("__proto__"); it != err->properties.end()) {
        return it->second;
      }
      std::string ctorName = "Error";
      switch (err->type) {
        case ErrorType::TypeError:
          ctorName = "TypeError";
          break;
        case ErrorType::ReferenceError:
          ctorName = "ReferenceError";
          break;
        case ErrorType::RangeError:
          ctorName = "RangeError";
          break;
        case ErrorType::SyntaxError:
          ctorName = "SyntaxError";
          break;
        case ErrorType::URIError:
          ctorName = "URIError";
          break;
        case ErrorType::EvalError:
          ctorName = "EvalError";
          break;
        case ErrorType::Error:
        default:
          ctorName = "Error";
          break;
      }
      if (auto ctor = env->get(ctorName); ctor && ctor->isFunction()) {
        auto fn = std::get<GCPtr<Function>>(ctor->data);
        auto protoIt = fn->properties.find("prototype");
        if (protoIt != fn->properties.end()) {
          return protoIt->second;
        }
      }
      return Value(Null{});
    }
    if (args[0].isFunction()) {
      auto fn = args[0].getGC<Function>();
      auto protoIt = fn->properties.find("__proto__");
      if (protoIt != fn->properties.end()) {
        return protoIt->second;
      }
      // Default: Function.prototype
      if (auto funcCtor = env->get("Function"); funcCtor && funcCtor->isFunction()) {
        auto funcFn = std::get<GCPtr<Function>>(funcCtor->data);
        auto fpIt = funcFn->properties.find("prototype");
        if (fpIt != funcFn->properties.end()) {
          return fpIt->second;
        }
      }
      return Value(Null{});
    }
    if (args[0].isBigInt()) {
      // Return BigInt.prototype
      if (auto bigIntCtor = env->get("BigInt"); bigIntCtor && bigIntCtor->isFunction()) {
        auto fn = std::get<GCPtr<Function>>(bigIntCtor->data);
        auto protoIt = fn->properties.find("prototype");
        if (protoIt != fn->properties.end()) {
          return protoIt->second;
        }
      }
      return Value(Null{});
    }
    if (args[0].isGenerator()) {
      auto gen = args[0].getGC<Generator>();
      auto it = gen->properties.find("__proto__");
      if (it != gen->properties.end()) {
        return it->second;
      }
      return Value(Null{});
    }
    if (args[0].isClass()) {
      auto cls = args[0].getGC<Class>();
      auto it = cls->properties.find("__proto__");
      if (it != cls->properties.end()) {
        return it->second;
      }
      // Default: Function.prototype
      if (auto funcCtor = env->get("Function"); funcCtor && funcCtor->isFunction()) {
        auto funcFn = std::get<GCPtr<Function>>(funcCtor->data);
        auto fpIt = funcFn->properties.find("prototype");
        if (fpIt != funcFn->properties.end()) {
          return fpIt->second;
        }
      }
      return Value(Null{});
    }
    if (args[0].isArray()) {
      auto arr = args[0].getGC<Array>();
      auto it = arr->properties.find("__proto__");
      if (it != arr->properties.end()) {
        return it->second;
      }
      return Value(Null{});
    }
    // Primitive types: return their constructor's prototype
    if (args[0].isSymbol() || args[0].isString() || args[0].isNumber() || args[0].isBool()) {
      std::string ctorName;
      if (args[0].isSymbol()) ctorName = "Symbol";
      else if (args[0].isString()) ctorName = "String";
      else if (args[0].isNumber()) ctorName = "Number";
      else ctorName = "Boolean";
      if (auto ctor = env->get(ctorName); ctor && ctor->isFunction()) {
        auto ctorFn = std::get<GCPtr<Function>>(ctor->data);
        auto protoIt = ctorFn->properties.find("prototype");
        if (protoIt != ctorFn->properties.end()) {
          return protoIt->second;
        }
      }
      return Value(Null{});
    }
    if (!args[0].isObject()) {
      return Value(Null{});
    }
    auto obj = args[0].getGC<Object>();
    if (obj->isModuleNamespace) {
      return Value(Null{});
    }
    auto it = obj->properties.find("__proto__");
    if (it != obj->properties.end()) {
      return it->second;
    }
    return Value(Null{});
  };
  objectConstructor->properties["getPrototypeOf"] = Value(objectGetPrototypeOf);

  auto objectSetPrototypeOf = GarbageCollector::makeGC<Function>();
  objectSetPrototypeOf->isNative = true;
  objectSetPrototypeOf->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) {
      throw std::runtime_error("TypeError: Object.setPrototypeOf requires two arguments");
    }
    // RequireObjectCoercible(O)
    if (args[0].isNull() || args[0].isUndefined()) {
      throw std::runtime_error("TypeError: Object.setPrototypeOf called on null or undefined");
    }
    // proto must be Object or null
    if (!args[1].isNull() && !args[1].isObject() && !args[1].isFunction() && !args[1].isClass() &&
        !args[1].isArray() && !args[1].isError() && !args[1].isRegex() && !args[1].isMap() &&
        !args[1].isSet() && !args[1].isPromise() && !args[1].isProxy()) {
      throw std::runtime_error("TypeError: Object prototype may only be an Object or null");
    }
    // If O is not Object, return O
    if (!args[0].isObject() && !args[0].isClass() && !args[0].isArray() && !args[0].isFunction() &&
        !args[0].isError() && !args[0].isRegex() && !args[0].isMap() && !args[0].isSet()) {
      return args[0];
    }
    if (args[0].isClass()) {
      auto cls = args[0].getGC<Class>();
      // Check extensibility
      if (cls->properties.count("__non_extensible__")) {
        throw std::runtime_error("TypeError: #<Object> is not extensible");
      }
      cls->properties["__proto__"] = args[1];
      return args[0];
    }
    if (args[0].isObject()) {
      auto obj = args[0].getGC<Object>();
      if (obj->isModuleNamespace) {
        if (args[1].isNull()) return args[0];
        throw std::runtime_error("TypeError: Cannot set prototype of module namespace object");
      }
      // Check extensibility
      if (obj->nonExtensible || obj->properties.count("__non_extensible__")) {
        throw std::runtime_error("TypeError: #<Object> is not extensible");
      }
      // Cycle detection
      if (args[1].isObject()) {
        auto check = args[1].getGC<Object>();
        int depth = 0;
        while (check && depth < 100) {
          if (check.get() == obj.get()) {
            throw std::runtime_error("TypeError: Cyclic __proto__ value");
          }
          auto it = check->properties.find("__proto__");
          if (it != check->properties.end() && it->second.isObject()) {
            check = it->second.getGC<Object>();
            depth++;
          } else {
            break;
          }
        }
      }
      obj->properties["__proto__"] = args[1];
      return args[0];
    }
    // Array, Function, etc — set __proto__ via properties
    if (args[0].isArray()) {
      args[0].getGC<Array>()->properties["__proto__"] = args[1];
    } else if (args[0].isFunction()) {
      args[0].getGC<Function>()->properties["__proto__"] = args[1];
    }
    return args[0];
  };
  objectConstructor->properties["setPrototypeOf"] = Value(objectSetPrototypeOf);

  auto objectIsExtensible = GarbageCollector::makeGC<Function>();
  objectIsExtensible->isNative = true;
  objectIsExtensible->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(false);
    if (args[0].isClass()) {
      auto cls = args[0].getGC<Class>();
      auto it = cls->properties.find("__non_extensible__");
      bool nonExtensible = it != cls->properties.end() &&
                           it->second.isBool() &&
                           it->second.toBool();
      return Value(!nonExtensible);
    }
    if (args[0].isFunction() || args[0].isArray()) {
      return Value(true);  // Functions and arrays are always extensible
    }
    if (args[0].isTypedArray()) {
      auto ta = args[0].getGC<TypedArray>();
      return Value(ta->properties.find("__non_extensible__") == ta->properties.end());
    }
    if (args[0].isArrayBuffer()) {
      auto ab = args[0].getGC<ArrayBuffer>();
      return Value(ab->properties.find("__non_extensible__") == ab->properties.end());
    }
    if (args[0].isDataView()) {
      auto dv = args[0].getGC<DataView>();
      return Value(dv->properties.find("__non_extensible__") == dv->properties.end());
    }
    if (!args[0].isObject()) {
      return Value(false);
    }
    auto obj = args[0].getGC<Object>();
    if (obj->isModuleNamespace) {
      return Value(false);
    }
    return Value(!obj->sealed && !obj->frozen && !obj->nonExtensible);
  };
  objectConstructor->properties["isExtensible"] = Value(objectIsExtensible);

  auto objectPreventExtensions = GarbageCollector::makeGC<Function>();
  objectPreventExtensions->isNative = true;
  objectPreventExtensions->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return args.empty() ? Value(Undefined{}) : args[0];
    }
    if (args[0].isClass()) {
      auto cls = args[0].getGC<Class>();
      cls->properties["__non_extensible__"] = Value(true);
      return args[0];
    }
    if (args[0].isTypedArray()) {
      args[0].getGC<TypedArray>()->properties["__non_extensible__"] = Value(true);
      return args[0];
    }
    if (args[0].isArrayBuffer()) {
      args[0].getGC<ArrayBuffer>()->properties["__non_extensible__"] = Value(true);
      return args[0];
    }
    if (args[0].isDataView()) {
      args[0].getGC<DataView>()->properties["__non_extensible__"] = Value(true);
      return args[0];
    }
    if (!args[0].isObject()) {
      return args[0];
    }
    auto obj = args[0].getGC<Object>();
    if (!obj->isModuleNamespace) {
      obj->nonExtensible = true;
    }
    return args[0];
  };
  objectConstructor->properties["preventExtensions"] = Value(objectPreventExtensions);

  // Object.freeze - makes an object immutable
  auto objectFreeze = GarbageCollector::makeGC<Function>();
  objectFreeze->isNative = true;
  objectFreeze->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isObject()) {
      if (!args.empty() && args[0].isTypedArray()) {
        args[0].getGC<TypedArray>()->properties["__frozen__"] = Value(true);
        args[0].getGC<TypedArray>()->properties["__sealed__"] = Value(true);
      } else if (!args.empty() && args[0].isArrayBuffer()) {
        args[0].getGC<ArrayBuffer>()->properties["__frozen__"] = Value(true);
        args[0].getGC<ArrayBuffer>()->properties["__sealed__"] = Value(true);
      } else if (!args.empty() && args[0].isDataView()) {
        args[0].getGC<DataView>()->properties["__frozen__"] = Value(true);
        args[0].getGC<DataView>()->properties["__sealed__"] = Value(true);
      }
      return args.empty() ? Value(Undefined{}) : args[0];
    }
    auto obj = args[0].getGC<Object>();
    if (obj->isModuleNamespace) {
      throw std::runtime_error("TypeError: Cannot freeze module namespace object");
    }
    obj->frozen = true;
    obj->sealed = true;  // Frozen objects are also sealed
    return args[0];
  };
  objectConstructor->properties["freeze"] = Value(objectFreeze);

  // Object.seal - prevents adding or removing properties
  auto objectSeal = GarbageCollector::makeGC<Function>();
  objectSeal->isNative = true;
  objectSeal->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isObject()) {
      if (!args.empty() && args[0].isTypedArray()) {
        args[0].getGC<TypedArray>()->properties["__sealed__"] = Value(true);
      } else if (!args.empty() && args[0].isArrayBuffer()) {
        args[0].getGC<ArrayBuffer>()->properties["__sealed__"] = Value(true);
      } else if (!args.empty() && args[0].isDataView()) {
        args[0].getGC<DataView>()->properties["__sealed__"] = Value(true);
      }
      return args.empty() ? Value(Undefined{}) : args[0];
    }
    auto obj = args[0].getGC<Object>();
    obj->sealed = true;
    return args[0];
  };
  objectConstructor->properties["seal"] = Value(objectSeal);

  // Helper: check if all own properties in a properties bag are frozen/sealed
  auto checkBagFrozen = [](const auto& props) -> bool {
    for (const auto& [k, v] : props) {
      if (k.rfind("__non_writable_", 0) == 0 || k.rfind("__non_enum_", 0) == 0 ||
          k.rfind("__non_configurable_", 0) == 0 || k.rfind("__enum_", 0) == 0 ||
          k.rfind("__get_", 0) == 0 || k.rfind("__set_", 0) == 0) continue;
      if (k.size() >= 4 && k.substr(0, 2) == "__" && k.substr(k.size() - 2) == "__") continue;
      // Every own property must be non-configurable
      if (props.find("__non_configurable_" + k) == props.end()) return false;
      // Data properties must also be non-writable (accessor properties don't have writable)
      bool isAccessor = props.find("__get_" + k) != props.end() || props.find("__set_" + k) != props.end();
      if (!isAccessor && props.find("__non_writable_" + k) == props.end()) return false;
    }
    return true;
  };
  auto checkBagSealed = [](const auto& props) -> bool {
    for (const auto& [k, v] : props) {
      if (k.rfind("__non_writable_", 0) == 0 || k.rfind("__non_enum_", 0) == 0 ||
          k.rfind("__non_configurable_", 0) == 0 || k.rfind("__enum_", 0) == 0 ||
          k.rfind("__get_", 0) == 0 || k.rfind("__set_", 0) == 0) continue;
      if (k.size() >= 4 && k.substr(0, 2) == "__" && k.substr(k.size() - 2) == "__") continue;
      // Every own property must be non-configurable
      if (props.find("__non_configurable_" + k) == props.end()) return false;
    }
    return true;
  };
  auto isObjectLikeForFrozenCheck = [](const Value& v) -> bool {
    return v.isObject() || v.isFunction() || v.isArray() || v.isError() || v.isRegex() ||
           v.isClass() || v.isPromise() || v.isMap() || v.isSet() ||
           v.isWeakMap() || v.isWeakSet() || v.isGenerator() || v.isProxy() ||
           v.isTypedArray() || v.isArrayBuffer() || v.isDataView();
  };

  // Object.isFrozen - check if object is frozen
  auto objectIsFrozen = GarbageCollector::makeGC<Function>();
  objectIsFrozen->isNative = true;
  objectIsFrozen->nativeFunc = [isObjectLikeForFrozenCheck, checkBagFrozen](const std::vector<Value>& args) -> Value {
    if (args.empty() || !isObjectLikeForFrozenCheck(args[0])) {
      return Value(true);  // Non-objects are considered frozen
    }
    // Check extensibility
    bool isNonExtensible = false;
    if (args[0].isObject()) {
      auto obj = args[0].getGC<Object>();
      isNonExtensible = obj->frozen || obj->sealed || obj->nonExtensible ||
                        obj->properties.count("__non_extensible__");
      if (!isNonExtensible) return Value(false);
      if (obj->frozen) return Value(true);
      return Value(checkBagFrozen(obj->properties));
    }
    if (args[0].isFunction()) {
      auto fn = args[0].getGC<Function>();
      isNonExtensible = fn->properties.count("__non_extensible__");
      if (!isNonExtensible) return Value(false);
      return Value(checkBagFrozen(fn->properties));
    }
    if (args[0].isArray()) {
      auto arr = args[0].getGC<Array>();
      isNonExtensible = arr->properties.count("__non_extensible__");
      if (!isNonExtensible) return Value(false);
      return Value(checkBagFrozen(arr->properties));
    }
    // For other types, check __non_extensible__ + __frozen__ markers
    auto checkOrderedBag = [](const auto& props) -> bool {
      if (props.find("__non_extensible__") == props.end()) return false;
      if (props.find("__frozen__") != props.end()) return true;
      for (const auto& [k, v] : props) {
        if (k.rfind("__non_writable_", 0) == 0 || k.rfind("__non_enum_", 0) == 0 ||
            k.rfind("__non_configurable_", 0) == 0 || k.rfind("__enum_", 0) == 0 ||
            k.rfind("__get_", 0) == 0 || k.rfind("__set_", 0) == 0) continue;
        if (k.size() >= 4 && k.substr(0, 2) == "__" && k.substr(k.size() - 2) == "__") continue;
        if (props.find("__non_configurable_" + k) == props.end()) return false;
        bool isAccessor = props.find("__get_" + k) != props.end() || props.find("__set_" + k) != props.end();
        if (!isAccessor && props.find("__non_writable_" + k) == props.end()) return false;
      }
      return true;
    };
    if (args[0].isError()) return Value(checkOrderedBag(args[0].getGC<Error>()->properties));
    if (args[0].isRegex()) return Value(checkOrderedBag(args[0].getGC<Regex>()->properties));
    if (args[0].isClass()) return Value(checkOrderedBag(args[0].getGC<Class>()->properties));
    if (args[0].isPromise()) return Value(checkOrderedBag(args[0].getGC<Promise>()->properties));
    if (args[0].isMap()) return Value(checkOrderedBag(args[0].getGC<Map>()->properties));
    if (args[0].isSet()) return Value(checkOrderedBag(args[0].getGC<Set>()->properties));
    if (args[0].isTypedArray()) return Value(checkOrderedBag(args[0].getGC<TypedArray>()->properties));
    if (args[0].isArrayBuffer()) return Value(checkOrderedBag(args[0].getGC<ArrayBuffer>()->properties));
    if (args[0].isDataView()) return Value(checkOrderedBag(args[0].getGC<DataView>()->properties));
    return Value(true);
  };
  objectConstructor->properties["isFrozen"] = Value(objectIsFrozen);

  // Object.isSealed - check if object is sealed
  auto objectIsSealed = GarbageCollector::makeGC<Function>();
  objectIsSealed->isNative = true;
  objectIsSealed->nativeFunc = [isObjectLikeForFrozenCheck, checkBagSealed](const std::vector<Value>& args) -> Value {
    if (args.empty() || !isObjectLikeForFrozenCheck(args[0])) {
      return Value(true);  // Non-objects are considered sealed
    }
    // Check extensibility
    if (args[0].isObject()) {
      auto obj = args[0].getGC<Object>();
      bool isNonExtensible = obj->frozen || obj->sealed || obj->nonExtensible ||
                             obj->properties.count("__non_extensible__");
      if (!isNonExtensible) return Value(false);
      if (obj->sealed || obj->frozen) return Value(true);
      return Value(checkBagSealed(obj->properties));
    }
    if (args[0].isFunction()) {
      auto fn = args[0].getGC<Function>();
      if (!fn->properties.count("__non_extensible__")) return Value(false);
      return Value(checkBagSealed(fn->properties));
    }
    if (args[0].isArray()) {
      auto arr = args[0].getGC<Array>();
      if (!arr->properties.count("__non_extensible__")) return Value(false);
      return Value(checkBagSealed(arr->properties));
    }
    auto checkOrderedBag = [](const auto& props) -> bool {
      if (props.find("__non_extensible__") == props.end()) return false;
      if (props.find("__sealed__") != props.end()) return true;
      for (const auto& [k, v] : props) {
        if (k.rfind("__non_writable_", 0) == 0 || k.rfind("__non_enum_", 0) == 0 ||
            k.rfind("__non_configurable_", 0) == 0 || k.rfind("__enum_", 0) == 0 ||
            k.rfind("__get_", 0) == 0 || k.rfind("__set_", 0) == 0) continue;
        if (k.size() >= 4 && k.substr(0, 2) == "__" && k.substr(k.size() - 2) == "__") continue;
        if (props.find("__non_configurable_" + k) == props.end()) return false;
      }
      return true;
    };
    if (args[0].isError()) return Value(checkOrderedBag(args[0].getGC<Error>()->properties));
    if (args[0].isRegex()) return Value(checkOrderedBag(args[0].getGC<Regex>()->properties));
    if (args[0].isClass()) return Value(checkOrderedBag(args[0].getGC<Class>()->properties));
    if (args[0].isPromise()) return Value(checkOrderedBag(args[0].getGC<Promise>()->properties));
    if (args[0].isMap()) return Value(checkOrderedBag(args[0].getGC<Map>()->properties));
    if (args[0].isSet()) return Value(checkOrderedBag(args[0].getGC<Set>()->properties));
    if (args[0].isTypedArray()) return Value(checkOrderedBag(args[0].getGC<TypedArray>()->properties));
    if (args[0].isArrayBuffer()) return Value(checkOrderedBag(args[0].getGC<ArrayBuffer>()->properties));
    if (args[0].isDataView()) return Value(checkOrderedBag(args[0].getGC<DataView>()->properties));
    return Value(true);
  };
  objectConstructor->properties["isSealed"] = Value(objectIsSealed);

  // Object.is - SameValue comparison (handles NaN and -0 correctly)
  auto objectIs = GarbageCollector::makeGC<Function>();
  objectIs->isNative = true;
  objectIs->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // Missing args are treated as undefined
    Value a = args.size() > 0 ? args[0] : Value(Undefined{});
    Value b = args.size() > 1 ? args[1] : Value(Undefined{});

    // Check for same type
    if (a.data.index() != b.data.index()) return Value(false);

    // Handle numbers specially (NaN === NaN, +0 !== -0)
    if (a.isNumber() && b.isNumber()) {
      double x = a.toNumber();
      double y = b.toNumber();
      if (std::isnan(x) && std::isnan(y)) return Value(true);
      if (x == 0 && y == 0) {
        return Value(std::signbit(x) == std::signbit(y));
      }
      return Value(x == y);
    }

    if (a.isUndefined() && b.isUndefined()) return Value(true);
    if (a.isNull() && b.isNull()) return Value(true);
    if (a.isBool() && b.isBool()) {
      return Value(std::get<bool>(a.data) == std::get<bool>(b.data));
    }
    if (a.isString() && b.isString()) {
      return Value(std::get<std::string>(a.data) == std::get<std::string>(b.data));
    }
    if (a.isBigInt() && b.isBigInt()) {
      return Value(a.toBigInt() == b.toBigInt());
    }
    if (a.isSymbol() && b.isSymbol()) {
      return Value(std::get<Symbol>(a.data) == std::get<Symbol>(b.data));
    }
    // For objects, check reference equality
    if (a.isObject() && b.isObject()) {
      return Value(a.getGC<Object>().get() ==
                   b.getGC<Object>().get());
    }
    if (a.isArray() && b.isArray()) {
      return Value(a.getGC<Array>().get() ==
                   b.getGC<Array>().get());
    }
    if (a.isFunction() && b.isFunction()) {
      return Value(a.getGC<Function>().get() ==
                   b.getGC<Function>().get());
    }
    if (a.isClass() && b.isClass()) {
      return Value(a.getGC<Class>().get() ==
                   b.getGC<Class>().get());
    }
    if (a.isPromise() && b.isPromise()) {
      return Value(a.getGC<Promise>().get() ==
                   b.getGC<Promise>().get());
    }
    if (a.isRegex() && b.isRegex()) {
      return Value(a.getGC<Regex>().get() ==
                   b.getGC<Regex>().get());
    }

    return Value(false);
  };
  objectConstructor->properties["is"] = Value(objectIs);

  // Object.getOwnPropertyDescriptor - get property descriptor
  auto objectGetOwnPropertyDescriptor = GarbageCollector::makeGC<Function>();
  objectGetOwnPropertyDescriptor->isNative = true;
  objectGetOwnPropertyDescriptor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 1) {
      throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
    }
    // Step 1: ToObject - throw TypeError for undefined/null
    if (args[0].isUndefined() || args[0].isNull()) {
      throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
    }
    // Step 2: ToPropertyKey - call toString/valueOf on object keys
    std::string key;
    if (args.size() >= 2) {
      key = valueToPropertyKey(args[1]);
    } else {
      key = "undefined";
    }
    // Handle string primitives: box to object with indexed char properties
    if (args[0].isString()) {
      const std::string& str = std::get<std::string>(args[0].data);
      auto descriptor = GarbageCollector::makeGC<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      // Check for numeric index
      if (!key.empty() && key[0] >= '0' && key[0] <= '9') {
        bool allDigits = true;
        for (char c : key) { if (!std::isdigit(static_cast<unsigned char>(c))) { allDigits = false; break; } }
        if (allDigits) {
          size_t idx = 0;
          try { idx = std::stoull(key); } catch (...) { return Value(Undefined{}); }
          // Get the character at the code point index
          size_t cpIdx = 0;
          size_t bytePos = 0;
          while (bytePos < str.size() && cpIdx < idx) {
            unsigned char ch = static_cast<unsigned char>(str[bytePos]);
            if (ch < 0x80) bytePos += 1;
            else if ((ch & 0xE0) == 0xC0) bytePos += 2;
            else if ((ch & 0xF0) == 0xE0) bytePos += 3;
            else bytePos += 4;
            cpIdx++;
          }
          if (cpIdx == idx && bytePos < str.size()) {
            unsigned char ch = static_cast<unsigned char>(str[bytePos]);
            size_t charLen = 1;
            if (ch >= 0x80) {
              if ((ch & 0xE0) == 0xC0) charLen = 2;
              else if ((ch & 0xF0) == 0xE0) charLen = 3;
              else charLen = 4;
            }
            descriptor->properties["value"] = Value(str.substr(bytePos, charLen));
            descriptor->properties["writable"] = Value(false);
            descriptor->properties["enumerable"] = Value(true);
            descriptor->properties["configurable"] = Value(false);
            return Value(descriptor);
          }
          return Value(Undefined{});
        }
      }
      if (key == "length") {
        // Count code points
        size_t cpCount = 0;
        size_t bytePos = 0;
        while (bytePos < str.size()) {
          unsigned char ch = static_cast<unsigned char>(str[bytePos]);
          if (ch < 0x80) bytePos += 1;
          else if ((ch & 0xE0) == 0xC0) bytePos += 2;
          else if ((ch & 0xF0) == 0xE0) bytePos += 3;
          else bytePos += 4;
          cpCount++;
        }
        descriptor->properties["value"] = Value(static_cast<double>(cpCount));
        descriptor->properties["writable"] = Value(false);
        descriptor->properties["enumerable"] = Value(false);
        descriptor->properties["configurable"] = Value(false);
        return Value(descriptor);
      }
      return Value(Undefined{});
    }
    // Non-object primitives (number, boolean, symbol, bigint): return undefined
    if (!args[0].isObject() && !args[0].isFunction() && !args[0].isPromise() && !args[0].isRegex() && !args[0].isClass() &&
         !args[0].isArray() && !args[0].isError() && !args[0].isTypedArray() &&
         !args[0].isArrayBuffer() && !args[0].isDataView() && !args[0].isMap() && !args[0].isSet() &&
         !args[0].isWeakMap() && !args[0].isWeakSet() && !args[0].isGenerator()) {
      return Value(Undefined{});
    }

    auto descriptor = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));

    if (args[0].isClass()) {
      auto cls = args[0].getGC<Class>();
      // Internal properties are not visible.
      if (isInternalPropertyKeyForReflection(key)) {
        return Value(Undefined{});
      }
      auto getterIt = cls->properties.find("__get_" + key);
      if (getterIt != cls->properties.end() && getterIt->second.isFunction()) {
        descriptor->properties["get"] = getterIt->second;
      }
      auto setterIt = cls->properties.find("__set_" + key);
      if (setterIt != cls->properties.end() && setterIt->second.isFunction()) {
        descriptor->properties["set"] = setterIt->second;
      }

      auto it = cls->properties.find(key);
      if (it == cls->properties.end() && getterIt == cls->properties.end() && setterIt == cls->properties.end()) {
        return Value(Undefined{});
      }
      if (it != cls->properties.end()) {
        descriptor->properties["value"] = it->second;
      }
      // Check for per-property attribute markers
      if (key == "name" || key == "length") {
        // Default: non-writable, non-enumerable, configurable
        bool writable = cls->properties.find("__non_writable_" + key) == cls->properties.end();
        descriptor->properties["writable"] = Value(writable);
        descriptor->properties["enumerable"] = Value(false);
        descriptor->properties["configurable"] = Value(true);
      } else if (key == "prototype") {
        bool writable = cls->properties.find("__non_writable_prototype") == cls->properties.end();
        bool configurable = cls->properties.find("__non_configurable_prototype") == cls->properties.end();
        descriptor->properties["writable"] = Value(writable);
        descriptor->properties["enumerable"] = Value(false);
        descriptor->properties["configurable"] = Value(configurable);
      } else {
        bool writable = cls->properties.find("__non_writable_" + key) == cls->properties.end();
        bool enumerable = cls->properties.find("__enum_" + key) != cls->properties.end();
        bool configurable = cls->properties.find("__non_configurable_" + key) == cls->properties.end();
        descriptor->properties["writable"] = Value(writable);
        descriptor->properties["enumerable"] = Value(enumerable);
        descriptor->properties["configurable"] = Value(configurable);
      }
      return Value(descriptor);
    }

    if (args[0].isFunction()) {
      auto fn = args[0].getGC<Function>();

      // Internal properties are not visible.
      if (isInternalPropertyKeyForReflection(key)) {
        return Value(Undefined{});
      }

      auto getterIt = fn->properties.find("__get_" + key);
      auto setterIt = fn->properties.find("__set_" + key);
      bool hasAccessor = (getterIt != fn->properties.end()) || (setterIt != fn->properties.end());

      auto it = fn->properties.find(key);
      if (it == fn->properties.end() && !hasAccessor) {
        return Value(Undefined{});
      }
      if (hasAccessor) {
        if (getterIt != fn->properties.end()) {
          descriptor->properties["get"] = getterIt->second;
        } else {
          descriptor->properties["get"] = Value(Undefined{});
        }
        if (setterIt != fn->properties.end()) {
          descriptor->properties["set"] = setterIt->second;
        } else {
          descriptor->properties["set"] = Value(Undefined{});
        }
      } else if (it != fn->properties.end()) {
        descriptor->properties["value"] = it->second;
      }

      // name and length: non-writable, non-enumerable, configurable
      if (key == "name" || key == "length") {
        descriptor->properties["writable"] = Value(false);
        descriptor->properties["enumerable"] = Value(false);
        descriptor->properties["configurable"] = Value(true);
      } else if (key == "prototype") {
        bool protoWritable = fn->properties.find("__non_writable_prototype") == fn->properties.end();
        bool protoConfigurable = fn->properties.find("__non_configurable_prototype") == fn->properties.end();
        descriptor->properties["enumerable"] = Value(false);
        descriptor->properties["configurable"] = Value(protoConfigurable);
        if (!hasAccessor) {
          descriptor->properties["writable"] = Value(protoWritable);
        }
      } else {
        // Check for per-property attribute markers
        bool enumerable = false; // Built-in function properties default to non-enumerable
        bool configurable = true;
        auto neIt = fn->properties.find("__enum_" + key);
        if (neIt != fn->properties.end()) enumerable = true;
        auto ncIt = fn->properties.find("__non_configurable_" + key);
        if (ncIt != fn->properties.end()) configurable = false;
        descriptor->properties["enumerable"] = Value(enumerable);
        descriptor->properties["configurable"] = Value(configurable);
        if (!hasAccessor) {
          bool writable = true;
          auto nwIt = fn->properties.find("__non_writable_" + key);
          if (nwIt != fn->properties.end()) writable = false;
          descriptor->properties["writable"] = Value(writable);
        }
      }
      return Value(descriptor);
    }

    if (args[0].isArray()) {
      auto arr = args[0].getGC<Array>();
      // Internal properties are not visible.
      if (isInternalPropertyKeyForReflection(key)) {
        return Value(Undefined{});
      }
      // `length` is a special own data property with fixed attributes.
      if (key == "length") {
        auto overriddenIt = arr->properties.find("__overridden_length__");
        if (overriddenIt != arr->properties.end()) {
          descriptor->properties["value"] = overriddenIt->second;
        } else {
          descriptor->properties["value"] = Value(static_cast<double>(arr->elements.size()));
        }
        bool writable = arr->properties.find("__non_writable_length") == arr->properties.end();
        descriptor->properties["writable"] = Value(writable);
        descriptor->properties["enumerable"] = Value(false);
        // Arguments objects have configurable length; regular arrays do not.
        bool isArgumentsObject = false;
        auto isArgsIt = arr->properties.find("__is_arguments_object__");
        if (isArgsIt != arr->properties.end() && isArgsIt->second.isBool() && isArgsIt->second.toBool()) {
          isArgumentsObject = true;
        }
        bool configurable = isArgumentsObject &&
          arr->properties.find("__non_configurable_length") == arr->properties.end();
        descriptor->properties["configurable"] = Value(configurable);
        return Value(descriptor);
      }
      // Array index properties: consult per-index attribute markers and accessors.
      auto isCanonicalArrayIndex = [&](const std::string& s, size_t& outIdx) -> bool {
        if (s.empty()) return false;
        if (s.size() > 1 && s[0] == '0') return false;  // no leading zeros
        for (unsigned char c : s) {
          if (std::isdigit(c) == 0) return false;
        }
        try {
          outIdx = static_cast<size_t>(std::stoull(s));
        } catch (...) {
          return false;
        }
        // Ignore the 2^32-1 sentinel; keep behavior simple for our tests.
        return true;
      };
      size_t idx = 0;
      if (isCanonicalArrayIndex(key, idx)) {
        bool isArgumentsObject = false;
        auto isArgsIt = arr->properties.find("__is_arguments_object__");
        if (isArgsIt != arr->properties.end() && isArgsIt->second.isBool() && isArgsIt->second.toBool()) {
          isArgumentsObject = true;
        }
        bool isMappedArgumentsIndex = false;
        if (isArgumentsObject) {
          isMappedArgumentsIndex =
            arr->properties.find("__mapped_arg_index_" + key + "__") != arr->properties.end();
        }
        auto getterIt = arr->properties.find("__get_" + key);
        auto setterIt = arr->properties.find("__set_" + key);
        bool hasAccessor = getterIt != arr->properties.end() || setterIt != arr->properties.end();
        bool isDeleted = arr->properties.find("__deleted_" + key + "__") != arr->properties.end();
        bool hasData = (idx < arr->elements.size() && !isDeleted) || arr->properties.find(key) != arr->properties.end();
        if (!hasAccessor && !hasData) {
          return Value(Undefined{});
        }

        if (isMappedArgumentsIndex && getterIt != arr->properties.end() && getterIt->second.isFunction()) {
          // Mapped arguments exotic objects expose data descriptors for indices.
          // The value is the current parameter binding (served by our internal getter).
          auto getterFn = getterIt->second.getGC<Function>();
          Value v = getterFn->nativeFunc({});
          descriptor->properties["value"] = v;
          bool writable = arr->properties.find("__non_writable_" + key) == arr->properties.end();
          descriptor->properties["writable"] = Value(writable);
        } else if (hasAccessor) {
          if (getterIt != arr->properties.end()) {
            descriptor->properties["get"] = getterIt->second;
          } else {
            descriptor->properties["get"] = Value(Undefined{});
          }
          if (setterIt != arr->properties.end()) {
            descriptor->properties["set"] = setterIt->second;
          } else {
            descriptor->properties["set"] = Value(Undefined{});
          }
        } else {
          if (idx < arr->elements.size()) {
            descriptor->properties["value"] = arr->elements[idx];
          } else {
            descriptor->properties["value"] = arr->properties.at(key);
          }
          bool writable = arr->properties.find("__non_writable_" + key) == arr->properties.end();
          descriptor->properties["writable"] = Value(writable);
        }

        bool enumerable = arr->properties.find("__non_enum_" + key) == arr->properties.end();
        bool configurable = arr->properties.find("__non_configurable_" + key) == arr->properties.end();
        descriptor->properties["enumerable"] = Value(enumerable);
        descriptor->properties["configurable"] = Value(configurable);
        return Value(descriptor);
      }
      // Regular named properties (including accessors like strict arguments callee)
      auto getterIt = arr->properties.find("__get_" + key);
      auto setterIt = arr->properties.find("__set_" + key);
      auto it = arr->properties.find(key);
      bool hasAccessor = getterIt != arr->properties.end() || setterIt != arr->properties.end();
      if (it == arr->properties.end() && !hasAccessor) {
        return Value(Undefined{});
      }
      if (hasAccessor) {
        // Accessor descriptor (e.g., strict mode callee, or data-to-accessor conversion)
        if (getterIt != arr->properties.end()) {
          descriptor->properties["get"] = getterIt->second;
        } else {
          descriptor->properties["get"] = Value(Undefined{});
        }
        if (setterIt != arr->properties.end()) {
          descriptor->properties["set"] = setterIt->second;
        } else {
          descriptor->properties["set"] = Value(Undefined{});
        }
      } else if (it != arr->properties.end()) {
        // Data descriptor
        descriptor->properties["value"] = it->second;
        bool writable = arr->properties.find("__non_writable_" + key) == arr->properties.end();
        descriptor->properties["writable"] = Value(writable);
      }
      bool enumerable = arr->properties.find("__non_enum_" + key) == arr->properties.end();
      bool configurable = arr->properties.find("__non_configurable_" + key) == arr->properties.end();
      descriptor->properties["enumerable"] = Value(enumerable);
      descriptor->properties["configurable"] = Value(configurable);
      return Value(descriptor);
    }

    if (args[0].isError()) {
      auto err = args[0].getGC<Error>();
      // Internal properties are not visible
      if (key.size() >= 4 && key.substr(0, 2) == "__" &&
          key.substr(key.size() - 2) == "__") {
        return Value(Undefined{});
      }
      auto getterIt = err->properties.find("__get_" + key);
      auto setterIt = err->properties.find("__set_" + key);
      auto it = err->properties.find(key);
      if (it == err->properties.end() && getterIt == err->properties.end() && setterIt == err->properties.end()) {
        return Value(Undefined{});
      }
      if (getterIt != err->properties.end() && getterIt->second.isFunction()) {
        descriptor->properties["get"] = getterIt->second;
      }
      if (setterIt != err->properties.end() && setterIt->second.isFunction()) {
        descriptor->properties["set"] = setterIt->second;
      }
      if (it != err->properties.end()) {
        descriptor->properties["value"] = it->second;
      }
      bool writable = err->properties.find("__non_writable_" + key) == err->properties.end();
      bool enumerable = err->properties.find("__non_enum_" + key) == err->properties.end();
      bool configurable = err->properties.find("__non_configurable_" + key) == err->properties.end();
      descriptor->properties["writable"] = Value(writable);
      descriptor->properties["enumerable"] = Value(enumerable);
      descriptor->properties["configurable"] = Value(configurable);
      return Value(descriptor);
    }

    if (args[0].isPromise()) {
      auto promise = args[0].getGC<Promise>();
      auto getterIt = promise->properties.find("__get_" + key);
      if (getterIt != promise->properties.end() && getterIt->second.isFunction()) {
        descriptor->properties["get"] = getterIt->second;
      }

      auto it = promise->properties.find(key);
      if (it == promise->properties.end() && getterIt == promise->properties.end()) {
        return Value(Undefined{});
      }
      if (it != promise->properties.end()) {
        descriptor->properties["value"] = it->second;
      }
      descriptor->properties["writable"] = Value(true);
      descriptor->properties["enumerable"] = Value(true);
      descriptor->properties["configurable"] = Value(true);
      return Value(descriptor);
    }

    if (args[0].isTypedArray() || args[0].isArrayBuffer() || args[0].isDataView()) {
      const OrderedMap<std::string, Value>* bag = nullptr;
      if (args[0].isTypedArray()) {
        bag = &args[0].getGC<TypedArray>()->properties;
      } else if (args[0].isArrayBuffer()) {
        bag = &args[0].getGC<ArrayBuffer>()->properties;
      } else {
        bag = &args[0].getGC<DataView>()->properties;
      }

      if (isInternalPropertyKeyForReflection(key)) {
        return Value(Undefined{});
      }

      auto getterIt = bag->find("__get_" + key);
      auto setterIt = bag->find("__set_" + key);
      auto it = bag->find(key);
      if (it == bag->end() && getterIt == bag->end() && setterIt == bag->end()) {
        return Value(Undefined{});
      }
      if (getterIt != bag->end() && getterIt->second.isFunction()) {
        descriptor->properties["get"] = getterIt->second;
      }
      if (setterIt != bag->end() && setterIt->second.isFunction()) {
        descriptor->properties["set"] = setterIt->second;
      }
      if (it != bag->end()) {
        descriptor->properties["value"] = it->second;
      }
      descriptor->properties["writable"] = Value(bag->find("__non_writable_" + key) == bag->end());
      descriptor->properties["enumerable"] = Value(bag->find("__non_enum_" + key) == bag->end());
      descriptor->properties["configurable"] = Value(bag->find("__non_configurable_" + key) == bag->end());
      return Value(descriptor);
    }

    // Generic handler for Map, Set, WeakMap, WeakSet, Generator (all use OrderedMap properties)
    if (args[0].isMap() || args[0].isSet() || args[0].isWeakMap() || args[0].isWeakSet() || args[0].isGenerator()) {
      const OrderedMap<std::string, Value>* bag = nullptr;
      if (args[0].isMap()) bag = &args[0].getGC<Map>()->properties;
      else if (args[0].isSet()) bag = &args[0].getGC<Set>()->properties;
      else if (args[0].isWeakMap()) bag = &args[0].getGC<WeakMap>()->properties;
      else if (args[0].isWeakSet()) bag = &args[0].getGC<WeakSet>()->properties;
      else bag = &args[0].getGC<Generator>()->properties;

      if (isInternalPropertyKeyForReflection(key)) {
        return Value(Undefined{});
      }

      auto getterIt = bag->find("__get_" + key);
      auto setterIt = bag->find("__set_" + key);
      bool hasAccessor = (getterIt != bag->end()) || (setterIt != bag->end());
      auto it = bag->find(key);
      if (it == bag->end() && !hasAccessor) {
        return Value(Undefined{});
      }
      if (hasAccessor) {
        if (getterIt != bag->end()) {
          descriptor->properties["get"] = getterIt->second;
        } else {
          descriptor->properties["get"] = Value(Undefined{});
        }
        if (setterIt != bag->end()) {
          descriptor->properties["set"] = setterIt->second;
        } else {
          descriptor->properties["set"] = Value(Undefined{});
        }
      } else {
        if (it != bag->end()) {
          descriptor->properties["value"] = it->second;
        }
        descriptor->properties["writable"] = Value(bag->find("__non_writable_" + key) == bag->end());
      }
      descriptor->properties["enumerable"] = Value(bag->find("__non_enum_" + key) == bag->end());
      descriptor->properties["configurable"] = Value(bag->find("__non_configurable_" + key) == bag->end());
      return Value(descriptor);
    }

    if (args[0].isObject()) {
      auto obj = args[0].getGC<Object>();

      // String wrapper objects: expose indexed character properties
      auto primIt = obj->properties.find("__primitive_value__");
      if (primIt != obj->properties.end() && primIt->second.isString()) {
        const std::string& str = std::get<std::string>(primIt->second.data);
        if (!key.empty() && key[0] >= '0' && key[0] <= '9') {
          bool allDigits = true;
          for (char c : key) { if (!std::isdigit(static_cast<unsigned char>(c))) { allDigits = false; break; } }
          if (allDigits) {
            size_t idx = 0;
            try { idx = std::stoull(key); } catch (...) { /* fall through */ }
            // Get the character at the code point index
            size_t cpIdx = 0;
            size_t bytePos = 0;
            while (bytePos < str.size() && cpIdx < idx) {
              unsigned char ch = static_cast<unsigned char>(str[bytePos]);
              if (ch < 0x80) bytePos += 1;
              else if ((ch & 0xE0) == 0xC0) bytePos += 2;
              else if ((ch & 0xF0) == 0xE0) bytePos += 3;
              else bytePos += 4;
              cpIdx++;
            }
            if (cpIdx == idx && bytePos < str.size()) {
              unsigned char ch = static_cast<unsigned char>(str[bytePos]);
              size_t charLen = 1;
              if (ch >= 0x80) {
                if ((ch & 0xE0) == 0xC0) charLen = 2;
                else if ((ch & 0xF0) == 0xE0) charLen = 3;
                else charLen = 4;
              }
              descriptor->properties["value"] = Value(str.substr(bytePos, charLen));
              descriptor->properties["writable"] = Value(false);
              descriptor->properties["enumerable"] = Value(true);
              descriptor->properties["configurable"] = Value(false);
              return Value(descriptor);
            }
          }
        }
        if (key == "length") {
          // Count code points
          size_t cpCount = 0;
          size_t bytePos = 0;
          while (bytePos < str.size()) {
            unsigned char ch = static_cast<unsigned char>(str[bytePos]);
            if (ch < 0x80) bytePos += 1;
            else if ((ch & 0xE0) == 0xC0) bytePos += 2;
            else if ((ch & 0xF0) == 0xE0) bytePos += 3;
            else bytePos += 4;
            cpCount++;
          }
          descriptor->properties["value"] = Value(static_cast<double>(cpCount));
          descriptor->properties["writable"] = Value(false);
          descriptor->properties["enumerable"] = Value(false);
          descriptor->properties["configurable"] = Value(false);
          return Value(descriptor);
        }
      }

      // __proto__ in properties is the prototype chain link, not an own property.
      // Own __proto__ is stored as __own_prop___proto__ (from computed/defineProperty
      // and non-Annex B object-literal properties).
      if (key == "__proto__") {
        const std::string ownKey = "__own_prop___proto__";
        auto ownIt = obj->properties.find(ownKey);
        auto getterIt = obj->properties.find("__get_" + ownKey);
        auto setterIt = obj->properties.find("__set_" + ownKey);
        if (getterIt == obj->properties.end()) {
          getterIt = obj->properties.find("__get___proto__");
        }
        if (setterIt == obj->properties.end()) {
          setterIt = obj->properties.find("__set___proto__");
        }
        bool hasAccessor = (getterIt != obj->properties.end() && getterIt->second.isFunction()) ||
                           (setterIt != obj->properties.end() && setterIt->second.isFunction());
        if (ownIt == obj->properties.end() && !hasAccessor) {
          return Value(Undefined{});
        }
        if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
          descriptor->properties["get"] = getterIt->second;
        }
        if (setterIt != obj->properties.end() && setterIt->second.isFunction()) {
          descriptor->properties["set"] = setterIt->second;
        }
        if (!hasAccessor && ownIt != obj->properties.end()) {
          descriptor->properties["value"] = ownIt->second;
          bool writable = obj->properties.find("__non_writable_" + ownKey) == obj->properties.end();
          descriptor->properties["writable"] = Value(writable);
        }
        const std::string markerKey =
            (ownIt != obj->properties.end() ||
             obj->properties.find("__non_enum_" + ownKey) != obj->properties.end() ||
             obj->properties.find("__non_configurable_" + ownKey) != obj->properties.end())
              ? ownKey
              : "__proto__";
        bool enumerable = obj->properties.find("__non_enum_" + markerKey) == obj->properties.end();
        bool configurable = obj->properties.find("__non_configurable_" + markerKey) == obj->properties.end();
        descriptor->properties["enumerable"] = Value(enumerable);
        descriptor->properties["configurable"] = Value(configurable);
        return Value(descriptor);
      }

      if (obj->isModuleNamespace) {
        if (key == WellKnownSymbols::toStringTagKey()) {
          descriptor->properties["value"] = Value(std::string("Module"));
          descriptor->properties["writable"] = Value(false);
          descriptor->properties["enumerable"] = Value(false);
          descriptor->properties["configurable"] = Value(false);
          return Value(descriptor);
        }
        if (!isModuleNamespaceExportKey(obj, key)) {
          return Value(Undefined{});
        }
        auto getterIt = obj->properties.find("__get_" + key);
        if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
          Interpreter* interpreter = getGlobalInterpreter();
          if (interpreter) {
            Value out = interpreter->callForHarness(getterIt->second, {}, args[0]);
            if (interpreter->hasError()) {
              Value err = interpreter->getError();
              interpreter->clearError();
              throw std::runtime_error(err.toString());
            }
            descriptor->properties["value"] = out;
          } else {
            descriptor->properties["value"] = Value(Undefined{});
          }
        } else if (auto it = obj->properties.find(key); it != obj->properties.end()) {
          descriptor->properties["value"] = it->second;
        } else {
          descriptor->properties["value"] = Value(Undefined{});
        }
        descriptor->properties["writable"] = Value(true);
        descriptor->properties["enumerable"] = Value(true);
        descriptor->properties["configurable"] = Value(false);
        return Value(descriptor);
      }
      // Internal properties are not visible.
      if (isInternalPropertyKeyForReflection(key)) {
        return Value(Undefined{});
      }

      auto getterIt2 = obj->properties.find("__get_" + key);
      auto setterIt2 = obj->properties.find("__set_" + key);
      auto it = obj->properties.find(key);
      if (it == obj->properties.end() && getterIt2 == obj->properties.end() && setterIt2 == obj->properties.end()) {
        return Value(Undefined{});
      }
      bool hasAccessor = (getterIt2 != obj->properties.end()) ||
                         (setterIt2 != obj->properties.end());
      if (hasAccessor) {
        // Accessor descriptor: get/set + enumerable/configurable, no value/writable
        if (getterIt2 != obj->properties.end()) {
          descriptor->properties["get"] = getterIt2->second;
        } else {
          descriptor->properties["get"] = Value(Undefined{});
        }
        if (setterIt2 != obj->properties.end()) {
          descriptor->properties["set"] = setterIt2->second;
        } else {
          descriptor->properties["set"] = Value(Undefined{});
        }
      } else {
        if (it != obj->properties.end()) {
          descriptor->properties["value"] = it->second;
        }
        bool writable = !obj->frozen;
        auto nwIt = obj->properties.find("__non_writable_" + key);
        if (nwIt != obj->properties.end()) writable = false;
        descriptor->properties["writable"] = Value(writable);
      }

      // Check for per-property attribute markers
      bool enumerable = true;
      bool configurable = !obj->sealed;
      auto neIt = obj->properties.find("__non_enum_" + key);
      if (neIt != obj->properties.end()) enumerable = false;
      auto ncIt = obj->properties.find("__non_configurable_" + key);
      if (ncIt != obj->properties.end()) configurable = false;
      descriptor->properties["enumerable"] = Value(enumerable);
      descriptor->properties["configurable"] = Value(configurable);
      return Value(descriptor);
    }

    auto regex = args[0].getGC<Regex>();
    auto getterIt = regex->properties.find("__get_" + key);
    if (getterIt != regex->properties.end() && getterIt->second.isFunction()) {
      descriptor->properties["get"] = getterIt->second;
    }

    auto it = regex->properties.find(key);
    if (it != regex->properties.end()) {
      descriptor->properties["value"] = it->second;
    } else if (key == "source") {
      descriptor->properties["value"] = Value(regex->pattern);
    } else if (key == "flags") {
      descriptor->properties["value"] = Value(regex->flags);
    } else if (getterIt == regex->properties.end()) {
      return Value(Undefined{});
    }

    if (key == "lastIndex") {
      descriptor->properties["writable"] = Value(true);
      descriptor->properties["enumerable"] = Value(false);
      descriptor->properties["configurable"] = Value(false);
    } else {
      descriptor->properties["writable"] = Value(true);
      descriptor->properties["enumerable"] = Value(true);
      descriptor->properties["configurable"] = Value(true);
    }
    return Value(descriptor);
  };
  objectConstructor->properties["getOwnPropertyDescriptor"] = Value(objectGetOwnPropertyDescriptor);

  // Object.getOwnPropertyDescriptors (plural)
  auto objectGetOwnPropertyDescriptors = GarbageCollector::makeGC<Function>();
  objectGetOwnPropertyDescriptors->isNative = true;
  objectGetOwnPropertyDescriptors->nativeFunc = [objectGetOwnPropertyDescriptor](const std::vector<Value>& args) -> Value {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
      throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
    }
    auto result = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));

    // Handle string primitives: enumerate indices + length
    if (args[0].isString()) {
      const std::string& str = std::get<std::string>(args[0].data);
      size_t cpCount = 0;
      size_t bytePos = 0;
      while (bytePos < str.size()) {
        unsigned char ch = static_cast<unsigned char>(str[bytePos]);
        size_t charLen = 1;
        if (ch >= 0x80) {
          if ((ch & 0xE0) == 0xC0) charLen = 2;
          else if ((ch & 0xF0) == 0xE0) charLen = 3;
          else charLen = 4;
        }
        std::string key = std::to_string(cpCount);
        Value desc = objectGetOwnPropertyDescriptor->nativeFunc({args[0], Value(key)});
        if (!desc.isUndefined()) {
          result->properties[key] = desc;
        }
        bytePos += charLen;
        cpCount++;
      }
      Value lengthDesc = objectGetOwnPropertyDescriptor->nativeFunc({args[0], Value(std::string("length"))});
      if (!lengthDesc.isUndefined()) {
        result->properties["length"] = lengthDesc;
      }
      return Value(result);
    }

    // Collect keys from the target object
    OrderedMap<std::string, Value>* props = nullptr;
    if (args[0].isObject()) {
      props = &args[0].getGC<Object>()->properties;
    } else if (args[0].isFunction()) {
      props = &args[0].getGC<Function>()->properties;
    } else if (args[0].isClass()) {
      props = &args[0].getGC<Class>()->properties;
    } else if (args[0].isArray()) {
      props = &args[0].getGC<Array>()->properties;
    } else if (args[0].isError()) {
      props = &args[0].getGC<Error>()->properties;
    } else if (args[0].isRegex()) {
      props = &args[0].getGC<Regex>()->properties;
    } else if (args[0].isMap()) {
      props = &args[0].getGC<Map>()->properties;
    } else if (args[0].isSet()) {
      props = &args[0].getGC<Set>()->properties;
    } else if (args[0].isPromise()) {
      props = &args[0].getGC<Promise>()->properties;
    } else if (args[0].isTypedArray()) {
      props = &args[0].getGC<TypedArray>()->properties;
    } else if (args[0].isArrayBuffer()) {
      props = &args[0].getGC<ArrayBuffer>()->properties;
    } else if (args[0].isDataView()) {
      props = &args[0].getGC<DataView>()->properties;
    }
    if (props) {
      for (const auto& key : props->orderedKeys()) {
        // Skip internal properties
        if (key.size() >= 4 && key.substr(0, 2) == "__" && key.substr(key.size() - 2) == "__") continue;
        if (key.size() > 6 && (key.substr(0, 6) == "__get_" || key.substr(0, 6) == "__set_")) continue;
        if (key.size() > 10 && key.substr(0, 10) == "__non_enum") continue;
        if (key.size() > 14 && key.substr(0, 14) == "__non_writable") continue;
        if (key.size() > 18 && key.substr(0, 18) == "__non_configurable") continue;
        if (key.size() > 7 && key.substr(0, 7) == "__enum_") continue;
        Value desc = objectGetOwnPropertyDescriptor->nativeFunc({args[0], Value(key)});
        if (!desc.isUndefined()) {
          result->properties[key] = desc;
        }
      }
    }
    // Other primitives (number, boolean): return empty object
    return Value(result);
  };
  objectConstructor->properties["getOwnPropertyDescriptors"] = Value(objectGetOwnPropertyDescriptors);

  // Object.defineProperty - define property with descriptor
  auto objectDefineProperty = GarbageCollector::makeGC<Function>();
  objectDefineProperty->isNative = true;
  objectDefineProperty->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 1 || (!args[0].isObject() && !args[0].isFunction() && !args[0].isPromise() && !args[0].isRegex() &&
         !args[0].isArray() && !args[0].isTypedArray() && !args[0].isArrayBuffer() && !args[0].isDataView() &&
         !args[0].isError() && !args[0].isClass() && !args[0].isMap() && !args[0].isSet() &&
         !args[0].isWeakMap() && !args[0].isWeakSet() && !args[0].isGenerator() && !args[0].isProxy())) {
      throw std::runtime_error("TypeError: Object.defineProperty called on non-object");
    }
    if (args.size() < 3) {
      throw std::runtime_error("TypeError: Property description must be an object");
    }
    std::string key = valueToPropertyKey(args[1]);

    // ToPropertyDescriptor: descriptor must be an object
    {
      const auto& d = args[2];
      if (!d.isObject() && !d.isFunction() && !d.isArray() && !d.isClass() &&
          !d.isProxy() && !d.isRegex() && !d.isError() && !d.isMap() &&
          !d.isSet() && !d.isPromise() && !d.isWeakMap() && !d.isWeakSet() &&
          !d.isTypedArray() && !d.isArrayBuffer() && !d.isDataView() &&
          !d.isGenerator()) {
        throw std::runtime_error("TypeError: Property description must be an object: " + d.toString());
      }
    }

    {
      auto readDescriptorField = [&](const std::string& name) -> std::optional<Value> {
        const Value& desc = args[2];

        // Walk prototype chain from a starting value
        auto walkProto = [&](Value proto) -> std::optional<Value> {
          int depth = 0;
          while (!proto.isNull() && !proto.isUndefined() && depth < 100) {
            ++depth;
            if (proto.isObject()) {
              auto protoObj = proto.getGC<Object>();
              auto propIt = protoObj->properties.find(name);
              if (propIt != protoObj->properties.end()) return propIt->second;
              auto nextIt = protoObj->properties.find("__proto__");
              if (nextIt != protoObj->properties.end()) proto = nextIt->second;
              else break;
            } else if (proto.isFunction()) {
              auto protoFn = proto.getGC<Function>();
              auto propIt = protoFn->properties.find(name);
              if (propIt != protoFn->properties.end()) return propIt->second;
              auto nextIt = protoFn->properties.find("__proto__");
              if (nextIt != protoFn->properties.end()) proto = nextIt->second;
              else break;
            } else break;
          }
          return std::nullopt;
        };

        // Check own props + walk proto chain (auto& to support both unordered_map and OrderedMap)
        auto readFromProps = [&](const auto& props) -> std::optional<Value> {
          auto it = props.find(name);
          if (it != props.end()) return it->second;
          auto protoIt = props.find("__proto__");
          if (protoIt != props.end()) return walkProto(protoIt->second);
          return std::nullopt;
        };

        if (desc.isObject()) {
          auto obj = desc.getGC<Object>();
          auto it = obj->properties.find(name);
          if (it != obj->properties.end()) return it->second;
          if (obj->shape) {
            int offset = obj->shape->getPropertyOffset(name);
            if (offset >= 0) {
              Value slotValue;
              if (obj->getSlot(offset, slotValue)) return slotValue;
            }
          }
          auto protoIt = obj->properties.find("__proto__");
          if (protoIt != obj->properties.end()) return walkProto(protoIt->second);
          return std::nullopt;
        } else if (desc.isFunction()) {
          return readFromProps(desc.getGC<Function>()->properties);
        } else if (desc.isArray()) {
          return readFromProps(desc.getGC<Array>()->properties);
        } else if (desc.isError()) {
          return readFromProps(desc.getGC<Error>()->properties);
        } else if (desc.isRegex()) {
          return readFromProps(desc.getGC<Regex>()->properties);
        } else if (desc.isClass()) {
          return readFromProps(desc.getGC<Class>()->properties);
        } else if (desc.isMap()) {
          return readFromProps(desc.getGC<Map>()->properties);
        } else if (desc.isSet()) {
          return readFromProps(desc.getGC<Set>()->properties);
        } else if (desc.isPromise()) {
          return readFromProps(desc.getGC<Promise>()->properties);
        } else if (desc.isWeakMap()) {
          return readFromProps(desc.getGC<WeakMap>()->properties);
        } else if (desc.isWeakSet()) {
          return readFromProps(desc.getGC<WeakSet>()->properties);
        } else if (desc.isTypedArray()) {
          return readFromProps(desc.getGC<TypedArray>()->properties);
        }
        return std::nullopt;
      };
      // Validate descriptor per ES spec ToPropertyDescriptor (8.10.5)
      {
        auto getField = readDescriptorField("get");
        auto setField = readDescriptorField("set");
        auto valueField = readDescriptorField("value");
        auto writableField = readDescriptorField("writable");
        bool hasGetField = getField.has_value();
        bool hasSetField = setField.has_value();
        bool hasValueField = valueField.has_value();
        bool hasWritableField = writableField.has_value();
        // getter must be callable or undefined
        if (hasGetField && !getField->isUndefined() && !getField->isFunction() && !getField->isClass()) {
          throw std::runtime_error("TypeError: Getter must be a function: " + getField->toString());
        }
        // setter must be callable or undefined
        if (hasSetField && !setField->isUndefined() && !setField->isFunction() && !setField->isClass()) {
          throw std::runtime_error("TypeError: Setter must be a function: " + setField->toString());
        }
        // Cannot mix data and accessor descriptor fields
        if ((hasGetField || hasSetField) && (hasValueField || hasWritableField)) {
          throw std::runtime_error("TypeError: Invalid property descriptor. Cannot both specify accessors and a value or writable attribute");
        }
      }

      auto applyDescriptorToBag = [&](auto& bag) {
        bool hadExistingProperty =
          (bag.find(key) != bag.end()) ||
          (bag.find("__get_" + key) != bag.end()) ||
          (bag.find("__set_" + key) != bag.end());

        auto valueField = readDescriptorField("value");
        auto getField = readDescriptorField("get");
        auto setField = readDescriptorField("set");
        auto writableField = readDescriptorField("writable");
        auto enumField = readDescriptorField("enumerable");
        auto configField = readDescriptorField("configurable");
        bool hasValueField = valueField.has_value();
        bool hasGetField = getField.has_value();
        bool hasSetField = setField.has_value();
        bool isAccessorDesc = hasGetField || hasSetField;

        // ValidateAndApplyPropertyDescriptor: non-configurable constraints
        if (hadExistingProperty) {
          bool isNonConfigurable = bag.find("__non_configurable_" + key) != bag.end();
          if (isNonConfigurable) {
            if (configField.has_value() && configField->toBool()) {
              throw std::runtime_error("TypeError: Cannot redefine property: " + key);
            }
            bool currentNonEnum = bag.find("__non_enum_" + key) != bag.end();
            if (enumField.has_value() && (enumField->toBool() == currentNonEnum)) {
              throw std::runtime_error("TypeError: Cannot redefine property: " + key);
            }
            bool currentIsAccessor = bag.find("__get_" + key) != bag.end() ||
                                     bag.find("__set_" + key) != bag.end();
            if (currentIsAccessor && (hasValueField || writableField.has_value()) && !isAccessorDesc) {
              throw std::runtime_error("TypeError: Cannot redefine property: " + key);
            }
            if (!currentIsAccessor && isAccessorDesc && !hasValueField) {
              throw std::runtime_error("TypeError: Cannot redefine property: " + key);
            }
            if (!currentIsAccessor) {
              bool isNonWritable = bag.find("__non_writable_" + key) != bag.end();
              if (isNonWritable) {
                if (writableField.has_value() && writableField->toBool()) {
                  throw std::runtime_error("TypeError: Cannot redefine property: " + key);
                }
              }
            }
          }
        }

        // When setting value, remove accessor markers
        if (hasValueField) {
          bag.erase("__get_" + key);
          bag.erase("__set_" + key);
          bag[key] = *valueField;
        }
        // When setting accessor, remove data markers
        if (hasGetField) {
          if (!hasValueField) {
            bag.erase("__non_writable_" + key);
          }
          if (getField->isFunction() || getField->isClass()) {
            bag["__get_" + key] = *getField;
          } else {
            bag["__get_" + key] = Value(Undefined{});
          }
        }
        if (hasSetField) {
          if (!hasValueField) {
            bag.erase("__non_writable_" + key);
          }
          if (setField->isFunction() || setField->isClass()) {
            bag["__set_" + key] = *setField;
          } else {
            bag["__set_" + key] = Value(Undefined{});
          }
        }
        if (!hadExistingProperty && !hasValueField && !hasGetField && !hasSetField) {
          bag[key] = Value(Undefined{});
        }
        if (!hadExistingProperty && !hasValueField && (hasGetField || hasSetField) &&
            bag.find(key) == bag.end()) {
          bag[key] = Value(Undefined{});
        }

        if (writableField.has_value()) {
          if (!writableField->toBool()) {
            bag["__non_writable_" + key] = Value(true);
          } else {
            bag.erase("__non_writable_" + key);
          }
        }
        if (enumField.has_value()) {
          if (!enumField->toBool()) {
            bag["__non_enum_" + key] = Value(true);
          } else {
            bag.erase("__non_enum_" + key);
          }
        }
        if (configField.has_value()) {
          if (!configField->toBool()) {
            bag["__non_configurable_" + key] = Value(true);
          } else {
            bag.erase("__non_configurable_" + key);
          }
        }

        // Defaults for new properties: unspecified attributes default to false
        if (!hadExistingProperty) {
          if (!isAccessorDesc && !writableField.has_value()) {
            bag["__non_writable_" + key] = Value(true);
          }
          if (!enumField.has_value()) {
            bag["__non_enum_" + key] = Value(true);
          }
          if (!configField.has_value()) {
            bag["__non_configurable_" + key] = Value(true);
          }
        }

        // Conversion defaults: accessor<->data
        if (hadExistingProperty) {
          bool wasAccessor = bag.find("__get_" + key) != bag.end() ||
                             bag.find("__set_" + key) != bag.end();
          if (!wasAccessor && isAccessorDesc && !hasValueField) {
            // Data -> Accessor: remove writable, ensure get/set markers
            bag.erase("__non_writable_" + key);
            if (!hasGetField && bag.find("__get_" + key) == bag.end()) {
              bag["__get_" + key] = Value(Undefined{});
            }
            if (!hasSetField && bag.find("__set_" + key) == bag.end()) {
              bag["__set_" + key] = Value(Undefined{});
            }
          }
        }
      };
      if (args[0].isObject()) {
        auto obj = args[0].getGC<Object>();
        if (obj->isModuleNamespace) {
          if (!defineModuleNamespaceProperty(obj, key, args[2].getGC<Object>())) {
            throw std::runtime_error("TypeError: Cannot redefine module namespace property");
          }
          return args[0];
        }

        if (obj->frozen) {
          throw std::runtime_error("TypeError: Cannot define property " + key + ", object is frozen");
        }
        if ((obj->sealed || obj->nonExtensible) && obj->properties.find(key) == obj->properties.end()) {
          throw std::runtime_error("TypeError: Cannot define property " + key + ", object is not extensible");
        }

        bool hadExistingProperty =
          (obj->properties.find(key) != obj->properties.end()) ||
          (obj->properties.find("__get_" + key) != obj->properties.end()) ||
          (obj->properties.find("__set_" + key) != obj->properties.end());

        auto valueField = readDescriptorField("value");
        auto getField = readDescriptorField("get");
        auto setField = readDescriptorField("set");
        auto writableField = readDescriptorField("writable");
        auto enumField = readDescriptorField("enumerable");
        auto configField = readDescriptorField("configurable");
        bool hasValueField = valueField.has_value();
        bool hasGetField = getField.has_value();
        bool hasSetField = setField.has_value();
        bool objCurrentIsAccessor = obj->properties.find("__get_" + key) != obj->properties.end() ||
                                    obj->properties.find("__set_" + key) != obj->properties.end();

        // ValidateAndApplyPropertyDescriptor: non-configurable constraints
        if (hadExistingProperty) {
          bool isNonConfigurable = obj->properties.find("__non_configurable_" + key) != obj->properties.end();
          if (isNonConfigurable) {
            // Cannot make configurable
            if (configField.has_value() && configField->toBool()) {
              throw std::runtime_error("TypeError: Cannot redefine property: " + key);
            }
            // Cannot change enumerable
            bool currentNonEnum = obj->properties.find("__non_enum_" + key) != obj->properties.end();
            if (enumField.has_value() && (enumField->toBool() == currentNonEnum)) {
              throw std::runtime_error("TypeError: Cannot redefine property: " + key);
            }
            // Check accessor vs data conversion
            bool currentIsAccessor = obj->properties.find("__get_" + key) != obj->properties.end() ||
                                     obj->properties.find("__set_" + key) != obj->properties.end();
            bool newIsAccessor = hasGetField || hasSetField;
            bool newIsData = hasValueField || writableField.has_value();
            if (currentIsAccessor && newIsData && !newIsAccessor) {
              throw std::runtime_error("TypeError: Cannot redefine property: " + key);
            }
            if (!currentIsAccessor && newIsAccessor && !newIsData) {
              throw std::runtime_error("TypeError: Cannot redefine property: " + key);
            }
            if (currentIsAccessor) {
              // Cannot change get or set on non-configurable accessor
              if (hasGetField) {
                auto currentGet = obj->properties.find("__get_" + key);
                Value currentGetVal = (currentGet != obj->properties.end()) ? currentGet->second : Value(Undefined{});
                if (getField->isFunction() != currentGetVal.isFunction() ||
                    (getField->isFunction() && currentGetVal.isFunction() &&
                     getField->getGC<Function>() != currentGetVal.getGC<Function>())) {
                  throw std::runtime_error("TypeError: Cannot redefine property: " + key);
                }
              }
              if (hasSetField) {
                auto currentSet = obj->properties.find("__set_" + key);
                Value currentSetVal = (currentSet != obj->properties.end()) ? currentSet->second : Value(Undefined{});
                if (setField->isFunction() != currentSetVal.isFunction() ||
                    (setField->isFunction() && currentSetVal.isFunction() &&
                     setField->getGC<Function>() != currentSetVal.getGC<Function>())) {
                  throw std::runtime_error("TypeError: Cannot redefine property: " + key);
                }
              }
            } else {
              // Data property: non-configurable + non-writable => cannot change value or make writable
              bool isNonWritable = obj->properties.find("__non_writable_" + key) != obj->properties.end();
              if (isNonWritable) {
                if (writableField.has_value() && writableField->toBool()) {
                  throw std::runtime_error("TypeError: Cannot redefine property: " + key);
                }
                if (hasValueField) {
                  auto currentIt = obj->properties.find(key);
                  if (currentIt != obj->properties.end()) {
                    // SameValue check
                    Value currentVal = currentIt->second;
                    Value newVal = *valueField;
                    bool sameValue = false;
                    if (currentVal.isNumber() && newVal.isNumber()) {
                      double a = currentVal.toNumber(), b = newVal.toNumber();
                      if (std::isnan(a) && std::isnan(b)) sameValue = true;
                      else if (a == 0.0 && b == 0.0) sameValue = (std::signbit(a) == std::signbit(b));
                      else sameValue = (a == b);
                    } else if (currentVal.isString() && newVal.isString()) {
                      sameValue = (std::get<std::string>(currentVal.data) == std::get<std::string>(newVal.data));
                    } else if (currentVal.isBool() && newVal.isBool()) {
                      sameValue = (currentVal.toBool() == newVal.toBool());
                    } else if (currentVal.isUndefined() && newVal.isUndefined()) {
                      sameValue = true;
                    } else if (currentVal.isNull() && newVal.isNull()) {
                      sameValue = true;
                    } else if (currentVal.isObject() && newVal.isObject()) {
                      sameValue = currentVal.getGC<Object>().get() == newVal.getGC<Object>().get();
                    } else if (currentVal.isFunction() && newVal.isFunction()) {
                      sameValue = currentVal.getGC<Function>().get() == newVal.getGC<Function>().get();
                    } else if (currentVal.isArray() && newVal.isArray()) {
                      sameValue = currentVal.getGC<Array>().get() == newVal.getGC<Array>().get();
                    } else if (currentVal.isRegex() && newVal.isRegex()) {
                      sameValue = currentVal.getGC<Regex>().get() == newVal.getGC<Regex>().get();
                    } else if (currentVal.isSymbol() && newVal.isSymbol()) {
                      sameValue = std::get<Symbol>(currentVal.data) == std::get<Symbol>(newVal.data);
                    } else if (currentVal.isBigInt() && newVal.isBigInt()) {
                      sameValue = currentVal.toBigInt() == newVal.toBigInt();
                    } else {
                      sameValue = false;
                    }
                    if (!sameValue) {
                      throw std::runtime_error("TypeError: Cannot redefine property: " + key);
                    }
                  }
                }
              }
            }
          }
        }

        if (hasValueField) {
          // When setting value on an accessor property, remove accessor markers
          if (obj->properties.find("__get_" + key) != obj->properties.end()) {
            obj->properties.erase("__get_" + key);
          }
          if (obj->properties.find("__set_" + key) != obj->properties.end()) {
            obj->properties.erase("__set_" + key);
          }
          obj->properties[key] = *valueField;
        }

        if (hasGetField) {
          if (getField->isFunction() || getField->isClass()) {
            obj->properties["__get_" + key] = *getField;
          } else {
            obj->properties["__get_" + key] = Value(Undefined{});
          }
        }

        if (hasSetField) {
          if (setField->isFunction() || setField->isClass()) {
            obj->properties["__set_" + key] = *setField;
          } else {
            obj->properties["__set_" + key] = Value(Undefined{});
          }
        }

        // Defining a new data property with attributes-only descriptor
        // still creates the property with value `undefined`.
        if (!hadExistingProperty && !hasValueField && !hasGetField && !hasSetField) {
          obj->properties[key] = Value(Undefined{});
        }

        // Ensure accessor-only properties have a visible key for enumeration.
        if (!hadExistingProperty && !hasValueField && (hasGetField || hasSetField)) {
          if (obj->properties.find(key) == obj->properties.end()) {
            obj->properties[key] = Value(Undefined{});
          }
        }

        if (writableField.has_value()) {
          if (!writableField->toBool()) {
            obj->properties["__non_writable_" + key] = Value(true);
          } else {
            obj->properties.erase("__non_writable_" + key);
          }
        }

        // Handle enumerable descriptor
        if (enumField.has_value()) {
          if (!enumField->toBool()) {
            obj->properties["__non_enum_" + key] = Value(true);
          } else {
            obj->properties.erase("__non_enum_" + key);
          }
        }

        // Handle configurable descriptor
        if (configField.has_value()) {
          if (!configField->toBool()) {
            obj->properties["__non_configurable_" + key] = Value(true);
          } else {
            obj->properties.erase("__non_configurable_" + key);
          }
        }

        // Defaults: for new properties, unspecified attributes default to false.
        if (!hadExistingProperty) {
          bool isAccessorDescriptor = hasGetField || hasSetField;
          bool isDataDescriptor = !isAccessorDescriptor &&
                                  (hasValueField || writableField.has_value() || (!hasGetField && !hasSetField));
          if (isDataDescriptor && !writableField.has_value()) {
            obj->properties["__non_writable_" + key] = Value(true);
          }
          if (!enumField.has_value()) {
            obj->properties["__non_enum_" + key] = Value(true);
          }
          if (!configField.has_value()) {
            obj->properties["__non_configurable_" + key] = Value(true);
          }
        }

        // Conversion defaults: accessor<->data
        if (hadExistingProperty) {
          bool isNowAccessor = obj->properties.find("__get_" + key) != obj->properties.end() ||
                               obj->properties.find("__set_" + key) != obj->properties.end();
          if (objCurrentIsAccessor && !isNowAccessor) {
            // Accessor -> Data: default writable to false
            if (!writableField.has_value()) {
              obj->properties["__non_writable_" + key] = Value(true);
            }
          }
          if (!objCurrentIsAccessor && isNowAccessor) {
            // Data -> Accessor: remove writable, ensure get/set
            obj->properties.erase("__non_writable_" + key);
            if (!hasGetField && obj->properties.find("__get_" + key) == obj->properties.end()) {
              obj->properties["__get_" + key] = Value(Undefined{});
            }
            if (!hasSetField && obj->properties.find("__set_" + key) == obj->properties.end()) {
              obj->properties["__set_" + key] = Value(Undefined{});
            }
          }
        }
      } else if (args[0].isFunction()) {
        applyDescriptorToBag(args[0].getGC<Function>()->properties);
      } else if (args[0].isPromise()) {
        applyDescriptorToBag(args[0].getGC<Promise>()->properties);
      } else if (args[0].isTypedArray()) {
        applyDescriptorToBag(args[0].getGC<TypedArray>()->properties);
      } else if (args[0].isArrayBuffer()) {
        applyDescriptorToBag(args[0].getGC<ArrayBuffer>()->properties);
      } else if (args[0].isDataView()) {
        applyDescriptorToBag(args[0].getGC<DataView>()->properties);
      } else if (args[0].isArray()) {
        auto arr = args[0].getGC<Array>();

        // Special handling for "length" property on arrays
        if (key == "length") {
          auto valueField = readDescriptorField("value");
          auto writableField = readDescriptorField("writable");
          auto enumField = readDescriptorField("enumerable");
          auto configField = readDescriptorField("configurable");
          auto getField = readDescriptorField("get");
          auto setField = readDescriptorField("set");

          // Array length is always non-configurable, non-enumerable
          if (configField.has_value() && configField->toBool()) {
            throw std::runtime_error("TypeError: Cannot redefine property: length");
          }
          if (enumField.has_value() && enumField->toBool()) {
            throw std::runtime_error("TypeError: Cannot redefine property: length");
          }
          // Cannot convert to accessor
          if (getField.has_value() || setField.has_value()) {
            throw std::runtime_error("TypeError: Cannot redefine property: length");
          }

          bool lengthNonWritable = arr->properties.find("__non_writable_length") != arr->properties.end();

          // If already non-writable, cannot make writable
          if (lengthNonWritable && writableField.has_value() && writableField->toBool()) {
            throw std::runtime_error("TypeError: Cannot redefine property: length");
          }

          if (valueField.has_value()) {
            double numVal = valueField->toNumber();
            uint32_t newLen = static_cast<uint32_t>(numVal);
            if (static_cast<double>(newLen) != numVal || std::isinf(numVal) || std::isnan(numVal)) {
              throw std::runtime_error("RangeError: Invalid array length");
            }

            if (lengthNonWritable && newLen != static_cast<uint32_t>(arr->elements.size())) {
              throw std::runtime_error("TypeError: Cannot redefine property: length");
            }

            size_t oldLen = arr->elements.size();
            if (newLen < oldLen) {
              arr->elements.resize(newLen);
            } else if (newLen > oldLen) {
              arr->elements.resize(newLen, Value(Undefined{}));
            }
          }

          if (writableField.has_value() && !writableField->toBool()) {
            arr->properties["__non_writable_length"] = Value(true);
          } else if (writableField.has_value()) {
            arr->properties.erase("__non_writable_length");
          }

          return args[0];
        }

        bool isArgumentsObject = false;
        auto isArgsIt = arr->properties.find("__is_arguments_object__");
        if (isArgsIt != arr->properties.end() && isArgsIt->second.isBool() && isArgsIt->second.toBool()) {
          isArgumentsObject = true;
        }
        auto isCanonicalArrayIndex = [&](const std::string& s) -> bool {
          if (s.empty()) return false;
          if (s.size() > 1 && s[0] == '0') return false;
          for (unsigned char c : s) {
            if (std::isdigit(c) == 0) return false;
          }
          return true;
        };
        bool isIndexKey = isCanonicalArrayIndex(key);
        bool hadMappedArgumentsBinding =
          isArgumentsObject && isIndexKey &&
          (arr->properties.find("__mapped_arg_index_" + key + "__") != arr->properties.end());

        auto valueField = readDescriptorField("value");
        auto getField = readDescriptorField("get");
        auto setField = readDescriptorField("set");
        auto writableField = readDescriptorField("writable");
        auto enumField = readDescriptorField("enumerable");
        auto configField = readDescriptorField("configurable");

        bool isAccessorDescriptor = getField.has_value() || setField.has_value();
        bool makeNonWritable = writableField.has_value() && !writableField->toBool();

        // Compute hadExistingProperty before any modifications
        bool hadExistingProperty = false;
        if (isIndexKey) {
          try {
            size_t idx = std::stoul(key);
            hadExistingProperty = (idx < arr->elements.size());
          } catch (...) {}
        }
        if (!hadExistingProperty) {
          hadExistingProperty = arr->properties.count(key) > 0 ||
                                arr->properties.count("__get_" + key) > 0 ||
                                arr->properties.count("__set_" + key) > 0;
        }

        // DefineOwnProperty invariants (minimal): reject invalid redefinitions of
        // non-configurable properties so Test262's define-failure tests pass.
        auto sameValue = [](const Value& a, const Value& b) -> bool {
          if (a.data.index() != b.data.index()) return false;
          if (a.isNumber() && b.isNumber()) {
            double x = a.toNumber();
            double y = b.toNumber();
            if (std::isnan(x) && std::isnan(y)) return true;
            if (x == 0.0 && y == 0.0) return std::signbit(x) == std::signbit(y);
            return x == y;
          }
          if (a.isUndefined() && b.isUndefined()) return true;
          if (a.isNull() && b.isNull()) return true;
          if (a.isBool() && b.isBool()) return std::get<bool>(a.data) == std::get<bool>(b.data);
          if (a.isString() && b.isString()) return std::get<std::string>(a.data) == std::get<std::string>(b.data);
          if (a.isBigInt() && b.isBigInt()) return a.toBigInt() == b.toBigInt();
          if (a.isSymbol() && b.isSymbol()) return std::get<Symbol>(a.data) == std::get<Symbol>(b.data);
          // Objects compare by reference identity in SameValue.
          if (a.isFunction() && b.isFunction()) return a.getGC<Function>().get() == b.getGC<Function>().get();
          if (a.isArray() && b.isArray()) return a.getGC<Array>().get() == b.getGC<Array>().get();
          if (a.isObject() && b.isObject()) return a.getGC<Object>().get() == b.getGC<Object>().get();
          if (a.isClass() && b.isClass()) return a.getGC<Class>().get() == b.getGC<Class>().get();
          if (a.isRegex() && b.isRegex()) return a.getGC<Regex>().get() == b.getGC<Regex>().get();
          if (a.isPromise() && b.isPromise()) return a.getGC<Promise>().get() == b.getGC<Promise>().get();
          return false;
        };

        bool currentNonConfigurable = arr->properties.find("__non_configurable_" + key) != arr->properties.end();
        bool currentEnumerable = arr->properties.find("__non_enum_" + key) == arr->properties.end();
        bool currentWritable = arr->properties.find("__non_writable_" + key) == arr->properties.end();
        bool currentIsMapped = hadMappedArgumentsBinding;
        bool currentHasAccessor =
          arr->properties.find("__get_" + key) != arr->properties.end() ||
          arr->properties.find("__set_" + key) != arr->properties.end();
        bool currentIsAccessor = currentHasAccessor && !currentIsMapped;

        auto getCurrentIndexValue = [&]() -> Value {
          if (currentIsMapped) {
            auto it = arr->properties.find("__get_" + key);
            if (it != arr->properties.end() && it->second.isFunction()) {
              auto fn = it->second.getGC<Function>();
              return fn->nativeFunc({});
            }
          }
          bool isNumeric = true;
          size_t idx = 0;
          try { idx = std::stoul(key); } catch (...) { isNumeric = false; }
          if (isNumeric && idx < arr->elements.size()) {
            return arr->elements[idx];
          }
          auto it = arr->properties.find(key);
          if (it != arr->properties.end()) return it->second;
          return Value(Undefined{});
        };

        if (currentNonConfigurable) {
          if (configField.has_value() && configField->toBool()) {
            throw std::runtime_error("TypeError: Cannot redefine property");
          }
          if (enumField.has_value() && enumField->toBool() != currentEnumerable) {
            throw std::runtime_error("TypeError: Cannot redefine property");
          }
          if (currentIsAccessor) {
            if (valueField.has_value() || writableField.has_value()) {
              throw std::runtime_error("TypeError: Cannot redefine property");
            }
            // Cannot change get/set on non-configurable accessor
            if (getField.has_value()) {
              auto curGetIt = arr->properties.find("__get_" + key);
              Value curGet = (curGetIt != arr->properties.end()) ? curGetIt->second : Value(Undefined{});
              if (!sameValue(*getField, curGet)) {
                throw std::runtime_error("TypeError: Cannot redefine property");
              }
            }
            if (setField.has_value()) {
              auto curSetIt = arr->properties.find("__set_" + key);
              Value curSet = (curSetIt != arr->properties.end()) ? curSetIt->second : Value(Undefined{});
              if (!sameValue(*setField, curSet)) {
                throw std::runtime_error("TypeError: Cannot redefine property");
              }
            }
          } else {
            if (isAccessorDescriptor) {
              throw std::runtime_error("TypeError: Cannot redefine property");
            }
            if (!currentWritable) {
              if (writableField.has_value() && writableField->toBool()) {
                throw std::runtime_error("TypeError: Cannot redefine property");
              }
              if (valueField.has_value()) {
                Value cur = getCurrentIndexValue();
                if (!sameValue(cur, *valueField)) {
                  throw std::runtime_error("TypeError: Cannot redefine property");
                }
              }
            }
          }
        }

        // Clear delete/hole markers when (re)defining a property on an index
        if (isIndexKey) {
          arr->properties.erase("__deleted_" + key + "__");
          arr->properties.erase("__hole_" + key + "__");
        }

        // Arguments exotic object: redefining a mapped index with an accessor removes mapping.
        if (hadMappedArgumentsBinding && isAccessorDescriptor) {
          arr->properties.erase("__get_" + key);
          arr->properties.erase("__set_" + key);
          arr->properties.erase("__mapped_arg_index_" + key + "__");
          hadMappedArgumentsBinding = false;
        }

        if (valueField.has_value()) {
          // If this is a mapped arguments index, sync value via the parameter setter
          // BEFORE erasing accessor markers (which are the mapping getters/setters).
          bool isNumeric = true;
          size_t idx = 0;
          try { idx = std::stoul(key); } catch (...) { isNumeric = false; }
          if (hadMappedArgumentsBinding && isNumeric) {
            auto setIt = arr->properties.find("__set_" + key);
            if (setIt != arr->properties.end() && setIt->second.isFunction()) {
              auto fn = setIt->second.getGC<Function>();
              fn->nativeFunc({*valueField});
            }
          }
          // Converting accessor -> data: remove accessor markers
          // BUT skip for mapped arguments (their __get_/__set_ are parameter bindings)
          if (!hadMappedArgumentsBinding) {
            arr->properties.erase("__get_" + key);
            arr->properties.erase("__set_" + key);
          }
          if (isNumeric && idx < arr->elements.size()) {
            arr->elements[idx] = *valueField;
          } else if (isNumeric && idx < arr->elements.size() + 1024) {
            // Extend elements array for nearby numeric indices
            arr->elements.resize(idx + 1, Value(Undefined{}));
            arr->elements[idx] = *valueField;
          } else {
            arr->properties[key] = *valueField;
          }
        }

        if (getField.has_value()) {
          // Converting data -> accessor: remove writable marker
          arr->properties.erase("__non_writable_" + key);
          if (getField->isFunction() || getField->isClass()) {
            arr->properties["__get_" + key] = *getField;
          } else {
            arr->properties["__get_" + key] = Value(Undefined{});
          }
          // For numeric indices, ensure elements array covers this index
          if (isIndexKey) {
            try {
              size_t gIdx = std::stoul(key);
              if (gIdx >= arr->elements.size()) {
                arr->elements.resize(gIdx + 1, Value(Undefined{}));
              }
            } catch (...) {}
          }
        }

        if (setField.has_value()) {
          arr->properties.erase("__non_writable_" + key);
          if (setField->isFunction() || setField->isClass()) {
            arr->properties["__set_" + key] = *setField;
          } else {
            arr->properties["__set_" + key] = Value(Undefined{});
          }
        }

        // Ensure accessor-only properties have a visible key for enumeration.
        if (!valueField.has_value() && (getField.has_value() || setField.has_value())) {
          if (arr->properties.find(key) == arr->properties.end()) {
            arr->properties[key] = Value(Undefined{});
          }
        }

        // For new properties without explicit value/get/set, create with undefined
        if (!hadExistingProperty && !valueField.has_value() && !getField.has_value() && !setField.has_value()) {
          if (isIndexKey) {
            try {
              size_t idx = std::stoul(key);
              if (idx < arr->elements.size() + 1024) {
                arr->elements.resize(idx + 1, Value(Undefined{}));
              } else {
                arr->properties[key] = Value(Undefined{});
              }
            } catch (...) {
              arr->properties[key] = Value(Undefined{});
            }
          } else {
            arr->properties[key] = Value(Undefined{});
          }
        }

        // Handle writable descriptor
        if (writableField.has_value()) {
          if (!writableField->toBool()) {
            arr->properties["__non_writable_" + key] = Value(true);
          } else {
            arr->properties.erase("__non_writable_" + key);
          }
        }

        // Handle enumerable descriptor
        if (enumField.has_value()) {
          if (!enumField->toBool()) {
            arr->properties["__non_enum_" + key] = Value(true);
          } else {
            arr->properties.erase("__non_enum_" + key);
          }
        }

        // Handle configurable descriptor
        if (configField.has_value()) {
          if (!configField->toBool()) {
            arr->properties["__non_configurable_" + key] = Value(true);
          } else {
            arr->properties.erase("__non_configurable_" + key);
          }
        }

        // Defaults for new properties: unspecified attributes default to false
        if (!hadExistingProperty) {
          if (!isAccessorDescriptor && !writableField.has_value()) {
            arr->properties["__non_writable_" + key] = Value(true);
          }
          if (!enumField.has_value()) {
            arr->properties["__non_enum_" + key] = Value(true);
          }
          if (!configField.has_value()) {
            arr->properties["__non_configurable_" + key] = Value(true);
          }
        }

        // Conversion defaults: accessor<->data
        if (hadExistingProperty) {
          // Accessor -> Data: default writable to false
          if (currentIsAccessor && (valueField.has_value() || writableField.has_value()) && !isAccessorDescriptor) {
            if (!writableField.has_value()) {
              arr->properties["__non_writable_" + key] = Value(true);
            }
          }
          // Data -> Accessor: ensure get/set markers, remove writable
          if (!currentIsAccessor && isAccessorDescriptor && !valueField.has_value()) {
            arr->properties.erase("__non_writable_" + key);
            if (!getField.has_value() && arr->properties.find("__get_" + key) == arr->properties.end()) {
              arr->properties["__get_" + key] = Value(Undefined{});
            }
            if (!setField.has_value() && arr->properties.find("__set_" + key) == arr->properties.end()) {
              arr->properties["__set_" + key] = Value(Undefined{});
            }
          }
        }

        // Arguments exotic object: certain descriptor changes remove the parameter mapping.
        if (hadMappedArgumentsBinding) {
          if (makeNonWritable) {
            // Snapshot the current mapped value into the actual index property
            // before removing the mapping (value may have been updated via param assignment).
            if (!valueField.has_value()) {
              Value cur = getCurrentIndexValue();
              bool isNumeric = true;
              size_t idx = 0;
              try { idx = std::stoul(key); } catch (...) { isNumeric = false; }
              if (isNumeric && idx < arr->elements.size()) {
                arr->elements[idx] = cur;
              } else if (isNumeric) {
                arr->properties[key] = cur;
              }
            }
            arr->properties.erase("__mapped_arg_index_" + key + "__");
            arr->properties.erase("__get_" + key);
            arr->properties.erase("__set_" + key);
          }
        }
      } else if (args[0].isRegex()) {
        applyDescriptorToBag(args[0].getGC<Regex>()->properties);
      } else if (args[0].isError()) {
        applyDescriptorToBag(args[0].getGC<Error>()->properties);
      } else if (args[0].isClass()) {
        applyDescriptorToBag(args[0].getGC<Class>()->properties);
      } else if (args[0].isMap()) {
        applyDescriptorToBag(args[0].getGC<Map>()->properties);
      } else if (args[0].isSet()) {
        applyDescriptorToBag(args[0].getGC<Set>()->properties);
      } else if (args[0].isWeakMap()) {
        applyDescriptorToBag(args[0].getGC<WeakMap>()->properties);
      } else if (args[0].isWeakSet()) {
        applyDescriptorToBag(args[0].getGC<WeakSet>()->properties);
      } else if (args[0].isGenerator()) {
        applyDescriptorToBag(args[0].getGC<Function>()->properties);
      }
    }
    return args[0];
  };
  objectConstructor->properties["defineProperty"] = Value(objectDefineProperty);

  // Object.defineProperties - define multiple properties
  auto objectDefineProperties = GarbageCollector::makeGC<Function>();
  objectDefineProperties->isNative = true;
  objectDefineProperties->nativeFunc = [objectDefineProperty](const std::vector<Value>& args) -> Value {
    if (args.size() < 1) {
      throw std::runtime_error("TypeError: Object.defineProperties called on non-object");
    }
    if (args.size() < 2 || args[1].isUndefined() || args[1].isNull()) {
      throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
    }
    // Target must be an object
    const auto& target = args[0];
    if (!target.isObject() && !target.isFunction() && !target.isPromise() && !target.isRegex() &&
        !target.isArray() && !target.isTypedArray() && !target.isArrayBuffer() && !target.isDataView() &&
        !target.isError() && !target.isClass() && !target.isMap() && !target.isSet() &&
        !target.isWeakMap() && !target.isWeakSet() && !target.isGenerator() && !target.isProxy()) {
      throw std::runtime_error("TypeError: Object.defineProperties called on non-object");
    }
    // Iterate enumerable own properties of the descriptor object
    auto iterateOwnEnumerable = [&](const auto& properties) {
      for (const auto& [key, descriptor] : properties) {
        // Skip internal markers
        if (key.size() >= 4 && key.substr(0, 2) == "__" && key.substr(key.size() - 2) == "__") continue;
        if (key.rfind("__non_writable_", 0) == 0) continue;
        if (key.rfind("__non_enum_", 0) == 0) continue;
        if (key.rfind("__non_configurable_", 0) == 0) continue;
        if (key.rfind("__enum_", 0) == 0) continue;
        if (key.rfind("__get_", 0) == 0) continue;
        if (key.rfind("__set_", 0) == 0) continue;
        // Skip non-enumerable properties
        if (properties.find("__non_enum_" + key) != properties.end()) continue;
        objectDefineProperty->nativeFunc({target, Value(key), descriptor});
      }
    };
    if (args[1].isObject()) {
      iterateOwnEnumerable(args[1].getGC<Object>()->properties);
    } else if (args[1].isFunction()) {
      iterateOwnEnumerable(args[1].getGC<Function>()->properties);
    } else if (args[1].isArray()) {
      auto arr = args[1].getGC<Array>();
      for (size_t i = 0; i < arr->elements.size(); ++i) {
        objectDefineProperty->nativeFunc({target, Value(std::to_string(i)), arr->elements[i]});
      }
      iterateOwnEnumerable(arr->properties);
    }
    return args[0];
  };
  objectConstructor->properties["defineProperties"] = Value(objectDefineProperties);

  // Object.create - uses defineProperty for the properties argument
  {
    auto objectCreate = GarbageCollector::makeGC<Function>();
    objectCreate->isNative = true;
    objectCreate->nativeFunc = [objectDefineProperty](const std::vector<Value>& args) -> Value {
      if (args.empty()) {
        throw std::runtime_error("TypeError: Object prototype may only be an Object or null: undefined");
      }
      const Value& proto = args[0];
      // Step 1: proto must be Object or null
      bool isObjectLike = proto.isObject() || proto.isArray() || proto.isFunction() || proto.isRegex() ||
             proto.isProxy() || proto.isPromise() || proto.isGenerator() || proto.isClass() ||
             proto.isMap() || proto.isSet() || proto.isWeakMap() || proto.isWeakSet() ||
             proto.isTypedArray() || proto.isArrayBuffer() || proto.isDataView() || proto.isError();
      if (!isObjectLike && !proto.isNull()) {
        throw std::runtime_error("TypeError: Object prototype may only be an Object or null: " + proto.toString());
      }

      auto newObj = GarbageCollector::makeGC<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      if (isObjectLike) {
        newObj->properties["__proto__"] = proto;
      } else {
        // null prototype
        newObj->properties["__proto__"] = Value(Null{});
      }

      // Step 2: If Properties is present, process with defineProperty
      if (args.size() > 1 && !args[1].isUndefined()) {
        const Value& propsArg = args[1];
        // Properties must be coercible to object
        if (propsArg.isNull()) {
          throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
        }

        // Iterate own enumerable properties, invoking getters when needed
        auto iterateOwnEnumerable = [&](const auto& props, const Value& propsVal,
                                        std::function<void(const std::string&, const Value&)> callback) {
          for (const auto& [k, v] : props) {
            // Skip internal markers
            if (k.rfind("__non_writable_", 0) == 0 || k.rfind("__non_enum_", 0) == 0 ||
                k.rfind("__non_configurable_", 0) == 0 || k.rfind("__enum_", 0) == 0 ||
                k.rfind("__get_", 0) == 0 || k.rfind("__set_", 0) == 0) continue;
            if (k.size() >= 4 && k.substr(0, 2) == "__" && k.substr(k.size() - 2) == "__") continue;
            // Check if non-enumerable
            if (props.find("__non_enum_" + k) != props.end()) continue;
            // Check for getter - if present, invoke it to get the actual value
            auto getterIt = props.find("__get_" + k);
            if (getterIt != props.end() && getterIt->second.isFunction()) {
              auto* interp = getGlobalInterpreter();
              if (interp) {
                Value val = interp->callForHarness(getterIt->second, {}, propsVal);
                callback(k, val);
              }
            } else {
              callback(k, v);
            }
          }
        };

        auto applyDescriptor = [&](const std::string& propKey, const Value& descVal) {
          // Call defineProperty(newObj, propKey, descVal)
          objectDefineProperty->nativeFunc({Value(newObj), Value(propKey), descVal});
        };

        auto getPropsFromValue = [&](const Value& val) -> const auto* {
          if (val.isObject()) return static_cast<const void*>(&val.getGC<Object>()->properties);
          return static_cast<const void*>(nullptr);
        };

        auto doIterate = [&](const auto& props) {
          iterateOwnEnumerable(props, propsArg, [&](const std::string& k, const Value& v) {
            applyDescriptor(k, v);
          });
        };

        if (propsArg.isObject()) {
          doIterate(propsArg.getGC<Object>()->properties);
        } else if (propsArg.isFunction()) {
          doIterate(propsArg.getGC<Function>()->properties);
        } else if (propsArg.isArray()) {
          doIterate(propsArg.getGC<Array>()->properties);
        } else if (propsArg.isError()) {
          doIterate(propsArg.getGC<Error>()->properties);
        } else if (propsArg.isRegex()) {
          doIterate(propsArg.getGC<Regex>()->properties);
        } else if (propsArg.isClass()) {
          doIterate(propsArg.getGC<Class>()->properties);
        } else if (propsArg.isMap()) {
          doIterate(propsArg.getGC<Map>()->properties);
        } else if (propsArg.isSet()) {
          doIterate(propsArg.getGC<Set>()->properties);
        } else if (propsArg.isPromise()) {
          doIterate(propsArg.getGC<Promise>()->properties);
        }
        // Primitives other than undefined are silently ignored
      }

      return Value(newObj);
    };
    objectConstructor->properties["create"] = Value(objectCreate);
  }

  // Set name/length on all Object static methods that don't already have them
  {
    struct ObjMethodInfo { std::string name; int length; };
    std::vector<ObjMethodInfo> objectMethods = {
      {"assign", 2}, {"create", 2}, {"defineProperty", 3}, {"defineProperties", 2},
      {"freeze", 1}, {"fromEntries", 1}, {"getOwnPropertyDescriptor", 2},
      {"getOwnPropertyDescriptors", 1}, {"getOwnPropertyNames", 1},
      {"getOwnPropertySymbols", 1}, {"getPrototypeOf", 1}, {"hasOwn", 2},
      {"is", 2}, {"isExtensible", 1}, {"isFrozen", 1}, {"isSealed", 1},
      {"preventExtensions", 1}, {"seal", 1}, {"setPrototypeOf", 2},
    };
    for (const auto& m : objectMethods) {
      auto it = objectConstructor->properties.find(m.name);
      if (it != objectConstructor->properties.end() && it->second.isFunction()) {
        auto fn = it->second.getGC<Function>();
        if (fn->properties.find("name") == fn->properties.end()) {
          fn->properties["name"] = Value(m.name);
          fn->properties["__non_writable_name"] = Value(true);
          fn->properties["__non_enum_name"] = Value(true);
        }
        if (fn->properties.find("length") == fn->properties.end()) {
          fn->properties["length"] = Value(static_cast<double>(m.length));
          fn->properties["__non_writable_length"] = Value(true);
          fn->properties["__non_enum_length"] = Value(true);
        }
      }
    }
  }

  env->define("Object", Value(objectConstructor));

  // Math object
  auto mathObj = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // Math[Symbol.toStringTag] = "Math"
  mathObj->properties[WellKnownSymbols::toStringTagKey()] = Value(std::string("Math"));
  mathObj->properties["__non_writable_" + WellKnownSymbols::toStringTagKey()] = Value(true);
  mathObj->properties["__non_enum_" + WellKnownSymbols::toStringTagKey()] = Value(true);

  // Math constants
  auto defineMathConst = [&](const std::string& name, double value) {
    mathObj->properties[name] = Value(value);
    mathObj->properties["__non_writable_" + name] = Value(true);
    mathObj->properties["__non_enum_" + name] = Value(true);
    mathObj->properties["__non_configurable_" + name] = Value(true);
  };
  defineMathConst("PI", 3.141592653589793);
  defineMathConst("E", 2.718281828459045);
  defineMathConst("LN2", 0.6931471805599453);
  defineMathConst("LN10", 2.302585092994046);
  defineMathConst("LOG2E", 1.4426950408889634);
  defineMathConst("LOG10E", 0.4342944819032518);
  defineMathConst("SQRT1_2", 0.7071067811865476);
  defineMathConst("SQRT2", 1.4142135623730951);

  // Math methods - use registerMathFn for proper name/length/non-enum
  // (registerMathFn is defined below, so forward-reference via lambda)
  auto registerMathMethod = [&](const std::string& name, std::function<Value(const std::vector<Value>&)> fn, int length = 1) {
    auto f = GarbageCollector::makeGC<Function>();
    f->isNative = true;
    f->nativeFunc = fn;
    f->properties["name"] = Value(name);
    f->properties["length"] = Value(static_cast<double>(length));
    f->properties["__non_writable_name"] = Value(true);
    f->properties["__non_enum_name"] = Value(true);
    f->properties["__non_writable_length"] = Value(true);
    f->properties["__non_enum_length"] = Value(true);
    mathObj->properties[name] = Value(f);
    mathObj->properties["__non_enum_" + name] = Value(true);
  };
  registerMathMethod("abs", Math_abs);
  registerMathMethod("ceil", Math_ceil);
  registerMathMethod("floor", Math_floor);
  registerMathMethod("round", Math_round);

  registerMathMethod("trunc", Math_trunc);
  registerMathMethod("max", Math_max, 2);
  registerMathMethod("min", Math_min, 2);
  registerMathMethod("pow", Math_pow, 2);
  registerMathMethod("sqrt", Math_sqrt);
  registerMathMethod("sin", Math_sin);
  registerMathMethod("cos", Math_cos);
  registerMathMethod("tan", Math_tan);
  registerMathMethod("random", Math_random, 0);
  registerMathMethod("sign", Math_sign);
  registerMathMethod("log", Math_log);
  registerMathMethod("log10", Math_log10);
  registerMathMethod("exp", Math_exp);
  registerMathMethod("cbrt", Math_cbrt);
  registerMathMethod("log2", Math_log2);
  registerMathMethod("hypot", Math_hypot, 2);
  registerMathMethod("expm1", Math_expm1);
  registerMathMethod("log1p", Math_log1p);
  registerMathMethod("fround", Math_fround);
  registerMathMethod("clz32", Math_clz32);
  registerMathMethod("imul", Math_imul, 2);
  registerMathMethod("asin", Math_asin);
  registerMathMethod("acos", Math_acos);
  registerMathMethod("atan", Math_atan);
  registerMathMethod("atan2", Math_atan2, 2);
  registerMathMethod("sinh", Math_sinh);
  registerMathMethod("cosh", Math_cosh);
  registerMathMethod("tanh", Math_tanh);
  registerMathMethod("asinh", Math_asinh);
  registerMathMethod("acosh", Math_acosh);
  registerMathMethod("atanh", Math_atanh);
  registerMathMethod("f16round", Math_f16round);
  registerMathMethod("sumPrecise", Math_sumPrecise);

  // Math's [[Prototype]] is Object.prototype
  if (auto objProtoVal = env->get("__object_prototype__"); objProtoVal) {
    mathObj->properties["__proto__"] = *objProtoVal;
  }
  env->define("Math", Value(mathObj));

  // Date constructor
  auto dateConstructor = GarbageCollector::makeGC<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  dateConstructor->isNative = true;
  dateConstructor->isConstructor = true;
  dateConstructor->properties["name"] = Value(std::string("Date"));
  dateConstructor->properties["length"] = Value(1.0);
  // `Date()` called as a function returns a string (not a Date object).
  // Construction is handled via `__native_construct__` below.
  dateConstructor->nativeFunc = [](const std::vector<Value>&) -> Value {
    return Value(std::string("[object Date]"));
  };

  auto dateConstruct = GarbageCollector::makeGC<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  dateConstruct->isNative = true;
  dateConstruct->isConstructor = true;
  dateConstruct->nativeFunc = [toPrimitive](const std::vector<Value>& args) -> Value {
    if (args.size() == 1) {
      Value primitive = toPrimitive(args[0], false);
      if (primitive.isSymbol() || primitive.isBigInt()) {
        throw std::runtime_error("TypeError: Cannot convert to number");
      }
      if (primitive.isString()) {
        return Date_constructor({primitive});
      }
      return Date_constructor({Value(primitive.toNumber())});
    }
    return Date_constructor(args);
  };
  dateConstructor->properties["__native_construct__"] = Value(dateConstruct);

  {
    auto datePrototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    dateConstructor->properties["prototype"] = Value(datePrototype);
    dateConstructor->properties["__non_writable_prototype"] = Value(true);
    dateConstructor->properties["__non_enum_prototype"] = Value(true);
    dateConstructor->properties["__non_configurable_prototype"] = Value(true);
    datePrototype->properties["constructor"] = Value(dateConstructor);
    datePrototype->properties["__non_enum_constructor"] = Value(true);
    datePrototype->properties["name"] = Value(std::string("Date"));
    datePrototype->properties["length"] = Value(0.0);
    datePrototype->properties["__proto__"] = Value(objectPrototype);

    auto installDateMethod =
      [&](const std::string& name,
          double length,
          std::function<Value(const std::vector<Value>&)> nativeFunc) {
        auto fn = GarbageCollector::makeGC<Function>();
        fn->isNative = true;
        fn->isConstructor = false;
        fn->properties["name"] = Value(name);
        fn->properties["length"] = Value(length);
        fn->properties["__non_writable_name"] = Value(true);
        fn->properties["__non_enum_name"] = Value(true);
        fn->properties["__non_writable_length"] = Value(true);
        fn->properties["__non_enum_length"] = Value(true);
        fn->properties["__uses_this_arg__"] = Value(true);
        fn->nativeFunc = std::move(nativeFunc);
        datePrototype->properties[name] = Value(fn);
        datePrototype->properties["__non_enum_" + name] = Value(true);
      };
    auto installDateStub =
      [&](const std::string& name, double length) {
        installDateMethod(name, length, [](const std::vector<Value>&) -> Value {
          return Value(Undefined{});
        });
      };

    installDateMethod("toString", 0.0, Date_toString);
    installDateMethod("valueOf", 0.0, Date_getTime);
    installDateMethod("getTime", 0.0, Date_getTime);
    installDateMethod("getFullYear", 0.0, Date_getFullYear);
    installDateMethod("getMonth", 0.0, Date_getMonth);
    installDateMethod("getDate", 0.0, Date_getDate);
    installDateMethod("getDay", 0.0, Date_getDay);
    installDateMethod("getHours", 0.0, Date_getHours);
    installDateMethod("getMinutes", 0.0, Date_getMinutes);
    installDateMethod("getSeconds", 0.0, Date_getSeconds);
    installDateMethod("toISOString", 0.0, Date_toISOString);

    installDateStub("getUTCFullYear", 0.0);
    installDateStub("getUTCMonth", 0.0);
    installDateStub("getUTCDate", 0.0);
    installDateStub("getUTCDay", 0.0);
    installDateStub("getUTCHours", 0.0);
    installDateStub("getUTCMinutes", 0.0);
    installDateStub("getUTCSeconds", 0.0);
    installDateStub("getMilliseconds", 0.0);
    installDateStub("getUTCMilliseconds", 0.0);
    installDateStub("setTime", 1.0);
    installDateStub("setMilliseconds", 1.0);
    installDateStub("setUTCMilliseconds", 1.0);
    installDateStub("setSeconds", 2.0);
    installDateStub("setUTCSeconds", 2.0);
    installDateStub("setMinutes", 3.0);
    installDateStub("setUTCMinutes", 3.0);
    installDateStub("setHours", 4.0);
    installDateStub("setUTCHours", 4.0);
    installDateStub("setDate", 1.0);
    installDateStub("setUTCDate", 1.0);
    installDateStub("setMonth", 2.0);
    installDateStub("setUTCMonth", 2.0);
    installDateStub("setFullYear", 3.0);
    installDateStub("setUTCFullYear", 3.0);
    installDateStub("toLocaleString", 0.0);
    installDateStub("toUTCString", 0.0);
    installDateStub("getTimezoneOffset", 0.0);
    installDateStub("toTimeString", 0.0);
    installDateStub("toDateString", 0.0);
    installDateStub("toLocaleDateString", 0.0);
    installDateStub("toLocaleTimeString", 0.0);
    installDateStub("toJSON", 1.0);
  }

  // Date static methods
  auto dateNow = GarbageCollector::makeGC<Function>();
  dateNow->isNative = true;
  dateNow->nativeFunc = Date_now;
  dateConstructor->properties["now"] = Value(dateNow);
  dateConstructor->properties["__non_enum_now"] = Value(true);

  auto dateParse = GarbageCollector::makeGC<Function>();
  dateParse->isNative = true;
  dateParse->nativeFunc = Date_parse;
  dateConstructor->properties["parse"] = Value(dateParse);
  dateConstructor->properties["__non_enum_parse"] = Value(true);

  auto dateUTC = GarbageCollector::makeGC<Function>();
  dateUTC->isNative = true;
  dateUTC->nativeFunc = [](const std::vector<Value>&) -> Value {
    return Value(0.0);
  };
  dateConstructor->properties["UTC"] = Value(dateUTC);
  dateConstructor->properties["__non_enum_UTC"] = Value(true);

  env->define("Date", Value(dateConstructor));

  // String constructor with static methods
  auto stringConstructorFn = GarbageCollector::makeGC<Function>();
  stringConstructorFn->isNative = true;
  stringConstructorFn->isConstructor = true;
  stringConstructorFn->properties["__wrap_primitive__"] = Value(true);
  stringConstructorFn->nativeFunc = [toPrimitive](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(std::string(""));
    }
    Value primitive = toPrimitive(args[0], true);
    return Value(primitive.toString());
  };

  // Wrap in an Object to hold static methods
  auto stringConstructorObj = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // The constructor itself
  stringConstructorObj->properties["__callable_object__"] = Value(true);
  stringConstructorObj->properties["__constructor_wrapper__"] = Value(true);
  stringConstructorObj->properties["constructor"] = Value(stringConstructorFn);
  stringConstructorObj->properties["length"] = Value(1.0);
  stringConstructorObj->properties["__non_writable_length"] = Value(true);
  stringConstructorObj->properties["__non_enum_length"] = Value(true);
  stringConstructorObj->properties["name"] = Value(std::string("String"));
  stringConstructorObj->properties["__non_writable_name"] = Value(true);
  stringConstructorObj->properties["__non_enum_name"] = Value(true);

  // String.fromCharCode
  auto fromCharCode = GarbageCollector::makeGC<Function>();
  fromCharCode->isNative = true;
  fromCharCode->isConstructor = false;
  fromCharCode->properties["name"] = Value(std::string("fromCharCode"));
  fromCharCode->properties["length"] = Value(1.0);
  fromCharCode->properties["__non_writable_name"] = Value(true);
  fromCharCode->properties["__non_enum_name"] = Value(true);
  fromCharCode->properties["__non_writable_length"] = Value(true);
  fromCharCode->properties["__non_enum_length"] = Value(true);
  fromCharCode->properties["__throw_on_new__"] = Value(true);
  fromCharCode->nativeFunc = String_fromCharCode;
  stringConstructorObj->properties["fromCharCode"] = Value(fromCharCode);
  stringConstructorObj->properties["__non_enum_fromCharCode"] = Value(true);

  // String.fromCodePoint
  auto fromCodePoint = GarbageCollector::makeGC<Function>();
  fromCodePoint->isNative = true;
  fromCodePoint->isConstructor = false;
  fromCodePoint->properties["name"] = Value(std::string("fromCodePoint"));
  fromCodePoint->properties["length"] = Value(1.0);
  fromCodePoint->properties["__non_writable_name"] = Value(true);
  fromCodePoint->properties["__non_enum_name"] = Value(true);
  fromCodePoint->properties["__non_writable_length"] = Value(true);
  fromCodePoint->properties["__non_enum_length"] = Value(true);
  fromCodePoint->properties["__throw_on_new__"] = Value(true);
  fromCodePoint->nativeFunc = String_fromCodePoint;
  stringConstructorObj->properties["fromCodePoint"] = Value(fromCodePoint);
  stringConstructorObj->properties["__non_enum_fromCodePoint"] = Value(true);

  // String.raw
  auto stringRaw = GarbageCollector::makeGC<Function>();
  stringRaw->isNative = true;
  stringRaw->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || (!args[0].isObject() && !args[0].isArray())) {
      return Value(std::string(""));
    }
    // Get the template object's raw property
    GCPtr<Array> rawArr;
    if (args[0].isObject()) {
      auto obj = args[0].getGC<Object>();
      auto rawIt = obj->properties.find("raw");
      if (rawIt != obj->properties.end() && rawIt->second.isArray()) {
        rawArr = rawIt->second.getGC<Array>();
      }
    }
    if (!rawArr) return Value(std::string(""));

    std::string result;
    size_t literalCount = rawArr->elements.size();
    for (size_t i = 0; i < literalCount; i++) {
      result += rawArr->elements[i].toString();
      if (i + 1 < literalCount && i + 1 < args.size()) {
        result += args[i + 1].toString();
      }
    }
    return Value(result);
  };
  stringConstructorObj->properties["raw"] = Value(stringRaw);

  auto stringPrototype = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // thisToString: Coerce this (args[0]) to string per ES spec §21.1.3
  // RequireObjectCoercible(this) then ToString(this)
  auto thisToString = [toPrimitive](const std::vector<Value>& args, const char* methodName) -> std::string {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
      throw std::runtime_error(std::string("TypeError: String.prototype.") + methodName + " called on null or undefined");
    }
    if (args[0].isString()) return std::get<std::string>(args[0].data);
    if (args[0].isSymbol()) {
      throw std::runtime_error("TypeError: Cannot convert a Symbol value to a string");
    }
    if (isObjectLikeValue(args[0])) {
      Value prim = toPrimitive(args[0], true);
      return prim.toString();
    }
    return args[0].toString();
  };

  auto installStringPrototypeMethod =
      [&](const std::string& name,
          int length,
          std::function<Value(const std::vector<Value>&)> nativeFunc,
          bool coerceThis = false) {
        auto fn = GarbageCollector::makeGC<Function>();
        fn->isNative = true;
        fn->isConstructor = false;
        fn->properties["name"] = Value(name);
        fn->properties["length"] = Value(static_cast<double>(length));
        fn->properties["__non_writable_name"] = Value(true);
        fn->properties["__non_enum_name"] = Value(true);
        fn->properties["__non_writable_length"] = Value(true);
        fn->properties["__non_enum_length"] = Value(true);
        fn->properties["__uses_this_arg__"] = Value(true);
        fn->properties["__throw_on_new__"] = Value(true);
        if (coerceThis) {
          // Wrap to coerce this (args[0]) to string before calling the real function
          fn->nativeFunc = [nativeFunc, thisToString, name](const std::vector<Value>& args) -> Value {
            std::string str = thisToString(args, name.c_str());
            std::vector<Value> newArgs;
            newArgs.reserve(args.size());
            newArgs.push_back(Value(str));
            for (size_t i = 1; i < args.size(); i++) {
              newArgs.push_back(args[i]);
            }
            return nativeFunc(newArgs);
          };
        } else {
          fn->nativeFunc = std::move(nativeFunc);
        }
        stringPrototype->properties[name] = Value(fn);
        stringPrototype->properties["__non_enum_" + name] = Value(true);
      };

  installStringPrototypeMethod("charAt", 1, String_charAt, true);
  installStringPrototypeMethod("charCodeAt", 1, String_charCodeAt, true);
  installStringPrototypeMethod("codePointAt", 1, String_codePointAt, true);
  installStringPrototypeMethod("at", 1, String_at, true);
  installStringPrototypeMethod("toString", 0, [](const std::vector<Value>& args) -> Value {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
      throw std::runtime_error("TypeError: String.prototype.toString called on null or undefined");
    }
    if (args[0].isString()) return args[0];
    if (args[0].isObject()) {
      auto obj = args[0].getGC<Object>();
      auto primIt = obj->properties.find("__primitive_value__");
      if (primIt != obj->properties.end() && primIt->second.isString()) {
        return primIt->second;
      }
    }
    throw std::runtime_error("TypeError: String.prototype.toString requires a String");
  });
  installStringPrototypeMethod("valueOf", 0, [](const std::vector<Value>& args) -> Value {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
      throw std::runtime_error("TypeError: String.prototype.valueOf called on null or undefined");
    }
    if (args[0].isString()) return args[0];
    if (args[0].isObject()) {
      auto obj = args[0].getGC<Object>();
      auto primIt = obj->properties.find("__primitive_value__");
      if (primIt != obj->properties.end() && primIt->second.isString()) {
        return primIt->second;
      }
    }
    throw std::runtime_error("TypeError: String.prototype.valueOf requires a String");
  });
  installStringPrototypeMethod("indexOf", 1, String_indexOf, true);
  installStringPrototypeMethod("lastIndexOf", 1, String_lastIndexOf, true);
  installStringPrototypeMethod("split", 2, String_split, true);
  installStringPrototypeMethod("substring", 2, String_substring, true);
  installStringPrototypeMethod("toLowerCase", 0, String_toLowerCase, true);
  installStringPrototypeMethod("toUpperCase", 0, String_toUpperCase, true);
  installStringPrototypeMethod("toLocaleLowerCase", 0, String_toLowerCase, true);
  installStringPrototypeMethod("toLocaleUpperCase", 0, String_toUpperCase, true);
  installStringPrototypeMethod("localeCompare", 1, [thisToString](const std::vector<Value>& args) -> Value {
    std::string self = thisToString(args, "localeCompare");
    std::string that = args.size() > 1 ? args[1].toString() : "";
    if (self < that) return Value(-1.0);
    if (self > that) return Value(1.0);
    return Value(0.0);
  }, false);
  installStringPrototypeMethod("toLocaleString", 0, [toPrimitive](const std::vector<Value>& args) -> Value {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
      throw std::runtime_error("TypeError: String.prototype.toLocaleString called on null or undefined");
    }
    Value primitive = isObjectLikeValue(args[0]) ? toPrimitive(args[0], true) : args[0];
    if (primitive.isSymbol()) {
      throw std::runtime_error("TypeError: Cannot convert Symbol to string");
    }
    return Value(primitive.toString());
  });
  installStringPrototypeMethod("trim", 0, [thisToString](const std::vector<Value>& args) -> Value {
    return Value(stripESWhitespace(thisToString(args, "trim")));
  }, false);  // trim already handles this conversion via its own thisToString capture
  installStringPrototypeMethod("trimStart", 0, [thisToString](const std::vector<Value>& args) -> Value {
    return Value(stripLeadingESWhitespace(thisToString(args, "trimStart")));
  }, false);
  installStringPrototypeMethod("trimEnd", 0, [thisToString](const std::vector<Value>& args) -> Value {
    return Value(stripTrailingESWhitespace(thisToString(args, "trimEnd")));
  }, false);

  // String.prototype.slice
  installStringPrototypeMethod("slice", 2, String_slice, true);

  // String.prototype.substr
  installStringPrototypeMethod("substr", 2, String_substr, true);

  // String.prototype.replace
  installStringPrototypeMethod("replace", 2, String_replace, true);

  // String.prototype.concat
  installStringPrototypeMethod("concat", 1, [thisToString](const std::vector<Value>& args) -> Value {
    std::string result = thisToString(args, "concat");
    for (size_t i = 1; i < args.size(); ++i) {
      result += args[i].toString();
    }
    return Value(result);
  }, false);

  // String.prototype.startsWith
  installStringPrototypeMethod("startsWith", 1, [thisToString](const std::vector<Value>& args) -> Value {
    std::string str = thisToString(args, "startsWith");
    if (args.size() < 2) return Value(false);
    // Check if arg is regex - throw TypeError
    if (args[1].isRegex()) {
      throw std::runtime_error("TypeError: First argument to String.prototype.startsWith must not be a regular expression");
    }
    std::string searchStr = args[1].toString();
    size_t pos = 0;
    if (args.size() > 2 && !args[2].isUndefined()) {
      double p = args[2].toNumber();
      if (std::isnan(p)) p = 0;
      pos = static_cast<size_t>(std::max(0.0, std::min(p, static_cast<double>(str.size()))));
    }
    if (pos + searchStr.size() > str.size()) return Value(false);
    return Value(str.compare(pos, searchStr.size(), searchStr) == 0);
  }, false);

  // String.prototype.endsWith
  installStringPrototypeMethod("endsWith", 1, [thisToString](const std::vector<Value>& args) -> Value {
    std::string str = thisToString(args, "endsWith");
    if (args.size() < 2) return Value(false);
    if (args[1].isRegex()) {
      throw std::runtime_error("TypeError: First argument to String.prototype.endsWith must not be a regular expression");
    }
    std::string searchStr = args[1].toString();
    size_t endPos = str.size();
    if (args.size() > 2 && !args[2].isUndefined()) {
      double e = args[2].toNumber();
      if (std::isnan(e)) e = 0;
      endPos = static_cast<size_t>(std::max(0.0, std::min(e, static_cast<double>(str.size()))));
    }
    if (searchStr.size() > endPos) return Value(false);
    return Value(str.compare(endPos - searchStr.size(), searchStr.size(), searchStr) == 0);
  }, false);

  // String.prototype.includes
  installStringPrototypeMethod("includes", 1, [thisToString](const std::vector<Value>& args) -> Value {
    std::string str = thisToString(args, "includes");
    if (args.size() < 2) return Value(false);
    if (args[1].isRegex()) {
      throw std::runtime_error("TypeError: First argument to String.prototype.includes must not be a regular expression");
    }
    std::string searchStr = args[1].toString();
    size_t pos = 0;
    if (args.size() > 2 && !args[2].isUndefined()) {
      double p = args[2].toNumber();
      if (!std::isnan(p)) pos = static_cast<size_t>(std::max(0.0, p));
    }
    return Value(str.find(searchStr, pos) != std::string::npos);
  }, false);

  // String.prototype.repeat
  installStringPrototypeMethod("repeat", 1, [thisToString](const std::vector<Value>& args) -> Value {
    std::string str = thisToString(args, "repeat");
    double count = args.size() > 1 ? args[1].toNumber() : 0;
    if (std::isnan(count) || count < 0 || count == std::numeric_limits<double>::infinity()) {
      throw std::runtime_error("RangeError: Invalid count value");
    }
    size_t n = static_cast<size_t>(count);
    // Guard against OOM: cap result at 256MB
    static constexpr size_t kMaxRepeatBytes = 256 * 1024 * 1024;
    if (str.size() > 0 && n > kMaxRepeatBytes / str.size()) {
      throw std::runtime_error("RangeError: Invalid count value");
    }
    std::string result;
    result.reserve(str.size() * n);
    for (size_t i = 0; i < n; ++i) result += str;
    return Value(result);
  }, false);

  // String.prototype.padStart
  installStringPrototypeMethod("padStart", 1, [thisToString](const std::vector<Value>& args) -> Value {
    std::string str = thisToString(args, "padStart");
    double maxLen = args.size() > 1 ? args[1].toNumber() : 0;
    if (std::isnan(maxLen)) maxLen = 0;
    size_t targetLen = static_cast<size_t>(maxLen);
    if (targetLen <= str.size()) return Value(str);
    std::string filler = args.size() > 2 && !args[2].isUndefined() ? args[2].toString() : " ";
    if (filler.empty()) return Value(str);
    size_t padLen = targetLen - str.size();
    std::string padding;
    while (padding.size() < padLen) padding += filler;
    padding = padding.substr(0, padLen);
    return Value(padding + str);
  }, false);

  // String.prototype.padEnd
  installStringPrototypeMethod("padEnd", 1, [thisToString](const std::vector<Value>& args) -> Value {
    std::string str = thisToString(args, "padEnd");
    double maxLen = args.size() > 1 ? args[1].toNumber() : 0;
    if (std::isnan(maxLen)) maxLen = 0;
    size_t targetLen = static_cast<size_t>(maxLen);
    if (targetLen <= str.size()) return Value(str);
    std::string filler = args.size() > 2 && !args[2].isUndefined() ? args[2].toString() : " ";
    if (filler.empty()) return Value(str);
    size_t padLen = targetLen - str.size();
    std::string padding;
    while (padding.size() < padLen) padding += filler;
    padding = padding.substr(0, padLen);
    return Value(str + padding);
  }, false);

  // String.prototype.search
  installStringPrototypeMethod("search", 1, [thisToString](const std::vector<Value>& args) -> Value {
    std::string str = thisToString(args, "search");
    if (args.size() < 2) return Value(-1.0);
    if (args[1].isRegex()) {
      // Use std::regex for matching
      auto rx = args[1].getGC<Regex>();
#if USE_SIMPLE_REGEX
      // Simple regex: use string find as fallback for now
      auto pos = str.find(rx->pattern);
      return Value(pos != std::string::npos ? static_cast<double>(pos) : -1.0);
#else
      std::smatch m;
      if (std::regex_search(str, m, rx->regex)) {
        return Value(static_cast<double>(m.position(0)));
      }
      return Value(-1.0);
#endif
    }
    std::string searchStr = args[1].toString();
    auto pos = str.find(searchStr);
    return Value(pos != std::string::npos ? static_cast<double>(pos) : -1.0);
  }, false);

  // String.prototype.match — regex matching delegated to evaluateMember in interpreter
  // Just register a basic version here for non-regex cases
  installStringPrototypeMethod("match", 1, [thisToString](const std::vector<Value>& args) -> Value {
    std::string str = thisToString(args, "match");
    if (args.size() < 2 || args[1].isUndefined()) {
      auto result = makeArrayWithPrototype();
      result->elements.push_back(Value(std::string("")));
      result->properties["index"] = Value(0.0);
      result->properties["input"] = Value(str);
      return Value(result);
    }
    if (args[1].isRegex()) {
      auto rx = args[1].getGC<Regex>();
      bool global = rx->flags.find('g') != std::string::npos;
#if USE_SIMPLE_REGEX
      (void)global;
      auto pos = str.find(rx->pattern);
      if (pos == std::string::npos) return Value(Null{});
      auto result = makeArrayWithPrototype();
      result->elements.push_back(Value(rx->pattern));
      result->properties["index"] = Value(static_cast<double>(pos));
      result->properties["input"] = Value(str);
      return Value(result);
#else
      if (!global) {
        std::smatch m;
        if (!std::regex_search(str, m, rx->regex)) return Value(Null{});
        auto result = makeArrayWithPrototype();
        result->elements.push_back(Value(m.str(0)));
        for (size_t i = 1; i < m.size(); ++i) {
          if (m[i].matched) result->elements.push_back(Value(m.str(i)));
          else result->elements.push_back(Value(Undefined{}));
        }
        result->properties["index"] = Value(static_cast<double>(m.position(0)));
        result->properties["input"] = Value(str);
        return Value(result);
      }
      // Global: all matches
      auto result = makeArrayWithPrototype();
      auto begin = std::sregex_iterator(str.begin(), str.end(), rx->regex);
      auto end = std::sregex_iterator();
      for (auto it = begin; it != end; ++it) {
        result->elements.push_back(Value(it->str()));
      }
      if (result->elements.empty()) return Value(Null{});
      return Value(result);
#endif
    }
    std::string searchStr = args[1].toString();
    auto pos = str.find(searchStr);
    if (pos == std::string::npos) return Value(Null{});
    auto result = makeArrayWithPrototype();
    result->elements.push_back(Value(searchStr));
    result->properties["index"] = Value(static_cast<double>(pos));
    result->properties["input"] = Value(str);
    return Value(result);
  }, false);

  // String.prototype.normalize
  installStringPrototypeMethod("normalize", 0, [thisToString](const std::vector<Value>& args) -> Value {
    std::string str = thisToString(args, "normalize");
    // Basic implementation - just return the string (proper Unicode normalization is complex)
    if (args.size() > 1 && !args[1].isUndefined()) {
      std::string form = args[1].toString();
      if (form != "NFC" && form != "NFD" && form != "NFKC" && form != "NFKD") {
        throw std::runtime_error("RangeError: The normalization form should be one of NFC, NFD, NFKC, NFKD.");
      }
    }
    return Value(str);
  }, false);

  // String.prototype.replaceAll
  installStringPrototypeMethod("replaceAll", 2, [thisToString](const std::vector<Value>& args) -> Value {
    std::string str = thisToString(args, "replaceAll");
    if (args.size() < 2) return Value(str);
    if (args[1].isRegex()) {
      auto rx = args[1].getGC<Regex>();
      if (rx->flags.find('g') == std::string::npos) {
        throw std::runtime_error("TypeError: String.prototype.replaceAll called with a non-global RegExp argument");
      }
      // Delegate to replace for global regex
      return String_replace(args);
    }
    std::string searchStr = args[1].toString();
    std::string replaceStr = args.size() > 2 ? args[2].toString() : "undefined";
    if (searchStr.empty()) {
      // Insert between every char
      std::string result;
      result += replaceStr;
      for (size_t i = 0; i < str.size(); ++i) {
        result += str[i];
        result += replaceStr;
      }
      return Value(result);
    }
    std::string result;
    size_t pos = 0;
    while (true) {
      size_t found = str.find(searchStr, pos);
      if (found == std::string::npos) {
        result += str.substr(pos);
        break;
      }
      result += str.substr(pos, found - pos);
      result += replaceStr;
      pos = found + searchStr.size();
    }
    return Value(result);
  }, false);

  {
    const auto& iterKey = WellKnownSymbols::iteratorKey();
    auto stringProtoIterator = GarbageCollector::makeGC<Function>();
    stringProtoIterator->isNative = true;
    stringProtoIterator->isConstructor = false;
    stringProtoIterator->properties["name"] = Value(std::string("[Symbol.iterator]"));
    stringProtoIterator->properties["length"] = Value(0.0);
    stringProtoIterator->properties["__non_writable_name"] = Value(true);
    stringProtoIterator->properties["__non_enum_name"] = Value(true);
    stringProtoIterator->properties["__non_writable_length"] = Value(true);
    stringProtoIterator->properties["__non_enum_length"] = Value(true);
    stringProtoIterator->properties["__uses_this_arg__"] = Value(true);
    stringProtoIterator->properties["__throw_on_new__"] = Value(true);
    stringProtoIterator->nativeFunc = String_iterator;
    stringPrototype->properties[iterKey] = Value(stringProtoIterator);
    stringPrototype->properties["__non_enum_" + iterKey] = Value(true);
  }

  auto stringMatchAll = GarbageCollector::makeGC<Function>();
  stringMatchAll->isNative = true;
  stringMatchAll->isConstructor = false;
  stringMatchAll->properties["name"] = Value(std::string("matchAll"));
  stringMatchAll->properties["length"] = Value(1.0);
  stringMatchAll->properties["__uses_this_arg__"] = Value(true);
  stringMatchAll->properties["__throw_on_new__"] = Value(true);
  stringMatchAll->nativeFunc = [regExpPrototype](const std::vector<Value>& args) -> Value {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
      throw std::runtime_error("TypeError: String.prototype.matchAll called on null or undefined");
    }

    Value thisValue = args[0];
    Value regexp = args.size() > 1 ? args[1] : Value(Undefined{});
    Interpreter* interpreter = getGlobalInterpreter();
    const std::string& matchAllKey = WellKnownSymbols::matchAllKey();

    auto callChecked = [&](const Value& callee,
                           const std::vector<Value>& callArgs,
                           const Value& thisArg) -> Value {
      if (!callee.isFunction()) {
        return Value(Undefined{});
      }

      if (interpreter) {
        Value out = interpreter->callForHarness(callee, callArgs, thisArg);
        if (interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw std::runtime_error(err.toString());
        }
        return out;
      }

      auto fn = callee.getGC<Function>();
      if (fn->isNative) {
        auto itUsesThis = fn->properties.find("__uses_this_arg__");
        if (itUsesThis != fn->properties.end() && itUsesThis->second.isBool() && itUsesThis->second.toBool()) {
          std::vector<Value> nativeArgs;
          nativeArgs.reserve(callArgs.size() + 1);
          nativeArgs.push_back(thisArg);
          nativeArgs.insert(nativeArgs.end(), callArgs.begin(), callArgs.end());
          return fn->nativeFunc(nativeArgs);
        }
        return fn->nativeFunc(callArgs);
      }

      return Value(Undefined{});
    };

    auto getObjectValue = [&](const GCPtr<Object>& obj,
                              const Value& thisArg,
                              const std::string& key) -> Value {
      std::string getterName = "__get_" + key;
      auto getterIt = obj->properties.find(getterName);
      if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
        return callChecked(getterIt->second, {}, thisArg);
      }

      auto it = obj->properties.find(key);
      if (it != obj->properties.end()) {
        return it->second;
      }
      return Value(Undefined{});
    };

    auto getRegexValue = [&](const GCPtr<Regex>& regex,
                             const std::string& key) -> Value {
      Value regexValue(regex);

      std::string getterName = "__get_" + key;
      auto getterIt = regex->properties.find(getterName);
      if (getterIt != regex->properties.end() && getterIt->second.isFunction()) {
        return callChecked(getterIt->second, {}, regexValue);
      }

      auto it = regex->properties.find(key);
      if (it != regex->properties.end()) {
        return it->second;
      }

      if (regExpPrototype) {
        std::string protoGetterName = "__get_" + key;
        auto protoGetterIt = regExpPrototype->properties.find(protoGetterName);
        if (protoGetterIt != regExpPrototype->properties.end() && protoGetterIt->second.isFunction()) {
          return callChecked(protoGetterIt->second, {}, regexValue);
        }

        auto protoIt = regExpPrototype->properties.find(key);
        if (protoIt != regExpPrototype->properties.end()) {
          return protoIt->second;
        }
      }

      if (key == "source") {
        return Value(regex->pattern);
      }
      if (key == "flags") {
        return Value(regex->flags);
      }

      return Value(Undefined{});
    };

    std::string input;
    if (thisValue.isObject()) {
      auto obj = thisValue.getGC<Object>();
      Value toPrimitive = Value(Undefined{});
      auto toPrimitiveIt = obj->properties.find(WellKnownSymbols::toPrimitiveKey());
      if (toPrimitiveIt != obj->properties.end()) {
        toPrimitive = toPrimitiveIt->second;
      } else {
        auto fallbackIt = obj->properties.find("undefined");
        if (fallbackIt != obj->properties.end()) {
          toPrimitive = fallbackIt->second;
        }
      }
      if (toPrimitive.isFunction()) {
        input = callChecked(toPrimitive, {}, thisValue).toString();
      } else {
        input = thisValue.toString();
      }
    } else {
      input = thisValue.toString();
    }

    if (!regexp.isUndefined() && !regexp.isNull()) {
      if (regexp.isRegex()) {
        auto regex = regexp.getGC<Regex>();
        Value flagsValue = getRegexValue(regex, "flags");
        if (flagsValue.isUndefined() || flagsValue.isNull()) {
          throw std::runtime_error("TypeError: RegExp flags is undefined or null");
        }
        if (flagsValue.toString().find('g') == std::string::npos) {
          throw std::runtime_error("TypeError: String.prototype.matchAll requires a global RegExp");
        }
      }

      Value matcher(Undefined{});
      if (regexp.isRegex()) {
        matcher = getRegexValue(regexp.getGC<Regex>(), matchAllKey);
      } else if (regexp.isObject()) {
        matcher = getObjectValue(regexp.getGC<Object>(), regexp, matchAllKey);
      } else if (regexp.isFunction()) {
        auto fn = regexp.getGC<Function>();
        auto it = fn->properties.find(matchAllKey);
        if (it != fn->properties.end()) {
          matcher = it->second;
        }
      }

      if (!matcher.isUndefined() && !matcher.isNull()) {
        if (!matcher.isFunction()) {
          throw std::runtime_error("TypeError: @@matchAll is not callable");
        }
        return callChecked(matcher, {Value(input)}, regexp);
      }
    }

    std::string pattern;
    if (regexp.isRegex()) {
      pattern = regexp.getGC<Regex>()->pattern;
    } else if (regexp.isUndefined()) {
      pattern = "";  // RegExp(undefined) uses empty pattern
    } else {
      pattern = regexp.toString();
    }

    auto rx = GarbageCollector::makeGC<Regex>(pattern, "g");
    Value rxValue(rx);
    Value matcher = getRegexValue(GCPtr<Regex>(rx), matchAllKey);
    if (!matcher.isFunction()) {
      throw std::runtime_error("TypeError: RegExp @@matchAll is not callable");
    }
    return callChecked(matcher, {Value(input)}, rxValue);
  };
  stringPrototype->properties["matchAll"] = Value(stringMatchAll);
  stringPrototype->properties["__non_enum_matchAll"] = Value(true);
  stringConstructorObj->properties["prototype"] = Value(stringPrototype);
  stringConstructorObj->properties["__non_writable_prototype"] = Value(true);
  stringConstructorObj->properties["__non_enum_prototype"] = Value(true);
  stringConstructorObj->properties["__non_configurable_prototype"] = Value(true);
  stringConstructorFn->properties["prototype"] = Value(stringPrototype);
  stringPrototype->properties["constructor"] = Value(stringConstructorObj);
  stringPrototype->properties["length"] = Value(0.0);
  stringPrototype->properties["__non_enum_constructor"] = Value(true);
  stringPrototype->properties["__proto__"] = Value(objectPrototype);

  // For simplicity, we can make the Object callable by storing the function
  env->define("String", Value(stringConstructorObj));

  // WebAssembly global object
  env->define("WebAssembly", wasm_js::createWebAssemblyGlobal());

  // globalThis - reference to the global object
  // Create a proxy object that reflects the current global environment
  auto globalThisObj = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // Copy all current global bindings into globalThis
  for (const auto& [name, value] : env->bindings_) {
    globalThisObj->properties[name] = value;
  }
  // Expose selected intrinsics as configurable global properties.
  // (We keep the intrinsic itself off `globalThis` to avoid leaking internal names.)
  if (auto intrinsicJSON = env->get("__intrinsic_JSON__")) {
    globalThisObj->properties["JSON"] = *intrinsicJSON;
    globalThisObj->properties.erase("__intrinsic_JSON__");
  }
  // Immutable global properties (not declarative bindings).
  globalThisObj->properties["undefined"] = Value(Undefined{});
  globalThisObj->properties["Infinity"] = Value(std::numeric_limits<double>::infinity());
  globalThisObj->properties["NaN"] = Value(std::numeric_limits<double>::quiet_NaN());
  const char* immutableGlobalNames[] = {"undefined", "Infinity", "NaN"};
  for (const char* name : immutableGlobalNames) {
    globalThisObj->properties[std::string("__non_writable_") + name] = Value(true);
    globalThisObj->properties[std::string("__non_configurable_") + name] = Value(true);
    globalThisObj->properties[std::string("__non_enum_") + name] = Value(true);
  }

  // Define globalThis pointing to the global object
  env->define("globalThis", Value(globalThisObj));
  // 'this' at global scope should be globalThis
  env->define("this", Value(globalThisObj));
  env->define("__var_scope__", Value(true), true);

  // Also add globalThis to itself
  globalThisObj->properties["globalThis"] = Value(globalThisObj);

  // Timer functions - setTimeout
  auto setTimeoutFn = GarbageCollector::makeGC<Function>();
  setTimeoutFn->isNative = true;
  setTimeoutFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isFunction()) {
      return Value(Undefined{});
    }

    auto callback = args[0].getGC<Function>();
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
  auto setIntervalFn = GarbageCollector::makeGC<Function>();
  setIntervalFn->isNative = true;
  setIntervalFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isFunction()) {
      return Value(Undefined{});
    }

    auto callback = args[0].getGC<Function>();
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
  auto clearTimeoutFn = GarbageCollector::makeGC<Function>();
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
  auto clearIntervalFn = GarbageCollector::makeGC<Function>();
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
  auto queueMicrotaskFn = GarbageCollector::makeGC<Function>();
  queueMicrotaskFn->isNative = true;
  queueMicrotaskFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isFunction()) {
      return Value(Undefined{});
    }

    auto callback = args[0].getGC<Function>();

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
  auto abortControllerCtor = GarbageCollector::makeGC<Function>();
  abortControllerCtor->isNative = true;
  abortControllerCtor->isConstructor = true;
  abortControllerCtor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto controller = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));

    // Create AbortSignal
    auto signal = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    signal->properties["aborted"] = Value(false);
    signal->properties["reason"] = Value(Undefined{});

    // Event listeners storage
    auto listeners = GarbageCollector::makeGC<Array>();
    signal->properties["_listeners"] = Value(listeners);

    // addEventListener method
    auto addEventListenerFn = GarbageCollector::makeGC<Function>();
    addEventListenerFn->isNative = true;
    addEventListenerFn->nativeFunc = [listeners](const std::vector<Value>& args) -> Value {
      if (args.size() >= 2 && args[0].toString() == "abort" && args[1].isFunction()) {
        listeners->elements.push_back(args[1]);
      }
      return Value(Undefined{});
    };
    signal->properties["addEventListener"] = Value(addEventListenerFn);

    // removeEventListener method
    auto removeEventListenerFn = GarbageCollector::makeGC<Function>();
    removeEventListenerFn->isNative = true;
    removeEventListenerFn->nativeFunc = [listeners](const std::vector<Value>& args) -> Value {
      // Simple implementation - just mark for removal
      return Value(Undefined{});
    };
    signal->properties["removeEventListener"] = Value(removeEventListenerFn);

    controller->properties["signal"] = Value(signal);

    // abort method
    auto abortFn = GarbageCollector::makeGC<Function>();
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
          auto fn = listener.getGC<Function>();
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
  auto abortSignalObj = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  auto abortStaticFn = GarbageCollector::makeGC<Function>();
  abortStaticFn->isNative = true;
  abortStaticFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto signal = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    signal->properties["aborted"] = Value(true);
    signal->properties["reason"] = args.empty() ?
        Value(std::string("AbortError: The operation was aborted")) : args[0];
    return Value(signal);
  };
  abortSignalObj->properties["abort"] = Value(abortStaticFn);

  // AbortSignal.timeout() static method
  auto timeoutStaticFn = GarbageCollector::makeGC<Function>();
  timeoutStaticFn->isNative = true;
  timeoutStaticFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto signal = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    signal->properties["aborted"] = Value(false);
    signal->properties["reason"] = Value(Undefined{});
    // Note: Actual timeout implementation would require event loop integration
    return Value(signal);
  };
  abortSignalObj->properties["timeout"] = Value(timeoutStaticFn);

  env->define("AbortSignal", Value(abortSignalObj));

  // Streams API - ReadableStream
  auto readableStreamCtor = GarbageCollector::makeGC<Function>();
  readableStreamCtor->isNative = true;
  readableStreamCtor->isConstructor = true;
  readableStreamCtor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // Create underlying source callbacks from argument
    GCPtr<Function> startFn = {};
    GCPtr<Function> pullFn = {};
    GCPtr<Function> cancelFn = {};
    double highWaterMark = 1.0;

    if (!args.empty() && args[0].isObject()) {
      auto srcObj = args[0].getGC<Object>();

      // Get start callback
      auto startIt = srcObj->properties.find("start");
      if (startIt != srcObj->properties.end() && startIt->second.isFunction()) {
        startFn = startIt->second.getGC<Function>();
      }

      // Get pull callback
      auto pullIt = srcObj->properties.find("pull");
      if (pullIt != srcObj->properties.end() && pullIt->second.isFunction()) {
        pullFn = pullIt->second.getGC<Function>();
      }

      // Get cancel callback
      auto cancelIt = srcObj->properties.find("cancel");
      if (cancelIt != srcObj->properties.end() && cancelIt->second.isFunction()) {
        cancelFn = cancelIt->second.getGC<Function>();
      }
    }

    // Create the stream
    auto stream = createReadableStream(startFn, pullFn, cancelFn, highWaterMark);
    return Value(stream);
  };
  env->define("ReadableStream", Value(readableStreamCtor));

  // Streams API - WritableStream
  auto writableStreamCtor = GarbageCollector::makeGC<Function>();
  writableStreamCtor->isNative = true;
  writableStreamCtor->isConstructor = true;
  writableStreamCtor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // Create underlying sink callbacks from argument
    GCPtr<Function> startFn = {};
    GCPtr<Function> writeFn = {};
    GCPtr<Function> closeFn = {};
    GCPtr<Function> abortFn = {};
    double highWaterMark = 1.0;

    if (!args.empty() && args[0].isObject()) {
      auto sinkObj = args[0].getGC<Object>();

      // Get start callback
      auto startIt = sinkObj->properties.find("start");
      if (startIt != sinkObj->properties.end() && startIt->second.isFunction()) {
        startFn = startIt->second.getGC<Function>();
      }

      // Get write callback
      auto writeIt = sinkObj->properties.find("write");
      if (writeIt != sinkObj->properties.end() && writeIt->second.isFunction()) {
        writeFn = writeIt->second.getGC<Function>();
      }

      // Get close callback
      auto closeIt = sinkObj->properties.find("close");
      if (closeIt != sinkObj->properties.end() && closeIt->second.isFunction()) {
        closeFn = closeIt->second.getGC<Function>();
      }

      // Get abort callback
      auto abortIt = sinkObj->properties.find("abort");
      if (abortIt != sinkObj->properties.end() && abortIt->second.isFunction()) {
        abortFn = abortIt->second.getGC<Function>();
      }
    }

    // Create the stream
    auto stream = createWritableStream(startFn, writeFn, closeFn, abortFn, highWaterMark);
    return Value(stream);
  };
  env->define("WritableStream", Value(writableStreamCtor));

  // Streams API - TransformStream
  auto transformStreamCtor = GarbageCollector::makeGC<Function>();
  transformStreamCtor->isNative = true;
  transformStreamCtor->isConstructor = true;
  transformStreamCtor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // Create transformer callbacks from argument
    GCPtr<Function> startFn = {};
    GCPtr<Function> transformFn = {};
    GCPtr<Function> flushFn = {};

    if (!args.empty() && args[0].isObject()) {
      auto transformerObj = args[0].getGC<Object>();

      // Get start callback
      auto startIt = transformerObj->properties.find("start");
      if (startIt != transformerObj->properties.end() && startIt->second.isFunction()) {
        startFn = startIt->second.getGC<Function>();
      }

      // Get transform callback
      auto transformIt = transformerObj->properties.find("transform");
      if (transformIt != transformerObj->properties.end() && transformIt->second.isFunction()) {
        transformFn = transformIt->second.getGC<Function>();
      }

      // Get flush callback
      auto flushIt = transformerObj->properties.find("flush");
      if (flushIt != transformerObj->properties.end() && flushIt->second.isFunction()) {
        flushFn = flushIt->second.getGC<Function>();
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

  auto performanceNowFn = GarbageCollector::makeGC<Function>();
  performanceNowFn->isNative = true;
  performanceNowFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - startTime).count();
    return Value(static_cast<double>(elapsed) / 1000.0);  // Return milliseconds
  };

  auto performanceObj = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  performanceObj->properties["now"] = Value(performanceNowFn);
  env->define("performance", Value(performanceObj));
  globalThisObj->properties["performance"] = Value(performanceObj);

  // structuredClone - deep clone objects, arrays, and primitives
  // Use heap-owned recursion so callable lifetime remains valid after this setup scope.
  auto deepClone = std::make_shared<std::function<Value(const Value&)>>();
  *deepClone = [deepClone](const Value& val) -> Value {
    // Primitives are returned as-is (they're already copies)
    if (val.isUndefined() || val.isNull() || val.isBool() ||
        val.isNumber() || val.isString() || val.isBigInt() || val.isSymbol()) {
      return val;
    }

    // Clone arrays
    if (val.isArray()) {
      auto arr = val.getGC<Array>();
      auto newArr = GarbageCollector::makeGC<Array>();
      GarbageCollector::instance().reportAllocation(sizeof(Array));
      for (const auto& elem : arr->elements) {
        newArr->elements.push_back((*deepClone)(elem));
      }
      return Value(newArr);
    }

    // Clone objects
    if (val.isObject()) {
      auto obj = val.getGC<Object>();
      auto newObj = GarbageCollector::makeGC<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      for (const auto& [key, value] : obj->properties) {
        newObj->properties[key] = (*deepClone)(value);
      }
      return Value(newObj);
    }

    // Functions, Promises, etc. cannot be cloned - return as-is
    return val;
  };

  auto structuredCloneFn = GarbageCollector::makeGC<Function>();
  structuredCloneFn->isNative = true;
  structuredCloneFn->nativeFunc = [deepClone](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(Undefined{});
    return (*deepClone)(args[0]);
  };
  env->define("structuredClone", Value(structuredCloneFn));
  globalThisObj->properties["structuredClone"] = Value(structuredCloneFn);

  // Base64 encoding table
  static const char base64Chars[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  // btoa - encode string to Base64
  auto btoaFn = GarbageCollector::makeGC<Function>();
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
  auto atobFn = GarbageCollector::makeGC<Function>();
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
  auto encodeURIComponentFn = GarbageCollector::makeGC<Function>();
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
  auto decodeURIComponentFn = GarbageCollector::makeGC<Function>();
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
  auto encodeURIFn = GarbageCollector::makeGC<Function>();
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
  auto decodeURIFn = GarbageCollector::makeGC<Function>();
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

  // ===== Function constructor =====
  auto functionConstructor = GarbageCollector::makeGC<Function>();
  functionConstructor->isNative = true;
  functionConstructor->isConstructor = true;
  functionConstructor->properties["name"] = Value(std::string("Function"));
  functionConstructor->properties["length"] = Value(1.0);
  functionConstructor->nativeFunc = [env, objectPrototype](const std::vector<Value>& args) -> Value {
    std::vector<std::string> params;
    std::string body;

    if (!args.empty()) {
      for (size_t i = 0; i + 1 < args.size(); ++i) {
        params.push_back(args[i].toString());
      }
      body = args.back().toString();
    }

    std::string source = "function anonymous(";
    for (size_t i = 0; i < params.size(); ++i) {
      if (i > 0) source += ",";
      source += params[i];
    }
    source += ") {\n";
    source += body;
    source += "\n}";

    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    for (size_t i = 0; i + 2 < tokens.size(); ++i) {
      if (tokens[i].type == TokenType::Import &&
          !tokens[i].escaped &&
          tokens[i + 1].type == TokenType::Dot &&
          tokens[i + 2].type == TokenType::Identifier &&
          !tokens[i + 2].escaped &&
          tokens[i + 2].value == "meta") {
        throw std::runtime_error("SyntaxError: Cannot use import.meta outside a module");
      }
    }
    Parser parser(tokens);
    auto program = parser.parse();
    if (!program || program->body.empty()) {
      throw std::runtime_error("SyntaxError: Function constructor parse error");
    }

    auto compiledProgram = std::make_shared<Program>(std::move(*program));
    auto* fnDecl = std::get_if<FunctionDeclaration>(&compiledProgram->body[0]->node);
    if (!fnDecl) {
      throw std::runtime_error("SyntaxError: Function constructor parse error");
    }

    auto fn = GarbageCollector::makeGC<Function>();
    fn->isNative = false;
    fn->isAsync = fnDecl->isAsync;
    fn->isGenerator = fnDecl->isGenerator;
    fn->isConstructor = true;
    fn->closure = env;

    auto hasUseStrictDirective = [](const std::vector<StmtPtr>& bodyStmts) -> bool {
      for (const auto& stmt : bodyStmts) {
        if (!stmt) break;
        auto* exprStmt = std::get_if<ExpressionStmt>(&stmt->node);
        if (!exprStmt || !exprStmt->expression) break;
        auto* str = std::get_if<StringLiteral>(&exprStmt->expression->node);
        if (!str) break;
        if (str->value == "use strict") return true;
      }
      return false;
    };
    fn->isStrict = hasUseStrictDirective(fnDecl->body);

    for (const auto& param : fnDecl->params) {
      FunctionParam funcParam;
      funcParam.name = param.name.name;
      if (param.defaultValue) {
        funcParam.defaultValue = std::shared_ptr<void>(
          const_cast<Expression*>(param.defaultValue.get()),
          [](void*) {});
      }
      fn->params.push_back(funcParam);
    }

    if (fnDecl->restParam.has_value()) {
      fn->restParam = fnDecl->restParam->name;
    }

    fn->body = std::shared_ptr<void>(
      const_cast<std::vector<StmtPtr>*>(&fnDecl->body),
      [](void*) {});
    fn->astOwner = compiledProgram;
    fn->properties["name"] = Value(std::string("anonymous"));
    fn->properties["length"] = Value(static_cast<double>(fn->params.size()));

    auto fnPrototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    fnPrototype->properties["constructor"] = Value(fn);
    fnPrototype->properties["__proto__"] = Value(objectPrototype);
    fn->properties["prototype"] = Value(fnPrototype);

    // Set __proto__ to Function.prototype for proper prototype chain
    auto funcVal = env->get("Function");
    if (funcVal.has_value() && funcVal->isFunction()) {
      auto funcCtor = std::get<GCPtr<Function>>(funcVal->data);
      auto protoIt = funcCtor->properties.find("prototype");
      if (protoIt != funcCtor->properties.end()) {
        fn->properties["__proto__"] = protoIt->second;
      }
    }

    return Value(fn);
  };

  // Function.prototype - a minimal prototype with call/apply/bind
  // Per spec, Function.prototype is itself callable (it's a function that accepts any arguments and returns undefined)
  auto functionPrototype = GarbageCollector::makeGC<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  functionPrototype->isNative = true;
  functionPrototype->isConstructor = false;
  functionPrototype->nativeFunc = [](const std::vector<Value>&) -> Value {
    return Value(Undefined{});
  };
  functionPrototype->properties["length"] = Value(0.0);
  functionPrototype->properties["name"] = Value(std::string(""));
  functionPrototype->properties["__non_writable_length"] = Value(true);
  functionPrototype->properties["__non_enum_length"] = Value(true);
  functionPrototype->properties["__non_writable_name"] = Value(true);
  functionPrototype->properties["__non_enum_name"] = Value(true);
  functionPrototype->properties["__proto__"] = Value(objectPrototype);
  functionPrototype->properties["__callable_object__"] = Value(true);

  auto fpToString = GarbageCollector::makeGC<Function>();
  fpToString->isNative = true;
  fpToString->properties["name"] = Value(std::string("toString"));
  fpToString->properties["length"] = Value(0.0);
  fpToString->properties["__uses_this_arg__"] = Value(true);
  fpToString->nativeFunc = [](const std::vector<Value>&) -> Value {
    return Value(std::string("[Function]"));
  };
  functionPrototype->properties["toString"] = Value(fpToString);
  functionPrototype->properties["__non_enum_toString"] = Value(true);

  // Function.prototype.call - uses __uses_this_arg__ so args[0] = this (the function to call)
  auto fpCall = GarbageCollector::makeGC<Function>();
  fpCall->isNative = true;
  fpCall->properties["name"] = Value(std::string("call"));
  fpCall->properties["length"] = Value(1.0);
  fpCall->properties["__uses_this_arg__"] = Value(true);
  fpCall->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // args[0] = this (the function to invoke), args[1] = thisArg, args[2+] = call args
    if (args.empty() || !args[0].isFunction()) {
      throw std::runtime_error("TypeError: Function.prototype.call called on non-function");
    }
    auto fn = args[0].getGC<Function>();
    Value thisArg = args.size() > 1 ? args[1] : Value(Undefined{});
    std::vector<Value> callArgs;
    if (args.size() > 2) {
      callArgs.insert(callArgs.end(), args.begin() + 2, args.end());
    }
    if (fn->isNative) {
      auto requireNewIt = fn->properties.find("__require_new__");
      if (requireNewIt != fn->properties.end() &&
          requireNewIt->second.isBool() &&
          requireNewIt->second.toBool()) {
        throw std::runtime_error("TypeError: Constructor requires 'new'");
      }
      // For native functions that use this_arg, prepend thisArg
      auto itUsesThis = fn->properties.find("__uses_this_arg__");
      if (itUsesThis != fn->properties.end() && itUsesThis->second.isBool() && itUsesThis->second.toBool()) {
        std::vector<Value> nativeArgs;
        nativeArgs.push_back(thisArg);
        nativeArgs.insert(nativeArgs.end(), callArgs.begin(), callArgs.end());
        return fn->nativeFunc(nativeArgs);
      }
      return fn->nativeFunc(callArgs);
    }
    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) {
      throw std::runtime_error("TypeError: Interpreter unavailable");
    }
    return interpreter->callForHarness(Value(fn), callArgs, thisArg);
  };
  functionPrototype->properties["call"] = Value(fpCall);
  functionPrototype->properties["__non_enum_call"] = Value(true);

  // Function.prototype.apply
  auto fpApply = GarbageCollector::makeGC<Function>();
  fpApply->isNative = true;
  fpApply->properties["name"] = Value(std::string("apply"));
  fpApply->properties["length"] = Value(2.0);
  fpApply->properties["__uses_this_arg__"] = Value(true);
  fpApply->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isFunction()) {
      throw std::runtime_error("TypeError: Function.prototype.apply called on non-function");
    }
    auto fn = args[0].getGC<Function>();
    Value thisArg = args.size() > 1 ? args[1] : Value(Undefined{});
    std::vector<Value> callArgs;
    if (args.size() > 2 && args[2].isArray()) {
      auto arr = args[2].getGC<Array>();
      callArgs = arr->elements;
    }
    if (fn->isNative) {
      auto requireNewIt = fn->properties.find("__require_new__");
      if (requireNewIt != fn->properties.end() &&
          requireNewIt->second.isBool() &&
          requireNewIt->second.toBool()) {
        throw std::runtime_error("TypeError: Constructor requires 'new'");
      }
      auto itUsesThis = fn->properties.find("__uses_this_arg__");
      if (itUsesThis != fn->properties.end() && itUsesThis->second.isBool() && itUsesThis->second.toBool()) {
        std::vector<Value> nativeArgs;
        nativeArgs.push_back(thisArg);
        nativeArgs.insert(nativeArgs.end(), callArgs.begin(), callArgs.end());
        return fn->nativeFunc(nativeArgs);
      }
      return fn->nativeFunc(callArgs);
    }
    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) {
      throw std::runtime_error("TypeError: Interpreter unavailable");
    }
    return interpreter->callForHarness(Value(fn), callArgs, thisArg);
  };
  functionPrototype->properties["apply"] = Value(fpApply);
  functionPrototype->properties["__non_enum_apply"] = Value(true);

  // Function.prototype.bind
  auto fpBind = GarbageCollector::makeGC<Function>();
  fpBind->isNative = true;
  fpBind->properties["name"] = Value(std::string("bind"));
  fpBind->properties["length"] = Value(1.0);
  fpBind->properties["__uses_this_arg__"] = Value(true);
  fpBind->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // args[0] = this (the function to bind), args[1] = boundThis, args[2+] = bound args
    if (args.empty() || (!args[0].isFunction() && !args[0].isClass())) {
      throw std::runtime_error("TypeError: Function.prototype.bind called on non-function");
    }
    Value target = args[0];
    GCPtr<Function> targetFn;
    GCPtr<Class> targetCls;
    bool targetIsConstructor = false;
    if (target.isFunction()) {
      targetFn = target.getGC<Function>();
      targetIsConstructor = targetFn && targetFn->isConstructor;
    } else if (target.isClass()) {
      targetCls = target.getGC<Class>();
      targetIsConstructor = true;
    }
    Value boundThis = args.size() > 1 ? args[1] : Value(Undefined{});
    std::vector<Value> boundArgs;
    if (args.size() > 2) {
      boundArgs.insert(boundArgs.end(), args.begin() + 2, args.end());
    }
    auto boundFn = GarbageCollector::makeGC<Function>();
    boundFn->isNative = true;
    boundFn->isConstructor = targetIsConstructor;
    boundFn->properties["__bound_target__"] = target;
    boundFn->properties["__bound_this__"] = boundThis;
    auto boundArgsArr = GarbageCollector::makeGC<Array>();
    boundArgsArr->elements = boundArgs;
    boundFn->properties["__bound_args__"] = Value(boundArgsArr);

    std::string targetName;
    if (targetFn) {
      targetName = targetFn->properties.count("name") ? targetFn->properties["name"].toString() : "";
    } else if (targetCls) {
      auto it = targetCls->properties.find("name");
      if (it != targetCls->properties.end() && it->second.isString()) {
        targetName = it->second.toString();
      } else {
        targetName = targetCls->name;
      }
    }
    boundFn->properties["name"] = Value(std::string("bound " + targetName));
    boundFn->nativeFunc = [target, boundThis, boundArgs](const std::vector<Value>& callArgs) -> Value {
      // [[Call]] of a bound function: call target with boundThis and boundArgs + callArgs.
      // Bound class constructors are not callable without 'new'.
      if (target.isClass()) {
        throw std::runtime_error("TypeError: Class constructor cannot be invoked without 'new'");
      }
      auto targetFn = target.getGC<Function>();
      if (!targetFn) {
        throw std::runtime_error("TypeError: Bound target is not callable");
      }
      std::vector<Value> finalArgs = boundArgs;
      finalArgs.insert(finalArgs.end(), callArgs.begin(), callArgs.end());
      if (targetFn->isNative) {
        auto itUsesThis = targetFn->properties.find("__uses_this_arg__");
        if (itUsesThis != targetFn->properties.end() && itUsesThis->second.isBool() && itUsesThis->second.toBool()) {
          std::vector<Value> nativeArgs;
          nativeArgs.push_back(boundThis);
          nativeArgs.insert(nativeArgs.end(), finalArgs.begin(), finalArgs.end());
          return targetFn->nativeFunc(nativeArgs);
        }
        return targetFn->nativeFunc(finalArgs);
      }
      Interpreter* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        throw std::runtime_error("TypeError: Interpreter unavailable");
      }
      return interpreter->callForHarness(Value(targetFn), finalArgs, boundThis);
    };
    return Value(boundFn);
  };
  functionPrototype->properties["bind"] = Value(fpBind);
  functionPrototype->properties["__non_enum_bind"] = Value(true);

  // %FunctionPrototype%.caller / .arguments are restricted in strict mode and
  // for classes. Model as ThrowTypeError accessors.
  auto throwTypeErrorAccessor = GarbageCollector::makeGC<Function>();
  throwTypeErrorAccessor->isNative = true;
  throwTypeErrorAccessor->nativeFunc = [](const std::vector<Value>&) -> Value {
    throw std::runtime_error("TypeError: 'caller', 'callee', and 'arguments' properties may not be accessed");
  };
  functionPrototype->properties["__get_caller"] = Value(throwTypeErrorAccessor);
  functionPrototype->properties["__set_caller"] = Value(throwTypeErrorAccessor);
  functionPrototype->properties["__get_arguments"] = Value(throwTypeErrorAccessor);
  functionPrototype->properties["__set_arguments"] = Value(throwTypeErrorAccessor);
  functionPrototype->properties["__non_enum_caller"] = Value(true);
  functionPrototype->properties["__non_enum_arguments"] = Value(true);

  // Function.prototype[Symbol.hasInstance]
  auto fpHasInstance = GarbageCollector::makeGC<Function>();
  fpHasInstance->isNative = true;
  fpHasInstance->properties["name"] = Value(std::string("[Symbol.hasInstance]"));
  fpHasInstance->properties["__non_writable_name"] = Value(true);
  fpHasInstance->properties["__non_enum_name"] = Value(true);
  fpHasInstance->properties["length"] = Value(1.0);
  fpHasInstance->properties["__non_writable_length"] = Value(true);
  fpHasInstance->properties["__non_enum_length"] = Value(true);
  fpHasInstance->properties["__uses_this_arg__"] = Value(true);
  fpHasInstance->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // args[0] = this (the constructor function), args[1] = V (the value to check)
    if (args.empty()) return Value(false);
    Value F = args[0];
    if (!F.isFunction() && !F.isClass()) return Value(false);
    if (args.size() < 2) return Value(false);
    Value V = args[1];
    // OrdinaryHasInstance: check if V is an object
    if (!V.isObject() && !V.isArray() && !V.isFunction() && !V.isClass() &&
        !V.isError() && !V.isRegex() && !V.isPromise() && !V.isMap() &&
        !V.isSet() && !V.isWeakMap() && !V.isWeakSet() && !V.isTypedArray() &&
        !V.isArrayBuffer() && !V.isDataView() && !V.isGenerator()) {
      return Value(false);
    }
    // Get F.prototype
    Value Fproto;
    if (F.isFunction()) {
      auto fn = F.getGC<Function>();
      auto it = fn->properties.find("prototype");
      if (it != fn->properties.end()) Fproto = it->second;
      else return Value(false);
    } else if (F.isClass()) {
      auto cls = F.getGC<Class>();
      auto it = cls->properties.find("prototype");
      if (it != cls->properties.end()) Fproto = it->second;
      else return Value(false);
    }
    if (!Fproto.isObject() && !Fproto.isFunction()) {
      throw std::runtime_error("TypeError: Function has non-object prototype in instanceof check");
    }
    // Walk V's prototype chain using getPrototypeValue
    Value current = V;
    int depth = 0;
    while (depth < 100) {
      auto proto = getPrototypeValue(current);
      if (!proto.has_value()) return Value(false);
      current = proto.value();
      if (current.isNull() || current.isUndefined()) return Value(false);
      // SameValue comparison with Fproto (pointer identity)
      if (current.isObject() && Fproto.isObject() &&
          current.getGC<Object>().get() == Fproto.getGC<Object>().get()) {
        return Value(true);
      }
      if (current.isFunction() && Fproto.isFunction() &&
          current.getGC<Function>().get() == Fproto.getGC<Function>().get()) {
        return Value(true);
      }
      depth++;
    }
    return Value(false);
  };
  functionPrototype->properties[WellKnownSymbols::hasInstanceKey()] = Value(fpHasInstance);
  functionPrototype->properties["__non_writable_" + WellKnownSymbols::hasInstanceKey()] = Value(true);
  functionPrototype->properties["__non_enum_" + WellKnownSymbols::hasInstanceKey()] = Value(true);
  functionPrototype->properties["__non_configurable_" + WellKnownSymbols::hasInstanceKey()] = Value(true);

  functionPrototype->properties["constructor"] = Value(functionConstructor);
  functionPrototype->properties["__non_enum_constructor"] = Value(true);
  functionConstructor->properties["prototype"] = Value(functionPrototype);
  functionConstructor->properties["__non_writable_prototype"] = Value(true);
  functionConstructor->properties["__non_enum_prototype"] = Value(true);
  functionConstructor->properties["__non_configurable_prototype"] = Value(true);
  // Set Function constructor's own __proto__ to Function.prototype
  functionConstructor->properties["__proto__"] = Value(functionPrototype);
  env->define("Function", Value(functionConstructor));
  globalThisObj->properties["Function"] = Value(functionConstructor);

  // Set __proto__ to Function.prototype on built-in constructor functions
  // so that Function.prototype.isPrototypeOf(Number) etc. returns true
  {
    auto setFuncProto = [&](const std::string& name) {
      auto val = env->get(name);
      if (val.has_value()) {
        if (val->isFunction()) {
          val->getGC<Function>()->properties["__proto__"] = Value(functionPrototype);
        } else if (val->isObject()) {
          val->getGC<Object>()->properties["__proto__"] = Value(functionPrototype);
        }
      }
    };
    setFuncProto("Number");
    setFuncProto("Boolean");
    setFuncProto("String");
    setFuncProto("Object");
    setFuncProto("Array");
    setFuncProto("RegExp");
    setFuncProto("Error");
    setFuncProto("TypeError");
    setFuncProto("RangeError");
    setFuncProto("ReferenceError");
    setFuncProto("SyntaxError");
    setFuncProto("URIError");
    setFuncProto("EvalError");
    setFuncProto("Date");
    setFuncProto("Map");
    setFuncProto("Set");
    setFuncProto("WeakMap");
    setFuncProto("WeakSet");
    setFuncProto("Promise");
    setFuncProto("Symbol");
    setFuncProto("ArrayBuffer");
    setFuncProto("DataView");
    // JSON, Math, Reflect are plain objects (not constructors),
    // their [[Prototype]] is Object.prototype (set earlier), not Function.prototype
    setFuncProto("Proxy");
  }

  // Generator function intrinsics setup
  auto generatorFunctionPrototype = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  auto generatorPrototype = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  
  // %GeneratorFunction.prototype% inherits from Function.prototype
  generatorFunctionPrototype->properties["__proto__"] = Value(functionPrototype);
  generatorFunctionPrototype->properties["__get_caller"] = Value(throwTypeErrorAccessor);
  generatorFunctionPrototype->properties["__set_caller"] = Value(throwTypeErrorAccessor);
  generatorFunctionPrototype->properties["__get_arguments"] = Value(throwTypeErrorAccessor);
  generatorFunctionPrototype->properties["__set_arguments"] = Value(throwTypeErrorAccessor);
  generatorFunctionPrototype->properties["__non_enum_caller"] = Value(true);
  generatorFunctionPrototype->properties["__non_enum_arguments"] = Value(true);
  // %GeneratorFunction.prototype.prototype% is %GeneratorPrototype%
  generatorFunctionPrototype->properties["prototype"] = Value(generatorPrototype);
  // %GeneratorPrototype% inherits from Object.prototype
  if (auto objCtor = env->get("Object"); objCtor && objCtor->isFunction()) {
    auto fn = std::get<GCPtr<Function>>(objCtor->data);
    auto protoIt = fn->properties.find("prototype");
    if (protoIt != fn->properties.end()) {
      generatorPrototype->properties["__proto__"] = protoIt->second;
    }
  }
  // %GeneratorPrototype%.constructor is %GeneratorFunction.prototype%
  generatorPrototype->properties["constructor"] = Value(generatorFunctionPrototype);

  // GeneratorFunction constructor (not a global binding; reachable via
  // Object.getPrototypeOf(function*(){}).constructor)
  auto generatorFunctionConstructor = GarbageCollector::makeGC<Function>();
  generatorFunctionConstructor->isNative = true;
  generatorFunctionConstructor->isConstructor = true;
  generatorFunctionConstructor->properties["name"] = Value(std::string("GeneratorFunction"));
  generatorFunctionConstructor->properties["length"] = Value(1.0);
  // %GeneratorFunction% inherits from %Function%.
  generatorFunctionConstructor->properties["__proto__"] = Value(functionPrototype);
  generatorFunctionConstructor->nativeFunc = [env, generatorFunctionPrototype, generatorPrototype](const std::vector<Value>& args) -> Value {
    std::vector<std::string> params;
    std::string body;

    if (!args.empty()) {
      for (size_t i = 0; i + 1 < args.size(); ++i) {
        params.push_back(args[i].toString());
      }
      body = args.back().toString();
    }

    std::string source = "function* anonymous(";
    for (size_t i = 0; i < params.size(); ++i) {
      if (i > 0) source += ",";
      source += params[i];
    }
    source += ") {\n";
    source += body;
    source += "\n}";

    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    for (size_t i = 0; i + 2 < tokens.size(); ++i) {
      if (tokens[i].type == TokenType::Import &&
          !tokens[i].escaped &&
          tokens[i + 1].type == TokenType::Dot &&
          tokens[i + 2].type == TokenType::Identifier &&
          !tokens[i + 2].escaped &&
          tokens[i + 2].value == "meta") {
        throw std::runtime_error("SyntaxError: Cannot use import.meta outside a module");
      }
    }
    Parser parser(tokens);
    auto program = parser.parse();
    if (!program || program->body.empty()) {
      throw std::runtime_error("SyntaxError: Function constructor parse error");
    }

    auto compiledProgram = std::make_shared<Program>(std::move(*program));
    auto* fnDecl = std::get_if<FunctionDeclaration>(&compiledProgram->body[0]->node);
    if (!fnDecl || !fnDecl->isGenerator) {
      throw std::runtime_error("SyntaxError: Function constructor parse error");
    }

    auto fn = GarbageCollector::makeGC<Function>();
    fn->isNative = false;
    fn->isAsync = fnDecl->isAsync;
    fn->isGenerator = fnDecl->isGenerator;
    fn->isConstructor = false;
    fn->closure = env;

    auto hasUseStrictDirective = [](const std::vector<StmtPtr>& bodyStmts) -> bool {
      for (const auto& stmt : bodyStmts) {
        if (!stmt) break;
        auto* exprStmt = std::get_if<ExpressionStmt>(&stmt->node);
        if (!exprStmt || !exprStmt->expression) break;
        auto* str = std::get_if<StringLiteral>(&exprStmt->expression->node);
        if (!str) break;
        if (str->value == "use strict") return true;
      }
      return false;
    };
    fn->isStrict = hasUseStrictDirective(fnDecl->body);

    for (const auto& param : fnDecl->params) {
      FunctionParam funcParam;
      funcParam.name = param.name.name;
      if (param.defaultValue) {
        funcParam.defaultValue = std::shared_ptr<void>(
          const_cast<Expression*>(param.defaultValue.get()),
          [](void*) {});
      }
      fn->params.push_back(funcParam);
    }
    if (fnDecl->restParam.has_value()) {
      fn->restParam = fnDecl->restParam->name;
    }
    fn->body = std::shared_ptr<void>(
      const_cast<std::vector<StmtPtr>*>(&fnDecl->body),
      [](void*) {});
    fn->astOwner = compiledProgram;
    fn->properties["name"] = Value(std::string("anonymous"));
    fn->properties["length"] = Value(static_cast<double>(fn->params.size()));

    // Generator functions have a `.prototype` that is the generator object prototype.
    auto genProtoObj = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    genProtoObj->properties["__proto__"] = Value(generatorPrototype);
    fn->properties["prototype"] = Value(genProtoObj);
    fn->properties["__non_enum_prototype"] = Value(true);
    fn->properties["__non_configurable_prototype"] = Value(true);

    // %GeneratorFunction.prototype% as [[Prototype]] of generator function objects.
    fn->properties["__proto__"] = Value(generatorFunctionPrototype);

    return Value(fn);
  };

  // Wire: GeneratorFunction.prototype and .constructor
  generatorFunctionConstructor->properties["prototype"] = Value(generatorFunctionPrototype);
  generatorFunctionPrototype->properties["constructor"] = Value(generatorFunctionConstructor);
  generatorFunctionPrototype->properties["__non_enum_constructor"] = Value(true);

  // Store these in environment for internal use
  env->define("__generator_function_prototype__", Value(generatorFunctionPrototype));
  env->define("__generator_prototype__", Value(generatorPrototype));

  // ===== Deferred prototype chain setup =====
  // These must happen after all constructors are defined.

  // BigInt.prototype.__proto__ = Object.prototype
  if (auto bigIntCtor = env->get("BigInt"); bigIntCtor && bigIntCtor->isFunction()) {
    auto bigIntFnPtr = std::get<GCPtr<Function>>(bigIntCtor->data);
    auto protoIt = bigIntFnPtr->properties.find("prototype");
    if (protoIt != bigIntFnPtr->properties.end() && protoIt->second.isObject()) {
      auto bigIntProtoPtr = protoIt->second.getGC<Object>();
      bigIntProtoPtr->properties["__proto__"] = Value(objectPrototype);
    }
    // BigInt.__proto__ = Function.prototype
    bigIntFnPtr->properties["__proto__"] = Value(functionPrototype);
  }

  // Promise.prototype.__proto__ = Object.prototype (already set via promisePrototype)
  // Promise.__proto__ = Function.prototype
  if (auto promiseCtor = env->get("Promise"); promiseCtor && promiseCtor->isFunction()) {
    auto promiseFnPtr = std::get<GCPtr<Function>>(promiseCtor->data);
    promiseFnPtr->properties["__proto__"] = Value(functionPrototype);
  }

  // Mark built-in constructors as non-enumerable on globalThis (per ES spec)
  static const char* builtinNames[] = {
    "BigInt", "Object", "Array", "String", "Number", "Boolean", "Symbol",
    "Promise", "RegExp", "Map", "Set", "Proxy", "Reflect", "Error",
    "TypeError", "RangeError", "ReferenceError", "SyntaxError", "URIError",
    "EvalError", "Function", "Date", "Math", "JSON", "console",
    "ArrayBuffer", "DataView", "Int8Array", "Uint8Array", "Uint8ClampedArray",
    "Int16Array", "Uint16Array", "Int32Array", "Uint32Array",
    "Float16Array", "Float32Array", "Float64Array",
    "BigInt64Array", "BigUint64Array", "WeakRef", "FinalizationRegistry",
    "globalThis", "undefined", "NaN", "Infinity",
    "eval", "parseInt", "parseFloat", "isNaN", "isFinite",
    "encodeURIComponent", "decodeURIComponent", "encodeURI", "decodeURI",
    "setTimeout", "clearTimeout", "setInterval", "clearInterval",
    "queueMicrotask", "structuredClone", "btoa", "atob",
    "fetch", "crypto", "WebAssembly", "performance"
  };
  for (const char* name : builtinNames) {
    if (globalThisObj->properties.count(name)) {
      globalThisObj->properties["__non_enum_" + std::string(name)] = Value(true);
    }
  }

  return env;
}

Environment* Environment::getRoot() {
  Environment* current = this;
  while (current->parent_) {
    current = current->parent_.get();
  }
  return current;
}

GCPtr<Object> Environment::getGlobal() const {
  // Walk up to the root environment
  const Environment* current = this;
  while (current->parent_) {
    current = current->parent_.get();
  }

  // Use the live global object when available.
  auto globalThisIt = current->bindings_.find("globalThis");
  if (globalThisIt != current->bindings_.end() && globalThisIt->second.isObject()) {
    return globalThisIt->second.getGC<Object>();
  }

  auto globalObj = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // Add all global bindings to the object
  for (const auto& [name, value] : current->bindings_) {
    globalObj->properties[name] = value;
  }

  return GCPtr<Object>(globalObj);
}

}
