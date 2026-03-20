#pragma once

#ifndef USE_SIMPLE_REGEX
#define USE_SIMPLE_REGEX 0
#endif

#include <functional>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "object_shape.h"
#include "ordered_map.h"
#include "value_core.h"

#if USE_SIMPLE_REGEX
#include "simple_regex.h"
#else
#include <regex>
#endif

namespace lightjs {

using NativeFunction = std::function<Value(const std::vector<Value>&)>;

struct FunctionParam {
  std::string name;
  std::shared_ptr<void> defaultValue;  // Stores ExprPtr for default value
};

struct Function : public GCObject {
  std::vector<FunctionParam> params;
  std::optional<std::string> restParam;
  std::shared_ptr<void> body;
  std::shared_ptr<void> destructurePrologue;  // Synthetic bindings for destructuring params
  // Keeps parsed AST storage alive when params/body point into transient parses
  // (e.g. Function constructor-created functions).
  std::shared_ptr<void> astOwner;
  GCPtr<Environment> closure;
  bool isNative;
  bool isAsync;
  bool isGenerator;
  bool isStrict;
  bool isConstructor = false;  // Can be called with 'new'
  std::string sourceText;  // Original source for Function.prototype.toString
  NativeFunction nativeFunc;
  OrderedMap<std::string, Value> properties;

  Function() : isNative(false), isAsync(false), isGenerator(false), isStrict(false), isConstructor(false) {}

  // GCObject interface
  const char* typeName() const override { return "Function"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

// Class structure for ES6 classes
struct Class : public GCObject {
  std::string name;
  uint64_t privateBrandId = 0;
  GCPtr<Function> constructor;  // The constructor function
  GCPtr<Class> superClass;       // Parent class (if any)
  GCPtr<Class> lexicalParentClass;  // Enclosing class for private-name lexical resolution
  std::unordered_map<std::string, GCPtr<Function>> methods;        // Instance methods
  std::unordered_map<std::string, GCPtr<Function>> staticMethods;  // Static methods
  std::unordered_map<std::string, GCPtr<Function>> getters;        // Getter methods
  std::unordered_map<std::string, GCPtr<Function>> setters;        // Setter methods
  OrderedMap<std::string, Value> properties;  // Own properties (name, length, prototype, etc.)
  std::shared_ptr<void> astOwner;  // Keeps class method/field AST storage alive.
  GCPtr<Environment> closure;  // Closure environment

  // Field initializers (public and private) - evaluated during construction
  struct FieldInit {
    std::string name;       // Field name (e.g., "x" or "#x")
    bool isPrivate;
    std::shared_ptr<void> initExpr;  // Stores Expression* for initializer (nullable = undefined)
  };
  std::vector<FieldInit> fieldInitializers;

  Class() = default;
  Class(const std::string& n) : name(n) {}

  // GCObject interface
  const char* typeName() const override { return "Class"; }
  void getReferences(std::vector<GCObject*>& refs) const override {
    if (constructor) refs.push_back(constructor.get());
    if (superClass) refs.push_back(superClass.get());
    if (lexicalParentClass) refs.push_back(lexicalParentClass.get());
    for (const auto& [_, method] : methods) {
      if (method) refs.push_back(method.get());
    }
    for (const auto& [_, method] : staticMethods) {
      if (method) refs.push_back(method.get());
    }
  }
};

struct Array : public GCObject {
  std::vector<Value> elements;
  OrderedMap<std::string, Value> properties;

  // GCObject interface
  const char* typeName() const override { return "Array"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

struct Object : public GCObject {
  OrderedMap<std::string, Value> properties;  // Fallback for dynamic properties
  std::vector<Value> slots;  // Fast slot-based storage for known properties
  std::shared_ptr<ObjectShape> shape;  // Shape for inline caching optimization
  bool isModuleNamespace = false;  // Special handling for ES module namespace objects.
  std::vector<std::string> moduleExportNames;  // Sorted string export keys.
  bool frozen = false;  // Object.freeze() prevents adding/removing/modifying properties
  bool sealed = false;  // Object.seal() prevents adding/removing properties (can still modify)
  bool nonExtensible = false;  // Object.preventExtensions() prevents adding new properties only
  bool useSlots = false;  // Whether to use slot-based storage

  Object() : shape(nullptr) {}  // Shape created lazily when needed

  // Fast property access using slots (defined after Value is complete)
  bool getSlot(int offset, Value& out) const;
  void setSlot(int offset, const Value& value);

  // GCObject interface
  const char* typeName() const override { return "Object"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

// Map collection - maintains insertion order
struct Map : public GCObject {
  std::vector<std::pair<Value, Value>> entries;
  OrderedMap<std::string, Value> properties;

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
  OrderedMap<std::string, Value> properties;

  bool add(const Value& value);
  bool has(const Value& value) const;
  bool deleteValue(const Value& value);
  void clear() { values.clear(); }
  size_t size() const { return values.size(); }

  // GCObject interface
  const char* typeName() const override { return "Set"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

// WeakMap - weak references to object keys (keys can be garbage collected)
// Note: Simplified implementation using regular references
// A full implementation would use weak_ptr or custom GC weak references
struct WeakMap : public GCObject {
  std::unordered_map<GCObject*, Value> entries;  // Map from object pointer to value
  OrderedMap<std::string, Value> properties;

  void set(const Value& key, const Value& value);
  bool has(const Value& key) const;
  Value get(const Value& key) const;
  bool deleteKey(const Value& key);
  size_t size() const { return entries.size(); }

  // GCObject interface
  const char* typeName() const override { return "WeakMap"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

// WeakSet - weak references to objects
struct WeakSet : public GCObject {
  std::unordered_set<GCObject*> values;  // Set of object pointers
  OrderedMap<std::string, Value> properties;

  bool add(const Value& value);
  bool has(const Value& value) const;
  bool deleteValue(const Value& value);
  size_t size() const { return values.size(); }

  // GCObject interface
  const char* typeName() const override { return "WeakSet"; }
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
  OrderedMap<std::string, Value> properties;

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
      if (flag == 'm') options |= std::regex::multiline;
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
  void getReferences(std::vector<GCObject*>& refs) const override;
};

// Error types for JavaScript exceptions
enum class ErrorType {
  Error,           // Generic Error
  TypeError,       // Type-related errors
  ReferenceError,  // Reference to undefined variable
  RangeError,      // Value out of range
  SyntaxError,     // Parsing/syntax errors
  URIError,        // URI handling errors
  EvalError        // eval() errors (legacy)
};

struct Error : public GCObject {
  ErrorType type;
  std::string message;
  std::string stack;  // Optional stack trace
  OrderedMap<std::string, Value> properties;

  Error(ErrorType t = ErrorType::Error, const std::string& msg = "")
    : type(t), message(msg) {}

  std::string getName() const {
    switch (type) {
      case ErrorType::Error: return "Error";
      case ErrorType::TypeError: return "TypeError";
      case ErrorType::ReferenceError: return "ReferenceError";
      case ErrorType::RangeError: return "RangeError";
      case ErrorType::SyntaxError: return "SyntaxError";
      case ErrorType::URIError: return "URIError";
      case ErrorType::EvalError: return "EvalError";
    }
    return "Error";
  }

  std::string toString() const {
    if (message.empty()) {
      return getName();
    }
    return getName() + ": " + message;
  }

  // GCObject interface
  const char* typeName() const override { return "Error"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

// Generator state for iterator protocol
enum class GeneratorState {
  SuspendedStart,  // Created but next() not called yet
  SuspendedYield,  // Suspended at a yield expression
  Executing,       // Currently executing
  Completed        // Generator has returned
};
struct Generator : public GCObject {
  GCPtr<Function> function;  // The generator function
  GCPtr<Environment> context;       // Execution context (closure)
  GeneratorState state;
  std::shared_ptr<Value> currentValue;  // Last yielded or returned value
  size_t yieldIndex;   // Index of last yield point (for resumption)
  std::shared_ptr<void> suspendedTask;  // Suspended C++ coroutine (Task)
  OrderedMap<std::string, Value> properties;

  Generator(GCPtr<Function> func, GCPtr<Environment> ctx)
    : function(func), context(ctx), state(GeneratorState::SuspendedStart),
      currentValue(std::make_shared<Value>(Undefined{})), yieldIndex(0),
      suspendedTask(nullptr, [](void* p) { 
        // This deleter will be called when suspendedTask shared_ptr is reset or destroyed.
        // We will set this up in runGeneratorNext.
      }) {}

  // GCObject interface
  const char* typeName() const override { return "Generator"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

// Proxy for intercept operations on objects
struct Proxy : public GCObject {
  std::shared_ptr<Value> target;   // The target object being proxied
  std::shared_ptr<Value> handler;  // Handler object with trap functions
  // Proxy [[Call]]-ness is fixed at creation time. Needed for typeof on revoked proxies.
  bool isCallable = false;
  bool revoked = false;

  Proxy(const Value& t, const Value& h)
    : target(std::make_shared<Value>(t)),
      handler(std::make_shared<Value>(h)) {}

  // GCObject interface
  const char* typeName() const override { return "Proxy"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

// ArrayBuffer - Fixed-length raw binary data buffer
struct ArrayBuffer : public GCObject {
  std::vector<uint8_t> data;
  size_t byteLength;
  size_t maxByteLength;
  bool resizable;
  bool detached;
  bool immutable;
  std::vector<GCPtr<TypedArray>> views;
  OrderedMap<std::string, Value> properties;

  ArrayBuffer(size_t length, size_t maxLength = 0)
    : byteLength(length),
      maxByteLength(maxLength == 0 ? length : maxLength),
      resizable(maxLength != 0 && maxLength != length),
      detached(false),
      immutable(false) {
    data.resize(length, 0);
  }

  // Constructor from existing data
  ArrayBuffer(const std::vector<uint8_t>& sourceData)
    : data(sourceData),
      byteLength(sourceData.size()),
      maxByteLength(sourceData.size()),
      resizable(false),
      detached(false),
      immutable(false) {}

  bool resize(size_t newByteLength) {
    if (detached || immutable || !resizable || newByteLength > maxByteLength) {
      return false;
    }
    data.resize(newByteLength, 0);
    byteLength = newByteLength;
    return true;
  }

  void detach() {
    detached = true;
    data.clear();
    byteLength = 0;
  }

  // GCObject interface
  const char* typeName() const override { return "ArrayBuffer"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

// DataView - Low-level interface for reading/writing multiple number types in an ArrayBuffer
struct DataView : public GCObject {
  GCPtr<ArrayBuffer> buffer;
  size_t byteOffset;
  size_t byteLength;
  bool lengthTracking;
  OrderedMap<std::string, Value> properties;

  DataView(GCPtr<ArrayBuffer> buf, size_t offset = 0, size_t length = 0)
    : buffer(buf), byteOffset(offset), byteLength(length), lengthTracking(false) {
    if (buf) {
      if (byteOffset > buf->byteLength) {
        byteLength = 0;
      }
    }
  }

  size_t currentByteLength() const {
    if (!buffer || byteOffset > buffer->byteLength) {
      return 0;
    }
    size_t available = buffer->byteLength - byteOffset;
    return lengthTracking ? available : (byteLength < available ? byteLength : available);
  }

  // Get methods with optional little-endian parameter (default true for DataView)
  int8_t getInt8(size_t byteOffset) const;
  uint8_t getUint8(size_t byteOffset) const;
  int16_t getInt16(size_t byteOffset, bool littleEndian = false) const;
  uint16_t getUint16(size_t byteOffset, bool littleEndian = false) const;
  int32_t getInt32(size_t byteOffset, bool littleEndian = false) const;
  uint32_t getUint32(size_t byteOffset, bool littleEndian = false) const;
  float getFloat16(size_t byteOffset, bool littleEndian = false) const;
  float getFloat32(size_t byteOffset, bool littleEndian = false) const;
  double getFloat64(size_t byteOffset, bool littleEndian = false) const;
  int64_t getBigInt64(size_t byteOffset, bool littleEndian = false) const;
  uint64_t getBigUint64(size_t byteOffset, bool littleEndian = false) const;

  // Set methods with optional little-endian parameter
  void setInt8(size_t byteOffset, int8_t value);
  void setUint8(size_t byteOffset, uint8_t value);
  void setInt16(size_t byteOffset, int16_t value, bool littleEndian = false);
  void setUint16(size_t byteOffset, uint16_t value, bool littleEndian = false);
  void setInt32(size_t byteOffset, int32_t value, bool littleEndian = false);
  void setUint32(size_t byteOffset, uint32_t value, bool littleEndian = false);
  void setFloat16(size_t byteOffset, double value, bool littleEndian = false);
  void setFloat32(size_t byteOffset, float value, bool littleEndian = false);
  void setFloat64(size_t byteOffset, double value, bool littleEndian = false);
  void setBigInt64(size_t byteOffset, int64_t value, bool littleEndian = false);
  void setBigUint64(size_t byteOffset, uint64_t value, bool littleEndian = false);

  // GCObject interface
  const char* typeName() const override { return "DataView"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
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

inline uint32_t round_to_even_uint32(long double value) {
  long double floorValue = std::floor(value);
  uint32_t rounded = static_cast<uint32_t>(floorValue);
  long double fraction = value - floorValue;
  if (fraction > 0.5L) {
    return rounded + 1;
  }
  if (fraction < 0.5L) {
    return rounded;
  }
  return (rounded & 1u) ? rounded + 1 : rounded;
}

inline uint16_t float64_to_float16(double value) {
  uint16_t sign = std::signbit(value) ? 0x8000 : 0;
  if (std::isnan(value)) {
    return static_cast<uint16_t>(sign | 0x7E00);
  }
  if (std::isinf(value)) {
    return static_cast<uint16_t>(sign | 0x7C00);
  }
  if (value == 0.0) {
    return sign;
  }

  long double absValue = std::fabs(static_cast<long double>(value));
  constexpr long double kMinSubnormal = 0x1p-24L;
  constexpr long double kMinNormal = 0x1p-14L;

  if (absValue < kMinNormal) {
    uint32_t subnormalMantissa = round_to_even_uint32(absValue / kMinSubnormal);
    if (subnormalMantissa == 0) {
      return sign;
    }
    if (subnormalMantissa >= 1024) {
      return static_cast<uint16_t>(sign | 0x0400);
    }
    return static_cast<uint16_t>(sign | subnormalMantissa);
  }

  int exponent = 0;
  std::frexp(absValue, &exponent);
  int32_t halfExponent = exponent - 1 + 15;
  if (halfExponent >= 0x1F) {
    return static_cast<uint16_t>(sign | 0x7C00);
  }

  long double normalized = std::ldexp(absValue, -(exponent - 1));
  uint32_t mantissa = round_to_even_uint32((normalized - 1.0L) * 1024.0L);
  if (mantissa == 1024) {
    mantissa = 0;
    halfExponent++;
    if (halfExponent >= 0x1F) {
      return static_cast<uint16_t>(sign | 0x7C00);
    }
  }

  return static_cast<uint16_t>(sign | (static_cast<uint32_t>(halfExponent) << 10) | mantissa);
}

inline uint16_t float32_to_float16(float value) {
  return float64_to_float16(static_cast<double>(value));
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
  GCPtr<ArrayBuffer> viewedBuffer;
  size_t byteOffset;
  size_t length;
  bool lengthTracking;
  OrderedMap<std::string, Value> properties;

  TypedArray(TypedArrayType t, size_t len)
    : type(t), byteOffset(0), length(len), lengthTracking(false) {
    buffer.resize(len * elementSize());
  }

  TypedArray(TypedArrayType t, GCPtr<ArrayBuffer> backing, size_t offset,
             size_t len, bool isLengthTracking = false)
    : type(t),
      viewedBuffer(backing),
      byteOffset(offset),
      length(len),
      lengthTracking(isLengthTracking) {}

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
  uint64_t getBigUintElement(size_t index) const;
  void setBigIntElement(size_t index, int64_t value);
  void setBigUintElement(size_t index, uint64_t value);
  bool isView() const { return static_cast<bool>(viewedBuffer); }
  bool isOutOfBounds() const;
  size_t currentLength() const;
  size_t currentByteLength() const;
  std::vector<uint8_t>& storage();
  const std::vector<uint8_t>& storage() const;

  // Bulk operations (SIMD-accelerated when USE_SIMD=1)
  // Copy elements from another TypedArray with type conversion
  void copyFrom(const TypedArray& source, size_t srcOffset, size_t dstOffset, size_t count);

  // Fill all elements with a single value
  void fill(double value);
  void fill(double value, size_t start, size_t end);

  // Set multiple elements from a double array
  void setElements(const double* values, size_t offset, size_t count);

  // Get multiple elements to a double array
  void getElements(double* values, size_t offset, size_t count) const;

  // GCObject interface
  const char* typeName() const override { return "TypedArray"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
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
  std::vector<GCPtr<Promise>> chainedPromises;
  OrderedMap<std::string, Value> properties;

  Promise() : state(PromiseState::Pending), result(Undefined{}) {}

  void resolve(Value val);
  void reject(Value val);

  GCPtr<Promise> then(
    std::function<Value(Value)> onFulfilled,
    std::function<Value(Value)> onRejected = nullptr);

  GCPtr<Promise> catch_(std::function<Value(Value)> onRejected);
  GCPtr<Promise> finally(std::function<Value()> onFinally);

  // Static methods
  static GCPtr<Promise> all(const std::vector<GCPtr<Promise>>& promises);
  static GCPtr<Promise> race(const std::vector<GCPtr<Promise>>& promises);
  static GCPtr<Promise> resolved(const Value& value);
  static GCPtr<Promise> rejected(const Value& reason);

  // GCObject interface
  const char* typeName() const override { return "Promise"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

// Pre-allocated common values to avoid repeated allocation
class CommonValues {
public:
  static const Value& undefined() {
    static Value v(Undefined{});
    return v;
  }

  static const Value& null() {
    static Value v(Null{});
    return v;
  }

  static const Value& trueValue() {
    static Value v(true);
    return v;
  }

  static const Value& falseValue() {
    static Value v(false);
    return v;
  }

  static const Value& zero() {
    static Value v(0.0);
    return v;
  }

  static const Value& one() {
    static Value v(1.0);
    return v;
  }

  static const Value& negOne() {
    static Value v(-1.0);
    return v;
  }

  static const Value& emptyString() {
    static Value v(std::string(""));
    return v;
  }
};

// Small integer cache for frequently used values (0-255)
class SmallIntCache {
public:
  static constexpr int MIN_CACHED = 0;
  static constexpr int MAX_CACHED = 255;

  static const Value& get(int n) {
    static SmallIntCache instance;
    if (n >= MIN_CACHED && n <= MAX_CACHED) {
      return instance.cache_[n - MIN_CACHED];
    }
    // Return a reference to a static value for out-of-range
    static Value outOfRange;
    outOfRange = Value(static_cast<double>(n));
    return outOfRange;
  }

  static bool inRange(double d) {
    int i = static_cast<int>(d);
    return d == static_cast<double>(i) && i >= MIN_CACHED && i <= MAX_CACHED;
  }

private:
  SmallIntCache() {
    for (int i = MIN_CACHED; i <= MAX_CACHED; ++i) {
      cache_[i - MIN_CACHED] = Value(static_cast<double>(i));
    }
  }

  Value cache_[MAX_CACHED - MIN_CACHED + 1];
};

// Internal property-key helpers used by runtime objects/builtins.
// Symbol keys are encoded as stable internal strings so different symbols with
// the same description don't collide in OrderedMap<string, Value>.
std::string symbolToPropertyKey(const Symbol& symbol);
bool isSymbolPropertyKey(const std::string& key);
bool propertyKeyToSymbol(const std::string& key, Symbol& outSymbol);
std::string valueToPropertyKey(const Value& value);
std::string ecmaNumberToString(double value);

// ES spec Unicode whitespace helpers (shared across value.cc, string_methods.cc, interpreter.cc)
bool isESWhitespace(const std::string& str, size_t pos, size_t& advance);
std::string stripESWhitespace(const std::string& str);
std::string stripLeadingESWhitespace(const std::string& str);
std::string stripTrailingESWhitespace(const std::string& str);

}
