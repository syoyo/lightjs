#pragma once

#include <variant>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>

namespace tinyjs {

struct Value;
struct Object;
struct Function;

using ValuePtr = std::shared_ptr<Value>;

struct Undefined {};
struct Null {};

using NativeFunction = std::function<Value(const std::vector<Value>&)>;

struct Function {
  std::vector<std::string> params;
  std::shared_ptr<void> body;
  std::shared_ptr<void> closure;
  bool isNative;
  NativeFunction nativeFunc;

  Function() : isNative(false) {}
};

struct Array {
  std::vector<Value> elements;
};

struct Object {
  std::unordered_map<std::string, Value> properties;
};

struct Value {
  std::variant<
    Undefined,
    Null,
    bool,
    double,
    std::string,
    std::shared_ptr<Function>,
    std::shared_ptr<Array>,
    std::shared_ptr<Object>
  > data;

  Value() : data(Undefined{}) {}
  Value(Undefined u) : data(u) {}
  Value(Null n) : data(n) {}
  Value(bool b) : data(b) {}
  Value(double d) : data(d) {}
  Value(int i) : data(static_cast<double>(i)) {}
  Value(const std::string& s) : data(s) {}
  Value(const char* s) : data(std::string(s)) {}
  Value(std::shared_ptr<Function> f) : data(f) {}
  Value(std::shared_ptr<Array> a) : data(a) {}
  Value(std::shared_ptr<Object> o) : data(o) {}

  bool isUndefined() const { return std::holds_alternative<Undefined>(data); }
  bool isNull() const { return std::holds_alternative<Null>(data); }
  bool isBool() const { return std::holds_alternative<bool>(data); }
  bool isNumber() const { return std::holds_alternative<double>(data); }
  bool isString() const { return std::holds_alternative<std::string>(data); }
  bool isFunction() const { return std::holds_alternative<std::shared_ptr<Function>>(data); }
  bool isArray() const { return std::holds_alternative<std::shared_ptr<Array>>(data); }
  bool isObject() const { return std::holds_alternative<std::shared_ptr<Object>>(data); }

  bool toBool() const;
  double toNumber() const;
  std::string toString() const;
};

}