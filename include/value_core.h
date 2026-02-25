#pragma once

#ifndef USE_SIMPLE_REGEX
#define USE_SIMPLE_REGEX 0
#endif

#include <variant>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <functional>
#include <optional>
#include <cstdint>
#include <cstring>
#include "bigint.h"
#include "gc.h"

#if USE_SIMPLE_REGEX
#include "simple_regex.h"
#else
#include <regex>
#endif

namespace lightjs {

struct Class;
struct Array;
struct Object;
struct Function;
struct Promise;
struct Regex;
struct Error;
struct Generator;
struct Proxy;
struct WeakMap;
struct WeakSet;
struct ArrayBuffer;
struct DataView;
struct WasmInstanceJS;
struct WasmMemoryJS;
struct ReadableStream;
struct WritableStream;
struct TransformStream;
struct Map;
struct Set;
class Module;

struct Undefined {};
struct Null {};
struct BigInt {
  bigint::BigIntValue value;
  BigInt() : value(0) {}
  BigInt(const bigint::BigIntValue& v) : value(v) {}
  BigInt(int64_t v) : value(v) {}
};

struct Symbol {
  static size_t nextId;
  size_t id;
  std::string description;

  Symbol(const std::string& desc = "") : id(nextId++), description(desc) {}

  bool operator==(const Symbol& other) const { return id == other.id; }
  bool operator!=(const Symbol& other) const { return id != other.id; }
};

struct ModuleBinding {
  std::weak_ptr<Module> module;
  std::string exportName;
};

struct TypedArray; // forward for variant

struct Value {
  std::variant<
    Undefined,
    Null,
    bool,
    double,
    BigInt,
    Symbol,
    ModuleBinding,
    std::string,
    std::shared_ptr<Function>,
    std::shared_ptr<Array>,
    std::shared_ptr<Object>,
    std::shared_ptr<TypedArray>,
    std::shared_ptr<Promise>,
    std::shared_ptr<Regex>,
    std::shared_ptr<Map>,
    std::shared_ptr<Set>,
    std::shared_ptr<Error>,
    std::shared_ptr<Generator>,
    std::shared_ptr<Proxy>,
    std::shared_ptr<WeakMap>,
    std::shared_ptr<WeakSet>,
    std::shared_ptr<ArrayBuffer>,
    std::shared_ptr<DataView>,
    std::shared_ptr<Class>,
    std::shared_ptr<WasmInstanceJS>,
    std::shared_ptr<WasmMemoryJS>,
    std::shared_ptr<ReadableStream>,
    std::shared_ptr<WritableStream>,
    std::shared_ptr<TransformStream>
  > data;

  Value() : data(Undefined{}) {}
  Value(Undefined u) : data(u) {}
  Value(Null n) : data(n) {}
  Value(bool b) : data(b) {}
  Value(double d) : data(d) {}
  Value(int i) : data(static_cast<double>(i)) {}
  Value(BigInt bi) : data(bi) {}
  Value(const bigint::BigIntValue& bi) : data(BigInt(bi)) {}
  Value(int64_t i) : data(BigInt(i)) {}
  Value(Symbol sym) : data(sym) {}
  Value(ModuleBinding binding) : data(std::move(binding)) {}
  Value(const std::string& s) : data(s) {}
  Value(const char* s) : data(std::string(s)) {}
  Value(std::shared_ptr<Function> f) : data(f) {}
  Value(std::shared_ptr<Array> a) : data(a) {}
  Value(std::shared_ptr<Object> o) : data(o) {}
  Value(std::shared_ptr<TypedArray> ta) : data(ta) {}
  Value(std::shared_ptr<Promise> p) : data(p) {}
  Value(std::shared_ptr<Regex> r) : data(r) {}
  Value(std::shared_ptr<Map> m) : data(m) {}
  Value(std::shared_ptr<Set> s) : data(s) {}
  Value(std::shared_ptr<Error> e) : data(e) {}
  Value(std::shared_ptr<Generator> g) : data(g) {}
  Value(std::shared_ptr<Proxy> p) : data(p) {}
  Value(std::shared_ptr<WeakMap> wm) : data(wm) {}
  Value(std::shared_ptr<WeakSet> ws) : data(ws) {}
  Value(std::shared_ptr<ArrayBuffer> ab) : data(ab) {}
  Value(std::shared_ptr<DataView> dv) : data(dv) {}
  Value(std::shared_ptr<Class> c) : data(c) {}
  Value(std::shared_ptr<WasmInstanceJS> wi) : data(wi) {}
  Value(std::shared_ptr<WasmMemoryJS> wm) : data(wm) {}
  Value(std::shared_ptr<ReadableStream> rs) : data(rs) {}
  Value(std::shared_ptr<WritableStream> ws) : data(ws) {}
  Value(std::shared_ptr<TransformStream> ts) : data(ts) {}

  bool isUndefined() const { return std::holds_alternative<Undefined>(data); }
  bool isNull() const { return std::holds_alternative<Null>(data); }
  bool isBool() const { return std::holds_alternative<bool>(data); }
  bool isNumber() const { return std::holds_alternative<double>(data); }
  bool isBigInt() const { return std::holds_alternative<BigInt>(data); }
  bool isSymbol() const { return std::holds_alternative<Symbol>(data); }
  bool isModuleBinding() const { return std::holds_alternative<ModuleBinding>(data); }
  bool isString() const { return std::holds_alternative<std::string>(data); }
  bool isFunction() const { return std::holds_alternative<std::shared_ptr<Function>>(data); }
  bool isArray() const { return std::holds_alternative<std::shared_ptr<Array>>(data); }
  bool isObject() const { return std::holds_alternative<std::shared_ptr<Object>>(data); }
  bool isTypedArray() const { return std::holds_alternative<std::shared_ptr<TypedArray>>(data); }
  bool isPromise() const { return std::holds_alternative<std::shared_ptr<Promise>>(data); }
  bool isRegex() const { return std::holds_alternative<std::shared_ptr<Regex>>(data); }
  bool isMap() const { return std::holds_alternative<std::shared_ptr<Map>>(data); }
  bool isSet() const { return std::holds_alternative<std::shared_ptr<Set>>(data); }
  bool isError() const { return std::holds_alternative<std::shared_ptr<Error>>(data); }
  bool isGenerator() const { return std::holds_alternative<std::shared_ptr<Generator>>(data); }
  bool isProxy() const { return std::holds_alternative<std::shared_ptr<Proxy>>(data); }
  bool isWeakMap() const { return std::holds_alternative<std::shared_ptr<WeakMap>>(data); }
  bool isWeakSet() const { return std::holds_alternative<std::shared_ptr<WeakSet>>(data); }
  bool isArrayBuffer() const { return std::holds_alternative<std::shared_ptr<ArrayBuffer>>(data); }
  bool isDataView() const { return std::holds_alternative<std::shared_ptr<DataView>>(data); }
  bool isClass() const { return std::holds_alternative<std::shared_ptr<Class>>(data); }
  bool isWasmInstance() const { return std::holds_alternative<std::shared_ptr<WasmInstanceJS>>(data); }
  bool isWasmMemory() const { return std::holds_alternative<std::shared_ptr<WasmMemoryJS>>(data); }
  bool isReadableStream() const { return std::holds_alternative<std::shared_ptr<ReadableStream>>(data); }
  bool isWritableStream() const { return std::holds_alternative<std::shared_ptr<WritableStream>>(data); }
  bool isTransformStream() const { return std::holds_alternative<std::shared_ptr<TransformStream>>(data); }

  bool toBool() const;
  double toNumber() const;
  bigint::BigIntValue toBigInt() const;
  std::string toString() const;
  // Display string for REPL/debugging - BigInt shows "42n" suffix
  std::string toDisplayString() const;
};

using ValuePtr = std::shared_ptr<Value>;

}
