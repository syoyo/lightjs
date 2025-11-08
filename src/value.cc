#include "value.h"
#include <sstream>
#include <cmath>

namespace lightjs {

// Initialize static member for Symbol IDs
size_t Symbol::nextId = 0;

bool Value::toBool() const {
  return std::visit([](auto&& arg) -> bool {
    using T = std::decay_t<decltype(arg)>;
    if constexpr (std::is_same_v<T, Undefined>) {
      return false;
    } else if constexpr (std::is_same_v<T, Null>) {
      return false;
    } else if constexpr (std::is_same_v<T, bool>) {
      return arg;
    } else if constexpr (std::is_same_v<T, double>) {
      return arg != 0.0 && !std::isnan(arg);
    } else if constexpr (std::is_same_v<T, BigInt>) {
      return arg.value != 0;
    } else if constexpr (std::is_same_v<T, Symbol>) {
      return true;  // Symbols are always truthy
    } else if constexpr (std::is_same_v<T, std::string>) {
      return !arg.empty();
    } else {
      return true;
    }
  }, data);
}

double Value::toNumber() const {
  return std::visit([](auto&& arg) -> double {
    using T = std::decay_t<decltype(arg)>;
    if constexpr (std::is_same_v<T, Undefined>) {
      return std::nan("");
    } else if constexpr (std::is_same_v<T, Null>) {
      return 0.0;
    } else if constexpr (std::is_same_v<T, bool>) {
      return arg ? 1.0 : 0.0;
    } else if constexpr (std::is_same_v<T, double>) {
      return arg;
    } else if constexpr (std::is_same_v<T, BigInt>) {
      return static_cast<double>(arg.value);
    } else if constexpr (std::is_same_v<T, std::string>) {
      try {
        return std::stod(arg);
      } catch (...) {
        return std::nan("");
      }
    } else {
      return std::nan("");
    }
  }, data);
}

int64_t Value::toBigInt() const {
  return std::visit([](auto&& arg) -> int64_t {
    using T = std::decay_t<decltype(arg)>;
    if constexpr (std::is_same_v<T, Undefined>) {
      return 0;
    } else if constexpr (std::is_same_v<T, Null>) {
      return 0;
    } else if constexpr (std::is_same_v<T, bool>) {
      return arg ? 1 : 0;
    } else if constexpr (std::is_same_v<T, double>) {
      return static_cast<int64_t>(arg);
    } else if constexpr (std::is_same_v<T, BigInt>) {
      return arg.value;
    } else if constexpr (std::is_same_v<T, std::string>) {
      try {
        return std::stoll(arg);
      } catch (...) {
        return 0;
      }
    } else {
      return 0;
    }
  }, data);
}

std::string Value::toString() const {
  return std::visit([](auto&& arg) -> std::string {
    using T = std::decay_t<decltype(arg)>;
    if constexpr (std::is_same_v<T, Undefined>) {
      return "undefined";
    } else if constexpr (std::is_same_v<T, Null>) {
      return "null";
    } else if constexpr (std::is_same_v<T, bool>) {
      return arg ? "true" : "false";
    } else if constexpr (std::is_same_v<T, double>) {
      std::ostringstream oss;
      oss << arg;
      return oss.str();
    } else if constexpr (std::is_same_v<T, BigInt>) {
      return std::to_string(arg.value) + "n";
    } else if constexpr (std::is_same_v<T, Symbol>) {
      return "Symbol(" + arg.description + ")";
    } else if constexpr (std::is_same_v<T, std::string>) {
      return arg;
    } else if constexpr (std::is_same_v<T, std::shared_ptr<Function>>) {
      return "[Function]";
    } else if constexpr (std::is_same_v<T, std::shared_ptr<Array>>) {
      return "[Array]";
    } else if constexpr (std::is_same_v<T, std::shared_ptr<Object>>) {
      return "[Object]";
    } else if constexpr (std::is_same_v<T, std::shared_ptr<TypedArray>>) {
      return "[TypedArray]";
    } else if constexpr (std::is_same_v<T, std::shared_ptr<Promise>>) {
      return "[Promise]";
    } else if constexpr (std::is_same_v<T, std::shared_ptr<Regex>>) {
      return "/" + arg->pattern + "/" + arg->flags;
    } else if constexpr (std::is_same_v<T, std::shared_ptr<Error>>) {
      return arg->toString();
    } else if constexpr (std::is_same_v<T, std::shared_ptr<Generator>>) {
      return "[Generator]";
    } else if constexpr (std::is_same_v<T, std::shared_ptr<Proxy>>) {
      return "[Proxy]";
    } else if constexpr (std::is_same_v<T, std::shared_ptr<WeakMap>>) {
      return "[WeakMap]";
    } else if constexpr (std::is_same_v<T, std::shared_ptr<WeakSet>>) {
      return "[WeakSet]";
    } else if constexpr (std::is_same_v<T, std::shared_ptr<ArrayBuffer>>) {
      return "[ArrayBuffer]";
    } else if constexpr (std::is_same_v<T, std::shared_ptr<DataView>>) {
      return "[DataView]";
    } else {
      return "";
    }
  }, data);
}

double TypedArray::getElement(size_t index) const {
  if (index >= length) return 0.0;

  size_t byteIndex = byteOffset + index * elementSize();
  const uint8_t* ptr = &buffer[byteIndex];

  switch (type) {
    case TypedArrayType::Int8:
      return static_cast<double>(*reinterpret_cast<const int8_t*>(ptr));
    case TypedArrayType::Uint8:
    case TypedArrayType::Uint8Clamped:
      return static_cast<double>(*reinterpret_cast<const uint8_t*>(ptr));
    case TypedArrayType::Int16:
      return static_cast<double>(*reinterpret_cast<const int16_t*>(ptr));
    case TypedArrayType::Uint16:
      return static_cast<double>(*reinterpret_cast<const uint16_t*>(ptr));
    case TypedArrayType::Float16:
      return static_cast<double>(float16_to_float32(*reinterpret_cast<const uint16_t*>(ptr)));
    case TypedArrayType::Int32:
      return static_cast<double>(*reinterpret_cast<const int32_t*>(ptr));
    case TypedArrayType::Uint32:
      return static_cast<double>(*reinterpret_cast<const uint32_t*>(ptr));
    case TypedArrayType::Float32:
      return static_cast<double>(*reinterpret_cast<const float*>(ptr));
    case TypedArrayType::Float64:
      return *reinterpret_cast<const double*>(ptr);
    default:
      return 0.0;
  }
}

void TypedArray::setElement(size_t index, double value) {
  if (index >= length) return;

  size_t byteIndex = byteOffset + index * elementSize();
  uint8_t* ptr = &buffer[byteIndex];

  switch (type) {
    case TypedArrayType::Int8:
      *reinterpret_cast<int8_t*>(ptr) = static_cast<int8_t>(value);
      break;
    case TypedArrayType::Uint8:
      *reinterpret_cast<uint8_t*>(ptr) = static_cast<uint8_t>(value);
      break;
    case TypedArrayType::Uint8Clamped: {
      int32_t clamped = static_cast<int32_t>(value);
      if (clamped < 0) clamped = 0;
      if (clamped > 255) clamped = 255;
      *reinterpret_cast<uint8_t*>(ptr) = static_cast<uint8_t>(clamped);
      break;
    }
    case TypedArrayType::Int16:
      *reinterpret_cast<int16_t*>(ptr) = static_cast<int16_t>(value);
      break;
    case TypedArrayType::Uint16:
      *reinterpret_cast<uint16_t*>(ptr) = static_cast<uint16_t>(value);
      break;
    case TypedArrayType::Float16:
      *reinterpret_cast<uint16_t*>(ptr) = float32_to_float16(static_cast<float>(value));
      break;
    case TypedArrayType::Int32:
      *reinterpret_cast<int32_t*>(ptr) = static_cast<int32_t>(value);
      break;
    case TypedArrayType::Uint32:
      *reinterpret_cast<uint32_t*>(ptr) = static_cast<uint32_t>(value);
      break;
    case TypedArrayType::Float32:
      *reinterpret_cast<float*>(ptr) = static_cast<float>(value);
      break;
    case TypedArrayType::Float64:
      *reinterpret_cast<double*>(ptr) = value;
      break;
    default:
      break;
  }
}

int64_t TypedArray::getBigIntElement(size_t index) const {
  if (index >= length) return 0;

  size_t byteIndex = byteOffset + index * elementSize();
  const uint8_t* ptr = &buffer[byteIndex];

  switch (type) {
    case TypedArrayType::BigInt64:
      return *reinterpret_cast<const int64_t*>(ptr);
    case TypedArrayType::BigUint64:
      return static_cast<int64_t>(*reinterpret_cast<const uint64_t*>(ptr));
    default:
      return static_cast<int64_t>(getElement(index));
  }
}

void TypedArray::setBigIntElement(size_t index, int64_t value) {
  if (index >= length) return;

  size_t byteIndex = byteOffset + index * elementSize();
  uint8_t* ptr = &buffer[byteIndex];

  switch (type) {
    case TypedArrayType::BigInt64:
      *reinterpret_cast<int64_t*>(ptr) = value;
      break;
    case TypedArrayType::BigUint64:
      *reinterpret_cast<uint64_t*>(ptr) = static_cast<uint64_t>(value);
      break;
    default:
      setElement(index, static_cast<double>(value));
      break;
  }
}

// Helper function for value equality in Map/Set
static bool valuesEqual(const Value& a, const Value& b) {
  if (a.data.index() != b.data.index()) return false;

  if (a.isNumber() && b.isNumber()) {
    double an = std::get<double>(a.data);
    double bn = std::get<double>(b.data);
    // Handle NaN
    if (std::isnan(an) && std::isnan(bn)) return true;
    return an == bn;
  }

  if (a.isString()) return std::get<std::string>(a.data) == std::get<std::string>(b.data);
  if (a.isBool()) return std::get<bool>(a.data) == std::get<bool>(b.data);
  if (a.isBigInt()) return std::get<BigInt>(a.data).value == std::get<BigInt>(b.data).value;
  if (a.isNull() || a.isUndefined()) return true;

  // For objects, compare by reference
  if (a.isObject()) return std::get<std::shared_ptr<Object>>(a.data) == std::get<std::shared_ptr<Object>>(b.data);
  if (a.isArray()) return std::get<std::shared_ptr<Array>>(a.data) == std::get<std::shared_ptr<Array>>(b.data);
  if (a.isFunction()) return std::get<std::shared_ptr<Function>>(a.data) == std::get<std::shared_ptr<Function>>(b.data);
  if (a.isMap()) return std::get<std::shared_ptr<Map>>(a.data) == std::get<std::shared_ptr<Map>>(b.data);
  if (a.isSet()) return std::get<std::shared_ptr<Set>>(a.data) == std::get<std::shared_ptr<Set>>(b.data);

  return false;
}

// Map implementation
void Map::set(const Value& key, const Value& value) {
  for (auto& entry : entries) {
    if (valuesEqual(entry.first, key)) {
      entry.second = value;
      return;
    }
  }
  entries.push_back({key, value});
}

bool Map::has(const Value& key) const {
  for (const auto& entry : entries) {
    if (valuesEqual(entry.first, key)) {
      return true;
    }
  }
  return false;
}

Value Map::get(const Value& key) const {
  for (const auto& entry : entries) {
    if (valuesEqual(entry.first, key)) {
      return entry.second;
    }
  }
  return Value(Undefined{});
}

bool Map::deleteKey(const Value& key) {
  for (auto it = entries.begin(); it != entries.end(); ++it) {
    if (valuesEqual(it->first, key)) {
      entries.erase(it);
      return true;
    }
  }
  return false;
}

// Set implementation
bool Set::add(const Value& value) {
  if (!has(value)) {
    values.push_back(value);
    return true;
  }
  return false;
}

bool Set::has(const Value& value) const {
  for (const auto& v : values) {
    if (valuesEqual(v, value)) {
      return true;
    }
  }
  return false;
}

bool Set::deleteValue(const Value& value) {
  for (auto it = values.begin(); it != values.end(); ++it) {
    if (valuesEqual(*it, value)) {
      values.erase(it);
      return true;
    }
  }
  return false;
}

// Promise implementation
void Promise::resolve(Value val) {
  if (state != PromiseState::Pending) return;

  state = PromiseState::Fulfilled;
  result = val;

  // Execute all fulfilled callbacks
  for (size_t i = 0; i < fulfilledCallbacks.size(); ++i) {
    try {
      Value callbackResult = fulfilledCallbacks[i](val);
      if (i < chainedPromises.size()) {
        chainedPromises[i]->resolve(callbackResult);
      }
    } catch (...) {
      if (i < chainedPromises.size()) {
        chainedPromises[i]->reject(Value("Callback error"));
      }
    }
  }
}

void Promise::reject(Value val) {
  if (state != PromiseState::Pending) return;

  state = PromiseState::Rejected;
  result = val;

  // Execute all rejected callbacks
  for (size_t i = 0; i < rejectedCallbacks.size(); ++i) {
    if (rejectedCallbacks[i]) {
      try {
        Value callbackResult = rejectedCallbacks[i](val);
        if (i < chainedPromises.size()) {
          chainedPromises[i]->resolve(callbackResult);
        }
      } catch (...) {
        if (i < chainedPromises.size()) {
          chainedPromises[i]->reject(Value("Callback error"));
        }
      }
    } else {
      // No rejection handler, propagate the rejection
      if (i < chainedPromises.size()) {
        chainedPromises[i]->reject(val);
      }
    }
  }
}

std::shared_ptr<Promise> Promise::then(
    std::function<Value(Value)> onFulfilled,
    std::function<Value(Value)> onRejected) {
  auto chainedPromise = std::make_shared<Promise>();

  if (state == PromiseState::Pending) {
    fulfilledCallbacks.push_back(onFulfilled ? onFulfilled : [](Value v) { return v; });
    rejectedCallbacks.push_back(onRejected);
    chainedPromises.push_back(chainedPromise);
  } else if (state == PromiseState::Fulfilled) {
    if (onFulfilled) {
      try {
        Value callbackResult = onFulfilled(result);
        chainedPromise->resolve(callbackResult);
      } catch (...) {
        chainedPromise->reject(Value("Callback error"));
      }
    } else {
      chainedPromise->resolve(result);
    }
  } else if (state == PromiseState::Rejected) {
    if (onRejected) {
      try {
        Value callbackResult = onRejected(result);
        chainedPromise->resolve(callbackResult);
      } catch (...) {
        chainedPromise->reject(Value("Callback error"));
      }
    } else {
      chainedPromise->reject(result);
    }
  }

  return chainedPromise;
}

std::shared_ptr<Promise> Promise::catch_(std::function<Value(Value)> onRejected) {
  return then(nullptr, onRejected);
}

std::shared_ptr<Promise> Promise::finally(std::function<Value()> onFinally) {
  auto finallyHandler = [onFinally](Value v) -> Value {
    onFinally();
    return v;
  };
  return then(finallyHandler, finallyHandler);
}

std::shared_ptr<Promise> Promise::all(const std::vector<std::shared_ptr<Promise>>& promises) {
  auto resultPromise = std::make_shared<Promise>();

  if (promises.empty()) {
    auto emptyArray = std::make_shared<Array>();
    resultPromise->resolve(Value(emptyArray));
    return resultPromise;
  }

  auto results = std::make_shared<std::vector<Value>>(promises.size());
  auto resolvedCount = std::make_shared<size_t>(0);

  for (size_t i = 0; i < promises.size(); ++i) {
    size_t index = i;
    promises[i]->then(
      [resultPromise, results, resolvedCount, index, promiseCount = promises.size()](Value v) -> Value {
        (*results)[index] = v;
        (*resolvedCount)++;
        if (*resolvedCount == promiseCount) {
          auto arrayResult = std::make_shared<Array>();
          arrayResult->elements = *results;
          resultPromise->resolve(Value(arrayResult));
        }
        return v;
      },
      [resultPromise](Value reason) -> Value {
        resultPromise->reject(reason);
        return reason;
      }
    );
  }

  return resultPromise;
}

std::shared_ptr<Promise> Promise::race(const std::vector<std::shared_ptr<Promise>>& promises) {
  auto resultPromise = std::make_shared<Promise>();

  for (auto& promise : promises) {
    promise->then(
      [resultPromise](Value v) -> Value {
        resultPromise->resolve(v);
        return v;
      },
      [resultPromise](Value reason) -> Value {
        resultPromise->reject(reason);
        return reason;
      }
    );
  }

  return resultPromise;
}

std::shared_ptr<Promise> Promise::resolved(const Value& value) {
  auto promise = std::make_shared<Promise>();
  promise->resolve(value);
  return promise;
}

std::shared_ptr<Promise> Promise::rejected(const Value& reason) {
  auto promise = std::make_shared<Promise>();
  promise->reject(reason);
  return promise;
}

// Object static methods implementation
Value Object_keys(const std::vector<Value>& args) {
  if (args.empty() || !args[0].isObject()) {
    auto emptyArray = std::make_shared<Array>();
    return Value(emptyArray);
  }

  auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
  auto result = std::make_shared<Array>();

  for (const auto& [key, value] : obj->properties) {
    result->elements.push_back(Value(key));
  }

  return Value(result);
}

Value Object_values(const std::vector<Value>& args) {
  if (args.empty() || !args[0].isObject()) {
    auto emptyArray = std::make_shared<Array>();
    return Value(emptyArray);
  }

  auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
  auto result = std::make_shared<Array>();

  for (const auto& [key, value] : obj->properties) {
    result->elements.push_back(value);
  }

  return Value(result);
}

Value Object_entries(const std::vector<Value>& args) {
  if (args.empty() || !args[0].isObject()) {
    auto emptyArray = std::make_shared<Array>();
    return Value(emptyArray);
  }

  auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
  auto result = std::make_shared<Array>();

  for (const auto& [key, value] : obj->properties) {
    auto entry = std::make_shared<Array>();
    entry->elements.push_back(Value(key));
    entry->elements.push_back(value);
    result->elements.push_back(Value(entry));
  }

  return Value(result);
}

Value Object_assign(const std::vector<Value>& args) {
  if (args.empty()) {
    throw std::runtime_error("Object.assign requires at least 1 argument");
  }

  Value target = args[0];
  if (target.isNull() || target.isUndefined()) {
    throw std::runtime_error("Cannot convert undefined or null to object");
  }

  // Convert target to object if needed
  std::shared_ptr<Object> targetObj;
  if (target.isObject()) {
    targetObj = std::get<std::shared_ptr<Object>>(target.data);
  } else {
    targetObj = std::make_shared<Object>();
  }

  // Copy properties from sources
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i].isNull() || args[i].isUndefined()) {
      continue; // Skip null/undefined sources
    }

    if (args[i].isObject()) {
      auto sourceObj = std::get<std::shared_ptr<Object>>(args[i].data);
      for (const auto& [key, value] : sourceObj->properties) {
        targetObj->properties[key] = value;
      }
    }
  }

  return Value(targetObj);
}

Value Object_hasOwnProperty(const std::vector<Value>& args) {
  if (args.size() < 2) {
    return Value(false);
  }

  if (!args[0].isObject()) {
    return Value(false);
  }

  if (!args[1].isString()) {
    return Value(false);
  }

  auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
  std::string key = std::get<std::string>(args[1].data);

  return Value(obj->properties.find(key) != obj->properties.end());
}

Value Object_getOwnPropertyNames(const std::vector<Value>& args) {
  if (args.empty() || !args[0].isObject()) {
    auto emptyArray = std::make_shared<Array>();
    return Value(emptyArray);
  }

  auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
  auto result = std::make_shared<Array>();

  for (const auto& [key, value] : obj->properties) {
    result->elements.push_back(Value(key));
  }

  return Value(result);
}

Value Object_create(const std::vector<Value>& args) {
  auto newObj = std::make_shared<Object>();

  // Simple implementation - doesn't handle prototype properly
  // but creates a new object
  if (args.size() > 1 && args[1].isObject()) {
    // Add properties from the properties descriptor object
    auto props = std::get<std::shared_ptr<Object>>(args[1].data);
    for (const auto& [key, descriptor] : props->properties) {
      if (descriptor.isObject()) {
        auto desc = std::get<std::shared_ptr<Object>>(descriptor.data);
        auto valueIt = desc->properties.find("value");
        if (valueIt != desc->properties.end()) {
          newObj->properties[key] = valueIt->second;
        }
      }
    }
  }

  return Value(newObj);
}

// Helper function to swap bytes for endianness conversion
template<typename T>
T swapEndian(T value) {
  union {
    T value;
    uint8_t bytes[sizeof(T)];
  } source, dest;

  source.value = value;
  for (size_t i = 0; i < sizeof(T); i++) {
    dest.bytes[i] = source.bytes[sizeof(T) - i - 1];
  }
  return dest.value;
}

// Check if system is little-endian
inline bool isLittleEndian() {
  uint16_t test = 0x0001;
  return *reinterpret_cast<uint8_t*>(&test) == 0x01;
}

// DataView get methods
int8_t DataView::getInt8(size_t offset) const {
  if (offset >= byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }
  return static_cast<int8_t>(buffer->data[byteOffset + offset]);
}

uint8_t DataView::getUint8(size_t offset) const {
  if (offset >= byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }
  return buffer->data[byteOffset + offset];
}

int16_t DataView::getInt16(size_t offset, bool littleEndian) const {
  if (offset + sizeof(int16_t) > byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  int16_t value;
  std::memcpy(&value, &buffer->data[byteOffset + offset], sizeof(int16_t));

  // If requested endianness doesn't match system, swap bytes
  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  return value;
}

uint16_t DataView::getUint16(size_t offset, bool littleEndian) const {
  if (offset + sizeof(uint16_t) > byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  uint16_t value;
  std::memcpy(&value, &buffer->data[byteOffset + offset], sizeof(uint16_t));

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  return value;
}

int32_t DataView::getInt32(size_t offset, bool littleEndian) const {
  if (offset + sizeof(int32_t) > byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  int32_t value;
  std::memcpy(&value, &buffer->data[byteOffset + offset], sizeof(int32_t));

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  return value;
}

uint32_t DataView::getUint32(size_t offset, bool littleEndian) const {
  if (offset + sizeof(uint32_t) > byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  uint32_t value;
  std::memcpy(&value, &buffer->data[byteOffset + offset], sizeof(uint32_t));

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  return value;
}

float DataView::getFloat32(size_t offset, bool littleEndian) const {
  if (offset + sizeof(float) > byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  float value;
  std::memcpy(&value, &buffer->data[byteOffset + offset], sizeof(float));

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  return value;
}

double DataView::getFloat64(size_t offset, bool littleEndian) const {
  if (offset + sizeof(double) > byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  double value;
  std::memcpy(&value, &buffer->data[byteOffset + offset], sizeof(double));

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  return value;
}

int64_t DataView::getBigInt64(size_t offset, bool littleEndian) const {
  if (offset + sizeof(int64_t) > byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  int64_t value;
  std::memcpy(&value, &buffer->data[byteOffset + offset], sizeof(int64_t));

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  return value;
}

uint64_t DataView::getBigUint64(size_t offset, bool littleEndian) const {
  if (offset + sizeof(uint64_t) > byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  uint64_t value;
  std::memcpy(&value, &buffer->data[byteOffset + offset], sizeof(uint64_t));

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  return value;
}

// DataView set methods
void DataView::setInt8(size_t offset, int8_t value) {
  if (offset >= byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }
  buffer->data[byteOffset + offset] = static_cast<uint8_t>(value);
}

void DataView::setUint8(size_t offset, uint8_t value) {
  if (offset >= byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }
  buffer->data[byteOffset + offset] = value;
}

void DataView::setInt16(size_t offset, int16_t value, bool littleEndian) {
  if (offset + sizeof(int16_t) > byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  std::memcpy(&buffer->data[byteOffset + offset], &value, sizeof(int16_t));
}

void DataView::setUint16(size_t offset, uint16_t value, bool littleEndian) {
  if (offset + sizeof(uint16_t) > byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  std::memcpy(&buffer->data[byteOffset + offset], &value, sizeof(uint16_t));
}

void DataView::setInt32(size_t offset, int32_t value, bool littleEndian) {
  if (offset + sizeof(int32_t) > byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  std::memcpy(&buffer->data[byteOffset + offset], &value, sizeof(int32_t));
}

void DataView::setUint32(size_t offset, uint32_t value, bool littleEndian) {
  if (offset + sizeof(uint32_t) > byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  std::memcpy(&buffer->data[byteOffset + offset], &value, sizeof(uint32_t));
}

void DataView::setFloat32(size_t offset, float value, bool littleEndian) {
  if (offset + sizeof(float) > byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  std::memcpy(&buffer->data[byteOffset + offset], &value, sizeof(float));
}

void DataView::setFloat64(size_t offset, double value, bool littleEndian) {
  if (offset + sizeof(double) > byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  std::memcpy(&buffer->data[byteOffset + offset], &value, sizeof(double));
}

void DataView::setBigInt64(size_t offset, int64_t value, bool littleEndian) {
  if (offset + sizeof(int64_t) > byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  std::memcpy(&buffer->data[byteOffset + offset], &value, sizeof(int64_t));
}

void DataView::setBigUint64(size_t offset, uint64_t value, bool littleEndian) {
  if (offset + sizeof(uint64_t) > byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  std::memcpy(&buffer->data[byteOffset + offset], &value, sizeof(uint64_t));
}

}