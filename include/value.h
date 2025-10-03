#pragma once

#ifndef USE_SIMPLE_REGEX
#define USE_SIMPLE_REGEX 0
#endif

#include <variant>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <optional>
#include <cstdint>
#include <cstring>
#include "gc.h"

#if USE_SIMPLE_REGEX
#include "simple_regex.h"
#else
#include <regex>
#endif

namespace tinyjs {

struct Value;
struct Object;
struct Function;
struct Promise;
struct Regex;

using ValuePtr = std::shared_ptr<Value>;

struct Undefined {};
struct Null {};
struct BigInt {
  int64_t value;
  BigInt(int64_t v = 0) : value(v) {}
};

using NativeFunction = std::function<Value(const std::vector<Value>&)>;

struct Function : public GCObject {
  std::vector<std::string> params;
  std::optional<std::string> restParam;
  std::shared_ptr<void> body;
  std::shared_ptr<void> closure;
  bool isNative;
  bool isAsync;
  NativeFunction nativeFunc;

  Function() : isNative(false), isAsync(false) {}

  // GCObject interface
  const char* typeName() const override { return "Function"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

struct Array : public GCObject {
  std::vector<Value> elements;

  // GCObject interface
  const char* typeName() const override { return "Array"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

struct Object : public GCObject {
  std::unordered_map<std::string, Value> properties;
  bool frozen = false;  // Object.freeze() prevents adding/removing/modifying properties
  bool sealed = false;  // Object.seal() prevents adding/removing properties (can still modify)

  // GCObject interface
  const char* typeName() const override { return "Object"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

// Map collection - maintains insertion order
struct Map : public GCObject {
  std::vector<std::pair<Value, Value>> entries;

  void set(const Value& key, const Value& value);
  bool has(const Value& key) const;
  Value get(const Value& key) const;
  bool deleteKey(const Value& key);
  void clear() { entries.clear(); }
  size_t size() const { return entries.size(); }

  // GCObject interface
  const char* typeName() const override { return "Map"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

// Set collection - maintains insertion order
struct Set : public GCObject {
  std::vector<Value> values;

  bool add(const Value& value);
  bool has(const Value& value) const;
  bool deleteValue(const Value& value);
  void clear() { values.clear(); }
  size_t size() const { return values.size(); }

  // GCObject interface
  const char* typeName() const override { return "Set"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

struct Regex : public GCObject {
#if USE_SIMPLE_REGEX
  simple_regex::Regex* regex;
#else
  std::regex regex;
#endif
  std::string pattern;
  std::string flags;

  Regex(const std::string& p, const std::string& f = "")
    : pattern(p), flags(f) {
#if USE_SIMPLE_REGEX
    bool caseInsensitive = false;
    for (char flag : flags) {
      if (flag == 'i') caseInsensitive = true;
    }
    regex = new simple_regex::Regex(pattern, caseInsensitive);
#else
    std::regex::flag_type options = std::regex::ECMAScript;
    for (char flag : flags) {
      if (flag == 'i') options |= std::regex::icase;
    }
    regex = std::regex(pattern, options);
#endif
  }

  ~Regex() {
#if USE_SIMPLE_REGEX
    delete regex;
#endif
  }

  Regex(const Regex& other) : pattern(other.pattern), flags(other.flags) {
#if USE_SIMPLE_REGEX
    bool caseInsensitive = false;
    for (char flag : flags) {
      if (flag == 'i') caseInsensitive = true;
    }
    regex = new simple_regex::Regex(pattern, caseInsensitive);
#else
    regex = other.regex;
#endif
  }

  Regex& operator=(const Regex& other) {
    if (this != &other) {
#if USE_SIMPLE_REGEX
      delete regex;
#endif
      pattern = other.pattern;
      flags = other.flags;
#if USE_SIMPLE_REGEX
      bool caseInsensitive = false;
      for (char flag : flags) {
        if (flag == 'i') caseInsensitive = true;
      }
      regex = new simple_regex::Regex(pattern, caseInsensitive);
#else
      regex = other.regex;
#endif
    }
    return *this;
  }

  // GCObject interface
  const char* typeName() const override { return "Regex"; }
  void getReferences(std::vector<GCObject*>& refs) const override {}
};

enum class TypedArrayType {
  Int8,
  Uint8,
  Uint8Clamped,
  Int16,
  Uint16,
  Int32,
  Uint32,
  Float16,
  Float32,
  Float64,
  BigInt64,
  BigUint64
};

inline uint16_t float32_to_float16(float value) {
  uint32_t f32;
  std::memcpy(&f32, &value, sizeof(float));

  uint32_t sign = (f32 >> 16) & 0x8000;
  int32_t exponent = ((f32 >> 23) & 0xFF) - 127 + 15;
  uint32_t mantissa = f32 & 0x7FFFFF;

  if (exponent <= 0) {
    if (exponent < -10) {
      return static_cast<uint16_t>(sign);
    }
    mantissa = (mantissa | 0x800000) >> (1 - exponent);
    return static_cast<uint16_t>(sign | (mantissa >> 13));
  } else if (exponent >= 0x1F) {
    return static_cast<uint16_t>(sign | 0x7C00);
  }

  return static_cast<uint16_t>(sign | (exponent << 10) | (mantissa >> 13));
}

inline float float16_to_float32(uint16_t value) {
  uint32_t sign = (value & 0x8000) << 16;
  uint32_t exponent = (value >> 10) & 0x1F;
  uint32_t mantissa = value & 0x3FF;

  uint32_t f32;
  if (exponent == 0) {
    if (mantissa == 0) {
      f32 = sign;
    } else {
      exponent = 1;
      while (!(mantissa & 0x400)) {
        mantissa <<= 1;
        exponent--;
      }
      mantissa &= 0x3FF;
      f32 = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
    }
  } else if (exponent == 0x1F) {
    f32 = sign | 0x7F800000 | (mantissa << 13);
  } else {
    f32 = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
  }

  float result;
  std::memcpy(&result, &f32, sizeof(float));
  return result;
}

struct TypedArray : public GCObject {
  TypedArrayType type;
  std::vector<uint8_t> buffer;
  size_t byteOffset;
  size_t length;

  TypedArray(TypedArrayType t, size_t len)
    : type(t), byteOffset(0), length(len) {
    buffer.resize(len * elementSize());
  }

  size_t elementSize() const {
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

  double getElement(size_t index) const;
  void setElement(size_t index, double value);
  int64_t getBigIntElement(size_t index) const;
  void setBigIntElement(size_t index, int64_t value);

  // GCObject interface
  const char* typeName() const override { return "TypedArray"; }
  void getReferences(std::vector<GCObject*>& refs) const override {}
};

struct Value {
  std::variant<
    Undefined,
    Null,
    bool,
    double,
    BigInt,
    std::string,
    std::shared_ptr<Function>,
    std::shared_ptr<Array>,
    std::shared_ptr<Object>,
    std::shared_ptr<TypedArray>,
    std::shared_ptr<Promise>,
    std::shared_ptr<Regex>,
    std::shared_ptr<Map>,
    std::shared_ptr<Set>
  > data;

  Value() : data(Undefined{}) {}
  Value(Undefined u) : data(u) {}
  Value(Null n) : data(n) {}
  Value(bool b) : data(b) {}
  Value(double d) : data(d) {}
  Value(int i) : data(static_cast<double>(i)) {}
  Value(BigInt bi) : data(bi) {}
  Value(int64_t i) : data(BigInt(i)) {}
  Value(const std::string& s) : data(s) {}
  Value(const char* s) : data(std::string(s)) {}
  Value(std::shared_ptr<Function> f) : data(f) {}
  Value(std::shared_ptr<Array> a) : data(a) {}
  Value(std::shared_ptr<Object> o) : data(o) {}
  Value(std::shared_ptr<TypedArray> ta) : data(ta) {}
  Value(std::shared_ptr<Promise> p) : data(p) {}
  Value(std::shared_ptr<Regex> r) : data(r) {}

  bool isUndefined() const { return std::holds_alternative<Undefined>(data); }
  bool isNull() const { return std::holds_alternative<Null>(data); }
  bool isBool() const { return std::holds_alternative<bool>(data); }
  bool isNumber() const { return std::holds_alternative<double>(data); }
  bool isBigInt() const { return std::holds_alternative<BigInt>(data); }
  bool isString() const { return std::holds_alternative<std::string>(data); }
  bool isFunction() const { return std::holds_alternative<std::shared_ptr<Function>>(data); }
  bool isArray() const { return std::holds_alternative<std::shared_ptr<Array>>(data); }
  bool isObject() const { return std::holds_alternative<std::shared_ptr<Object>>(data); }
  bool isTypedArray() const { return std::holds_alternative<std::shared_ptr<TypedArray>>(data); }
  bool isPromise() const { return std::holds_alternative<std::shared_ptr<Promise>>(data); }
  bool isRegex() const { return std::holds_alternative<std::shared_ptr<Regex>>(data); }
  bool isMap() const { return std::holds_alternative<std::shared_ptr<Map>>(data); }
  bool isSet() const { return std::holds_alternative<std::shared_ptr<Set>>(data); }

  bool toBool() const;
  double toNumber() const;
  int64_t toBigInt() const;
  std::string toString() const;
};

enum class PromiseState {
  Pending,
  Fulfilled,
  Rejected
};

struct Promise : public GCObject {
  PromiseState state;
  Value result;
  std::vector<std::function<Value(Value)>> fulfilledCallbacks;
  std::vector<std::function<Value(Value)>> rejectedCallbacks;
  std::vector<std::shared_ptr<Promise>> chainedPromises;

  Promise() : state(PromiseState::Pending), result(Undefined{}) {}

  void resolve(Value val);
  void reject(Value val);

  std::shared_ptr<Promise> then(
    std::function<Value(Value)> onFulfilled,
    std::function<Value(Value)> onRejected = nullptr);

  std::shared_ptr<Promise> catch_(std::function<Value(Value)> onRejected);
  std::shared_ptr<Promise> finally(std::function<Value()> onFinally);

  // Static methods
  static std::shared_ptr<Promise> all(const std::vector<std::shared_ptr<Promise>>& promises);
  static std::shared_ptr<Promise> race(const std::vector<std::shared_ptr<Promise>>& promises);
  static std::shared_ptr<Promise> resolved(const Value& value);
  static std::shared_ptr<Promise> rejected(const Value& reason);

  // GCObject interface
  const char* typeName() const override { return "Promise"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

}