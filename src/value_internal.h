#pragma once

#include "value.h"
#include "checked_arithmetic.h"
#include "environment.h"
#include "interpreter.h"
#include "streams.h"
#include "wasm_js.h"
#include "event_loop.h"
#include "simd.h"
#include "symbols.h"
#include "streams.h"
#include "wasm_js.h"
#include <sstream>
#include <cmath>
#include <iomanip>
#include <algorithm>
#include <cstring>
#include <limits>
#include <cstdlib>
#include <cctype>

namespace lightjs {

namespace {
template <typename T>
T readScalarUnaligned(const uint8_t* ptr) {
  T value{};
  std::memcpy(&value, ptr, sizeof(T));
  return value;
}

template <typename T>
void writeScalarUnaligned(uint8_t* ptr, T value) {
  std::memcpy(ptr, &value, sizeof(T));
}

std::vector<std::string> moduleNamespaceExportNames(const GCPtr<Object>& obj) {
  if (!obj) {
    return {};
  }
  if (!obj->moduleExportNames.empty()) {
    return obj->moduleExportNames;
  }

  std::vector<std::string> names;
  std::unordered_set<std::string> seen;
  for (const auto& key : obj->properties.orderedKeys()) {
    if (key.rfind("__get_", 0) == 0 && key.size() > 6) {
      std::string exposed = key.substr(6);
      if (seen.insert(exposed).second) {
        names.push_back(exposed);
      }
      continue;
    }
    if (key == WellKnownSymbols::toStringTagKey() ||
        key.rfind("__", 0) == 0 ||
        isSymbolPropertyKey(key)) {
      continue;
    }
    if (seen.insert(key).second) {
      names.push_back(key);
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

void triggerDeferredNamespaceOwnKeys(const GCPtr<Object>& obj) {
  if (!obj || !obj->isModuleNamespace) {
    return;
  }
  auto pendingIt = obj->properties.find("__deferred_pending__");
  if (pendingIt == obj->properties.end() ||
      !pendingIt->second.isBool() ||
      !pendingIt->second.toBool()) {
    return;
  }
  auto evalIt = obj->properties.find("__deferred_eval__");
  if (evalIt == obj->properties.end() || !evalIt->second.isFunction()) {
    return;
  }
  auto deferredEvalFn = evalIt->second.getGC<Function>();
  if (!deferredEvalFn || !deferredEvalFn->isNative) {
    return;
  }
  deferredEvalFn->nativeFunc({});
  obj->properties["__deferred_pending__"] = Value(false);
}

double toIntegerOrZero(double value) {
  if (!std::isfinite(value) || value == 0.0) {
    return 0.0;
  }
  return std::trunc(value);
}

template <typename UnsignedT>
UnsignedT toUintN(double value) {
  constexpr uint64_t kModulus = uint64_t{1} << (sizeof(UnsignedT) * 8);
  long double integer = static_cast<long double>(toIntegerOrZero(value));
  long double wrapped = std::fmod(integer, static_cast<long double>(kModulus));
  if (wrapped < 0) {
    wrapped += static_cast<long double>(kModulus);
  }
  return static_cast<UnsignedT>(static_cast<uint64_t>(wrapped));
}

int32_t toInt32Element(double value) {
  return static_cast<int32_t>(toUintN<uint32_t>(value));
}

uint8_t toUint8Clamp(double value) {
  if (std::isnan(value) || value <= 0.0) {
    return 0;
  }
  if (value >= 255.0) {
    return 255;
  }
  double floorValue = std::floor(value);
  double fraction = value - floorValue;
  if (fraction > 0.5) {
    return static_cast<uint8_t>(floorValue + 1.0);
  }
  if (fraction < 0.5) {
    return static_cast<uint8_t>(floorValue);
  }
  uint8_t truncated = static_cast<uint8_t>(floorValue);
  return (truncated % 2 == 0) ? truncated : static_cast<uint8_t>(truncated + 1);
}

double readTypedArrayNumberUnchecked(TypedArrayType type, const uint8_t* ptr) {
  switch (type) {
    case TypedArrayType::Int8:
      return static_cast<double>(static_cast<int8_t>(*ptr));
    case TypedArrayType::Uint8:
    case TypedArrayType::Uint8Clamped:
      return static_cast<double>(*ptr);
    case TypedArrayType::Int16:
      return static_cast<double>(readScalarUnaligned<int16_t>(ptr));
    case TypedArrayType::Uint16:
      return static_cast<double>(readScalarUnaligned<uint16_t>(ptr));
    case TypedArrayType::Float16:
      return static_cast<double>(float16_to_float32(readScalarUnaligned<uint16_t>(ptr)));
    case TypedArrayType::Int32:
      return static_cast<double>(readScalarUnaligned<int32_t>(ptr));
    case TypedArrayType::Uint32:
      return static_cast<double>(readScalarUnaligned<uint32_t>(ptr));
    case TypedArrayType::Float32:
      return static_cast<double>(readScalarUnaligned<float>(ptr));
    case TypedArrayType::Float64:
      return readScalarUnaligned<double>(ptr);
    default:
      return 0.0;
  }
}

void writeTypedArrayNumberUnchecked(TypedArrayType type, uint8_t* ptr, double value) {
  switch (type) {
    case TypedArrayType::Int8:
      *ptr = static_cast<uint8_t>(static_cast<int8_t>(toUintN<uint8_t>(value)));
      break;
    case TypedArrayType::Uint8:
      *ptr = toUintN<uint8_t>(value);
      break;
    case TypedArrayType::Uint8Clamped:
      *ptr = toUint8Clamp(value);
      break;
    case TypedArrayType::Int16:
      writeScalarUnaligned<int16_t>(ptr, static_cast<int16_t>(toUintN<uint16_t>(value)));
      break;
    case TypedArrayType::Uint16:
      writeScalarUnaligned<uint16_t>(ptr, toUintN<uint16_t>(value));
      break;
    case TypedArrayType::Float16:
      writeScalarUnaligned<uint16_t>(ptr, float64_to_float16(value));
      break;
    case TypedArrayType::Int32:
      writeScalarUnaligned<int32_t>(ptr, toInt32Element(value));
      break;
    case TypedArrayType::Uint32:
      writeScalarUnaligned<uint32_t>(ptr, toUintN<uint32_t>(value));
      break;
    case TypedArrayType::Float32:
      writeScalarUnaligned<float>(ptr, static_cast<float>(value));
      break;
    case TypedArrayType::Float64:
      writeScalarUnaligned<double>(ptr, value);
      break;
    default:
      break;
  }
}

void queuePromiseCallback(std::function<void()> callback) {
  EventLoopContext::instance().getLoop().queueMicrotask(std::move(callback));
}

void resolveChainedPromise(const GCPtr<Promise>& target, const Value& value) {
  if (!target || target->state != PromiseState::Pending) {
    return;
  }

  if (value.isPromise()) {
    auto nested = value.getGC<Promise>();
    if (!nested) {
      target->resolve(value);
      return;
    }
    if (nested.get() == target.get()) {
      target->reject(Value(std::make_shared<Error>(
        ErrorType::TypeError, "Cannot resolve promise with itself")));
      return;
    }
    if (nested->state == PromiseState::Fulfilled) {
      resolveChainedPromise(target, nested->result);
      return;
    }
    if (nested->state == PromiseState::Rejected) {
      target->reject(nested->result);
      return;
    }
    nested->then(
      [target](Value fulfilled) -> Value {
        resolveChainedPromise(target, fulfilled);
        return fulfilled;
      },
      [target](Value reason) -> Value {
        target->reject(reason);
        return reason;
      });
    return;
  }

  target->resolve(value);
}
}  // namespace

static bool isInternalProperty(const std::string& key) {
  if (key.size() >= 4 && key.substr(0, 2) == "__" &&
      key.substr(key.size() - 2) == "__") {
    // Annex B properties are NOT internal
    static const std::unordered_set<std::string> annexBProps = {
      "__defineGetter__", "__defineSetter__",
      "__lookupGetter__", "__lookupSetter__",
    };
    if (annexBProps.count(key)) return false;
    return true;
  }
  if (key.substr(0, 6) == "__get_" || key.substr(0, 6) == "__set_" ||
      key.substr(0, 11) == "__non_enum_" || key.substr(0, 15) == "__non_writable_" ||
      key.substr(0, 19) == "__non_configurable_" || key.substr(0, 7) == "__enum_" ||
      key.substr(0, 14) == "__json_source_") return true;
  return false;
}

// Helper: check if key is an array index (non-negative integer < 2^32 - 1)
static bool isArrayIndex(const std::string& key, uint32_t& out) {
  if (key.empty()) return false;
  if (key.size() > 1 && key[0] == '0') return false;
  for (char c : key) {
    if (c < '0' || c > '9') return false;
  }
  try {
    unsigned long long parsed = std::stoull(key);
    if (parsed >= static_cast<unsigned long long>(std::numeric_limits<uint32_t>::max())) return false;
    out = static_cast<uint32_t>(parsed);
    return true;
  } catch (...) { return false; }
}

// Sort keys per OrdinaryOwnPropertyKeys: integer indices ascending, then
// string keys in insertion order (orderedKeys preserves insertion order).
static std::vector<std::string> sortOwnPropertyKeys(
    const std::vector<std::string>& orderedKeys) {
  std::vector<std::pair<uint32_t, std::string>> indexKeys;
  std::vector<std::string> stringKeys;
  for (const auto& key : orderedKeys) {
    if (isInternalProperty(key)) continue;
    if (isSymbolPropertyKey(key)) continue;
    uint32_t idx;
    if (isArrayIndex(key, idx)) {
      indexKeys.push_back({idx, key});
    } else {
      stringKeys.push_back(key);
    }
  }
  std::sort(indexKeys.begin(), indexKeys.end(),
    [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<std::string> result;
  result.reserve(indexKeys.size() + stringKeys.size());
  for (const auto& [_, key] : indexKeys) result.push_back(key);
  for (const auto& key : stringKeys) result.push_back(key);
  return result;
}

// Object static methods implementation
// Helper to get the OrderedMap properties from any object-like value
static OrderedMap<std::string, Value>* getPropertiesMap(const Value& val) {
  if (val.isObject()) return &val.getGC<Object>()->properties;
  if (val.isFunction()) return &val.getGC<Function>()->properties;
  if (val.isArray()) return &val.getGC<Array>()->properties;
  if (val.isClass()) return &val.getGC<Class>()->properties;
  if (val.isPromise()) return &val.getGC<Promise>()->properties;
  if (val.isRegex()) return &val.getGC<Regex>()->properties;
  return nullptr;
}

}  // namespace lightjs
