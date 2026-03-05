#pragma once

#ifndef USE_SIMPLE_REGEX
#define USE_SIMPLE_REGEX 0
#endif

#include <variant>
#include <memory>
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
struct Environment;
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
// Internal completion record marker (spec "empty" completion value).
// This must never be observable to user code; it is used only by statement
// evaluation to implement UpdateEmpty correctly.
struct Empty {};
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
  Symbol(size_t symbolId, const std::string& desc) : id(symbolId), description(desc) {
    if (nextId <= symbolId) {
      nextId = symbolId + 1;
    }
  }

  bool operator==(const Symbol& other) const { return id == other.id; }
  bool operator!=(const Symbol& other) const { return id != other.id; }
};

struct ModuleBinding {
  std::weak_ptr<Module> module;
  std::string exportName;
};

struct TypedArray; // forward for variant
struct Date;

struct Value {
  std::variant<
    Undefined,
    Empty,
    Null,
    bool,
    double,
    BigInt,
    Symbol,
    ModuleBinding,
    std::string,
    GCPtr<Function>,
    GCPtr<Array>,
    GCPtr<Object>,
    GCPtr<TypedArray>,
    GCPtr<Promise>,
    GCPtr<Regex>,
    GCPtr<Map>,
    GCPtr<Set>,
    GCPtr<Error>,
    GCPtr<Generator>,
    GCPtr<Proxy>,
    GCPtr<WeakMap>,
    GCPtr<WeakSet>,
    GCPtr<ArrayBuffer>,
    GCPtr<DataView>,
    GCPtr<Class>,
    GCPtr<WasmInstanceJS>,
    GCPtr<WasmMemoryJS>,
    GCPtr<ReadableStream>,
    GCPtr<WritableStream>,
    GCPtr<TransformStream>,
    GCPtr<Environment>,
    std::shared_ptr<void>
  > data;

  Value() : data(Undefined{}) {}
  Value(Undefined u) : data(u) {}
  Value(Empty e) : data(e) {}
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

  // Backwards compatibility constructors (temporary)
  Value(GCPtr<Function> f) : data(f) {}
  Value(GCPtr<Array> a) : data(a) {}
  Value(GCPtr<Object> o) : data(o) {}
  Value(GCPtr<TypedArray> ta) : data(ta) {}
  Value(GCPtr<Promise> p) : data(p) {}
  Value(GCPtr<Regex> r) : data(r) {}
  Value(GCPtr<Map> m) : data(m) {}
  Value(GCPtr<Set> s) : data(s) {}
  Value(GCPtr<Error> e) : data(e) {}
  Value(GCPtr<Generator> g) : data(g) {}
  Value(GCPtr<Proxy> p) : data(p) {}
  Value(GCPtr<WeakMap> wm) : data(wm) {}
  Value(GCPtr<WeakSet> ws) : data(ws) {}
  Value(GCPtr<ArrayBuffer> ab) : data(ab) {}
  Value(GCPtr<DataView> dv) : data(dv) {}
  Value(GCPtr<Class> c) : data(c) {}
  Value(GCPtr<Environment> e) : data(e) {}
  Value(GCPtr<WasmInstanceJS> w) : data(w) {}
  Value(GCPtr<WasmMemoryJS> w) : data(w) {}
  Value(GCPtr<ReadableStream> rs) : data(rs) {}
  Value(GCPtr<WritableStream> ws) : data(ws) {}
  Value(GCPtr<TransformStream> ts) : data(ts) {}
  Value(std::shared_ptr<void> d) : data(d) {}

  bool isUndefined() const { return std::holds_alternative<Undefined>(data); }
  bool isEmpty() const { return std::holds_alternative<Empty>(data); }
  bool isNull() const { return std::holds_alternative<Null>(data); }
  bool isBool() const { return std::holds_alternative<bool>(data); }
  bool isNumber() const { return std::holds_alternative<double>(data); }
  bool isBigInt() const { return std::holds_alternative<BigInt>(data); }
  bool isSymbol() const { return std::holds_alternative<Symbol>(data); }
  bool isModuleBinding() const { return std::holds_alternative<ModuleBinding>(data); }
  bool isString() const { return std::holds_alternative<std::string>(data); }
  bool isFunction() const { return std::holds_alternative<GCPtr<Function>>(data); }
  bool isArray() const { return std::holds_alternative<GCPtr<Array>>(data); }
  bool isObject() const { return std::holds_alternative<GCPtr<Object>>(data); }
  bool isTypedArray() const { return std::holds_alternative<GCPtr<TypedArray>>(data); }
  bool isPromise() const { return std::holds_alternative<GCPtr<Promise>>(data); }
  bool isRegex() const { return std::holds_alternative<GCPtr<Regex>>(data); }
  bool isMap() const { return std::holds_alternative<GCPtr<Map>>(data); }
  bool isSet() const { return std::holds_alternative<GCPtr<Set>>(data); }
  bool isError() const { return std::holds_alternative<GCPtr<Error>>(data); }
  bool isGenerator() const { return std::holds_alternative<GCPtr<Generator>>(data); }
  bool isProxy() const { return std::holds_alternative<GCPtr<Proxy>>(data); }
  bool isWeakMap() const { return std::holds_alternative<GCPtr<WeakMap>>(data); }
  bool isWeakSet() const { return std::holds_alternative<GCPtr<WeakSet>>(data); }
  bool isArrayBuffer() const { return std::holds_alternative<GCPtr<ArrayBuffer>>(data); }
  bool isDataView() const { return std::holds_alternative<GCPtr<DataView>>(data); }
  bool isClass() const { return std::holds_alternative<GCPtr<Class>>(data); }
  bool isWasmInstance() const { return std::holds_alternative<GCPtr<WasmInstanceJS>>(data); }
  bool isWasmMemory() const { return std::holds_alternative<GCPtr<WasmMemoryJS>>(data); }
  bool isReadableStream() const { return std::holds_alternative<GCPtr<ReadableStream>>(data); }
  bool isWritableStream() const { return std::holds_alternative<GCPtr<WritableStream>>(data); }
  bool isTransformStream() const { return std::holds_alternative<GCPtr<TransformStream>>(data); }

  template<typename T>
  GCPtr<T> getGC() const {
    if (auto* ptr = std::get_if<GCPtr<T>>(&data)) {
      return *ptr;
    }
    return GCPtr<T>{};
  }

  bool toBool() const;
  double toNumber() const;
  bigint::BigIntValue toBigInt() const;
  std::string toString() const;
  // Display string for REPL/debugging - BigInt shows "42n" suffix
  std::string toDisplayString() const;

  void getReferences(std::vector<GCObject*>& refs) const;
};

using ValuePtr = std::shared_ptr<Value>;

}
