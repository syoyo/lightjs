#define LIGHTJS_INTERPRETER_INTERNAL
#include "interpreter.h"
#include "lexer.h"
#include "parser.h"

#if LIGHTJS_HAS_COROUTINES
namespace lightjs {

// Helper: set __proto__ on a newly created array so that Array.prototype methods and
// `constructor` are accessible via the prototype chain.
static void setArrayPrototype(const GCPtr<Array>& arr, Environment* env) {
  auto arrCtor = env->get("Array");
  if (!arrCtor) return;
  OrderedMap<std::string, Value>* ctorProps = nullptr;
  if (arrCtor->isFunction()) {
    ctorProps = &std::get<GCPtr<Function>>(arrCtor->data)->properties;
  } else if (arrCtor->isObject()) {
    ctorProps = &std::get<GCPtr<Object>>(arrCtor->data)->properties;
  }
  if (ctorProps) {
    auto protoIt = ctorProps->find("prototype");
    if (protoIt != ctorProps->end() && protoIt->second.isObject()) {
      arr->properties["__proto__"] = protoIt->second;
    }
  }
}

void TaskAwaiter::await_suspend(std::coroutine_handle<> awaiting) noexcept {
  while (!task.done()) {
    task.resume();
    if (interp.flow_.type == Interpreter::ControlFlow::Type::Yield) return;
  }
  awaiting.resume();
}
}
#endif

#include "module.h"
#include "array_methods.h"
#include "string_methods.h"
#include "unicode.h"
#include "gc.h"
#include "event_loop.h"
#include "symbols.h"
#include "streams.h"
#include "wasm_js.h"
#include <iostream>
#include <cmath>
#include <climits>
#include <sstream>
#include <iomanip>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <atomic>

namespace lightjs {

namespace {
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

bool typedArrayConstructorNameToType(const std::string& name, TypedArrayType& outType) {
  if (name == "Int8Array") {
    outType = TypedArrayType::Int8;
  } else if (name == "Uint8Array") {
    outType = TypedArrayType::Uint8;
  } else if (name == "Uint8ClampedArray") {
    outType = TypedArrayType::Uint8Clamped;
  } else if (name == "Int16Array") {
    outType = TypedArrayType::Int16;
  } else if (name == "Uint16Array") {
    outType = TypedArrayType::Uint16;
  } else if (name == "Float16Array") {
    outType = TypedArrayType::Float16;
  } else if (name == "Int32Array") {
    outType = TypedArrayType::Int32;
  } else if (name == "Uint32Array") {
    outType = TypedArrayType::Uint32;
  } else if (name == "Float32Array") {
    outType = TypedArrayType::Float32;
  } else if (name == "Float64Array") {
    outType = TypedArrayType::Float64;
  } else if (name == "BigInt64Array") {
    outType = TypedArrayType::BigInt64;
  } else if (name == "BigUint64Array") {
    outType = TypedArrayType::BigUint64;
  } else {
    return false;
  }
  return true;
}

int32_t toInt32(double value) {
  if (!std::isfinite(value) || value == 0.0) {
    return 0;
  }

  double intPart = std::trunc(value);
  constexpr double kTwo32 = 4294967296.0;
  double wrapped = std::fmod(intPart, kTwo32);
  if (wrapped < 0) {
    wrapped += kTwo32;
  }
  if (wrapped >= 2147483648.0) {
    wrapped -= kTwo32;
  }
  return static_cast<int32_t>(wrapped);
}

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

// Parse JS integer-like strings used in BigInt/string comparisons.
bool parseBigIntString64(const std::string& raw, bigint::BigIntValue& out) {
  return bigint::parseBigIntString(raw, out);
}

bigint::BigIntValue powBigInt(bigint::BigIntValue base, bigint::BigIntValue exp) {
  bigint::BigIntValue result = 1;
  while (exp > 0) {
    if ((exp & 1) != 0) {
      result *= base;
    }
    exp >>= 1;
    if (exp != 0) {
      base *= base;
    }
  }
  return result;
}

bool toShiftCount(const bigint::BigIntValue& shift,
                  bool* negative,
                  size_t* outCount) {
  *negative = shift < 0;
  bigint::BigIntValue magnitude = *negative ? -shift : shift;
  return bigint::toSizeT(magnitude, *outCount);
}

bigint::BigIntValue applyBigIntShiftLeft(const bigint::BigIntValue& lhs,
                                         const bigint::BigIntValue& rhs,
                                         bool* ok) {
  bool negative = false;
  size_t count = 0;
  if (!toShiftCount(rhs, &negative, &count)) {
    *ok = false;
    return 0;
  }
  *ok = true;
  if (negative) {
    return bigint::BigIntValue(lhs >> count);
  }
  return bigint::BigIntValue(lhs << count);
}

bigint::BigIntValue applyBigIntShiftRight(const bigint::BigIntValue& lhs,
                                          const bigint::BigIntValue& rhs,
                                          bool* ok) {
  bool negative = false;
  size_t count = 0;
  if (!toShiftCount(rhs, &negative, &count)) {
    *ok = false;
    return 0;
  }
  *ok = true;
  if (negative) {
    return bigint::BigIntValue(lhs << count);
  }
  return bigint::BigIntValue(lhs >> count);
}

enum class BigIntNumberOrder { Less, Equal, Greater, Undefined };

std::atomic<uint64_t> g_classPrivateBrandCounter{1};

std::string privateStorageKey(const GCPtr<Class>& owner, const std::string& privateName) {
  if (owner && owner->privateBrandId != 0) {
    return "__private_" + std::to_string(owner->privateBrandId) + "_" + privateName + "__";
  }
  return "__private_" + privateName + "__";
}

std::string privateMethodMarkerKey(const GCPtr<Class>& owner, const std::string& privateName) {
  if (owner && owner->privateBrandId != 0) {
    return "__private_method_marker_" + std::to_string(owner->privateBrandId) + "_" + privateName + "__";
  }
  return "__private_method_marker_" + privateName + "__";
}

bool classDeclaresInstancePrivateName(const GCPtr<Class>& cls, const std::string& privateName) {
  if (!cls) return false;
  for (const auto& fi : cls->fieldInitializers) {
    if (fi.isPrivate && fi.name == privateName) {
      return true;
    }
  }
  if (cls->methods.find(privateName) != cls->methods.end()) return true;
  if (cls->getters.find(privateName) != cls->getters.end()) return true;
  if (cls->setters.find(privateName) != cls->setters.end()) return true;
  return false;
}

bool classDeclaresStaticPrivateName(const GCPtr<Class>& cls, const std::string& privateName) {
  if (!cls) return false;
  const std::string mangledName = privateStorageKey(cls, privateName);
  if (cls->properties.find(mangledName) != cls->properties.end()) return true;
  if (cls->properties.find("__get_" + mangledName) != cls->properties.end()) return true;
  if (cls->properties.find("__set_" + mangledName) != cls->properties.end()) return true;
  if (cls->staticMethods.find(mangledName) != cls->staticMethods.end()) return true;
  return false;
}

bool isOwnerInClassChain(GCPtr<Class> target, const GCPtr<Class>& owner) {
  int depth = 0;
  while (target && depth < 128) {
    if (target.get() == owner.get()) {
      return true;
    }
    target = target->superClass;
    depth++;
  }
  return false;
}

GCPtr<Class> resolveInstancePrivateOwnerClass(const GCPtr<Class>& startOwner,
                                              const std::string& privateName) {
  GCPtr<Class> cls = startOwner;
  int depth = 0;
  while (cls && depth < 128) {
    if (classDeclaresInstancePrivateName(cls, privateName)) {
      return cls;
    }
    cls = cls->lexicalParentClass;
    depth++;
  }
  return nullptr;
}

GCPtr<Class> resolveStaticPrivateOwnerClass(const GCPtr<Class>& startOwner,
                                            const std::string& privateName) {
  GCPtr<Class> cls = startOwner;
  int depth = 0;
  while (cls && depth < 128) {
    if (classDeclaresStaticPrivateName(cls, privateName)) {
      return cls;
    }
    cls = cls->lexicalParentClass;
    depth++;
  }
  return nullptr;
}

enum class PrivateNameKind {
  None,
  Instance,
  Static,
};

struct PrivateNameOwner {
  GCPtr<Class> owner = nullptr;
  PrivateNameKind kind = PrivateNameKind::None;
};

PrivateNameOwner resolvePrivateNameOwnerClass(const GCPtr<Class>& startOwner,
                                              const std::string& privateName) {
  GCPtr<Class> cls = startOwner;
  int depth = 0;
  while (cls && depth < 128) {
    const bool hasInstance = classDeclaresInstancePrivateName(cls, privateName);
    const bool hasStatic = classDeclaresStaticPrivateName(cls, privateName);
    if (hasInstance || hasStatic) {
      PrivateNameOwner resolved;
      resolved.owner = cls;
      resolved.kind =
          (hasInstance && !hasStatic) ? PrivateNameKind::Instance : PrivateNameKind::Static;
      return resolved;
    }
    cls = cls->lexicalParentClass;
    depth++;
  }
  return {};
}

GCPtr<Class> getConstructorClassForPrivateAccess(const Value& value, int depth = 0) {
  if (depth >= 16) {
    return nullptr;
  }
  if (value.isObject()) {
    auto obj = value.getGC<Object>();
    auto it = obj->properties.find("__constructor__");
    if (it != obj->properties.end() && it->second.isClass()) {
      return it->second.getGC<Class>();
    }
  } else if (value.isArray()) {
    auto arr = value.getGC<Array>();
    auto it = arr->properties.find("__constructor__");
    if (it != arr->properties.end() && it->second.isClass()) {
      return it->second.getGC<Class>();
    }
  } else if (value.isFunction()) {
    auto fn = value.getGC<Function>();
    auto it = fn->properties.find("__constructor__");
    if (it != fn->properties.end() && it->second.isClass()) {
      return it->second.getGC<Class>();
    }
  } else if (value.isRegex()) {
    auto regex = value.getGC<Regex>();
    auto it = regex->properties.find("__constructor__");
    if (it != regex->properties.end() && it->second.isClass()) {
      return it->second.getGC<Class>();
    }
  } else if (value.isPromise()) {
    auto promise = value.getGC<Promise>();
    auto it = promise->properties.find("__constructor__");
    if (it != promise->properties.end() && it->second.isClass()) {
      return it->second.getGC<Class>();
    }
  } else if (value.isProxy()) {
    auto proxy = value.getGC<Proxy>();
    if (proxy->target) {
      return getConstructorClassForPrivateAccess(*proxy->target, depth + 1);
    }
  }
  return nullptr;
}

OrderedMap<std::string, Value>* getPropertyStorageForPrivateAccess(Value& value) {
  if (value.isObject()) {
    return &value.getGC<Object>()->properties;
  }
  if (value.isArray()) {
    return &value.getGC<Array>()->properties;
  }
  if (value.isFunction()) {
    return &value.getGC<Function>()->properties;
  }
  if (value.isRegex()) {
    return &value.getGC<Regex>()->properties;
  }
  if (value.isPromise()) {
    return &value.getGC<Promise>()->properties;
  }
  return nullptr;
}

const OrderedMap<std::string, Value>* getPropertyStorageForPrivateAccess(const Value& value) {
  if (value.isObject()) {
    return &value.getGC<Object>()->properties;
  }
  if (value.isArray()) {
    return &value.getGC<Array>()->properties;
  }
  if (value.isFunction()) {
    return &value.getGC<Function>()->properties;
  }
  if (value.isRegex()) {
    return &value.getGC<Regex>()->properties;
  }
  if (value.isPromise()) {
    return &value.getGC<Promise>()->properties;
  }
  return nullptr;
}

BigIntNumberOrder compareBigIntAndNumber(const bigint::BigIntValue& bi, double n) {
  if (std::isnan(n)) return BigIntNumberOrder::Undefined;
  if (std::isinf(n)) {
    return n > 0 ? BigIntNumberOrder::Less : BigIntNumberOrder::Greater;
  }

  bigint::BigIntValue nAsBigInt = 0;
  if (bigint::fromIntegralDouble(n, nAsBigInt)) {
    if (bi < nAsBigInt) return BigIntNumberOrder::Less;
    if (bi > nAsBigInt) return BigIntNumberOrder::Greater;
    return BigIntNumberOrder::Equal;
  }

  double biAsDouble = bi.convert_to<double>();
  if (std::isinf(biAsDouble)) {
    return bi > 0 ? BigIntNumberOrder::Greater : BigIntNumberOrder::Less;
  }
  if (biAsDouble < n) return BigIntNumberOrder::Less;
  if (biAsDouble > n) return BigIntNumberOrder::Greater;

  double floorN = std::floor(n);
  bigint::BigIntValue floorAsBigInt = 0;
  if (!bigint::fromIntegralDouble(floorN, floorAsBigInt)) {
    return bi > 0 ? BigIntNumberOrder::Greater : BigIntNumberOrder::Less;
  }
  return (bi <= floorAsBigInt) ? BigIntNumberOrder::Less : BigIntNumberOrder::Greater;
}

// Helper: compute result of compound assignment (e.g., +=, -=, <<=, etc.)
// Does NOT handle AddAssign (needs toPrimitiveValue which requires Interpreter context)
// Does NOT handle AndAssign/OrAssign/NullishAssign (short-circuit semantics)
Value computeCompoundOp(AssignmentExpr::Op op, const Value& current, const Value& right) {
  switch (op) {
    case AssignmentExpr::Op::SubAssign:
      if (current.isBigInt() && right.isBigInt()) return Value(BigInt(current.toBigInt() - right.toBigInt()));
      return Value(current.toNumber() - right.toNumber());
    case AssignmentExpr::Op::MulAssign:
      if (current.isBigInt() && right.isBigInt()) return Value(BigInt(current.toBigInt() * right.toBigInt()));
      return Value(current.toNumber() * right.toNumber());
    case AssignmentExpr::Op::DivAssign:
      if (current.isBigInt() && right.isBigInt()) {
        auto divisor = right.toBigInt();
        if (divisor == 0) return Value(Undefined{});
        return Value(BigInt(current.toBigInt() / divisor));
      }
      return Value(current.toNumber() / right.toNumber());
    case AssignmentExpr::Op::ModAssign:
      if (current.isBigInt() && right.isBigInt()) {
        auto divisor = right.toBigInt();
        if (divisor == 0) return Value(Undefined{});
        return Value(BigInt(current.toBigInt() % divisor));
      }
      return Value(std::fmod(current.toNumber(), right.toNumber()));
    case AssignmentExpr::Op::ExpAssign:
      if (current.isBigInt() && right.isBigInt()) {
        auto base = current.toBigInt();
        auto exp = right.toBigInt();
        if (exp < 0) return Value(Undefined{});
        return Value(BigInt(powBigInt(base, exp)));
      }
      return Value(std::pow(current.toNumber(), right.toNumber()));
    case AssignmentExpr::Op::BitwiseAndAssign:
      if (current.isBigInt() && right.isBigInt()) return Value(BigInt(current.toBigInt() & right.toBigInt()));
      return Value(static_cast<double>(toInt32(current.toNumber()) & toInt32(right.toNumber())));
    case AssignmentExpr::Op::BitwiseOrAssign:
      if (current.isBigInt() && right.isBigInt()) return Value(BigInt(current.toBigInt() | right.toBigInt()));
      return Value(static_cast<double>(toInt32(current.toNumber()) | toInt32(right.toNumber())));
    case AssignmentExpr::Op::BitwiseXorAssign:
      if (current.isBigInt() && right.isBigInt()) return Value(BigInt(current.toBigInt() ^ right.toBigInt()));
      return Value(static_cast<double>(toInt32(current.toNumber()) ^ toInt32(right.toNumber())));
    case AssignmentExpr::Op::LeftShiftAssign:
      if (current.isBigInt() && right.isBigInt()) {
        bool ok = false;
        auto shifted = applyBigIntShiftLeft(current.toBigInt(), right.toBigInt(), &ok);
        if (!ok) return Value(Undefined{});
        return Value(BigInt(shifted));
      }
      return Value(static_cast<double>(toInt32(current.toNumber()) << (toInt32(right.toNumber()) & 0x1f)));
    case AssignmentExpr::Op::RightShiftAssign:
      if (current.isBigInt() && right.isBigInt()) {
        bool ok = false;
        auto shifted = applyBigIntShiftRight(current.toBigInt(), right.toBigInt(), &ok);
        if (!ok) return Value(Undefined{});
        return Value(BigInt(shifted));
      }
      return Value(static_cast<double>(toInt32(current.toNumber()) >> (toInt32(right.toNumber()) & 0x1f)));
    case AssignmentExpr::Op::UnsignedRightShiftAssign:
      return Value(static_cast<double>(static_cast<uint32_t>(toInt32(current.toNumber())) >> (toInt32(right.toNumber()) & 0x1f)));
    default:
      return right;
  }
}

std::string numberToPropertyKey(double value) {
  if (std::isnan(value)) return "NaN";
  if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
  if (value == 0.0) return "0";

  double integral = std::trunc(value);
  if (integral == value) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(0) << value;
    return oss.str();
  }

  std::ostringstream oss;
  oss << std::setprecision(15) << value;
  std::string out = oss.str();
  auto expPos = out.find_first_of("eE");
  if (expPos != std::string::npos) {
    std::string mantissa = out.substr(0, expPos);
    std::string exponent = out.substr(expPos + 1);
    char sign = '\0';
    size_t idx = 0;
    if (!exponent.empty() && (exponent[0] == '+' || exponent[0] == '-')) {
      sign = exponent[0];
      idx = 1;
    }
    while (idx < exponent.size() && exponent[idx] == '0') {
      idx++;
    }
    std::string expDigits = (idx < exponent.size()) ? exponent.substr(idx) : "0";
    out = mantissa + "e";
    if (sign == '-') out += "-";
    out += expDigits;
    return out;
  }
  auto dot = out.find('.');
  if (dot != std::string::npos) {
    while (!out.empty() && out.back() == '0') out.pop_back();
    if (!out.empty() && out.back() == '.') out.pop_back();
  }
  return out;
}

std::string toPropertyKeyString(const Value& value) {
  return valueToPropertyKey(value);
}

// Helper to set standard name/length properties on native functions
// Per ES spec: name is {writable:false, enumerable:false, configurable:true}
// Per ES spec: length is {writable:false, enumerable:false, configurable:true}
void setNativeFnProps(const GCPtr<Function>& fn, const std::string& name, int length) {
  fn->properties["name"] = Value(name);
  fn->properties["length"] = Value(static_cast<double>(length));
  fn->properties["__non_writable_name"] = Value(true);
  fn->properties["__non_enum_name"] = Value(true);
  fn->properties["__non_writable_length"] = Value(true);
  fn->properties["__non_enum_length"] = Value(true);
}

Value toPropertyKeyValue(const std::string& key) {
  Symbol symbolValue;
  if (propertyKeyToSymbol(key, symbolValue)) {
    return Value(symbolValue);
  }
  return Value(key);
}

bool parseArrayIndex(const std::string& key, size_t& index) {
  if (key.empty()) return false;
  if (key.size() > 1 && key[0] == '0') return false;
  constexpr uint64_t kMaxArrayIndex = static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) - 1;
  uint64_t parsed = 0;
  for (unsigned char c : key) {
    if (c < '0' || c > '9') return false;
    uint64_t digit = static_cast<uint64_t>(c - '0');
    if (parsed > (kMaxArrayIndex - digit) / 10) {
      return false;
    }
    parsed = parsed * 10 + digit;
  }
  index = static_cast<size_t>(parsed);
  return true;
}

// Compare JavaScript strings by UTF-16 code units (ECMAScript relational
// comparisons use code unit order, not UTF-8 byte order).
struct Utf16CodeUnitIter {
  const std::string& s;
  size_t i = 0;
  bool hasPending = false;
  uint16_t pending = 0;

  static uint32_t decodeNext(const std::string& str, size_t& index) {
    // Permissive UTF-8 decoder that also accepts surrogate code points.
    if (index >= str.size()) return 0;
    unsigned char c0 = static_cast<unsigned char>(str[index]);
    if (c0 < 0x80) {
      index += 1;
      return c0;
    }
    auto cont = [&](size_t pos) -> unsigned char {
      return static_cast<unsigned char>(str[pos]);
    };
    if ((c0 & 0xE0) == 0xC0 && index + 1 < str.size()) {
      unsigned char c1 = cont(index + 1);
      if ((c1 & 0xC0) != 0x80) {
        index += 1;
        return c0;
      }
      uint32_t cp = ((c0 & 0x1F) << 6) | (c1 & 0x3F);
      index += 2;
      return cp;
    }
    if ((c0 & 0xF0) == 0xE0 && index + 2 < str.size()) {
      unsigned char c1 = cont(index + 1);
      unsigned char c2 = cont(index + 2);
      if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) {
        index += 1;
        return c0;
      }
      uint32_t cp = ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
      index += 3;
      return cp;
    }
    if ((c0 & 0xF8) == 0xF0 && index + 3 < str.size()) {
      unsigned char c1 = cont(index + 1);
      unsigned char c2 = cont(index + 2);
      unsigned char c3 = cont(index + 3);
      if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) {
        index += 1;
        return c0;
      }
      uint32_t cp = ((c0 & 0x07) << 18) | ((c1 & 0x3F) << 12) |
                    ((c2 & 0x3F) << 6) | (c3 & 0x3F);
      index += 4;
      return cp;
    }
    // Invalid lead byte.
    index += 1;
    return c0;
  }

  bool next(uint16_t& out) {
    if (hasPending) {
      out = pending;
      hasPending = false;
      return true;
    }
    if (i >= s.size()) return false;
    uint32_t cp = decodeNext(s, i);
    if (cp <= 0xFFFF) {
      out = static_cast<uint16_t>(cp);
      return true;
    }
    cp -= 0x10000;
    uint16_t hi = static_cast<uint16_t>(0xD800 + (cp >> 10));
    uint16_t lo = static_cast<uint16_t>(0xDC00 + (cp & 0x3FF));
    out = hi;
    pending = lo;
    hasPending = true;
    return true;
  }
};

int compareStringsByUtf16CodeUnits(const std::string& a, const std::string& b) {
  Utf16CodeUnitIter ita{a};
  Utf16CodeUnitIter itb{b};
  uint16_t ua = 0;
  uint16_t ub = 0;
  while (true) {
    bool ha = ita.next(ua);
    bool hb = itb.next(ub);
    if (!ha && !hb) return 0;
    if (!ha) return -1;
    if (!hb) return 1;
    if (ua < ub) return -1;
    if (ua > ub) return 1;
  }
}

bool hasUseStrictDirective(const std::vector<StmtPtr>& body) {
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
    if (str->value == "use strict" && !str->hasEscape) {
      return true;
    }
  }
  return false;
}

}  // namespace

// Forward declaration for TDZ initialization
static void collectVarHoistNames(const Expression& expr, std::vector<std::string>& names);

Interpreter::Interpreter(GCPtr<Environment> env) : env_(env), maxLoopIterations_(getMaxLoopIterations()) {
  setGlobalInterpreter(this);
}

bool Interpreter::hasError() const {
  return flow_.type == ControlFlow::Type::Throw;
}

Value Interpreter::getError() const {
  return flow_.value;
}

void Interpreter::clearError() {
  flow_.type = ControlFlow::Type::None;
  flow_.value = Value(Undefined{});
}

bool Interpreter::activeFunctionIsArrow() const {
  if (!activeFunction_) {
    return false;
  }
  auto it = activeFunction_->properties.find("__is_arrow_function__");
  return it != activeFunction_->properties.end() &&
         it->second.isBool() &&
         it->second.toBool();
}

bool Interpreter::activeFunctionHasHomeObject() const {
  if (!activeFunction_) {
    return false;
  }
  return activeFunction_->properties.find("__home_object__") != activeFunction_->properties.end();
}

bool Interpreter::activeFunctionHasSuperClassBinding() const {
  if (!activeFunction_) {
    return false;
  }
  return activeFunction_->properties.find("__super_class__") != activeFunction_->properties.end();
}

bool Interpreter::activeFunctionIsConstructor() const {
  return activeFunction_ && activeFunction_->isConstructor;
}

std::set<std::string> Interpreter::activePrivateNamesForEval() const {
  std::set<std::string> names;
  if (!activePrivateOwnerClass_) {
    return names;
  }

  auto cls = activePrivateOwnerClass_;
  int depth = 0;
  while (cls && depth < 128) {
    for (const auto& fi : cls->fieldInitializers) {
      if (fi.isPrivate) {
        names.insert(fi.name);
      }
    }
    for (const auto& [name, _] : cls->methods) {
      names.insert(name);
    }
    for (const auto& [name, _] : cls->getters) {
      names.insert(name);
    }
    for (const auto& [name, _] : cls->setters) {
      names.insert(name);
    }

    const std::string prefix = "__private_" + std::to_string(cls->privateBrandId) + "_";
    auto collectFromStorageKey = [&](const std::string& rawKey) {
      std::string key = rawKey;
      if (key.rfind("__get_", 0) == 0 || key.rfind("__set_", 0) == 0) {
        key = key.substr(6);
      }
      if (key.rfind(prefix, 0) != 0) {
        return;
      }
      if (key.size() <= prefix.size() + 2 || key.substr(key.size() - 2) != "__") {
        return;
      }
      names.insert(key.substr(prefix.size(), key.size() - prefix.size() - 2));
    };

    for (const auto& key : cls->properties.orderedKeys()) {
      collectFromStorageKey(key);
    }
    for (const auto& [key, _] : cls->staticMethods) {
      collectFromStorageKey(key);
    }

    cls = cls->lexicalParentClass;
    depth++;
  }

  return names;
}

void Interpreter::inheritDirectEvalContextFrom(const Interpreter& caller) {
  activePrivateOwnerClass_ = caller.activePrivateOwnerClass_;
  activeFunction_ = caller.activeFunction_;
}

Value Interpreter::callForHarness(const Value& callee,
                                  const std::vector<Value>& args,
                                  const Value& thisValue) {
  return callFunction(callee, args, thisValue);
}

std::optional<Value> Interpreter::resolveVariable(const std::string& name) {
  return env_->get(name);
}

Value Interpreter::runScriptInGlobalScope(const std::string& source) {
  Lexer lexer(source);
  auto tokens = lexer.tokenize();
  Parser parser(tokens);
  parser.setSource(source);
  auto program = parser.parse();
  if (!program) {
    throwError(ErrorType::SyntaxError, "Parse error in evalScript");
    return Value(Undefined{});
  }

  // Save and restore environment - run in global scope
  auto savedEnv = env_;
  env_ = env_->getRoot();

  // Keep AST alive for the duration
  auto kept = std::make_shared<Program>(std::move(*program));
  auto savedKeepAlive = sourceKeepAlive_;
  sourceKeepAlive_ = std::static_pointer_cast<void>(kept);

  // GlobalDeclarationInstantiation: check restricted globals
  {
    GCPtr<Object> globalObj;
    auto globalThisOpt = env_->get("globalThis");
    if (globalThisOpt && globalThisOpt->isObject()) {
      globalObj = std::get<GCPtr<Object>>(globalThisOpt->data);
    }
    if (globalObj) {
      for (const auto& stmt : kept->body) {
        std::vector<std::string> lexNames;
        if (auto* varDecl = std::get_if<VarDeclaration>(&stmt->node)) {
          if (varDecl->kind == VarDeclaration::Kind::Let ||
              varDecl->kind == VarDeclaration::Kind::Const) {
            for (const auto& declarator : varDecl->declarations) {
              collectVarHoistNames(*declarator.pattern, lexNames);
            }
          }
        } else if (auto* classDecl = std::get_if<ClassDeclaration>(&stmt->node)) {
          lexNames.push_back(classDecl->id.name);
        }
        for (const auto& name : lexNames) {
          // Check HasRestrictedGlobalProperty
          auto propIt = globalObj->properties.find(name);
          if (propIt != globalObj->properties.end()) {
            auto ncIt = globalObj->properties.find("__non_configurable_" + name);
            if (ncIt != globalObj->properties.end()) {
              throwError(ErrorType::SyntaxError,
                "Cannot declare a lexical binding for '" + name + "': already a restricted global property");
              sourceKeepAlive_ = savedKeepAlive;
              env_ = savedEnv;
              return Value(Undefined{});
            }
          }
          // Check HasVarDeclaration - lexical cannot shadow script-level var bindings
          // (eval-created vars are configurable and don't count as var declarations per spec)
          if (env_->hasLocal(name) && !env_->hasLexicalLocal(name)) {
            // It's a var binding - check if it's a script-created (non-configurable) one
            auto ncIt = globalObj->properties.find("__non_configurable_" + name);
            if (ncIt != globalObj->properties.end()) {
              throwError(ErrorType::SyntaxError,
                "Identifier '" + name + "' has already been declared");
              sourceKeepAlive_ = savedKeepAlive;
              env_ = savedEnv;
              return Value(Undefined{});
            }
          }
          // Check HasLexicalDeclaration
          if (env_->hasLexicalLocal(name) || env_->isTDZ(name)) {
            throwError(ErrorType::SyntaxError,
              "Identifier '" + name + "' has already been declared");
            sourceKeepAlive_ = savedKeepAlive;
            env_ = savedEnv;
            return Value(Undefined{});
          }
        }
      }
      // Step 6: var names cannot shadow lexical declarations
      {
        std::vector<std::string> allVarNames;
        for (const auto& stmt : kept->body) {
          if (auto* varDecl = std::get_if<VarDeclaration>(&stmt->node)) {
            if (varDecl->kind == VarDeclaration::Kind::Var) {
              for (const auto& declarator : varDecl->declarations) {
                collectVarHoistNames(*declarator.pattern, allVarNames);
              }
            }
          } else if (auto* funcDecl = std::get_if<FunctionDeclaration>(&stmt->node)) {
            allVarNames.push_back(funcDecl->id.name);
          }
        }
        for (const auto& vn : allVarNames) {
          if (env_->hasLexicalLocal(vn) || env_->isTDZ(vn)) {
            throwError(ErrorType::SyntaxError,
              "Identifier '" + vn + "' has already been declared");
            sourceKeepAlive_ = savedKeepAlive;
            env_ = savedEnv;
            return Value(Undefined{});
          }
        }
      }

      // CanDeclareGlobalFunction checks for function declarations
      bool isExtensible = !globalObj->sealed && !globalObj->frozen && !globalObj->nonExtensible;
      for (const auto& stmt : kept->body) {
        if (auto* funcDecl = std::get_if<FunctionDeclaration>(&stmt->node)) {
          const std::string& fn = funcDecl->id.name;
          auto existingProp = globalObj->properties.find(fn);
          if (existingProp == globalObj->properties.end()) {
            // No existing property - check IsExtensible
            if (!isExtensible) {
              throwError(ErrorType::TypeError,
                "Cannot declare global function '" + fn + "': global object is not extensible");
              sourceKeepAlive_ = savedKeepAlive;
              env_ = savedEnv;
              return Value(Undefined{});
            }
          } else {
            // Check CanDeclareGlobalFunction: configurable, or data+writable+enumerable
            bool isConfigurable = globalObj->properties.find("__non_configurable_" + fn) == globalObj->properties.end();
            if (!isConfigurable) {
              bool isNonWritable = globalObj->properties.find("__non_writable_" + fn) != globalObj->properties.end();
              bool isNonEnum = globalObj->properties.find("__non_enum_" + fn) != globalObj->properties.end();
              bool isAccessor = globalObj->properties.find("__get_" + fn) != globalObj->properties.end() ||
                                globalObj->properties.find("__set_" + fn) != globalObj->properties.end();
              // Data property with writable:true AND enumerable:true is ok
              bool isWritableEnumerableData = !isAccessor && !isNonWritable && !isNonEnum;
              if (!isWritableEnumerableData) {
                throwError(ErrorType::TypeError,
                  "Cannot declare global function '" + fn + "': existing property is non-configurable");
                sourceKeepAlive_ = savedKeepAlive;
                env_ = savedEnv;
                return Value(Undefined{});
              }
            }
          }
        }
      }

      // CanDeclareGlobalVar checks for var declarations
      for (const auto& stmt : kept->body) {
        if (auto* varDecl = std::get_if<VarDeclaration>(&stmt->node)) {
          if (varDecl->kind == VarDeclaration::Kind::Var) {
            for (const auto& declarator : varDecl->declarations) {
              std::vector<std::string> varNames;
              collectVarHoistNames(*declarator.pattern, varNames);
              for (const auto& vn : varNames) {
                auto existingProp = globalObj->properties.find(vn);
                if (existingProp == globalObj->properties.end()) {
                  if (!isExtensible) {
                    throwError(ErrorType::TypeError,
                      "Cannot declare global variable '" + vn + "': global object is not extensible");
                    sourceKeepAlive_ = savedKeepAlive;
                    env_ = savedEnv;
                    return Value(Undefined{});
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  // Hoist var declarations
  hoistVarDeclarations(kept->body);

  // Hoist function declarations
  for (const auto& stmt : kept->body) {
    if (std::holds_alternative<FunctionDeclaration>(stmt->node)) {
      auto task = evaluate(*stmt);
      Value dummy;
      LIGHTJS_RUN_TASK_SYNC(task, dummy);
      if (flow_.type == ControlFlow::Type::Throw) {
        sourceKeepAlive_ = savedKeepAlive;
        env_ = savedEnv;
        return dummy;
      }
    }
  }

  // TDZ for lexical declarations
  for (const auto& stmt : kept->body) {
    if (auto* varDecl = std::get_if<VarDeclaration>(&stmt->node)) {
      if (varDecl->kind == VarDeclaration::Kind::Let ||
          varDecl->kind == VarDeclaration::Kind::Const) {
        for (const auto& declarator : varDecl->declarations) {
          std::vector<std::string> names;
          collectVarHoistNames(*declarator.pattern, names);
          for (const auto& name : names) {
            env_->defineTDZ(name);
          }
        }
      }
    } else if (auto* classDecl = std::get_if<ClassDeclaration>(&stmt->node)) {
      env_->defineTDZ(classDecl->id.name);
    }
  }

  // Evaluate statements
  Value result = Value(Undefined{});
  for (const auto& stmt : kept->body) {
    if (std::holds_alternative<FunctionDeclaration>(stmt->node)) {
      continue;  // Already hoisted
    }
    auto task = evaluate(*stmt);
    Value val;
    LIGHTJS_RUN_TASK_SYNC(task, val);
    if (!val.isEmpty()) {
      result = val;
    }
    if (flow_.type != ControlFlow::Type::None) {
      break;
    }
  }

  sourceKeepAlive_ = savedKeepAlive;
  env_ = savedEnv;
  return result;
}

Value Interpreter::constructFromNative(const Value& constructor,
                                       const std::vector<Value>& args) {
  auto task = constructValue(constructor, args);
  Value result;
  LIGHTJS_RUN_TASK_SYNC(task, result);
  return result;
}

std::pair<bool, Value> Interpreter::getPropertyForExternal(const Value& receiver,
                                                           const std::string& key) {
  auto direct = getPropertyForPrimitive(receiver, key);
  if (direct.first || isObjectLike(receiver)) {
    return direct;
  }

  auto boxPrimitiveAndLookup = [this, &key, &receiver](const std::string& ctorName) -> std::pair<bool, Value> {
    auto ctor = env_->get(ctorName);
    if (!ctor) {
      return {false, Value(Undefined{})};
    }
    auto wrapper = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    wrapper->properties["__primitive_value__"] = receiver;
    auto [foundProto, proto] = getPropertyForPrimitive(*ctor, "prototype");
    if (foundProto && (proto.isObject() || proto.isNull())) {
      wrapper->properties["__proto__"] = proto;
    }
    return getPropertyForPrimitive(Value(wrapper), key);
  };

  if (receiver.isBool()) {
    return boxPrimitiveAndLookup("Boolean");
  }
  if (receiver.isNumber()) {
    return boxPrimitiveAndLookup("Number");
  }
  if (receiver.isString()) {
    return boxPrimitiveAndLookup("String");
  }
  if (receiver.isSymbol()) {
    return boxPrimitiveAndLookup("Symbol");
  }
  if (receiver.isBigInt()) {
    return boxPrimitiveAndLookup("BigInt");
  }

  return direct;
}

Value Interpreter::generatorNext(const Value& generatorVal, const Value& resumeValue) {
  if (!generatorVal.isGenerator()) {
    return makeIteratorResult(Value(Undefined{}), true);
  }
  auto gen = generatorVal.getGC<Generator>();
  return runGeneratorNext(gen, ControlFlow::ResumeMode::Next, resumeValue);
}

bool Interpreter::isObjectLike(const Value& value) const {
  return value.isObject() || value.isArray() || value.isFunction() || value.isRegex() ||
         value.isProxy() || value.isPromise() || value.isGenerator() || value.isClass() ||
         value.isMap() || value.isSet() || value.isWeakMap() || value.isWeakSet() ||
         value.isTypedArray() || value.isArrayBuffer() || value.isDataView() || value.isError();
}

std::pair<bool, Value> Interpreter::getPropertyForPrimitive(const Value& receiver, const std::string& key) {
  auto getFromPropertyBag = [&](OrderedMap<std::string, Value>& bag) -> std::pair<bool, Value> {
    std::string getterKey = "__get_" + key;
    auto getterIt = bag.find(getterKey);
    if (getterIt != bag.end()) {
      if (getterIt->second.isFunction()) {
        return {true, callFunction(getterIt->second, {}, receiver)};
      }
      return {true, Value(Undefined{})};
    }
    // Accessor with setter only: Get returns undefined but the property exists.
    std::string setterKey = "__set_" + key;
    if (bag.find(setterKey) != bag.end()) {
      return {true, Value(Undefined{})};
    }

    auto it = bag.find(key);
    if (it != bag.end()) {
      return {true, it->second};
    }

    // Walk the prototype chain through any object-like value.
    auto protoIt = bag.find("__proto__");
    if (protoIt == bag.end() || !isObjectLike(protoIt->second)) {
      return {false, Value(Undefined{})};
    }
    if (!protoIt->second.isObject()) {
      return getPropertyForPrimitive(protoIt->second, key);
    }
    auto current = protoIt->second.getGC<Object>();
    int depth = 0;
    while (current && depth <= 16) {
      depth++;
      auto protoGetterIt = current->properties.find("__get_" + key);
      if (protoGetterIt != current->properties.end()) {
        if (protoGetterIt->second.isFunction()) {
          return {true, callFunction(protoGetterIt->second, {}, receiver)};
        }
        return {true, Value(Undefined{})};
      }
      if (current->properties.find("__set_" + key) != current->properties.end()) {
        return {true, Value(Undefined{})};
      }
      auto foundIt = current->properties.find(key);
      if (foundIt != current->properties.end()) {
        return {true, foundIt->second};
      }
      auto nextProto = current->properties.find("__proto__");
      if (nextProto == current->properties.end() || !isObjectLike(nextProto->second)) {
        break;
      }
      if (!nextProto->second.isObject()) {
        return getPropertyForPrimitive(nextProto->second, key);
      }
      current = nextProto->second.getGC<Object>();
    }

    return {false, Value(Undefined{})};
  };

  if (receiver.isObject()) {
    auto current = receiver.getGC<Object>();
    int depth = 0;
    while (current && depth <= 16) {
      depth++;

      std::string getterKey = "__get_" + key;
      auto getterIt = current->properties.find(getterKey);
      if (getterIt != current->properties.end()) {
        if (getterIt->second.isFunction()) {
          return {true, callFunction(getterIt->second, {}, receiver)};
        }
        return {true, Value(Undefined{})};
      }
      if (current->properties.find("__set_" + key) != current->properties.end()) {
        return {true, Value(Undefined{})};
      }

      auto it = current->properties.find(key);
      if (it != current->properties.end()) {
        return {true, it->second};
      }

      auto protoIt = current->properties.find("__proto__");
      if (protoIt == current->properties.end() || !isObjectLike(protoIt->second)) {
        break;
      }
      if (!protoIt->second.isObject()) {
        return getPropertyForPrimitive(protoIt->second, key);
      }
      current = protoIt->second.getGC<Object>();
    }
    return {false, Value(Undefined{})};
  }

  if (receiver.isArray()) {
    auto arr = receiver.getGC<Array>();
    if (key == "length") {
      // Arguments objects may have overridden length
      auto overriddenIt = arr->properties.find("__overridden_length__");
      if (overriddenIt != arr->properties.end()) {
        return {true, overriddenIt->second};
      }
      return {true, Value(static_cast<double>(arr->elements.size()))};
    }

    auto getterIt = arr->properties.find("__get_" + key);
    if (getterIt != arr->properties.end()) {
      if (getterIt->second.isFunction()) {
        return {true, callFunction(getterIt->second, {}, receiver)};
      }
      return {true, Value(Undefined{})};
    }
    if (arr->properties.find("__set_" + key) != arr->properties.end()) {
      return {true, Value(Undefined{})};
    }

    size_t index = 0;
    if (parseArrayIndex(key, index) && index < arr->elements.size()) {
      return {true, arr->elements[index]};
    }

    auto it = arr->properties.find(key);
    if (it != arr->properties.end()) {
      return {true, it->second};
    }

    auto protoIt = arr->properties.find("__proto__");
    if (protoIt != arr->properties.end() && isObjectLike(protoIt->second)) {
      if (protoIt->second.isObject()) {
        return getFromPropertyBag(protoIt->second.getGC<Object>()->properties);
      }
      return getPropertyForPrimitive(protoIt->second, key);
    }
    return {false, Value(Undefined{})};
  }

  if (receiver.isGenerator()) {
    auto gen = receiver.getGC<Generator>();
    return getFromPropertyBag(gen->properties);
  }

  if (receiver.isClass()) {
    auto cls = receiver.getGC<Class>();
    return getFromPropertyBag(cls->properties);
  }

  if (receiver.isMap()) {
    auto map = receiver.getGC<Map>();
    return getFromPropertyBag(map->properties);
  }

  if (receiver.isSet()) {
    auto set = receiver.getGC<Set>();
    return getFromPropertyBag(set->properties);
  }

  if (receiver.isWeakMap()) {
    auto wm = receiver.getGC<WeakMap>();
    return getFromPropertyBag(wm->properties);
  }

  if (receiver.isWeakSet()) {
    auto ws = receiver.getGC<WeakSet>();
    return getFromPropertyBag(ws->properties);
  }

  if (receiver.isTypedArray()) {
    auto ta = receiver.getGC<TypedArray>();
    const std::string getterKey = "__get_" + key;
    const std::string setterKey = "__set_" + key;
    size_t index = 0;
    if (parseArrayIndex(key, index) && index < ta->currentLength()) {
      if (ta->type == TypedArrayType::BigInt64 || ta->type == TypedArrayType::BigUint64) {
        if (ta->type == TypedArrayType::BigUint64) {
          return {true, Value(BigInt(bigint::BigIntValue(ta->getBigUintElement(index))))};
        }
        return {true, Value(BigInt(ta->getBigIntElement(index)))};
      }
      return {true, Value(ta->getElement(index))};
    }
    bool hasOwnOverride = ta->properties.find(key) != ta->properties.end() ||
                          ta->properties.find(getterKey) != ta->properties.end() ||
                          ta->properties.find(setterKey) != ta->properties.end();
    if (!hasOwnOverride) {
      if (key == "length") {
        return {true, Value(static_cast<double>(ta->currentLength()))};
      }
      if (key == "byteLength") {
        return {true, Value(static_cast<double>(ta->currentByteLength()))};
      }
      if (key == "byteOffset") {
        if (ta->viewedBuffer && (ta->viewedBuffer->detached || ta->isOutOfBounds())) {
          return {true, Value(0.0)};
        }
        return {true, Value(static_cast<double>(ta->byteOffset))};
      }
      if (key == "buffer") {
        return {true, ta->viewedBuffer ? Value(ta->viewedBuffer) : Value(Undefined{})};
      }
    }
    return getFromPropertyBag(ta->properties);
  }

  if (receiver.isArrayBuffer()) {
    auto ab = receiver.getGC<ArrayBuffer>();
    return getFromPropertyBag(ab->properties);
  }

  if (receiver.isDataView()) {
    auto dv = receiver.getGC<DataView>();
    return getFromPropertyBag(dv->properties);
  }

  if (receiver.isError()) {
    auto err = receiver.getGC<Error>();
    auto result = getFromPropertyBag(err->properties);
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
      if (auto ctor = env_->get(err->getName())) {
        return {true, *ctor};
      }
      return {true, Value(Undefined{})};
    }
    return {false, Value(Undefined{})};
  }

  if (receiver.isFunction()) {
    auto fn = receiver.getGC<Function>();
    return getFromPropertyBag(fn->properties);
  }

  if (receiver.isRegex()) {
    return getFromPropertyBag(receiver.getGC<Regex>()->properties);
  }

  if (receiver.isProxy()) {
    auto proxy = receiver.getGC<Proxy>();
    if (proxy->target) {
      return getPropertyForPrimitive(*proxy->target, key);
    }
  }

  return {false, Value(Undefined{})};
}

Value Interpreter::toPrimitiveValue(const Value& input, bool preferString, bool useDefaultHint) {
  if (!isObjectLike(input)) {
    return input;
  }

  const std::string& toPrimitiveKey = WellKnownSymbols::toPrimitiveKey();
  auto [hasExotic, exotic] = getPropertyForPrimitive(input, toPrimitiveKey);
  if (hasError()) {
    return Value(Undefined{});
  }
  if (hasExotic && !exotic.isUndefined() && !exotic.isNull()) {
    if (!exotic.isFunction()) {
      throwError(ErrorType::TypeError, "@@toPrimitive is not callable");
      return Value(Undefined{});
    }
    Value hint = Value(std::string(useDefaultHint ? "default" : (preferString ? "string" : "number")));
    Value result = callFunction(exotic, {hint}, input);
    if (hasError()) {
      return Value(Undefined{});
    }
    if (isObjectLike(result)) {
      throwError(ErrorType::TypeError, "@@toPrimitive must return a primitive");
      return Value(Undefined{});
    }
    return result;
  }

  bool ordinaryPreferString = preferString;
  if (useDefaultHint && input.isObject()) {
    auto obj = input.getGC<Object>();
    auto isDateIt = obj->properties.find("__is_date__");
    ordinaryPreferString = (isDateIt != obj->properties.end() &&
                            isDateIt->second.isBool() &&
                            isDateIt->second.toBool());
  }

  const char* firstMethod = ordinaryPreferString ? "toString" : "valueOf";
  const char* secondMethod = ordinaryPreferString ? "valueOf" : "toString";
  const char* methods[2] = {firstMethod, secondMethod};

  for (const char* methodName : methods) {
    // For arrays, toString should call join() (Array.prototype.toString behavior)
    if (std::string(methodName) == "toString" && input.isArray()) {
      auto arr = input.getGC<Array>();
      std::string out;
      for (size_t i = 0; i < arr->elements.size(); i++) {
        if (i > 0) out += ",";
        if (!arr->elements[i].isUndefined() && !arr->elements[i].isNull()) {
          out += arr->elements[i].toString();
        }
      }
      return Value(out);
    }
    auto [found, method] = getPropertyForPrimitive(input, methodName);
    if (hasError()) {
      return Value(Undefined{});
    }
    if (found) {
      if (method.isFunction()) {
        Value result = callFunction(method, {}, input);
        if (hasError()) {
          return Value(Undefined{});
        }
        if (!isObjectLike(result)) {
          return result;
        }
      }
      continue;
    }

    if (std::string(methodName) == "toString") {
      if (input.isArray()) {
        auto arr = input.getGC<Array>();
        std::string out;
        for (size_t i = 0; i < arr->elements.size(); i++) {
          if (i > 0) out += ",";
          out += arr->elements[i].toString();
        }
        return Value(out);
      }
      if (input.isObject()) {
        auto obj = input.getGC<Object>();
        auto protoIt = obj->properties.find("__proto__");
        if (protoIt != obj->properties.end() && protoIt->second.isObject()) {
          return Value(std::string("[object Object]"));
        }
      }
      if (input.isFunction()) return Value(std::string("[Function]"));
      if (input.isClass()) return Value(std::string("[Function]"));
      if (input.isRegex()) return Value(input.toString());
    }
  }

  throwError(ErrorType::TypeError, "Cannot convert object to primitive value");
  return Value(Undefined{});
}

bool Interpreter::checkMemoryLimit(size_t additionalBytes) {
  auto& gc = GarbageCollector::instance();

  if (!gc.checkHeapLimit(additionalBytes)) {
    // Try to free memory first
    gc.collect();

    // Check again after collection
    if (!gc.checkHeapLimit(additionalBytes)) {
      // Format memory statistics for error message
      size_t currentUsage = gc.getCurrentMemoryUsage();
      size_t heapLimit = gc.getHeapLimit();

      std::stringstream ss;
      ss << "JavaScript heap out of memory (";
      ss << (currentUsage / (1024 * 1024)) << " MB used, ";
      ss << (heapLimit / (1024 * 1024)) << " MB limit)";

      throwError(ErrorType::RangeError, ss.str());
      return false;
    }
  }
  return true;
}

Task Interpreter::evaluateGeneratorBody(const std::vector<StmtPtr>& body) {
  Value result = Value(Undefined{});
  for (const auto& stmt : body) {
    { auto _t = evaluate(*stmt); LIGHTJS_RUN_TASK(_t, result); }
    if (flow_.type == ControlFlow::Type::Return ||
        flow_.type == ControlFlow::Type::Throw) {
      break;
    }
  }
  LIGHTJS_RETURN(result);
}

Task Interpreter::evaluate(const Program& program) {
  bool previousStrictMode = strictMode_;
  // Inherit strict mode if already set (e.g., from direct eval in strict context)
  strictMode_ = strictMode_ || hasUseStrictDirective(program.body) || program.isModule;

  if (program.isModule) {
    for (const auto& stmt : program.body) {
      if (std::holds_alternative<ImportDeclaration>(stmt->node)) {
        auto task = evaluate(*stmt);
        LIGHTJS_RUN_TASK_VOID(task);
        if (flow_.type != ControlFlow::Type::None) {
          break;
        }
      }
    }
  }

  // GlobalDeclarationInstantiation: check HasRestrictedGlobalProperty for lexical names
  // At global scope, lexical declarations cannot shadow non-configurable global properties
  if (!env_->getParent()) {
    GCPtr<Object> globalObj;
    auto globalThisOpt = env_->get("globalThis");
    if (globalThisOpt && globalThisOpt->isObject()) {
      globalObj = std::get<GCPtr<Object>>(globalThisOpt->data);
    }
    if (globalObj) {
      for (const auto& stmt : program.body) {
        std::vector<std::string> lexNames;
        if (auto* varDecl = std::get_if<VarDeclaration>(&stmt->node)) {
          if (varDecl->kind == VarDeclaration::Kind::Let ||
              varDecl->kind == VarDeclaration::Kind::Const) {
            for (const auto& declarator : varDecl->declarations) {
              collectVarHoistNames(*declarator.pattern, lexNames);
            }
          }
        } else if (auto* classDecl = std::get_if<ClassDeclaration>(&stmt->node)) {
          lexNames.push_back(classDecl->id.name);
        }
        for (const auto& name : lexNames) {
          auto propIt = globalObj->properties.find(name);
          if (propIt != globalObj->properties.end()) {
            auto ncIt = globalObj->properties.find("__non_configurable_" + name);
            if (ncIt != globalObj->properties.end()) {
              throwError(ErrorType::SyntaxError,
                "Cannot declare a lexical binding for '" + name + "': already a restricted global property");
              LIGHTJS_RETURN(Value(Undefined{}));
            }
          }
        }
      }
    }
  }

  // Hoisting phase 0: Initialize TDZ for lexical declarations (non-recursive)
  for (const auto& stmt : program.body) {
    if (auto* varDecl = std::get_if<VarDeclaration>(&stmt->node)) {
      if (varDecl->kind == VarDeclaration::Kind::Let ||
          varDecl->kind == VarDeclaration::Kind::Const) {
        for (const auto& declarator : varDecl->declarations) {
          std::vector<std::string> names;
          collectVarHoistNames(*declarator.pattern, names);
          for (const auto& name : names) {
            env_->defineTDZ(name);
          }
        }
      }
    } else if (auto* classDecl = std::get_if<ClassDeclaration>(&stmt->node)) {
      env_->defineTDZ(classDecl->id.name);
    }
  }

  // Hoisting phase 1: Hoist var declarations (recursive scan)
  hoistVarDeclarations(program.body);

  // Hoisting phase 2: Hoist function declarations (top-level only)
  for (const auto& stmt : program.body) {
    if (std::holds_alternative<FunctionDeclaration>(stmt->node)) {
      auto task = evaluate(*stmt);
      LIGHTJS_RUN_TASK_VOID(task);
    }
  }

  // Script/module bodies use UpdateEmpty to preserve the last non-empty completion.
  Value result = Value(Empty{});
  for (const auto& stmt : program.body) {
    if (program.isModule && std::holds_alternative<ImportDeclaration>(stmt->node)) {
      continue;
    }
    // Skip function declarations - already hoisted
    if (std::holds_alternative<FunctionDeclaration>(stmt->node)) {
      continue;
    }
    auto task = evaluate(*stmt);
    LIGHTJS_RUN_TASK_VOID(task);
    if (!task.result().isEmpty()) {
      result = task.result();
    }

    if (flow_.type != ControlFlow::Type::None) {
      break;
    }
  }
  strictMode_ = previousStrictMode;
  if (result.isEmpty()) {
    result = Value(Undefined{});
  }
  LIGHTJS_RETURN(result);
}

Task Interpreter::evaluate(const Statement& stmt) {
  // Stack overflow protection
  StackGuard guard(stackDepth_, MAX_STACK_DEPTH);
  if (guard.overflowed()) {
    throwError(ErrorType::RangeError, "Maximum call stack size exceeded");
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  if (auto* node = std::get_if<VarDeclaration>(&stmt.node)) {
    { auto _t = evaluateVarDecl(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<FunctionDeclaration>(&stmt.node)) {
    { auto _t = evaluateFuncDecl(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<ClassDeclaration>(&stmt.node)) {
    // Create the class directly
    auto cls = GarbageCollector::makeGC<Class>(node->id.name);
    GarbageCollector::instance().reportAllocation(sizeof(Class));
    cls->privateBrandId = g_classPrivateBrandCounter.fetch_add(1, std::memory_order_relaxed);
    if (sourceKeepAlive_) {
      cls->astOwner = sourceKeepAlive_;
    }
    auto outerEnv = env_;
    struct ClassScopeEnvGuard {
      Interpreter* interpreter;
      GCPtr<Environment> previous;
      ~ClassScopeEnvGuard() {
        interpreter->env_ = previous;
      }
    } classScopeEnvGuard{this, outerEnv};

    // ClassDefinitionEvaluation creates a new declarative environment for the class name.
    // The inner binding is immutable, and class elements (including heritage) are evaluated
    // with this environment active.
    auto classScopeEnv = outerEnv->createChild();
    classScopeEnv->defineLexical(node->id.name, Value(cls), true);
    env_ = classScopeEnv;
    cls->closure = classScopeEnv;
    cls->lexicalParentClass = activePrivateOwnerClass_;

    // Handle superclass
    if (node->superClass) {
      struct StrictModeScopeGuard {
        Interpreter* interpreter;
        bool previous;
        ~StrictModeScopeGuard() {
          interpreter->strictMode_ = previous;
        }
      } strictModeScopeGuard{this, strictMode_};
      strictMode_ = true;
      auto superTask = evaluate(*node->superClass);
      Value superVal;
      LIGHTJS_RUN_TASK(superTask, superVal);
      auto isCallableObject = [](const Value& v) -> bool {
        if (!v.isObject()) return false;
        auto obj = v.getGC<Object>();
        auto callableIt = obj->properties.find("__callable_object__");
        return callableIt != obj->properties.end() &&
               callableIt->second.isBool() &&
               callableIt->second.toBool();
      };

      if (superVal.isNull()) {
        cls->properties["__extends_null__"] = Value(true);
        if (auto functionCtor = env_->get("Function");
            functionCtor && functionCtor->isFunction()) {
          auto fn = functionCtor->getGC<Function>();
          auto protoIt = fn->properties.find("prototype");
          if (protoIt != fn->properties.end()) {
            cls->properties["__super_constructor__"] = protoIt->second;
          }
        }
      } else if (superVal.isClass()) {
        cls->superClass = superVal.getGC<Class>();
      } else if (superVal.isFunction() || isCallableObject(superVal)) {
        bool isConstructable = true;
        if (superVal.isFunction()) {
          auto superFunc = superVal.getGC<Function>();
          // IsConstructor(superclass) must be checked before looking up `.prototype`.
          // Generator/async/arrow functions are not constructors.
          isConstructable = superFunc->isConstructor;
          if (superFunc->isGenerator || superFunc->isAsync) isConstructable = false;
          auto arrowIt = superFunc->properties.find("__is_arrow_function__");
          if (arrowIt != superFunc->properties.end() &&
              arrowIt->second.isBool() &&
              arrowIt->second.toBool()) {
            isConstructable = false;
          }
        } else if (superVal.isObject()) {
          // Callable wrapper objects (e.g. String, Array) behave as constructors
          // only if their inner `constructor` is a constructor.
          auto superObj = superVal.getGC<Object>();
          auto ctorIt = superObj->properties.find("constructor");
          if (ctorIt == superObj->properties.end()) {
            isConstructable = false;
          } else if (ctorIt->second.isFunction()) {
            auto inner = ctorIt->second.getGC<Function>();
            isConstructable = inner && inner->isConstructor;
          } else if (ctorIt->second.isClass()) {
            isConstructable = true;
          } else if (ctorIt->second.isProxy()) {
            isConstructable = true;
          } else {
            isConstructable = false;
          }
        }
        if (!isConstructable) {
          throwError(ErrorType::TypeError, "Class extends value is not a constructor or null");
          LIGHTJS_RETURN(Value(Undefined{}));
        }

        cls->properties["__super_constructor__"] = superVal;
        // Inherit static properties from function/callable super.
        if (superVal.isFunction()) {
          auto superFunc = superVal.getGC<Function>();
          for (const auto& [key, val] : superFunc->properties) {
            if (key.size() >= 2 && key[0] == '_' && key[1] == '_') continue;
            if (key == "name" || key == "length" || key == "prototype" ||
                key == "caller" || key == "arguments") continue;
            if (cls->properties.find(key) == cls->properties.end()) {
              cls->properties[key] = val;
            }
          }
        } else if (superVal.isObject()) {
          auto superObj = superVal.getGC<Object>();
          for (const auto& [key, val] : superObj->properties) {
            if (key.size() >= 2 && key[0] == '_' && key[1] == '_') continue;
            if (key == "name" || key == "length" || key == "prototype" ||
                key == "caller" || key == "arguments") continue;
            if (cls->properties.find(key) == cls->properties.end()) {
              cls->properties[key] = val;
            }
          }
        }
      } else {
        throwError(ErrorType::TypeError, "Class extends value is not a constructor or null");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
    }

    auto getPrototypeFromConstructor = [&](const Value& ctorValue) -> Value {
      return getPrototypeFromConstructorValue(ctorValue);
    };

    // Create Class.prototype object and wire prototype inheritance.
    auto classPrototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    auto extendsNullIt = cls->properties.find("__extends_null__");
    bool extendsNull = extendsNullIt != cls->properties.end() &&
                       extendsNullIt->second.isBool() &&
                       extendsNullIt->second.toBool();
    if (extendsNull) {
      classPrototype->properties["__proto__"] = Value(Null{});
    } else if (cls->superClass) {
      Value superProto = getPrototypeFromConstructor(Value(cls->superClass));
      if (hasError()) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (isObjectLike(superProto) || superProto.isNull()) {
        classPrototype->properties["__proto__"] = superProto;
      } else {
        throwError(ErrorType::TypeError, "Class extends value does not have a valid prototype");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
    } else if (auto superCtorIt = cls->properties.find("__super_constructor__");
               superCtorIt != cls->properties.end()) {
      Value superProto = getPrototypeFromConstructor(superCtorIt->second);
      if (hasError()) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (isObjectLike(superProto) || superProto.isNull()) {
        classPrototype->properties["__proto__"] = superProto;
      } else {
        throwError(ErrorType::TypeError, "Class extends value does not have a valid prototype");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
    } else if (auto objectCtor = env_->get("Object")) {
      Value objectProto = getPrototypeFromConstructor(*objectCtor);
      if (hasError()) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (isObjectLike(objectProto) || objectProto.isNull()) {
        classPrototype->properties["__proto__"] = objectProto;
      }
    }
    cls->properties["prototype"] = Value(classPrototype);
    cls->properties["__non_writable_prototype"] = Value(true);
    cls->properties["__non_enum_prototype"] = Value(true);
    cls->properties["__non_configurable_prototype"] = Value(true);

    // Ensure Class.prototype has an early "constructor" entry so own-property
    // key ordering matches ECMAScript (Test262 checks this).
    classPrototype->properties["constructor"] = Value(cls);
    classPrototype->properties["__non_enum_constructor"] = Value(true);

    auto resolveSuperForMethod = [&](const MethodDefinition& method) -> Value {
      if (method.kind == MethodDefinition::Kind::Constructor || method.isStatic) {
        if (cls->superClass) {
          return Value(cls->superClass);
        }
        auto superCtorIt = cls->properties.find("__super_constructor__");
        if (superCtorIt != cls->properties.end()) {
          return superCtorIt->second;
        }
        if (auto objectCtor = env_->get("Object")) {
          return *objectCtor;
        }
        return Value(Undefined{});
      }
      if (extendsNull) {
        return Value(Null{});
      }
      if (cls->superClass) {
        Value superProto = getPrototypeFromConstructor(Value(cls->superClass));
        if (hasError()) {
          return Value(Undefined{});
        }
        if (isObjectLike(superProto) || superProto.isNull()) {
          return superProto;
        }
      }
      auto superCtorIt = cls->properties.find("__super_constructor__");
      if (superCtorIt != cls->properties.end()) {
        Value superProto = getPrototypeFromConstructor(superCtorIt->second);
        if (hasError()) {
          return Value(Undefined{});
        }
        if (isObjectLike(superProto) || superProto.isNull()) {
          return superProto;
        }
      }
      if (auto objectCtor = env_->get("Object")) {
        Value objectProto = getPrototypeFromConstructor(*objectCtor);
        if (hasError()) {
          return Value(Undefined{});
        }
        if (isObjectLike(objectProto) || objectProto.isNull()) {
          return objectProto;
        }
      }
      return Value(Undefined{});
    };

    auto previousPrivateOwnerClass = activePrivateOwnerClass_;
    struct ActivePrivateOwnerClassScopeGuard {
      Interpreter* interpreter;
      GCPtr<Class> previous;
      ~ActivePrivateOwnerClassScopeGuard() {
        interpreter->activePrivateOwnerClass_ = previous;
      }
    } activePrivateOwnerClassScopeGuard{this, previousPrivateOwnerClass};
    activePrivateOwnerClass_ = cls;

    struct StaticInitStep {
      enum class Kind { Field, Block };
      Kind kind = Kind::Field;
      Class::FieldInit field;
      const std::vector<StmtPtr>* blockBody = nullptr;
    };
    std::vector<StaticInitStep> staticInitSteps;

    // Process methods and fields
    for (const auto& method : node->methods) {
      if (method.kind == MethodDefinition::Kind::StaticBlock) {
        StaticInitStep step;
        step.kind = StaticInitStep::Kind::Block;
        step.blockBody = &method.body;
        staticInitSteps.push_back(std::move(step));
        continue;
      }

      std::string methodName = method.key.name;
      Value propKeyForName(methodName);
      if (method.computed) {
        if (!method.computedKey) {
          throwError(ErrorType::SyntaxError, "Invalid computed class element name");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        auto keyTask = evaluate(*method.computedKey);
        Value keyValue;
        LIGHTJS_RUN_TASK(keyTask, keyValue);
        if (isObjectLike(keyValue)) {
          keyValue = toPrimitiveValue(keyValue, true);
          if (hasError()) {
            LIGHTJS_RETURN(Value(Undefined{}));
          }
        }
        propKeyForName = keyValue;
        methodName = toPropertyKeyString(keyValue);
      }

      // Handle field declarations
      if (method.kind == MethodDefinition::Kind::Field) {
        if (method.isStatic && !method.isPrivate && methodName == "prototype") {
          throwError(ErrorType::TypeError, "Cannot redefine property: prototype");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (method.isStatic) {
          Class::FieldInit fi;
          fi.name = methodName;
          fi.isPrivate = method.isPrivate;
          if (method.initializer) {
            fi.initExpr = std::shared_ptr<void>(
              const_cast<Expression*>(method.initializer.get()),
              [](void*){} // No-op deleter
            );
          }
          StaticInitStep step;
          step.kind = StaticInitStep::Kind::Field;
          step.field = std::move(fi);
          staticInitSteps.push_back(std::move(step));
        } else {
          Class::FieldInit fi;
          fi.name = methodName;
          fi.isPrivate = method.isPrivate;
          if (method.initializer) {
            fi.initExpr = std::shared_ptr<void>(
              const_cast<Expression*>(method.initializer.get()),
              [](void*){} // No-op deleter
            );
          }
          cls->fieldInitializers.push_back(std::move(fi));
        }
        continue;
      }

      if (method.isStatic && !method.isPrivate && methodName == "prototype") {
        throwError(ErrorType::TypeError, "Cannot redefine property: prototype");
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      auto func = GarbageCollector::makeGC<Function>();
      func->isNative = false;
      func->isAsync = method.isAsync;
      func->isGenerator = method.isGenerator;
      func->isStrict = true;  // Class bodies are always strict.
      if (sourceKeepAlive_) {
        func->astOwner = sourceKeepAlive_;
      }
      func->closure = env_;
      func->properties["__private_owner_class__"] = Value(cls);

      size_t methodLength = 0;
      bool sawDefault = false;
      for (const auto& param : method.params) {
        FunctionParam funcParam;
        funcParam.name = param.name.name;
        if (param.defaultValue) {
          funcParam.defaultValue = std::shared_ptr<void>(
              const_cast<Expression*>(param.defaultValue.get()),
              [](void*) {});
          sawDefault = true;
        } else if (!sawDefault) {
          methodLength++;
        }
        func->params.push_back(funcParam);
      }
      if (method.restParam.has_value()) {
        func->restParam = method.restParam->name;
      }

      // Store the body reference
      func->body = std::shared_ptr<void>(
        const_cast<std::vector<StmtPtr>*>(&method.body),
        [](void*){} // No-op deleter
      );
      func->destructurePrologue = std::shared_ptr<void>(
        const_cast<std::vector<StmtPtr>*>(&method.destructurePrologue),
        [](void*){} // No-op deleter
      );
      func->properties["length"] = Value(static_cast<double>(methodLength));
      if (method.kind == MethodDefinition::Kind::Constructor) {
        func->properties["name"] = Value(std::string("constructor"));
      } else {
        std::string baseName;
        if (propKeyForName.isSymbol()) {
          const auto& sym = std::get<Symbol>(propKeyForName.data);
          baseName = sym.description.empty() ? "" : "[" + sym.description + "]";
        } else {
          baseName = toPropertyKeyString(propKeyForName);
        }
        if (method.kind == MethodDefinition::Kind::Get) {
          func->properties["name"] = Value(std::string("get ") + baseName);
        } else if (method.kind == MethodDefinition::Kind::Set) {
          func->properties["name"] = Value(std::string("set ") + baseName);
        } else {
          func->properties["name"] = Value(baseName);
        }
      }
      func->properties["__non_writable_name"] = Value(true);
      func->properties["__non_enum_name"] = Value(true);
      func->properties["__non_writable_length"] = Value(true);
      func->properties["__non_enum_length"] = Value(true);

      Value superBase = resolveSuperForMethod(method);
      if (!superBase.isUndefined()) {
        func->properties["__super_class__"] = superBase;
      }

      if (method.kind == MethodDefinition::Kind::Constructor) {
        cls->constructor = func;
      } else if (method.isStatic) {
        if (method.isPrivate) {
          // Private static methods/getters/setters use mangled keys
          std::string mangledName = privateStorageKey(cls, methodName);
          if (method.kind == MethodDefinition::Kind::Get) {
            cls->properties["__get_" + mangledName] = Value(func);
          } else if (method.kind == MethodDefinition::Kind::Set) {
            cls->properties["__set_" + mangledName] = Value(func);
          } else {
            cls->staticMethods[mangledName] = func;
            cls->properties[mangledName] = Value(func);
          }
        } else {
          if (method.kind == MethodDefinition::Kind::Get) {
            cls->properties["__get_" + methodName] = Value(func);
            cls->properties["__non_enum_" + methodName] = Value(true);
          } else if (method.kind == MethodDefinition::Kind::Set) {
            cls->properties["__set_" + methodName] = Value(func);
            cls->properties["__non_enum_" + methodName] = Value(true);
          } else {
            cls->staticMethods[methodName] = func;
            cls->properties[methodName] = Value(func);
            cls->properties["__non_enum_" + methodName] = Value(true);
          }
        }
      } else if (method.isPrivate) {
        if (method.kind == MethodDefinition::Kind::Get) {
          cls->getters[methodName] = func;
        } else if (method.kind == MethodDefinition::Kind::Set) {
          cls->setters[methodName] = func;
        } else {
          cls->methods[methodName] = func;
        }
      } else if (method.kind == MethodDefinition::Kind::Get) {
        classPrototype->properties["__get_" + methodName] = Value(func);
        classPrototype->properties["__non_enum_" + methodName] = Value(true);
      } else if (method.kind == MethodDefinition::Kind::Set) {
        classPrototype->properties["__set_" + methodName] = Value(func);
        classPrototype->properties["__non_enum_" + methodName] = Value(true);
      } else {
        classPrototype->properties[methodName] = Value(func);
        classPrototype->properties["__non_enum_" + methodName] = Value(true);
      }
    }

    // Set name/length as own properties (per spec: SetFunctionName / FunctionLength).
    // Static class elements may define their own "name"/"length" property (method/accessor),
    // which should override the default. Don't clobber user-defined properties.
    if (!cls->name.empty() &&
        cls->properties.find("name") == cls->properties.end() &&
        cls->properties.find("__get_name") == cls->properties.end() &&
        cls->properties.find("__set_name") == cls->properties.end()) {
      cls->properties["name"] = Value(cls->name);
      cls->properties["__non_writable_name"] = Value(true);
      cls->properties["__non_enum_name"] = Value(true);
    }
    int ctorLen = cls->constructor ? static_cast<int>(cls->constructor->params.size()) : 0;
    if (cls->properties.find("length") == cls->properties.end() &&
        cls->properties.find("__get_length") == cls->properties.end() &&
        cls->properties.find("__set_length") == cls->properties.end()) {
      cls->properties["length"] = Value(static_cast<double>(ctorLen));
      cls->properties["__non_writable_length"] = Value(true);
      cls->properties["__non_enum_length"] = Value(true);
    }

    // Class constructor inheritance: [[Prototype]] is superclass when present.
    if (cls->superClass) {
      cls->properties["__proto__"] = Value(cls->superClass);
    } else if (auto superCtorIt = cls->properties.find("__super_constructor__");
               superCtorIt != cls->properties.end()) {
      cls->properties["__proto__"] = superCtorIt->second;
    } else if (auto funcVal = env_->get("Function"); funcVal && funcVal->isFunction()) {
      auto funcCtor = std::get<GCPtr<Function>>(funcVal->data);
      auto protoIt = funcCtor->properties.find("prototype");
      if (protoIt != funcCtor->properties.end()) {
        cls->properties["__proto__"] = protoIt->second;
      }
    }

    Value classVal = Value(cls);
    // ClassDeclaration creates a lexical binding (like let), not a var binding.
    // Lexical bindings at global scope must NOT appear on globalThis.
    outerEnv->defineLexical(node->id.name, classVal);

    auto runStaticBlock = [&](const std::vector<StmtPtr>& body) -> Task {
      auto prevEnvStatic = env_;
      bool prevStrict = strictMode_;
      env_ = cls->closure;
      env_ = env_->createChild();
      env_->define("__var_scope__", Value(true), true);
      env_->define("this", Value(cls));
      env_->define("__new_target__", Value(Undefined{}));
      auto superIt = cls->properties.find("__proto__");
      if (superIt != cls->properties.end()) {
        env_->define("__super__", superIt->second);
      }

      hoistVarDeclarations(body);
      env_ = env_->createChild();

      for (const auto& s : body) {
        if (auto* varDecl = std::get_if<VarDeclaration>(&s->node)) {
          if (varDecl->kind == VarDeclaration::Kind::Let ||
              varDecl->kind == VarDeclaration::Kind::Const) {
            for (const auto& declarator : varDecl->declarations) {
              std::vector<std::string> names;
              collectVarHoistNames(*declarator.pattern, names);
              for (const auto& name : names) {
                env_->defineTDZ(name);
              }
            }
          }
        } else if (auto* classDecl = std::get_if<ClassDeclaration>(&s->node)) {
          env_->defineTDZ(classDecl->id.name);
        }
      }

      strictMode_ = true;
      for (const auto& s : body) {
        auto t = evaluate(*s);
        LIGHTJS_RUN_TASK_VOID(t);
        if (flow_.type != ControlFlow::Type::None) {
          break;
        }
      }

      strictMode_ = prevStrict;
      env_ = prevEnvStatic;
      LIGHTJS_RETURN(Value(Undefined{}));
    };

    for (const auto& step : staticInitSteps) {
      if (step.kind == StaticInitStep::Kind::Field) {
        std::vector<Class::FieldInit> one;
        one.push_back(step.field);
        auto staticInitTask = initializeClassStaticFields(cls, one);
        LIGHTJS_RUN_TASK_VOID(staticInitTask);
        if (flow_.type != ControlFlow::Type::None) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        continue;
      }
      if (step.blockBody) {
        auto t = runStaticBlock(*step.blockBody);
        LIGHTJS_RUN_TASK_VOID(t);
        if (flow_.type != ControlFlow::Type::None) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      }
    }

    // ClassDeclaration completion value is empty (eval('class C {}') === undefined).
    LIGHTJS_RETURN(Value(Empty{}));
  } else if (auto* node = std::get_if<EmptyStmt>(&stmt.node)) {
    (void)node;
    LIGHTJS_RETURN(Value(Empty{}));
  } else if (auto* node = std::get_if<ReturnStmt>(&stmt.node)) {
    { auto _t = evaluateReturn(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<ExpressionStmt>(&stmt.node)) {
    { auto _t = evaluateExprStmt(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<BlockStmt>(&stmt.node)) {
    { auto _t = evaluateBlock(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<IfStmt>(&stmt.node)) {
    { auto _t = evaluateIf(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<WhileStmt>(&stmt.node)) {
    { auto _t = evaluateWhile(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<WithStmt>(&stmt.node)) {
    { auto _t = evaluateWith(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<DoWhileStmt>(&stmt.node)) {
    { auto _t = evaluateDoWhile(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<ForStmt>(&stmt.node)) {
    { auto _t = evaluateFor(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<ForInStmt>(&stmt.node)) {
    { auto _t = evaluateForIn(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<ForOfStmt>(&stmt.node)) {
    { auto _t = evaluateForOf(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<SwitchStmt>(&stmt.node)) {
    { auto _t = evaluateSwitch(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<BreakStmt>(&stmt.node)) {
    flow_.type = ControlFlow::Type::Break;
    flow_.label = node->label;
    LIGHTJS_RETURN(Value(Empty{}));
  } else if (auto* node = std::get_if<ContinueStmt>(&stmt.node)) {
    flow_.type = ControlFlow::Type::Continue;
    flow_.label = node->label;
    LIGHTJS_RETURN(Value(Empty{}));
  } else if (auto* node = std::get_if<DebuggerStmt>(&stmt.node)) {
    (void)node;
    LIGHTJS_RETURN(Value(Empty{}));
  } else if (auto* labelNode = std::get_if<LabelledStmt>(&stmt.node)) {
    // Set pending label so the next iteration statement can consume matching continues
    auto prevLabel = pendingIterationLabel_;
    pendingIterationLabel_ = labelNode->label;
    auto task = evaluate(*labelNode->body);
    Value labelResult;
    LIGHTJS_RUN_TASK(task, labelResult);
    pendingIterationLabel_ = prevLabel;
    // If break targets this label, consume it
    // Spec: set stmtResult to NormalCompletion(UpdateEmpty(stmtResult.[[Value]], undefined))
    if (flow_.type == ControlFlow::Type::Break && flow_.label == labelNode->label) {
      flow_.type = ControlFlow::Type::None;
      flow_.label.clear();
      if (labelResult.isEmpty()) {
        labelResult = Value(Undefined{});
      }
    }
    LIGHTJS_RETURN(labelResult);
  } else if (auto* node = std::get_if<ThrowStmt>(&stmt.node)) {
    auto task = evaluate(*node->argument);
    LIGHTJS_RUN_TASK_VOID(task);
    if (flow_.type == ControlFlow::Type::Throw) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    flow_.type = ControlFlow::Type::Throw;
    flow_.value = task.result();
    LIGHTJS_RETURN(Value(Undefined{}));
  } else if (auto* node = std::get_if<TryStmt>(&stmt.node)) {
    { auto _t = evaluateTry(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<ImportDeclaration>(&stmt.node)) {
    { auto _t = evaluateImport(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<ExportNamedDeclaration>(&stmt.node)) {
    { auto _t = evaluateExportNamed(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<ExportDefaultDeclaration>(&stmt.node)) {
    { auto _t = evaluateExportDefault(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<ExportAllDeclaration>(&stmt.node)) {
    { auto _t = evaluateExportAll(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  }
  LIGHTJS_RETURN(Value(Empty{}));
}

Task Interpreter::evaluate(const Expression& expr) {
  // Stack overflow protection
  StackGuard guard(stackDepth_, MAX_STACK_DEPTH);
  if (guard.overflowed()) {
    throwError(ErrorType::RangeError, "Maximum call stack size exceeded");
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  if (auto* node = std::get_if<Identifier>(&expr.node)) {
    // Check for temporal dead zone
    if (env_->isTDZ(node->name)) {
      throwError(ErrorType::ReferenceError,
                 formatError("Cannot access '" + node->name + "' before initialization", expr.loc));
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (auto withScopeValue = env_->resolveWithScopeValue(node->name)) {
      auto withValue = env_->getWithScopeBindingValue(*withScopeValue, node->name, strictMode_);
      if (hasError()) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (withValue.has_value()) {
        LIGHTJS_RETURN(*withValue);
      }
    }
    if (auto val = env_->getIgnoringWith(node->name)) {
      if (val->isModuleBinding()) {
        const auto& binding = std::get<ModuleBinding>(val->data);
        auto module = binding.module.lock();
        if (!module) {
          throwError(ErrorType::ReferenceError,
                     formatError("Cannot access '" + node->name + "' before initialization", expr.loc));
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        auto exportValue = module->getExport(binding.exportName);
        if (!exportValue) {
          throwError(ErrorType::ReferenceError,
                     formatError("Cannot access '" + node->name + "' before initialization", expr.loc));
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        LIGHTJS_RETURN(*exportValue);
      }
      LIGHTJS_RETURN(*val);
    }
    for (auto it = activeNamedExpressionStack_.rbegin();
         it != activeNamedExpressionStack_.rend();
         ++it) {
      const auto& fn = *it;
      auto nameIt = fn->properties.find("name");
      if (nameIt != fn->properties.end() &&
          nameIt->second.isString() &&
          nameIt->second.toString() == node->name) {
        LIGHTJS_RETURN(Value(fn));
      }
    }

    // As a last resort, resolve global object accessors as identifier bindings.
    // This is needed for cases like:
    //   Object.defineProperty(globalThis, "y", { get(){...} });
    //   y; // should invoke getter
    if (auto globalObj = env_->getGlobal()) {
      auto getterIt = globalObj->properties.find("__get_" + node->name);
      if (getterIt != globalObj->properties.end() && getterIt->second.isFunction()) {
        Value v = callFunction(getterIt->second, {}, Value(globalObj));
        if (flow_.type == ControlFlow::Type::Throw || hasError()) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        LIGHTJS_RETURN(v);
      }
      auto propIt = globalObj->properties.find(node->name);
      if (propIt != globalObj->properties.end()) {
        LIGHTJS_RETURN(propIt->second);
      }
    }

    // Throw ReferenceError for undefined variables with line info
    throwError(ErrorType::ReferenceError, formatError("'" + node->name + "' is not defined", expr.loc));
    LIGHTJS_RETURN(Value(Undefined{}));
  } else if (auto* node = std::get_if<NumberLiteral>(&expr.node)) {
    // Use cached value for small integers
    if (SmallIntCache::inRange(node->value)) {
      LIGHTJS_RETURN(SmallIntCache::get(static_cast<int>(node->value)));
    }
    LIGHTJS_RETURN(Value(node->value));
  } else if (auto* node = std::get_if<BigIntLiteral>(&expr.node)) {
    LIGHTJS_RETURN(Value(BigInt(node->value)));
  } else if (auto* node = std::get_if<StringLiteral>(&expr.node)) {
    LIGHTJS_RETURN(Value(node->value));
  } else if (auto* node = std::get_if<TemplateLiteral>(&expr.node)) {
    // Evaluate template literal with interpolation (untagged).
    std::string result;
    for (size_t i = 0; i < node->quasis.size(); i++) {
      if (!node->quasis[i].cooked.has_value()) {
        throwError(ErrorType::SyntaxError, "Invalid escape sequence in template literal");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      result += *node->quasis[i].cooked;
      if (i < node->expressions.size()) {
        auto exprTask = evaluate(*node->expressions[i]);
        Value interpolated = Value(Undefined{});
        LIGHTJS_RUN_TASK(exprTask, interpolated);
        if (flow_.type == ControlFlow::Type::Yield || hasError()) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (isObjectLike(interpolated)) {
          interpolated = toPrimitiveValue(interpolated, true);
          if (hasError()) {
            LIGHTJS_RETURN(Value(Undefined{}));
          }
        }
        result += interpolated.toString();
      }
    }
    LIGHTJS_RETURN(Value(result));
  } else if (auto* node = std::get_if<TemplateObjectExpr>(&expr.node)) {
    // GetTemplateObject for tagged templates.
    const void* site = static_cast<const void*>(node);
    auto it = templateObjectCache_.find(site);
    if (it != templateObjectCache_.end()) {
      LIGHTJS_RETURN(it->second);
    }

    auto rawArr = GarbageCollector::makeGC<Array>();
    GarbageCollector::instance().reportAllocation(sizeof(Array));
    auto cookedArr = GarbageCollector::makeGC<Array>();
    GarbageCollector::instance().reportAllocation(sizeof(Array));

    rawArr->elements.reserve(node->quasis.size());
    cookedArr->elements.reserve(node->quasis.size());
    for (const auto& q : node->quasis) {
      rawArr->elements.push_back(Value(q.raw));
      if (q.cooked.has_value()) cookedArr->elements.push_back(Value(*q.cooked));
      else cookedArr->elements.push_back(Value(Undefined{}));
    }

    // Define `.raw` on the cooked array.
    cookedArr->properties["raw"] = Value(rawArr);
    cookedArr->properties["__non_enum_raw"] = Value(true);
    cookedArr->properties["__non_writable_raw"] = Value(true);
    cookedArr->properties["__non_configurable_raw"] = Value(true);

    auto freezeArray = [&](const GCPtr<Array>& arr, bool freezeRawProp) {
      if (!arr) return;
      arr->properties["__non_extensible__"] = Value(true);
      arr->properties["__non_writable_length"] = Value(true);
      arr->properties["__non_configurable_length"] = Value(true);
      // Freeze indexed elements.
      for (size_t idx = 0; idx < arr->elements.size(); idx++) {
        std::string k = std::to_string(idx);
        arr->properties["__non_writable_" + k] = Value(true);
        arr->properties["__non_configurable_" + k] = Value(true);
      }
      if (freezeRawProp) {
        arr->properties["__non_writable_raw"] = Value(true);
        arr->properties["__non_configurable_raw"] = Value(true);
      }
    };

    freezeArray(rawArr, false);
    freezeArray(cookedArr, true);

    Value out = Value(cookedArr);
    templateObjectCache_.emplace(site, out);
    LIGHTJS_RETURN(out);
  } else if (auto* node = std::get_if<RegexLiteral>(&expr.node)) {
    auto regex = GarbageCollector::makeGC<Regex>(node->pattern, node->flags);
    // RegExp lastIndex: writable, not enumerable, not configurable (ES spec 21.2.3.2.1)
    regex->properties["lastIndex"] = Value(0.0);
    regex->properties["__non_enum_lastIndex"] = Value(true);
    regex->properties["__non_configurable_lastIndex"] = Value(true);
    // Set [[Prototype]] to RegExp.prototype
    if (auto regexpCtor = env_->get("RegExp"); regexpCtor.has_value() && regexpCtor->isFunction()) {
      auto protoIt = regexpCtor->getGC<Function>()->properties.find("prototype");
      if (protoIt != regexpCtor->getGC<Function>()->properties.end()) {
        regex->properties["__proto__"] = protoIt->second;
      }
    }
    LIGHTJS_RETURN(Value(regex));
  } else if (auto* node = std::get_if<BoolLiteral>(&expr.node)) {
    LIGHTJS_RETURN(Value(node->value));
  } else if (std::holds_alternative<NullLiteral>(expr.node)) {
    LIGHTJS_RETURN(Value(Null{}));
  } else if (auto* node = std::get_if<BinaryExpr>(&expr.node)) {
    { auto _t = evaluateBinary(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<UnaryExpr>(&expr.node)) {
    { auto _t = evaluateUnary(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<AssignmentExpr>(&expr.node)) {
    { auto _t = evaluateAssignment(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<UpdateExpr>(&expr.node)) {
    { auto _t = evaluateUpdate(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<CallExpr>(&expr.node)) {
    { auto _t = evaluateCall(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<MemberExpr>(&expr.node)) {
    { auto _t = evaluateMember(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<ConditionalExpr>(&expr.node)) {
    { auto _t = evaluateConditional(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<SequenceExpr>(&expr.node)) {
    Value last = Value(Undefined{});
    for (const auto& sequenceExpr : node->expressions) {
      if (!sequenceExpr) {
        continue;
      }
      { auto _t = evaluate(*sequenceExpr); LIGHTJS_RUN_TASK(_t, last); }
      if (flow_.type != ControlFlow::Type::None) {
        break;
      }
    }
    LIGHTJS_RETURN(last);
  } else if (auto* node = std::get_if<ArrayExpr>(&expr.node)) {
    { auto _t = evaluateArray(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<ObjectExpr>(&expr.node)) {
    { auto _t = evaluateObject(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<FunctionExpr>(&expr.node)) {
    { auto _t = evaluateFunction(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<AwaitExpr>(&expr.node)) {
    { auto _t = evaluateAwait(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<YieldExpr>(&expr.node)) {
    { auto _t = evaluateYield(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<NewExpr>(&expr.node)) {
    { auto _t = evaluateNew(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (auto* node = std::get_if<ClassExpr>(&expr.node)) {
    { auto _t = evaluateClass(*node); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  } else if (std::holds_alternative<ThisExpr>(expr.node)) {
    // In derived constructors, `this` is uninitialized before super().
    if (auto superCalled = env_->get("__super_called__");
        superCalled && superCalled->isBool() && !superCalled->toBool()) {
      throwError(ErrorType::ReferenceError,
                 "Must call super constructor in derived class before accessing 'this'");
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    // Look up 'this' in the current environment
    if (auto thisVal = env_->get("this")) {
      LIGHTJS_RETURN(*thisVal);
    }
    LIGHTJS_RETURN(Value(Undefined{}));
  } else if (std::holds_alternative<SuperExpr>(expr.node)) {
    // Compute super base from the active function's home object when available
    // (object/class methods), falling back to the environment binding.
    if (activeFunction_) {
      auto homeIt = activeFunction_->properties.find("__home_object__");
      if (homeIt != activeFunction_->properties.end()) {
        if (homeIt->second.isObject()) {
          auto homeObj = homeIt->second.getGC<Object>();
          auto protoIt = homeObj->properties.find("__proto__");
          if (protoIt != homeObj->properties.end()) {
            LIGHTJS_RETURN(protoIt->second);
          }
        } else if (homeIt->second.isClass()) {
          auto homeCls = homeIt->second.getGC<Class>();
          auto protoIt = homeCls->properties.find("__proto__");
          if (protoIt != homeCls->properties.end()) {
            LIGHTJS_RETURN(protoIt->second);
          }
        }
      }
    }

    // Look up '__super__' in the current environment (set during call binding).
    if (auto superVal = env_->get("__super__")) {
      LIGHTJS_RETURN(*superVal);
    }

    // Fallback for class field initializer arrow functions: derive super base
    // from lexical `this` when no explicit __super__ binding is available.
    if (auto thisVal = env_->get("this")) {
      if (thisVal->isClass()) {
        auto cls = thisVal->getGC<Class>();
        auto protoIt = cls->properties.find("__proto__");
        if (protoIt != cls->properties.end()) {
          LIGHTJS_RETURN(protoIt->second);
        }
      } else if (thisVal->isObject()) {
        auto obj = thisVal->getGC<Object>();
        auto thisProtoIt = obj->properties.find("__proto__");
        if (thisProtoIt != obj->properties.end() && thisProtoIt->second.isObject()) {
          auto homeProto = thisProtoIt->second.getGC<Object>();
          auto superProtoIt = homeProto->properties.find("__proto__");
          if (superProtoIt != homeProto->properties.end()) {
            LIGHTJS_RETURN(superProtoIt->second);
          }
        }
      }
    }

    throwError(ErrorType::ReferenceError, formatError("'super' keyword is not valid here", expr.loc));
    LIGHTJS_RETURN(Value(Undefined{}));
  } else if (auto* node = std::get_if<MetaProperty>(&expr.node)) {
    if (node->meta == "new" && node->property == "target") {
      if (auto newTarget = env_->get("__new_target__")) {
        LIGHTJS_RETURN(*newTarget);
      }
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    // import.meta - ES2020
    if (node->meta == "meta") {
      if (auto cached = env_->get("__import_meta_object__")) {
        LIGHTJS_RETURN(*cached);
      }

      // Create import.meta object with common properties.
      auto metaObj = GarbageCollector::makeGC<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));

      // import.meta.url - the URL of the current module
      if (auto moduleUrl = env_->get("__module_url__")) {
        metaObj->properties["url"] = *moduleUrl;
      } else {
        metaObj->properties["url"] = Value(std::string(""));
      }

      // import.meta.resolve - function to resolve module specifiers
      auto resolveFn = GarbageCollector::makeGC<Function>();
      resolveFn->isNative = true;
      resolveFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(std::string(""));
        // Simple implementation - just return the specifier as-is
        return Value(args[0].toString());
      };
      metaObj->properties["resolve"] = Value(resolveFn);
      metaObj->properties["__import_meta__"] = Value(true);

      Value metaValue(metaObj);
      env_->define("__import_meta_object__", metaValue);
      LIGHTJS_RETURN(metaValue);
    }
    LIGHTJS_RETURN(Value(Undefined{}));
  }
  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::evaluateBinary(const BinaryExpr& expr) {
  // Private `#name in rhs` does not evaluate the LHS as an identifier reference.
  if (expr.op == BinaryExpr::Op::In) {
    if (auto* leftIdent = std::get_if<Identifier>(&expr.left->node);
        leftIdent && !leftIdent->name.empty() && leftIdent->name[0] == '#') {
      auto rightTask = evaluate(*expr.right);
      Value rightValue;
      LIGHTJS_RUN_TASK(rightTask, rightValue);
      if (flow_.type == ControlFlow::Type::Throw) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      if (!isObjectLike(rightValue) && !rightValue.isClass()) {
        throwError(ErrorType::TypeError, "Right-hand side of 'in' should be an object");
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      if (!activePrivateOwnerClass_) {
        LIGHTJS_RETURN(Value(false));
      }

      PrivateNameOwner resolved =
          resolvePrivateNameOwnerClass(activePrivateOwnerClass_, leftIdent->name);
      if (!resolved.owner || resolved.kind == PrivateNameKind::None) {
        LIGHTJS_RETURN(Value(false));
      }
      if (resolved.kind == PrivateNameKind::Instance) {
        GCPtr<Class> targetClass = rightValue.isClass()
                                       ? rightValue.getGC<Class>()
                                       : getConstructorClassForPrivateAccess(rightValue);
        LIGHTJS_RETURN(Value(targetClass && isOwnerInClassChain(targetClass, resolved.owner)));
      } else {
        LIGHTJS_RETURN(Value(rightValue.isClass() &&
                             rightValue.getGC<Class>().get() == resolved.owner.get()));
      }
    }
  }

  auto leftTask = evaluate(*expr.left);
  Value left;
  LIGHTJS_RUN_TASK(leftTask, left);

  // Check for throw flow from left operand evaluation
  if (flow_.type == ControlFlow::Type::Throw) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  // Short-circuit evaluation for logical operators
  if (expr.op == BinaryExpr::Op::LogicalAnd) {
    if (!left.toBool()) {
      LIGHTJS_RETURN(left);
    }
    auto rTask = evaluate(*expr.right);
    Value rVal;
    LIGHTJS_RUN_TASK(rTask, rVal);
    LIGHTJS_RETURN(rVal);
  }
  if (expr.op == BinaryExpr::Op::LogicalOr) {
    if (left.toBool()) {
      LIGHTJS_RETURN(left);
    }
    auto rTask = evaluate(*expr.right);
    Value rVal;
    LIGHTJS_RUN_TASK(rTask, rVal);
    LIGHTJS_RETURN(rVal);
  }
  if (expr.op == BinaryExpr::Op::NullishCoalescing) {
    if (!left.isNull() && !left.isUndefined()) {
      LIGHTJS_RETURN(left);
    }
    auto rTask = evaluate(*expr.right);
    Value rVal;
    LIGHTJS_RUN_TASK(rTask, rVal);
    LIGHTJS_RETURN(rVal);
  }

  auto rightTask = evaluate(*expr.right);
  Value right;
  LIGHTJS_RUN_TASK(rightTask, right);
  if (flow_.type != ControlFlow::Type::None || hasError()) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  // Check for throw flow from operand evaluation
  if (flow_.type == ControlFlow::Type::Throw) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  // Fast path: both operands are numbers (most common case in loops)
  const bool leftIsNum = left.isNumber();
  const bool rightIsNum = right.isNumber();

  if (leftIsNum && rightIsNum) {
    double l = std::get<double>(left.data);
    double r = std::get<double>(right.data);
    switch (expr.op) {
      case BinaryExpr::Op::Add: LIGHTJS_RETURN(Value(l + r));
      case BinaryExpr::Op::Sub: LIGHTJS_RETURN(Value(l - r));
      case BinaryExpr::Op::Mul: LIGHTJS_RETURN(Value(l * r));
      case BinaryExpr::Op::Div: LIGHTJS_RETURN(Value(l / r));
      case BinaryExpr::Op::Mod: LIGHTJS_RETURN(Value(std::fmod(l, r)));
      case BinaryExpr::Op::BitwiseAnd: LIGHTJS_RETURN(Value(static_cast<double>(toInt32(l) & toInt32(r))));
      case BinaryExpr::Op::BitwiseOr: LIGHTJS_RETURN(Value(static_cast<double>(toInt32(l) | toInt32(r))));
      case BinaryExpr::Op::BitwiseXor: LIGHTJS_RETURN(Value(static_cast<double>(toInt32(l) ^ toInt32(r))));
      case BinaryExpr::Op::LeftShift: LIGHTJS_RETURN(Value(static_cast<double>(toInt32(l) << (toInt32(r) & 0x1f))));
      case BinaryExpr::Op::RightShift: LIGHTJS_RETURN(Value(static_cast<double>(toInt32(l) >> (toInt32(r) & 0x1f))));
      case BinaryExpr::Op::UnsignedRightShift: LIGHTJS_RETURN(Value(static_cast<double>(static_cast<uint32_t>(toInt32(l)) >> (toInt32(r) & 0x1f))));
      case BinaryExpr::Op::Less: LIGHTJS_RETURN(Value(l < r));
      case BinaryExpr::Op::Greater: LIGHTJS_RETURN(Value(l > r));
      case BinaryExpr::Op::LessEqual: LIGHTJS_RETURN(Value(l <= r));
      case BinaryExpr::Op::GreaterEqual: LIGHTJS_RETURN(Value(l >= r));
      case BinaryExpr::Op::Equal:
      case BinaryExpr::Op::StrictEqual: LIGHTJS_RETURN(Value(l == r));
      case BinaryExpr::Op::NotEqual:
      case BinaryExpr::Op::StrictNotEqual: LIGHTJS_RETURN(Value(l != r));
      default: break;  // Fall through for other ops
    }
  }

  auto toNumericOperand = [&](const Value& operand, Value& out) -> bool {
    out = isObjectLike(operand) ? toPrimitiveValue(operand, false) : operand;
    if (hasError()) return false;
    if (out.isSymbol()) {
      throwError(ErrorType::TypeError, "Cannot convert Symbol to number");
      return false;
    }
    return true;
  };

  // Abstract Equality Comparison helpers (ES spec 7.2.14)
  auto sameReference = [&](const Value& a, const Value& b) -> bool {
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
    if (a.isWeakMap()) return std::get<GCPtr<WeakMap>>(a.data).get() == std::get<GCPtr<WeakMap>>(b.data).get();
    if (a.isWeakSet()) return std::get<GCPtr<WeakSet>>(a.data).get() == std::get<GCPtr<WeakSet>>(b.data).get();
    if (a.isTypedArray()) return a.getGC<TypedArray>().get() == b.getGC<TypedArray>().get();
    if (a.isArrayBuffer()) return a.getGC<ArrayBuffer>().get() == b.getGC<ArrayBuffer>().get();
    if (a.isDataView()) return a.getGC<DataView>().get() == b.getGC<DataView>().get();
    if (a.isError()) return a.getGC<Error>().get() == b.getGC<Error>().get();
    return false;
  };

  auto abstractEqual = [&](Value x, Value y) -> bool {
    for (int iter = 0; iter < 32; iter++) {
      // Same type comparisons
      if ((x.isUndefined() && y.isUndefined()) || (x.isNull() && y.isNull())) return true;
      if (x.isNumber() && y.isNumber()) {
        double xn = x.toNumber();
        double yn = y.toNumber();
        if (std::isnan(xn) || std::isnan(yn)) return false;
        return xn == yn;
      }
      if (x.isString() && y.isString()) return x.toString() == y.toString();
      if (x.isBool() && y.isBool()) return x.toBool() == y.toBool();
      if (x.isBigInt() && y.isBigInt()) return x.toBigInt() == y.toBigInt();
      if (x.isSymbol() && y.isSymbol()) {
        const auto& xs = std::get<Symbol>(x.data);
        const auto& ys = std::get<Symbol>(y.data);
        return xs.id == ys.id;
      }
      if (isObjectLike(x) && isObjectLike(y)) {
        return sameReference(x, y);
      }

      // Cross-type nullish
      if ((x.isNull() && y.isUndefined()) || (x.isUndefined() && y.isNull())) return true;

      // BigInt / String
      if (x.isBigInt() && y.isString()) {
        bigint::BigIntValue parsed = 0;
        if (!parseBigIntString64(y.toString(), parsed)) return false;
        return x.toBigInt() == parsed;
      }
      if (x.isString() && y.isBigInt()) {
        bigint::BigIntValue parsed = 0;
        if (!parseBigIntString64(x.toString(), parsed)) return false;
        return parsed == y.toBigInt();
      }

      // BigInt / Number
      if (x.isBigInt() && y.isNumber()) {
        double n = y.toNumber();
        if (!std::isfinite(n) || std::trunc(n) != n) return false;
        return compareBigIntAndNumber(x.toBigInt(), n) == BigIntNumberOrder::Equal;
      }
      if (x.isNumber() && y.isBigInt()) {
        double n = x.toNumber();
        if (!std::isfinite(n) || std::trunc(n) != n) return false;
        return compareBigIntAndNumber(y.toBigInt(), n) == BigIntNumberOrder::Equal;
      }

      // Number / String
      if (x.isNumber() && y.isString()) {
        y = Value(y.toNumber());
        continue;
      }
      if (x.isString() && y.isNumber()) {
        x = Value(x.toNumber());
        continue;
      }

      // Boolean -> Number
      if (x.isBool()) {
        x = Value(x.toNumber());
        continue;
      }
      if (y.isBool()) {
        y = Value(y.toNumber());
        continue;
      }

      // Object -> Primitive (default hint)
      if (isObjectLike(x) && !isObjectLike(y) && !y.isNull() && !y.isUndefined()) {
        x = toPrimitiveValue(x, false, true);
        if (hasError()) return false;
        continue;
      }
      if (isObjectLike(y) && !isObjectLike(x) && !x.isNull() && !x.isUndefined()) {
        y = toPrimitiveValue(y, false, true);
        if (hasError()) return false;
        continue;
      }

      // Symbol compared to non-symbol is always false.
      return false;
    }
    // Loop limit exceeded: treat as not equal.
    return false;
  };

  switch (expr.op) {
    case BinaryExpr::Op::Add: {
      Value lhs = isObjectLike(left) ? toPrimitiveValue(left, false, true) : left;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs = isObjectLike(right) ? toPrimitiveValue(right, false, true) : right;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));

      if ((lhs.isString() || rhs.isString()) && (lhs.isSymbol() || rhs.isSymbol())) {
        throwError(ErrorType::TypeError, "Cannot convert Symbol to string");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (lhs.isString() || rhs.isString()) {
        LIGHTJS_RETURN(Value(lhs.toString() + rhs.toString()));
      }
      if (lhs.isBigInt() && rhs.isBigInt()) {
        LIGHTJS_RETURN(Value(BigInt(lhs.toBigInt() + rhs.toBigInt())));
      }
      if (lhs.isBigInt() != rhs.isBigInt()) {
        throwError(ErrorType::TypeError, "Cannot mix BigInt and other types");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (lhs.isSymbol() || rhs.isSymbol()) {
        throwError(ErrorType::TypeError, "Cannot convert Symbol to number");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(lhs.toNumber() + rhs.toNumber()));
    }
    case BinaryExpr::Op::Sub: {
      Value lhs;
      if (!toNumericOperand(left, lhs)) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs;
      if (!toNumericOperand(right, rhs)) LIGHTJS_RETURN(Value(Undefined{}));

      if (lhs.isBigInt() && rhs.isBigInt()) {
        LIGHTJS_RETURN(Value(BigInt(lhs.toBigInt() - rhs.toBigInt())));
      }
      if (lhs.isBigInt() != rhs.isBigInt()) {
        throwError(ErrorType::TypeError, "Cannot mix BigInt and other types");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(lhs.toNumber() - rhs.toNumber()));
    }
    case BinaryExpr::Op::Mul: {
      Value lhs;
      if (!toNumericOperand(left, lhs)) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs;
      if (!toNumericOperand(right, rhs)) LIGHTJS_RETURN(Value(Undefined{}));

      if (lhs.isBigInt() && rhs.isBigInt()) {
        LIGHTJS_RETURN(Value(BigInt(lhs.toBigInt() * rhs.toBigInt())));
      }
      if (lhs.isBigInt() != rhs.isBigInt()) {
        throwError(ErrorType::TypeError, "Cannot mix BigInt and other types");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(lhs.toNumber() * rhs.toNumber()));
    }
    case BinaryExpr::Op::Div: {
      Value lhs;
      if (!toNumericOperand(left, lhs)) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs;
      if (!toNumericOperand(right, rhs)) LIGHTJS_RETURN(Value(Undefined{}));

      if (lhs.isBigInt() && rhs.isBigInt()) {
        auto divisor = rhs.toBigInt();
        if (divisor == 0) {
          throwError(ErrorType::RangeError, "Division by zero");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        LIGHTJS_RETURN(Value(BigInt(lhs.toBigInt() / divisor)));
      }
      if (lhs.isBigInt() != rhs.isBigInt()) {
        throwError(ErrorType::TypeError, "Cannot mix BigInt and other types");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(lhs.toNumber() / rhs.toNumber()));
    }
    case BinaryExpr::Op::Mod: {
      Value lhs;
      if (!toNumericOperand(left, lhs)) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs;
      if (!toNumericOperand(right, rhs)) LIGHTJS_RETURN(Value(Undefined{}));

      if (lhs.isBigInt() && rhs.isBigInt()) {
        auto divisor = rhs.toBigInt();
        if (divisor == 0) {
          throwError(ErrorType::RangeError, "Division by zero");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        LIGHTJS_RETURN(Value(BigInt(lhs.toBigInt() % divisor)));
      }
      if (lhs.isBigInt() != rhs.isBigInt()) {
        throwError(ErrorType::TypeError, "Cannot mix BigInt and other types");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(std::fmod(lhs.toNumber(), rhs.toNumber())));
    }
    case BinaryExpr::Op::BitwiseAnd: {
      Value lhs;
      if (!toNumericOperand(left, lhs)) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs;
      if (!toNumericOperand(right, rhs)) LIGHTJS_RETURN(Value(Undefined{}));

      if (lhs.isBigInt() && rhs.isBigInt()) {
        LIGHTJS_RETURN(Value(BigInt(lhs.toBigInt() & rhs.toBigInt())));
      }
      if (lhs.isBigInt() != rhs.isBigInt()) {
        throwError(ErrorType::TypeError, "Cannot mix BigInt and other types in bitwise operations");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(static_cast<double>(toInt32(lhs.toNumber()) & toInt32(rhs.toNumber()))));
    }
    case BinaryExpr::Op::BitwiseOr: {
      Value lhs;
      if (!toNumericOperand(left, lhs)) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs;
      if (!toNumericOperand(right, rhs)) LIGHTJS_RETURN(Value(Undefined{}));

      if (lhs.isBigInt() && rhs.isBigInt()) {
        LIGHTJS_RETURN(Value(BigInt(lhs.toBigInt() | rhs.toBigInt())));
      }
      if (lhs.isBigInt() != rhs.isBigInt()) {
        throwError(ErrorType::TypeError, "Cannot mix BigInt and other types in bitwise operations");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(static_cast<double>(toInt32(lhs.toNumber()) | toInt32(rhs.toNumber()))));
    }
    case BinaryExpr::Op::BitwiseXor: {
      Value lhs;
      if (!toNumericOperand(left, lhs)) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs;
      if (!toNumericOperand(right, rhs)) LIGHTJS_RETURN(Value(Undefined{}));

      if (lhs.isBigInt() && rhs.isBigInt()) {
        LIGHTJS_RETURN(Value(BigInt(lhs.toBigInt() ^ rhs.toBigInt())));
      }
      if (lhs.isBigInt() != rhs.isBigInt()) {
        throwError(ErrorType::TypeError, "Cannot mix BigInt and other types in bitwise operations");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(static_cast<double>(toInt32(lhs.toNumber()) ^ toInt32(rhs.toNumber()))));
    }
    case BinaryExpr::Op::LeftShift: {
      Value lhs;
      if (!toNumericOperand(left, lhs)) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs;
      if (!toNumericOperand(right, rhs)) LIGHTJS_RETURN(Value(Undefined{}));

      if (lhs.isBigInt() && rhs.isBigInt()) {
        bool ok = false;
        auto shifted = applyBigIntShiftLeft(lhs.toBigInt(), rhs.toBigInt(), &ok);
        if (!ok) {
          throwError(ErrorType::RangeError, "Invalid BigInt shift count");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        LIGHTJS_RETURN(Value(BigInt(shifted)));
      }
      if (lhs.isBigInt() != rhs.isBigInt()) {
        throwError(ErrorType::TypeError, "Cannot mix BigInt and other types in bitwise operations");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(static_cast<double>(toInt32(lhs.toNumber()) << (toInt32(rhs.toNumber()) & 0x1f))));
    }
    case BinaryExpr::Op::RightShift: {
      Value lhs;
      if (!toNumericOperand(left, lhs)) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs;
      if (!toNumericOperand(right, rhs)) LIGHTJS_RETURN(Value(Undefined{}));

      if (lhs.isBigInt() && rhs.isBigInt()) {
        bool ok = false;
        auto shifted = applyBigIntShiftRight(lhs.toBigInt(), rhs.toBigInt(), &ok);
        if (!ok) {
          throwError(ErrorType::RangeError, "Invalid BigInt shift count");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        LIGHTJS_RETURN(Value(BigInt(shifted)));
      }
      if (lhs.isBigInt() != rhs.isBigInt()) {
        throwError(ErrorType::TypeError, "Cannot mix BigInt and other types in bitwise operations");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(static_cast<double>(toInt32(lhs.toNumber()) >> (toInt32(rhs.toNumber()) & 0x1f))));
    }
    case BinaryExpr::Op::UnsignedRightShift: {
      Value lhs;
      if (!toNumericOperand(left, lhs)) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs;
      if (!toNumericOperand(right, rhs)) LIGHTJS_RETURN(Value(Undefined{}));

      if (lhs.isBigInt() || rhs.isBigInt()) {
        throwError(ErrorType::TypeError, "Cannot use unsigned right shift on BigInt");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(static_cast<double>(static_cast<uint32_t>(toInt32(lhs.toNumber())) >> (toInt32(rhs.toNumber()) & 0x1f))));
    }
    case BinaryExpr::Op::Exp: {
      // Evaluation order for **:
      //   Evaluate lhs
      //   Evaluate rhs
      //   ToNumeric(lhs)
      //   ToNumeric(rhs)
      //
      // At this point `left` and `right` are already the results of evaluating
      // both operands, so we must ensure ToNumeric(lhs) happens before any
      // ToNumeric(rhs) coercion (Test262 order-of-evaluation).
      Value lhsPrim = isObjectLike(left) ? toPrimitiveValue(left, false) : left;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));

      Value lhsNumeric;
      if (lhsPrim.isBigInt()) {
        lhsNumeric = lhsPrim;
      } else {
        if (lhsPrim.isSymbol()) {
          throwError(ErrorType::TypeError, "Cannot convert Symbol to number");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        lhsNumeric = Value(lhsPrim.toNumber());
      }

      Value rhsPrim = isObjectLike(right) ? toPrimitiveValue(right, false) : right;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));

      Value rhsNumeric;
      if (rhsPrim.isBigInt()) {
        rhsNumeric = rhsPrim;
      } else {
        if (rhsPrim.isSymbol()) {
          throwError(ErrorType::TypeError, "Cannot convert Symbol to number");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        rhsNumeric = Value(rhsPrim.toNumber());
      }

      if (lhsNumeric.isBigInt() && rhsNumeric.isBigInt()) {
        auto base = lhsNumeric.toBigInt();
        auto exp = rhsNumeric.toBigInt();
        if (exp < 0) {
          throwError(ErrorType::RangeError, "BigInt negative exponent");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        LIGHTJS_RETURN(Value(BigInt(powBigInt(base, exp))));
      }
      if (lhsNumeric.isBigInt() != rhsNumeric.isBigInt()) {
        throwError(ErrorType::TypeError, "Cannot mix BigInt and other types");
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      double base = lhsNumeric.toNumber();
      double exp = rhsNumeric.toNumber();
      if (std::isinf(exp) && std::fabs(base) == 1.0) {
        LIGHTJS_RETURN(Value(std::numeric_limits<double>::quiet_NaN()));
      }
      LIGHTJS_RETURN(Value(std::pow(base, exp)));
    }
    case BinaryExpr::Op::Less: {
      Value lhs = isObjectLike(left) ? toPrimitiveValue(left, false) : left;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs = isObjectLike(right) ? toPrimitiveValue(right, false) : right;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      if (lhs.isString() && rhs.isString()) {
        LIGHTJS_RETURN(Value(compareStringsByUtf16CodeUnits(lhs.toString(), rhs.toString()) < 0));
      }
      if (lhs.isSymbol() || rhs.isSymbol()) {
        throwError(ErrorType::TypeError, "Cannot convert Symbol to number");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (lhs.isBigInt() && rhs.isBigInt()) {
        LIGHTJS_RETURN(Value(lhs.toBigInt() < rhs.toBigInt()));
      }
      if (lhs.isBigInt() && rhs.isString()) {
        bigint::BigIntValue parsed = 0;
        if (!parseBigIntString64(rhs.toString(), parsed)) {
          LIGHTJS_RETURN(Value(false));
        }
        LIGHTJS_RETURN(Value(lhs.toBigInt() < parsed));
      }
      if (lhs.isString() && rhs.isBigInt()) {
        bigint::BigIntValue parsed = 0;
        if (!parseBigIntString64(lhs.toString(), parsed)) {
          LIGHTJS_RETURN(Value(false));
        }
        LIGHTJS_RETURN(Value(parsed < rhs.toBigInt()));
      }
      if (lhs.isBigInt() && rhs.isNumber()) {
        auto cmp = compareBigIntAndNumber(lhs.toBigInt(), rhs.toNumber());
        LIGHTJS_RETURN(Value(cmp == BigIntNumberOrder::Less));
      }
      if (lhs.isNumber() && rhs.isBigInt()) {
        auto cmp = compareBigIntAndNumber(rhs.toBigInt(), lhs.toNumber());
        LIGHTJS_RETURN(Value(cmp == BigIntNumberOrder::Greater));
      }
      LIGHTJS_RETURN(Value(lhs.toNumber() < rhs.toNumber()));
    }
    case BinaryExpr::Op::Greater: {
      Value lhs = isObjectLike(left) ? toPrimitiveValue(left, false) : left;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs = isObjectLike(right) ? toPrimitiveValue(right, false) : right;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      if (lhs.isString() && rhs.isString()) {
        LIGHTJS_RETURN(Value(compareStringsByUtf16CodeUnits(lhs.toString(), rhs.toString()) > 0));
      }
      if (lhs.isSymbol() || rhs.isSymbol()) {
        throwError(ErrorType::TypeError, "Cannot convert Symbol to number");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (lhs.isBigInt() && rhs.isBigInt()) {
        LIGHTJS_RETURN(Value(lhs.toBigInt() > rhs.toBigInt()));
      }
      if (lhs.isBigInt() && rhs.isString()) {
        bigint::BigIntValue parsed = 0;
        if (!parseBigIntString64(rhs.toString(), parsed)) {
          LIGHTJS_RETURN(Value(false));
        }
        LIGHTJS_RETURN(Value(lhs.toBigInt() > parsed));
      }
      if (lhs.isString() && rhs.isBigInt()) {
        bigint::BigIntValue parsed = 0;
        if (!parseBigIntString64(lhs.toString(), parsed)) {
          LIGHTJS_RETURN(Value(false));
        }
        LIGHTJS_RETURN(Value(parsed > rhs.toBigInt()));
      }
      if (lhs.isBigInt() && rhs.isNumber()) {
        auto cmp = compareBigIntAndNumber(lhs.toBigInt(), rhs.toNumber());
        LIGHTJS_RETURN(Value(cmp == BigIntNumberOrder::Greater));
      }
      if (lhs.isNumber() && rhs.isBigInt()) {
        auto cmp = compareBigIntAndNumber(rhs.toBigInt(), lhs.toNumber());
        LIGHTJS_RETURN(Value(cmp == BigIntNumberOrder::Less));
      }
      LIGHTJS_RETURN(Value(lhs.toNumber() > rhs.toNumber()));
    }
    case BinaryExpr::Op::LessEqual: {
      Value lhs = isObjectLike(left) ? toPrimitiveValue(left, false) : left;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs = isObjectLike(right) ? toPrimitiveValue(right, false) : right;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      if (lhs.isString() && rhs.isString()) {
        LIGHTJS_RETURN(Value(compareStringsByUtf16CodeUnits(lhs.toString(), rhs.toString()) <= 0));
      }
      if (lhs.isSymbol() || rhs.isSymbol()) {
        throwError(ErrorType::TypeError, "Cannot convert Symbol to number");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (lhs.isBigInt() && rhs.isBigInt()) {
        LIGHTJS_RETURN(Value(lhs.toBigInt() <= rhs.toBigInt()));
      }
      if (lhs.isBigInt() && rhs.isString()) {
        bigint::BigIntValue parsed = 0;
        if (!parseBigIntString64(rhs.toString(), parsed)) {
          LIGHTJS_RETURN(Value(false));
        }
        LIGHTJS_RETURN(Value(lhs.toBigInt() <= parsed));
      }
      if (lhs.isString() && rhs.isBigInt()) {
        bigint::BigIntValue parsed = 0;
        if (!parseBigIntString64(lhs.toString(), parsed)) {
          LIGHTJS_RETURN(Value(false));
        }
        LIGHTJS_RETURN(Value(parsed <= rhs.toBigInt()));
      }
      if (lhs.isBigInt() && rhs.isNumber()) {
        auto cmp = compareBigIntAndNumber(lhs.toBigInt(), rhs.toNumber());
        LIGHTJS_RETURN(Value(cmp == BigIntNumberOrder::Less || cmp == BigIntNumberOrder::Equal));
      }
      if (lhs.isNumber() && rhs.isBigInt()) {
        auto cmp = compareBigIntAndNumber(rhs.toBigInt(), lhs.toNumber());
        LIGHTJS_RETURN(Value(cmp == BigIntNumberOrder::Greater || cmp == BigIntNumberOrder::Equal));
      }
      LIGHTJS_RETURN(Value(lhs.toNumber() <= rhs.toNumber()));
    }
    case BinaryExpr::Op::GreaterEqual: {
      Value lhs = isObjectLike(left) ? toPrimitiveValue(left, false) : left;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs = isObjectLike(right) ? toPrimitiveValue(right, false) : right;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      if (lhs.isString() && rhs.isString()) {
        LIGHTJS_RETURN(Value(compareStringsByUtf16CodeUnits(lhs.toString(), rhs.toString()) >= 0));
      }
      if (lhs.isSymbol() || rhs.isSymbol()) {
        throwError(ErrorType::TypeError, "Cannot convert Symbol to number");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (lhs.isBigInt() && rhs.isBigInt()) {
        LIGHTJS_RETURN(Value(lhs.toBigInt() >= rhs.toBigInt()));
      }
      if (lhs.isBigInt() && rhs.isString()) {
        bigint::BigIntValue parsed = 0;
        if (!parseBigIntString64(rhs.toString(), parsed)) {
          LIGHTJS_RETURN(Value(false));
        }
        LIGHTJS_RETURN(Value(lhs.toBigInt() >= parsed));
      }
      if (lhs.isString() && rhs.isBigInt()) {
        bigint::BigIntValue parsed = 0;
        if (!parseBigIntString64(lhs.toString(), parsed)) {
          LIGHTJS_RETURN(Value(false));
        }
        LIGHTJS_RETURN(Value(parsed >= rhs.toBigInt()));
      }
      if (lhs.isBigInt() && rhs.isNumber()) {
        auto cmp = compareBigIntAndNumber(lhs.toBigInt(), rhs.toNumber());
        LIGHTJS_RETURN(Value(cmp == BigIntNumberOrder::Greater || cmp == BigIntNumberOrder::Equal));
      }
      if (lhs.isNumber() && rhs.isBigInt()) {
        auto cmp = compareBigIntAndNumber(rhs.toBigInt(), lhs.toNumber());
        LIGHTJS_RETURN(Value(cmp == BigIntNumberOrder::Less || cmp == BigIntNumberOrder::Equal));
      }
      LIGHTJS_RETURN(Value(lhs.toNumber() >= rhs.toNumber()));
    }
    case BinaryExpr::Op::Equal: {
      // Abstract Equality Comparison (ES spec 7.2.14)
      bool eq = abstractEqual(left, right);
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      LIGHTJS_RETURN(Value(eq));
    }
    case BinaryExpr::Op::NotEqual: {
      bool eq = abstractEqual(left, right);
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      LIGHTJS_RETURN(Value(!eq));
    }
    case BinaryExpr::Op::StrictEqual: {
      // Strict equality requires same type
      if (left.data.index() != right.data.index()) {
        LIGHTJS_RETURN(Value(false));
      }

      if (left.isSymbol() && right.isSymbol()) {
        auto& lsym = std::get<Symbol>(left.data);
        auto& rsym = std::get<Symbol>(right.data);
        LIGHTJS_RETURN(Value(lsym.id == rsym.id));
      }

      if (left.isBigInt() && right.isBigInt()) {
        LIGHTJS_RETURN(Value(left.toBigInt() == right.toBigInt()));
      }

      if (left.isNumber() && right.isNumber()) {
        LIGHTJS_RETURN(Value(left.toNumber() == right.toNumber()));
      }

      if (left.isString() && right.isString()) {
        LIGHTJS_RETURN(Value(std::get<std::string>(left.data) == std::get<std::string>(right.data)));
      }

      if (left.isBool() && right.isBool()) {
        LIGHTJS_RETURN(Value(std::get<bool>(left.data) == std::get<bool>(right.data)));
      }

      if ((left.isNull() && right.isNull()) || (left.isUndefined() && right.isUndefined())) {
        LIGHTJS_RETURN(Value(true));
      }

      if (left.isObject() && right.isObject()) {
        LIGHTJS_RETURN(Value(left.getGC<Object>().get() ==
                             right.getGC<Object>().get()));
      }
      if (left.isArray() && right.isArray()) {
        LIGHTJS_RETURN(Value(left.getGC<Array>().get() ==
                             right.getGC<Array>().get()));
      }
      if (left.isFunction() && right.isFunction()) {
        LIGHTJS_RETURN(Value(left.getGC<Function>().get() ==
                             right.getGC<Function>().get()));
      }
      if (left.isTypedArray() && right.isTypedArray()) {
        LIGHTJS_RETURN(Value(left.getGC<TypedArray>().get() ==
                             right.getGC<TypedArray>().get()));
      }
      if (left.isPromise() && right.isPromise()) {
        LIGHTJS_RETURN(Value(left.getGC<Promise>().get() ==
                             right.getGC<Promise>().get()));
      }
      if (left.isRegex() && right.isRegex()) {
        LIGHTJS_RETURN(Value(left.getGC<Regex>().get() ==
                             right.getGC<Regex>().get()));
      }
      if (left.isMap() && right.isMap()) {
        LIGHTJS_RETURN(Value(left.getGC<Map>().get() ==
                             right.getGC<Map>().get()));
      }
      if (left.isSet() && right.isSet()) {
        LIGHTJS_RETURN(Value(left.getGC<Set>().get() ==
                             right.getGC<Set>().get()));
      }
      if (left.isError() && right.isError()) {
        LIGHTJS_RETURN(Value(left.getGC<Error>().get() ==
                             right.getGC<Error>().get()));
      }
      if (left.isGenerator() && right.isGenerator()) {
        LIGHTJS_RETURN(Value(left.getGC<Generator>().get() ==
                             right.getGC<Generator>().get()));
      }
      if (left.isProxy() && right.isProxy()) {
        LIGHTJS_RETURN(Value(left.getGC<Proxy>().get() ==
                             right.getGC<Proxy>().get()));
      }
      if (left.isWeakMap() && right.isWeakMap()) {
        LIGHTJS_RETURN(Value(std::get<GCPtr<WeakMap>>(left.data).get() ==
                             std::get<GCPtr<WeakMap>>(right.data).get()));
      }
      if (left.isWeakSet() && right.isWeakSet()) {
        LIGHTJS_RETURN(Value(std::get<GCPtr<WeakSet>>(left.data).get() ==
                             std::get<GCPtr<WeakSet>>(right.data).get()));
      }
      if (left.isArrayBuffer() && right.isArrayBuffer()) {
        LIGHTJS_RETURN(Value(left.getGC<ArrayBuffer>().get() ==
                             right.getGC<ArrayBuffer>().get()));
      }
      if (left.isDataView() && right.isDataView()) {
        LIGHTJS_RETURN(Value(left.getGC<DataView>().get() ==
                             right.getGC<DataView>().get()));
      }
      if (left.isClass() && right.isClass()) {
        LIGHTJS_RETURN(Value(left.getGC<Class>().get() ==
                             right.getGC<Class>().get()));
      }
      if (left.isWasmInstance() && right.isWasmInstance()) {
        LIGHTJS_RETURN(Value(left.getGC<WasmInstanceJS>().get() ==
                             right.getGC<WasmInstanceJS>().get()));
      }
      if (left.isWasmMemory() && right.isWasmMemory()) {
        LIGHTJS_RETURN(Value(left.getGC<WasmMemoryJS>().get() ==
                             right.getGC<WasmMemoryJS>().get()));
      }
      if (left.isReadableStream() && right.isReadableStream()) {
        LIGHTJS_RETURN(Value(left.getGC<ReadableStream>().get() ==
                             right.getGC<ReadableStream>().get()));
      }
      if (left.isWritableStream() && right.isWritableStream()) {
        LIGHTJS_RETURN(Value(left.getGC<WritableStream>().get() ==
                             right.getGC<WritableStream>().get()));
      }
      if (left.isTransformStream() && right.isTransformStream()) {
        LIGHTJS_RETURN(Value(left.getGC<TransformStream>().get() ==
                             right.getGC<TransformStream>().get()));
      }

      LIGHTJS_RETURN(Value(false));
    }
    case BinaryExpr::Op::StrictNotEqual: {
      // Reuse StrictEqual logic
      if (left.data.index() != right.data.index()) {
        LIGHTJS_RETURN(Value(true));
      }

      if (left.isSymbol() && right.isSymbol()) {
        auto& lsym = std::get<Symbol>(left.data);
        auto& rsym = std::get<Symbol>(right.data);
        LIGHTJS_RETURN(Value(lsym.id != rsym.id));
      }

      if (left.isBigInt() && right.isBigInt()) {
        LIGHTJS_RETURN(Value(left.toBigInt() != right.toBigInt()));
      }

      if (left.isNumber() && right.isNumber()) {
        LIGHTJS_RETURN(Value(left.toNumber() != right.toNumber()));
      }

      if (left.isString() && right.isString()) {
        LIGHTJS_RETURN(Value(std::get<std::string>(left.data) != std::get<std::string>(right.data)));
      }

      if (left.isBool() && right.isBool()) {
        LIGHTJS_RETURN(Value(std::get<bool>(left.data) != std::get<bool>(right.data)));
      }

      if ((left.isNull() && right.isNull()) || (left.isUndefined() && right.isUndefined())) {
        LIGHTJS_RETURN(Value(false));
      }

      // For all reference types, strict inequality is pointer inequality.
      auto equalByPointer = [&](const Value& a, const Value& b) -> bool {
        if (a.isObject() && b.isObject()) {
          return a.getGC<Object>().get() ==
                 b.getGC<Object>().get();
        }
        if (a.isArray() && b.isArray()) {
          return a.getGC<Array>().get() ==
                 b.getGC<Array>().get();
        }
        if (a.isFunction() && b.isFunction()) {
          return a.getGC<Function>().get() ==
                 b.getGC<Function>().get();
        }
        if (a.isTypedArray() && b.isTypedArray()) {
          return a.getGC<TypedArray>().get() ==
                 b.getGC<TypedArray>().get();
        }
        if (a.isPromise() && b.isPromise()) {
          return a.getGC<Promise>().get() ==
                 b.getGC<Promise>().get();
        }
        if (a.isRegex() && b.isRegex()) {
          return a.getGC<Regex>().get() ==
                 b.getGC<Regex>().get();
        }
        if (a.isMap() && b.isMap()) {
          return a.getGC<Map>().get() ==
                 b.getGC<Map>().get();
        }
        if (a.isSet() && b.isSet()) {
          return a.getGC<Set>().get() ==
                 b.getGC<Set>().get();
        }
        if (a.isError() && b.isError()) {
          return a.getGC<Error>().get() ==
                 b.getGC<Error>().get();
        }
        if (a.isGenerator() && b.isGenerator()) {
          return a.getGC<Generator>().get() ==
                 b.getGC<Generator>().get();
        }
        if (a.isProxy() && b.isProxy()) {
          return a.getGC<Proxy>().get() ==
                 b.getGC<Proxy>().get();
        }
        if (a.isWeakMap() && b.isWeakMap()) {
          return std::get<GCPtr<WeakMap>>(a.data).get() ==
                 std::get<GCPtr<WeakMap>>(b.data).get();
        }
        if (a.isWeakSet() && b.isWeakSet()) {
          return std::get<GCPtr<WeakSet>>(a.data).get() ==
                 std::get<GCPtr<WeakSet>>(b.data).get();
        }
        if (a.isArrayBuffer() && b.isArrayBuffer()) {
          return a.getGC<ArrayBuffer>().get() ==
                 b.getGC<ArrayBuffer>().get();
        }
        if (a.isDataView() && b.isDataView()) {
          return a.getGC<DataView>().get() ==
                 b.getGC<DataView>().get();
        }
        if (a.isClass() && b.isClass()) {
          return a.getGC<Class>().get() ==
                 b.getGC<Class>().get();
        }
        if (a.isWasmInstance() && b.isWasmInstance()) {
          return a.getGC<WasmInstanceJS>().get() ==
                 b.getGC<WasmInstanceJS>().get();
        }
        if (a.isWasmMemory() && b.isWasmMemory()) {
          return a.getGC<WasmMemoryJS>().get() ==
                 b.getGC<WasmMemoryJS>().get();
        }
        if (a.isReadableStream() && b.isReadableStream()) {
          return a.getGC<ReadableStream>().get() ==
                 b.getGC<ReadableStream>().get();
        }
        if (a.isWritableStream() && b.isWritableStream()) {
          return a.getGC<WritableStream>().get() ==
                 b.getGC<WritableStream>().get();
        }
        if (a.isTransformStream() && b.isTransformStream()) {
          return a.getGC<TransformStream>().get() ==
                 b.getGC<TransformStream>().get();
        }
        return false;
      };
      LIGHTJS_RETURN(Value(!equalByPointer(left, right)));
    }
    // LogicalAnd, LogicalOr, and NullishCoalescing are handled above
    // with short-circuit evaluation (right side not evaluated if not needed)
    case BinaryExpr::Op::In: {
      // 'prop in obj' - check if property exists in object
      std::string propName = toPropertyKeyString(left);

      // Handle Proxy has trap
      if (right.isProxy()) {
        auto proxyPtr = right.getGC<Proxy>();
        if (proxyPtr->handler && proxyPtr->handler->isObject()) {
          auto handlerObj = std::get<GCPtr<Object>>(proxyPtr->handler->data);
          auto trapIt = handlerObj->properties.find("has");
          if (trapIt != handlerObj->properties.end() && trapIt->second.isFunction()) {
            auto trap = trapIt->second.getGC<Function>();
            if (trap->isNative) {
              std::vector<Value> trapArgs = {*proxyPtr->target, toPropertyKeyValue(propName)};
              LIGHTJS_RETURN(trap->nativeFunc(trapArgs));
            }
          }
        }
        // Fall through to check target
        if (proxyPtr->target && proxyPtr->target->isObject()) {
          auto targetObj = std::get<GCPtr<Object>>(proxyPtr->target->data);
          LIGHTJS_RETURN(Value(targetObj->properties.find(propName) != targetObj->properties.end()));
        }
        LIGHTJS_RETURN(Value(false));
      }

      if (!isObjectLike(right) && !right.isClass()) {
        throwError(ErrorType::TypeError, "Right-hand side of 'in' should be an object");
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      // Helper lambda to walk prototype chain for 'in' operator
      auto hasPropertyInChain = [&](const OrderedMap<std::string, Value>& props) -> bool {
        if (props.find(propName) != props.end()) return true;
        auto protoIt = props.find("__proto__");
        if (protoIt != props.end() && protoIt->second.isObject()) {
          auto proto = protoIt->second.getGC<Object>();
          int depth = 0;
          while (proto && depth < 50) {
            if (proto->properties.find(propName) != proto->properties.end()) return true;
            auto nextProto = proto->properties.find("__proto__");
            if (nextProto == proto->properties.end() || !nextProto->second.isObject()) break;
            proto = nextProto->second.getGC<Object>();
            depth++;
          }
        }
        return false;
      };

      if (right.isObject()) {
        auto objPtr = right.getGC<Object>();
        LIGHTJS_RETURN(Value(hasPropertyInChain(objPtr->properties)));
      }

      if (right.isArray()) {
        auto arrPtr = right.getGC<Array>();
        size_t idx = 0;
        if (parseArrayIndex(propName, idx)) {
          if (idx >= arrPtr->elements.size()) LIGHTJS_RETURN(Value(false));
          // Check for holes and deleted elements
          if (arrPtr->properties.find("__deleted_" + propName + "__") != arrPtr->properties.end())
            LIGHTJS_RETURN(Value(false));
          if (arrPtr->properties.find("__hole_" + propName + "__") != arrPtr->properties.end())
            LIGHTJS_RETURN(Value(false));
          LIGHTJS_RETURN(Value(true));
        }
        if (propName == "length") {
          if (arrPtr->properties.find("__deleted_length__") != arrPtr->properties.end())
            LIGHTJS_RETURN(Value(false));
          LIGHTJS_RETURN(Value(true));
        }
        LIGHTJS_RETURN(Value(hasPropertyInChain(arrPtr->properties)));
      }

      if (right.isFunction()) {
        auto fnPtr = right.getGC<Function>();
        LIGHTJS_RETURN(Value(hasPropertyInChain(fnPtr->properties)));
      }

      if (right.isTypedArray()) {
        auto taPtr = right.getGC<TypedArray>();
        size_t idx = 0;
        if (parseArrayIndex(propName, idx)) {
          LIGHTJS_RETURN(Value(idx < taPtr->currentLength()));
        }
        if (propName == "NaN" || propName == "-0" ||
            propName == "Infinity" || propName == "-Infinity") {
          LIGHTJS_RETURN(Value(false));
        }
        LIGHTJS_RETURN(Value(hasPropertyInChain(taPtr->properties)));
      }

      if (right.isClass()) {
        auto clsPtr = right.getGC<Class>();
        LIGHTJS_RETURN(Value(hasPropertyInChain(clsPtr->properties)));
      }

      if (right.isRegex()) {
        auto regexPtr = right.getGC<Regex>();
        LIGHTJS_RETURN(Value(hasPropertyInChain(regexPtr->properties)));
      }

      if (right.isPromise()) {
        auto promisePtr = right.getGC<Promise>();
        LIGHTJS_RETURN(Value(hasPropertyInChain(promisePtr->properties)));
      }

      // Unhandled object-like should default to false after object checks.
      LIGHTJS_RETURN(Value(false));
    }
    case BinaryExpr::Op::Instanceof: {
      // ES2015 12.10.4 Runtime Semantics: InstanceofOperator(O, C)
      // 1. If Type(C) is not Object, throw a TypeError exception.
      if (!isObjectLike(right)) {
        throwError(ErrorType::TypeError, "Right-hand side of instanceof is not an object");
        LIGHTJS_RETURN(Value(false));
      }

      // 2. Let instOfHandler be GetMethod(C, @@hasInstance).
      // 3. If instOfHandler is not undefined, return ToBoolean(Call(instOfHandler, C, «O»)).
      // Note: Only check OWN @@hasInstance to avoid triggering the default
      // Function.prototype[@@hasInstance] (OrdinaryHasInstance), which is
      // already implemented below with better type coverage.
      {
        const std::string& hasInstanceKey = WellKnownSymbols::hasInstanceKey();
        bool foundOwn = false;
        Value handler;
        if (right.isFunction()) {
          auto fn = right.getGC<Function>();
          auto it = fn->properties.find(hasInstanceKey);
          if (it != fn->properties.end()) { foundOwn = true; handler = it->second; }
        } else if (right.isClass()) {
          auto cls = right.getGC<Class>();
          auto it = cls->properties.find(hasInstanceKey);
          if (it != cls->properties.end()) { foundOwn = true; handler = it->second; }
        } else if (right.isObject()) {
          auto obj = right.getGC<Object>();
          auto it = obj->properties.find(hasInstanceKey);
          if (it != obj->properties.end()) { foundOwn = true; handler = it->second; }
        }
        if (foundOwn && !handler.isUndefined()) {
          if (!handler.isFunction()) {
            throwError(ErrorType::TypeError, "Symbol.hasInstance is not callable");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          Value result = callFunction(handler, {left}, right);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
          LIGHTJS_RETURN(Value(result.toBool()));
        }
      }

      std::function<bool(const Value&)> isCallableValue = [&](const Value& v) -> bool {
        if (v.isFunction() || v.isClass()) return true;
        if (v.isProxy()) {
          auto p = v.getGC<Proxy>();
          if (p->target) {
            return isCallableValue(*p->target);
          }
          return false;
        }
        if (v.isObject()) {
          auto obj = v.getGC<Object>();
          auto callableIt = obj->properties.find("__callable_object__");
          return callableIt != obj->properties.end() &&
                 callableIt->second.isBool() &&
                 callableIt->second.toBool();
        }
        return false;
      };

      // 4. If IsCallable(C) is false, throw a TypeError exception.
      if (!isCallableValue(right)) {
        throwError(ErrorType::TypeError, "Right-hand side of instanceof is not callable");
        LIGHTJS_RETURN(Value(false));
      }

      // Per spec: if LHS is a primitive (not object-like), return false
      if (!left.isObject() && !left.isArray() && !left.isFunction() &&
          !left.isRegex() && !left.isPromise() && !left.isError() &&
          !left.isClass() && !left.isProxy() && !left.isGenerator() &&
          !left.isMap() && !left.isSet() && !left.isWeakMap() && !left.isWeakSet() &&
          !left.isTypedArray() && !left.isArrayBuffer() && !left.isDataView()) {
        LIGHTJS_RETURN(Value(false));
      }
      auto unwrapConstructor = [&](const Value& ctor) -> Value {
        if (ctor.isObject()) {
          auto obj = ctor.getGC<Object>();
          auto callableIt = obj->properties.find("__callable_object__");
          if (callableIt != obj->properties.end() &&
              callableIt->second.isBool() && callableIt->second.toBool()) {
            auto ctorIt = obj->properties.find("constructor");
            if (ctorIt != obj->properties.end() &&
                (ctorIt->second.isFunction() || ctorIt->second.isClass())) {
              return ctorIt->second;
            }
          }
        }
        return ctor;
      };

      auto sameCtor = [&](const Value& candidate, const Value& ctor) -> bool {
        if (candidate.data.index() != ctor.data.index()) {
          return false;
        }
        if (candidate.isFunction()) {
          return candidate.getGC<Function>() ==
                 ctor.getGC<Function>();
        }
        if (candidate.isClass()) {
          return candidate.getGC<Class>() ==
                 ctor.getGC<Class>();
        }
        return false;
      };

      auto matchesConstructor = [&](const Value& instance, const Value& ctor) -> bool {
        if (instance.isObject()) {
          auto obj = instance.getGC<Object>();
          auto it = obj->properties.find("__constructor__");
          return it != obj->properties.end() && sameCtor(it->second, ctor);
        }
        if (instance.isGenerator()) {
          auto gen = instance.getGC<Generator>();
          auto it = gen->properties.find("__constructor__");
          return it != gen->properties.end() && sameCtor(it->second, ctor);
        }
        if (instance.isArray()) {
          auto arr = instance.getGC<Array>();
          auto it = arr->properties.find("__constructor__");
          return it != arr->properties.end() && sameCtor(it->second, ctor);
        }
        if (instance.isFunction()) {
          auto fn = instance.getGC<Function>();
          auto it = fn->properties.find("__constructor__");
          return it != fn->properties.end() && sameCtor(it->second, ctor);
        }
        if (instance.isRegex()) {
          auto regex = instance.getGC<Regex>();
          auto it = regex->properties.find("__constructor__");
          return it != regex->properties.end() && sameCtor(it->second, ctor);
        }
        if (instance.isPromise()) {
          auto promise = instance.getGC<Promise>();
          auto it = promise->properties.find("__constructor__");
          return it != promise->properties.end() && sameCtor(it->second, ctor);
        }
        if (instance.isMap()) {
          auto map = instance.getGC<Map>();
          auto it = map->properties.find("__constructor__");
          return it != map->properties.end() && sameCtor(it->second, ctor);
        }
        if (instance.isSet()) {
          auto set = instance.getGC<Set>();
          auto it = set->properties.find("__constructor__");
          return it != set->properties.end() && sameCtor(it->second, ctor);
        }
        if (instance.isWeakMap()) {
          auto wm = instance.getGC<WeakMap>();
          auto it = wm->properties.find("__constructor__");
          return it != wm->properties.end() && sameCtor(it->second, ctor);
        }
        if (instance.isWeakSet()) {
          auto ws = instance.getGC<WeakSet>();
          auto it = ws->properties.find("__constructor__");
          return it != ws->properties.end() && sameCtor(it->second, ctor);
        }
        if (instance.isTypedArray()) {
          auto ta = instance.getGC<TypedArray>();
          auto it = ta->properties.find("__constructor__");
          return it != ta->properties.end() && sameCtor(it->second, ctor);
        }
        if (instance.isArrayBuffer()) {
          auto ab = instance.getGC<ArrayBuffer>();
          auto it = ab->properties.find("__constructor__");
          return it != ab->properties.end() && sameCtor(it->second, ctor);
        }
        if (instance.isDataView()) {
          auto dv = instance.getGC<DataView>();
          auto it = dv->properties.find("__constructor__");
          return it != dv->properties.end() && sameCtor(it->second, ctor);
        }
        if (instance.isError()) {
          auto err = instance.getGC<Error>();
          auto it = err->properties.find("__constructor__");
          return it != err->properties.end() && sameCtor(it->second, ctor);
        }
        return false;
      };

      Value ctorValue = unwrapConstructor(right);

      if (left.isTypedArray() && ctorValue.isFunction()) {
        auto ctor = ctorValue.getGC<Function>();
        auto nameIt = ctor->properties.find("name");
        if (nameIt != ctor->properties.end() && nameIt->second.isString()) {
          const std::string ctorName = nameIt->second.toString();
          if (ctorName == "TypedArray") {
            LIGHTJS_RETURN(Value(true));
          }
          TypedArrayType expectedType = TypedArrayType::Uint8;
          if (typedArrayConstructorNameToType(ctorName, expectedType)) {
            LIGHTJS_RETURN(Value(left.getGC<TypedArray>()->type == expectedType));
          }
        }
      }

      if (left.isError() && ctorValue.isFunction()) {
        auto ctor = ctorValue.getGC<Function>();
        auto tagIt = ctor->properties.find("__error_type__");
        if (tagIt != ctor->properties.end() && tagIt->second.isNumber()) {
          auto err = left.getGC<Error>();
          int expected = static_cast<int>(std::get<double>(tagIt->second.data));
          // Exact match (e.g., new TypeError instanceof TypeError)
          if (static_cast<int>(err->type) == expected) {
            LIGHTJS_RETURN(Value(true));
          }
          // Base Error constructor matches all error types (inheritance)
          if (expected == static_cast<int>(ErrorType::Error)) {
            LIGHTJS_RETURN(Value(true));
          }
          LIGHTJS_RETURN(Value(false));
        }
      }

      // OrdinaryHasInstance: walk the prototype chain
      // Get the constructor's .prototype property
      auto getCtorPrototype = [&](const Value& ctor) -> Value {
        auto [found, protoVal] = getPropertyForPrimitive(ctor, "prototype");
        if (hasError()) return Value(Undefined{});
        if (!found) return Value(Undefined{});
        if (!protoVal.isObject() && !protoVal.isFunction()) {
          // OrdinaryHasInstance requires `prototype` to be an Object (functions are objects).
          throwError(ErrorType::TypeError, "Function has non-object prototype in instanceof check");
          return Value(Undefined{});
        }
        return protoVal;
      };

      // Use the original RHS (right) for prototype chain walk, not the unwrapped ctorValue
      // This is important when RHS is a callable object like Function.prototype
      auto ctorProto = getCtorPrototype(right);
      if (hasError()) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (ctorProto.isUndefined()) {
        // Check if prototype property exists but is not an object → TypeError
        OrderedMap<std::string, Value>* checkProps = nullptr;
        if (right.isFunction()) {
          checkProps = &right.getGC<Function>()->properties;
        } else if (right.isObject()) {
          checkProps = &right.getGC<Object>()->properties;
        }
        if (checkProps) {
          auto protoIt = checkProps->find("prototype");
          if (protoIt != checkProps->end() &&
              !protoIt->second.isObject() &&
              !protoIt->second.isFunction()) {
            throwError(ErrorType::TypeError, "Function has non-object prototype in instanceof check");
            LIGHTJS_RETURN(Value(false));
          }
        }
      }
      if (!ctorProto.isUndefined()) {
        // Get the instance's __proto__ chain
        auto getProto = [](const Value& val) -> Value {
          OrderedMap<std::string, Value>* props = nullptr;
          if (val.isObject()) {
            props = &val.getGC<Object>()->properties;
          } else if (val.isArray()) {
            props = &val.getGC<Array>()->properties;
          } else if (val.isFunction()) {
            props = &val.getGC<Function>()->properties;
          } else if (val.isRegex()) {
            props = &val.getGC<Regex>()->properties;
          } else if (val.isPromise()) {
            props = &val.getGC<Promise>()->properties;
          } else if (val.isMap()) {
            props = &val.getGC<Map>()->properties;
          } else if (val.isSet()) {
            props = &val.getGC<Set>()->properties;
          } else if (val.isWeakMap()) {
            props = &val.getGC<WeakMap>()->properties;
          } else if (val.isWeakSet()) {
            props = &val.getGC<WeakSet>()->properties;
          } else if (val.isTypedArray()) {
            props = &val.getGC<TypedArray>()->properties;
          } else if (val.isArrayBuffer()) {
            props = &val.getGC<ArrayBuffer>()->properties;
          } else if (val.isDataView()) {
            props = &val.getGC<DataView>()->properties;
          } else if (val.isError()) {
            props = &val.getGC<Error>()->properties;
          }
          if (props) {
            auto it = props->find("__proto__");
            if (it != props->end() && (it->second.isObject() || it->second.isFunction())) {
              return it->second;
            }
          }
          return Value(Undefined{});
        };

        auto sameProtoIdentity = [](const Value& lhs, const Value& rhs) -> bool {
          if (lhs.isObject() && rhs.isObject()) {
            return lhs.getGC<Object>().get() == rhs.getGC<Object>().get();
          }
          if (lhs.isFunction() && rhs.isFunction()) {
            return lhs.getGC<Function>().get() == rhs.getGC<Function>().get();
          }
          return false;
        };

        auto proto = getProto(left);
        int depth = 0;
        while ((proto.isObject() || proto.isFunction()) && depth < 100) {
          if (sameProtoIdentity(proto, ctorProto)) {
            LIGHTJS_RETURN(Value(true));
          }
          OrderedMap<std::string, Value>* props = nullptr;
          if (proto.isObject()) {
            props = &proto.getGC<Object>()->properties;
          } else {
            props = &proto.getGC<Function>()->properties;
          }
          auto protoIt = props->find("__proto__");
          if (protoIt == props->end() ||
              (!protoIt->second.isObject() && !protoIt->second.isFunction())) {
            break;
          }
          proto = protoIt->second;
          depth++;
        }
      }

      // Fallback: check __constructor__ tag (for objects without proper __proto__ chain)
      if ((ctorValue.isFunction() || ctorValue.isClass()) && matchesConstructor(left, ctorValue)) {
        LIGHTJS_RETURN(Value(true));
      }

      // Built-in type checks for instanceof when __proto__ chain not available
      if (ctorValue.isFunction()) {
        auto ctor = ctorValue.getGC<Function>();
        auto nameIt = ctor->properties.find("name");
        if (nameIt != ctor->properties.end() && nameIt->second.isString()) {
          std::string ctorName = nameIt->second.toString();
          if (ctorName == "Object") {
            // Any object, array, function, regex is instanceof Object
            if (left.isObject()) {
              auto obj = left.getGC<Object>();
              if (obj->isModuleNamespace) {
                LIGHTJS_RETURN(Value(false));
              }
            }
            if (left.isObject() || left.isArray() || left.isFunction() ||
                left.isRegex() || left.isPromise() || left.isError()) {
              LIGHTJS_RETURN(Value(true));
            }
          } else if (ctorName == "Array") {
            if (left.isArray()) {
              LIGHTJS_RETURN(Value(true));
            }
          } else if (ctorName == "Function") {
            if (left.isFunction()) {
              LIGHTJS_RETURN(Value(true));
            }
          } else if (ctorName == "RegExp") {
            if (left.isRegex()) {
              LIGHTJS_RETURN(Value(true));
            }
          } else if (ctorName == "Promise") {
            if (left.isPromise()) {
              LIGHTJS_RETURN(Value(true));
            }
          }
        }
      }
      LIGHTJS_RETURN(Value(false));
    }
  }

  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::evaluateUnary(const UnaryExpr& expr) {
  // Handle delete operator specially - it needs direct access to the member expression
  if (expr.op == UnaryExpr::Op::Delete) {
    // delete only works on member expressions
    if (auto* member = std::get_if<MemberExpr>(&expr.argument->node)) {
      // SuperReferences may not be deleted (and must not perform ToPropertyKey).
      if (member->object && std::holds_alternative<SuperExpr>(member->object->node)) {
        throwError(ErrorType::ReferenceError, "Cannot delete super property");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      auto objTask = evaluate(*member->object);
      Value obj;
      LIGHTJS_RUN_TASK(objTask, obj);
      if (flow_.type != ControlFlow::Type::None) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      std::string propName;
      if (member->computed) {
        auto propTask = evaluate(*member->property);
        LIGHTJS_RUN_TASK_VOID(propTask);
        if (flow_.type != ControlFlow::Type::None || hasError()) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        propName = toPropertyKeyString(propTask.result());
      } else {
        if (auto* id = std::get_if<Identifier>(&member->property->node)) {
          propName = id->name;
        }
      }

      auto deleteFailed = [&]() -> Value {
        if (strictMode_) {
          throwError(ErrorType::TypeError, "Cannot delete property '" + propName + "'");
          return Value(Undefined{});
        }
        return Value(false);
      };

      // Per spec, delete on a property reference with null/undefined base throws.
      if (obj.isNull() || obj.isUndefined()) {
        throwError(ErrorType::TypeError, "Cannot convert undefined or null to object");
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      // Handle Proxy deleteProperty trap
      if (obj.isProxy()) {
        auto proxyPtr = obj.getGC<Proxy>();
        if (proxyPtr && proxyPtr->revoked) {
          throwError(ErrorType::TypeError, "Cannot perform 'deleteProperty' on a revoked Proxy");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (proxyPtr->handler && proxyPtr->handler->isObject()) {
          auto handlerObj = std::get<GCPtr<Object>>(proxyPtr->handler->data);
          auto trapIt = handlerObj->properties.find("deleteProperty");
          if (trapIt != handlerObj->properties.end() && trapIt->second.isFunction()) {
            auto trap = trapIt->second.getGC<Function>();
            if (trap->isNative) {
              std::vector<Value> trapArgs = {*proxyPtr->target, toPropertyKeyValue(propName)};
              Value trapResult = trap->nativeFunc(trapArgs);
              if (hasError()) {
                LIGHTJS_RETURN(Value(Undefined{}));
              }
              if (!trapResult.toBool()) {
                LIGHTJS_RETURN(deleteFailed());
              }
              LIGHTJS_RETURN(Value(true));
            }
          }
        }
        // Fall through to delete from target
        if (proxyPtr->target && proxyPtr->target->isObject()) {
          auto targetObj = std::get<GCPtr<Object>>(proxyPtr->target->data);
          bool deleted = false;
          deleted = targetObj->properties.erase(propName) > 0 || deleted;
          deleted = targetObj->properties.erase("__get_" + propName) > 0 || deleted;
          deleted = targetObj->properties.erase("__set_" + propName) > 0 || deleted;
          if (deleted && targetObj->shape) {
            targetObj->shape = nullptr; // Invalidate shape on delete
          }
          LIGHTJS_RETURN(Value(true));
        }
        LIGHTJS_RETURN(deleteFailed());
      }

      // Delete on function properties
      if (obj.isFunction()) {
        auto fnPtr = obj.getGC<Function>();
        if (fnPtr->properties.count("__non_configurable_" + propName)) {
          LIGHTJS_RETURN(deleteFailed());
        }
        bool deleted = false;
        deleted = fnPtr->properties.erase(propName) > 0 || deleted;
        deleted = fnPtr->properties.erase("__get_" + propName) > 0 || deleted;
        deleted = fnPtr->properties.erase("__set_" + propName) > 0 || deleted;
        deleted = fnPtr->properties.erase("__non_enum_" + propName) > 0 || deleted;
        deleted = fnPtr->properties.erase("__enum_" + propName) > 0 || deleted;
        deleted = fnPtr->properties.erase("__non_writable_" + propName) > 0 || deleted;
        deleted = fnPtr->properties.erase("__non_configurable_" + propName) > 0 || deleted;
        LIGHTJS_RETURN(Value(true));
      }

      // Delete on class properties
      if (obj.isClass()) {
        auto clsPtr = obj.getGC<Class>();
        if (propName == "prototype") {
          LIGHTJS_RETURN(deleteFailed());
        }
        if (clsPtr->properties.count("__non_configurable_" + propName)) {
          LIGHTJS_RETURN(deleteFailed());
        }
        clsPtr->properties.erase(propName);
        clsPtr->properties.erase("__get_" + propName);
        clsPtr->properties.erase("__set_" + propName);
        clsPtr->properties.erase("__non_enum_" + propName);
        clsPtr->properties.erase("__non_writable_" + propName);
        clsPtr->properties.erase("__non_configurable_" + propName);
        clsPtr->properties.erase("__enum_" + propName);
        LIGHTJS_RETURN(Value(true));
      }

      if (obj.isObject()) {
        auto objPtr = obj.getGC<Object>();
        if (objPtr->isModuleNamespace) {
          const std::string& toStringTagKey = WellKnownSymbols::toStringTagKey();
          bool isExport = std::find(
            objPtr->moduleExportNames.begin(),
            objPtr->moduleExportNames.end(),
            propName
          ) != objPtr->moduleExportNames.end();
          if (propName == toStringTagKey) {
            isExport = true;
          }
          if (isExport && strictMode_) {
            throwError(ErrorType::TypeError, "Cannot delete property '" + propName + "' of module namespace object");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          LIGHTJS_RETURN(Value(!isExport));
        }
        bool hasProp =
          objPtr->properties.count(propName) > 0 ||
          objPtr->properties.count("__get_" + propName) > 0 ||
          objPtr->properties.count("__set_" + propName) > 0 ||
          objPtr->properties.count("__non_configurable_" + propName) > 0;
        if (!hasProp) {
          LIGHTJS_RETURN(Value(true));
        }
        if (objPtr->frozen || objPtr->sealed) {
          LIGHTJS_RETURN(deleteFailed());
        }
        // Check __non_configurable_ marker
        if (objPtr->properties.count("__non_configurable_" + propName)) {
          LIGHTJS_RETURN(deleteFailed());
        }
        bool deleted = false;
        deleted = objPtr->properties.erase(propName) > 0 || deleted;
        deleted = objPtr->properties.erase("__get_" + propName) > 0 || deleted;
        deleted = objPtr->properties.erase("__set_" + propName) > 0 || deleted;
        if (deleted && objPtr->shape) {
          objPtr->shape = nullptr; // Invalidate shape on delete
        }
        LIGHTJS_RETURN(Value(true));
      }

      if (obj.isArray()) {
        auto arrPtr = obj.getGC<Array>();
        if (propName == "length") {
          // Arguments objects have configurable length; regular arrays do not.
          bool isArgumentsObject = false;
          auto isArgsIt = arrPtr->properties.find("__is_arguments_object__");
          if (isArgsIt != arrPtr->properties.end() && isArgsIt->second.isBool() && isArgsIt->second.toBool()) {
            isArgumentsObject = true;
          }
          if (!isArgumentsObject) {
            LIGHTJS_RETURN(deleteFailed());
          }
          // For arguments, length is configurable and deletable
          arrPtr->properties["__deleted_length__"] = Value(true);
          LIGHTJS_RETURN(Value(true));
        }
        if (arrPtr->properties.count("__non_configurable_" + propName)) {
          LIGHTJS_RETURN(deleteFailed());
        }
        size_t idx = 0;
        if (parseArrayIndex(propName, idx)) {
          if (idx < arrPtr->elements.size()) {
            arrPtr->elements[idx] = Value(Undefined{});
            // Mark this index as deleted so hasOwnProperty returns false
            arrPtr->properties["__deleted_" + propName + "__"] = Value(true);
          }
          // If this array is being used as a mapped arguments object, deleting an
          // element should also remove the mapping accessors.
          arrPtr->properties.erase(propName);
          arrPtr->properties.erase("__get_" + propName);
          arrPtr->properties.erase("__set_" + propName);
          arrPtr->properties.erase("__mapped_arg_index_" + propName + "__");
          arrPtr->properties.erase("__non_enum_" + propName);
          arrPtr->properties.erase("__non_writable_" + propName);
          arrPtr->properties.erase("__non_configurable_" + propName);
          arrPtr->properties.erase("__enum_" + propName);
          LIGHTJS_RETURN(Value(true));
        }
        arrPtr->properties.erase(propName);
        arrPtr->properties.erase("__get_" + propName);
        arrPtr->properties.erase("__set_" + propName);
        arrPtr->properties.erase("__mapped_arg_index_" + propName + "__");
        arrPtr->properties.erase("__non_enum_" + propName);
        arrPtr->properties.erase("__non_writable_" + propName);
        arrPtr->properties.erase("__non_configurable_" + propName);
        arrPtr->properties.erase("__enum_" + propName);
        LIGHTJS_RETURN(Value(true));
      }

      if (obj.isTypedArray()) {
        auto taPtr = obj.getGC<TypedArray>();
        if (taPtr->properties.count("__non_configurable_" + propName)) {
          LIGHTJS_RETURN(deleteFailed());
        }
        taPtr->properties.erase(propName);
        taPtr->properties.erase("__get_" + propName);
        taPtr->properties.erase("__set_" + propName);
        taPtr->properties.erase("__non_enum_" + propName);
        taPtr->properties.erase("__non_writable_" + propName);
        taPtr->properties.erase("__non_configurable_" + propName);
        taPtr->properties.erase("__enum_" + propName);
        LIGHTJS_RETURN(Value(true));
      }

      if (obj.isArrayBuffer()) {
        auto bufferPtr = obj.getGC<ArrayBuffer>();
        if (bufferPtr->properties.count("__non_configurable_" + propName)) {
          LIGHTJS_RETURN(deleteFailed());
        }
        bufferPtr->properties.erase(propName);
        bufferPtr->properties.erase("__get_" + propName);
        bufferPtr->properties.erase("__set_" + propName);
        bufferPtr->properties.erase("__non_enum_" + propName);
        bufferPtr->properties.erase("__non_writable_" + propName);
        bufferPtr->properties.erase("__non_configurable_" + propName);
        bufferPtr->properties.erase("__enum_" + propName);
        LIGHTJS_RETURN(Value(true));
      }

      if (obj.isDataView()) {
        auto viewPtr = obj.getGC<DataView>();
        if (viewPtr->properties.count("__non_configurable_" + propName)) {
          LIGHTJS_RETURN(deleteFailed());
        }
        viewPtr->properties.erase(propName);
        viewPtr->properties.erase("__get_" + propName);
        viewPtr->properties.erase("__set_" + propName);
        viewPtr->properties.erase("__non_enum_" + propName);
        viewPtr->properties.erase("__non_writable_" + propName);
        viewPtr->properties.erase("__non_configurable_" + propName);
        viewPtr->properties.erase("__enum_" + propName);
        LIGHTJS_RETURN(Value(true));
      }

      if (obj.isPromise()) {
        auto promisePtr = obj.getGC<Promise>();
        promisePtr->properties.erase(propName);
        promisePtr->properties.erase("__get_" + propName);
        promisePtr->properties.erase("__set_" + propName);
        LIGHTJS_RETURN(Value(true));
      }

      if (obj.isRegex()) {
        auto rxPtr = obj.getGC<Regex>();
        if (rxPtr->properties.count("__non_configurable_" + propName)) {
          LIGHTJS_RETURN(deleteFailed());
        }
        rxPtr->properties.erase(propName);
        rxPtr->properties.erase("__get_" + propName);
        rxPtr->properties.erase("__set_" + propName);
        rxPtr->properties.erase("__non_enum_" + propName);
        rxPtr->properties.erase("__non_writable_" + propName);
        rxPtr->properties.erase("__non_configurable_" + propName);
        rxPtr->properties.erase("__enum_" + propName);
        LIGHTJS_RETURN(Value(true));
      }

      if (obj.isError()) {
        auto errPtr = obj.getGC<Error>();
        if (errPtr->properties.count("__non_configurable_" + propName)) {
          LIGHTJS_RETURN(deleteFailed());
        }
        errPtr->properties.erase(propName);
        errPtr->properties.erase("__get_" + propName);
        errPtr->properties.erase("__set_" + propName);
        errPtr->properties.erase("__non_enum_" + propName);
        errPtr->properties.erase("__non_writable_" + propName);
        errPtr->properties.erase("__non_configurable_" + propName);
        errPtr->properties.erase("__enum_" + propName);
        if (propName == "message") {
          errPtr->message = "";
        }
        LIGHTJS_RETURN(Value(true));
      }

      // Delete on class properties
      if (obj.isClass()) {
        auto clsPtr = obj.getGC<Class>();
        if (clsPtr->properties.count("__non_configurable_" + propName)) {
          LIGHTJS_RETURN(deleteFailed());
        }
        clsPtr->properties.erase(propName);
        clsPtr->properties.erase("__non_writable_" + propName);
        clsPtr->properties.erase("__non_enum_" + propName);
        clsPtr->properties.erase("__enum_" + propName);
        LIGHTJS_RETURN(Value(true));
      }

      // delete on property references of primitives returns true (it has no effect).
      LIGHTJS_RETURN(Value(true));
    }

    // delete on identifier
    if (std::holds_alternative<Identifier>(expr.argument->node)) {
      auto& id = std::get<Identifier>(expr.argument->node);
      if (env_->hasLocal("__eval_deletable_bindings__") && env_->deleteLocalMutable(id.name)) {
        LIGHTJS_RETURN(Value(true));
      }
      // Check with-scope objects first (delete p3 inside with(myObj) deletes myObj.p3)
      int withResult = env_->deleteFromWithScope(id.name);
      if (withResult == 1) {
        LIGHTJS_RETURN(Value(true));   // deleted from with-scope object
      }
      if (withResult == -1) {
        LIGHTJS_RETURN(Value(false));  // non-configurable with-scope property
      }
      // In non-strict mode, delete on declared variables returns false.
      // However, unresolvable references may be global object properties
      // (created via sloppy assignment), which are deletable.
      if (strictMode_) {
        LIGHTJS_RETURN(Value(false));
      }
      Environment* bindingEnv = env_->resolveBindingEnvironment(id.name);
      if (bindingEnv) {
        LIGHTJS_RETURN(Value(false));
      }
      if (auto globalObj = env_->getRoot()->getGlobal()) {
        if (globalObj->properties.count("__non_configurable_" + id.name)) {
          LIGHTJS_RETURN(Value(false));
        }
        globalObj->properties.erase(id.name);
        globalObj->properties.erase("__get_" + id.name);
        globalObj->properties.erase("__set_" + id.name);
        LIGHTJS_RETURN(Value(true));  // Unresolvable references delete to true.
      }
      LIGHTJS_RETURN(Value(true));
    }

    // Non-reference: still evaluate operand for side effects (and propagate errors),
    // then return true.
    {
      auto sideEffectTask = evaluate(*expr.argument);
      Value ignored;
      LIGHTJS_RUN_TASK(sideEffectTask, ignored);
      if (flow_.type != ControlFlow::Type::None || hasError()) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
    }
    LIGHTJS_RETURN(Value(true));
  }

  // For typeof, handle undeclared identifiers specially (return "undefined" instead of throwing)
  if (expr.op == UnaryExpr::Op::Typeof) {
    if (auto* id = std::get_if<Identifier>(&expr.argument->node)) {
      if (env_->isTDZ(id->name)) {
        throwError(ErrorType::ReferenceError,
                   formatError("Cannot access '" + id->name + "' before initialization", expr.argument->loc));
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (!env_->has(id->name)) {
        // Global object accessor/data properties are resolvable references even
        // when no declarative binding exists.
        bool isResolvableGlobal = false;
        if (auto globalObj = env_->getRoot()->getGlobal()) {
          isResolvableGlobal =
            globalObj->properties.count(id->name) > 0 ||
            globalObj->properties.count("__get_" + id->name) > 0 ||
            globalObj->properties.count("__set_" + id->name) > 0;
        }
        if (!isResolvableGlobal) {
          // Unresolvable reference: typeof returns "undefined"
          LIGHTJS_RETURN(Value("undefined"));
        }
      }
    }
  }

  // For other unary operators, evaluate the argument first
  auto argTask = evaluate(*expr.argument);
  Value arg;
  LIGHTJS_RUN_TASK(argTask, arg);

  switch (expr.op) {
    case UnaryExpr::Op::Not:
      LIGHTJS_RETURN(Value(!arg.toBool()));
    case UnaryExpr::Op::Minus: {
      Value prim = isObjectLike(arg) ? toPrimitiveValue(arg, false) : arg;
      if (hasError()) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (prim.isBigInt()) {
        LIGHTJS_RETURN(Value(BigInt(-prim.toBigInt())));
      }
      if (prim.isSymbol()) {
        throwError(ErrorType::TypeError, "Cannot convert Symbol to number");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(-prim.toNumber()));
    }
    case UnaryExpr::Op::Plus: {
      Value prim = isObjectLike(arg) ? toPrimitiveValue(arg, false) : arg;
      if (hasError()) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (prim.isBigInt()) {
        throwError(ErrorType::TypeError, "Cannot convert BigInt value to number");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (prim.isSymbol()) {
        throwError(ErrorType::TypeError, "Cannot convert Symbol to number");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(prim.toNumber()));
    }
    case UnaryExpr::Op::Typeof: {
      if (arg.isUndefined()) LIGHTJS_RETURN(Value("undefined"));
      if (arg.isNull()) LIGHTJS_RETURN(Value("object"));
      if (arg.isBool()) LIGHTJS_RETURN(Value("boolean"));
      if (arg.isNumber()) LIGHTJS_RETURN(Value("number"));
      if (arg.isBigInt()) LIGHTJS_RETURN(Value("bigint"));
      if (arg.isSymbol()) LIGHTJS_RETURN(Value("symbol"));
      if (arg.isString()) LIGHTJS_RETURN(Value("string"));
      if (arg.isFunction()) LIGHTJS_RETURN(Value("function"));
      if (arg.isClass()) LIGHTJS_RETURN(Value("function"));
      if (arg.isProxy()) {
        auto proxy = arg.getGC<Proxy>();
        if (proxy && proxy->isCallable) {
          LIGHTJS_RETURN(Value("function"));
        }
        LIGHTJS_RETURN(Value("object"));
      }
      // Check for callable objects (e.g., String, Object constructors)
      if (arg.isObject()) {
        auto obj = arg.getGC<Object>();
        auto it = obj->properties.find("__callable_object__");
        if (it != obj->properties.end() && it->second.isBool() && it->second.toBool()) {
          LIGHTJS_RETURN(Value("function"));
        }
      }
      LIGHTJS_RETURN(Value("object"));
    }
    case UnaryExpr::Op::Void:
      LIGHTJS_RETURN(Value(Undefined{}));
    case UnaryExpr::Op::BitNot: {
      Value prim = isObjectLike(arg) ? toPrimitiveValue(arg, false) : arg;
      if (hasError()) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (prim.isBigInt()) {
        LIGHTJS_RETURN(Value(BigInt(~prim.toBigInt())));
      }
      if (prim.isSymbol()) {
        throwError(ErrorType::TypeError, "Cannot convert Symbol to number");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      int32_t number = toInt32(prim.toNumber());
      LIGHTJS_RETURN(Value(static_cast<double>(~number)));
    }
    case UnaryExpr::Op::Delete:
      // Already handled above
      break;
  }

  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::evaluateAssignment(const AssignmentExpr& expr) {
  // For logical assignment operators, evaluate left first and potentially short-circuit
  if (expr.op == AssignmentExpr::Op::AndAssign ||
      expr.op == AssignmentExpr::Op::OrAssign ||
      expr.op == AssignmentExpr::Op::NullishAssign) {

    if (auto* id = std::get_if<Identifier>(&expr.left->node)) {
      if (auto current = env_->get(id->name)) {
        // Short-circuit evaluation
        bool shouldAssign = false;
        if (expr.op == AssignmentExpr::Op::AndAssign) {
          shouldAssign = current->toBool();  // Only assign if left is truthy
        } else if (expr.op == AssignmentExpr::Op::OrAssign) {
          shouldAssign = !current->toBool();  // Only assign if left is falsy
        } else if (expr.op == AssignmentExpr::Op::NullishAssign) {
          shouldAssign = (current->isNull() || current->isUndefined());  // Only assign if left is nullish
        }

        if (shouldAssign) {
          auto rightTask = evaluate(*expr.right);
  Value right;
  LIGHTJS_RUN_TASK(rightTask, right);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
          // Named evaluation only when LHS is IdentifierReference (not parenthesized).
          if (!expr.left->parenthesized && right.isFunction()) {
            auto fn = right.getGC<Function>();
            auto nameIt = fn->properties.find("name");
            if (nameIt != fn->properties.end() && nameIt->second.isString() && nameIt->second.toString().empty()) {
              fn->properties["name"] = Value(id->name);
              fn->properties["__non_writable_name"] = Value(true);
              fn->properties["__non_enum_name"] = Value(true);
            }
          } else if (!expr.left->parenthesized && right.isClass()) {
            auto cls = right.getGC<Class>();
            auto nameIt = cls->properties.find("name");
            bool shouldSet = nameIt == cls->properties.end() ||
                             (nameIt->second.isString() && nameIt->second.toString().empty());
            if (shouldSet) {
              cls->name = id->name;
              cls->properties["name"] = Value(id->name);
              cls->properties["__non_writable_name"] = Value(true);
              cls->properties["__non_enum_name"] = Value(true);
            }
          }
          if (!env_->set(id->name, right)) {
            if (strictMode_) {
              throwError(ErrorType::ReferenceError, "'" + id->name + "' is not defined");
              LIGHTJS_RETURN(Value(Undefined{}));
            }
            auto globalObj = env_->getRoot()->getGlobal();
            if (globalObj) {
              globalObj->properties[id->name] = right;
            }
          }
          LIGHTJS_RETURN(right);
        } else {
          LIGHTJS_RETURN(*current);
        }
      } else {
        // Unresolved LHS: throw ReferenceError
        throwError(ErrorType::ReferenceError, "'" + id->name + "' is not defined");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
    }

    // Logical assignment on member expressions - short-circuit
    if (auto* member = std::get_if<MemberExpr>(&expr.left->node)) {
      auto objTask = evaluate(*member->object);
      Value obj;
      LIGHTJS_RUN_TASK(objTask, obj);
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));

      // Throw TypeError for null/undefined base before evaluating property key
      if (obj.isNull() || obj.isUndefined()) {
        if (member->computed) {
          // Evaluate computed property key first (spec: LHS before RHS)
          auto propTask = evaluate(*member->property);
          LIGHTJS_RUN_TASK_VOID(propTask);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
        }
        throwError(ErrorType::TypeError, "Cannot read properties of " + std::string(obj.isNull() ? "null" : "undefined"));
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      std::string propName;
      if (member->computed) {
        auto propTask = evaluate(*member->property);
        LIGHTJS_RUN_TASK_VOID(propTask);
        if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
        propName = toPropertyKeyString(propTask.result());
      } else {
        if (auto* id = std::get_if<Identifier>(&member->property->node)) {
          propName = id->name;
        }
      }

      // Handle private field logical assignment (#name)
      if (member->privateIdentifier) {
        if (!activePrivateOwnerClass_) {
          throwError(ErrorType::TypeError,
                     "Cannot read private member " + propName +
                         " from an object whose class did not declare it");
          LIGHTJS_RETURN(Value(Undefined{}));
        }

        Value privateReceiver = obj;
        int proxyDepth = 0;
        while (obj.isProxy() && proxyDepth < 16) {
          auto proxyPtr = obj.getGC<Proxy>();
          if (!proxyPtr->target) {
            throwError(ErrorType::TypeError,
                       "Cannot read private member " + propName +
                           " from an object whose class did not declare it");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          obj = *proxyPtr->target;
          proxyDepth++;
        }
        if (obj.isProxy()) {
          throwError(ErrorType::TypeError,
                     "Cannot read private member " + propName +
                         " from an object whose class did not declare it");
          LIGHTJS_RETURN(Value(Undefined{}));
        }

        PrivateNameOwner resolved = resolvePrivateNameOwnerClass(activePrivateOwnerClass_, propName);
        if (!resolved.owner || resolved.kind == PrivateNameKind::None) {
          throwError(ErrorType::TypeError,
                     "Cannot read private member " + propName +
                         " from an object whose class did not declare it");
          LIGHTJS_RETURN(Value(Undefined{}));
        }

        Value current(Undefined{});
        bool isMethod = false;
        bool hasPrivateGetter = false;
        bool hasPrivateSetter = false;
        bool hasValue = false;
        std::string mangledName = privateStorageKey(resolved.owner, propName);

        if (resolved.kind == PrivateNameKind::Static) {
          if (!obj.isClass() || obj.getGC<Class>().get() != resolved.owner.get()) {
            throwError(ErrorType::TypeError,
                       "Cannot read private member " + propName +
                           " from an object whose class did not declare it");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          auto clsPtr = obj.getGC<Class>();
          auto getterIt = clsPtr->properties.find("__get_" + mangledName);
          if (getterIt != clsPtr->properties.end() && getterIt->second.isFunction()) {
            hasPrivateGetter = true;
            current = callFunction(getterIt->second, {}, privateReceiver);
            if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
            hasValue = true;
          }
          auto setterIt = clsPtr->properties.find("__set_" + mangledName);
          if (setterIt != clsPtr->properties.end() && setterIt->second.isFunction()) {
            hasPrivateSetter = true;
          }
          auto methodIt = clsPtr->staticMethods.find(mangledName);
          if (methodIt != clsPtr->staticMethods.end()) {
            isMethod = true;
            current = Value(methodIt->second);
            hasValue = true;
          }
          if (!hasPrivateGetter && !isMethod) {
            auto it = clsPtr->properties.find(mangledName);
            if (it != clsPtr->properties.end()) {
              current = it->second;
              hasValue = true;
            }
          }
        } else {
          if (!isObjectLike(obj)) {
            throwError(ErrorType::TypeError,
                       "Cannot read private member " + propName +
                           " from an object whose class did not declare it");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          GCPtr<Class> targetClass = getConstructorClassForPrivateAccess(obj);
          if (!targetClass || !isOwnerInClassChain(targetClass, resolved.owner)) {
            throwError(ErrorType::TypeError,
                       "Cannot read private member " + propName +
                           " from an object whose class did not declare it");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          auto getterIt = resolved.owner->getters.find(propName);
          if (getterIt != resolved.owner->getters.end()) {
            hasPrivateGetter = true;
            current = invokeFunction(getterIt->second, {}, privateReceiver);
            if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
            hasValue = true;
          }
          auto setterIt = resolved.owner->setters.find(propName);
          if (setterIt != resolved.owner->setters.end()) {
            hasPrivateSetter = true;
          }
          auto methodIt = resolved.owner->methods.find(propName);
          if (methodIt != resolved.owner->methods.end()) {
            isMethod = true;
            current = Value(methodIt->second);
            hasValue = true;
          }
          if (!hasPrivateGetter && !isMethod) {
            if (auto* storage = getPropertyStorageForPrivateAccess(obj)) {
              auto it = storage->find(mangledName);
              if (it != storage->end()) {
                current = it->second;
                hasValue = true;
              }
            }
          }
        }

        if (!hasValue) {
          throwError(ErrorType::TypeError,
                     "Cannot read private member " + propName +
                         " from an object whose class did not declare it");
          LIGHTJS_RETURN(Value(Undefined{}));
        }

        bool shouldAssign = false;
        if (expr.op == AssignmentExpr::Op::AndAssign) {
          shouldAssign = current.toBool();
        } else if (expr.op == AssignmentExpr::Op::OrAssign) {
          shouldAssign = !current.toBool();
        } else if (expr.op == AssignmentExpr::Op::NullishAssign) {
          shouldAssign = (current.isNull() || current.isUndefined());
        }

        if (!shouldAssign) {
          LIGHTJS_RETURN(current);
        }

        if (isMethod) {
          auto rightTask2 = evaluate(*expr.right);
          LIGHTJS_RUN_TASK_VOID(rightTask2);
          throwError(ErrorType::TypeError, "Cannot assign to private method " + propName);
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (hasPrivateGetter && !hasPrivateSetter) {
          auto rightTask2 = evaluate(*expr.right);
          LIGHTJS_RUN_TASK_VOID(rightTask2);
          throwError(ErrorType::TypeError, "Cannot set property " + propName + " which has only a getter");
          LIGHTJS_RETURN(Value(Undefined{}));
        }

        auto rightTask2 = evaluate(*expr.right);
        Value right2;
        LIGHTJS_RUN_TASK(rightTask2, right2);
        if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));

        if (resolved.kind == PrivateNameKind::Static) {
          auto clsPtr = obj.getGC<Class>();
          if (hasPrivateSetter) {
            auto setterIt = clsPtr->properties.find("__set_" + mangledName);
            if (setterIt != clsPtr->properties.end()) {
              callFunction(setterIt->second, {right2}, privateReceiver);
              if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
            }
          } else {
            clsPtr->properties[mangledName] = right2;
          }
        } else {
          if (hasPrivateSetter) {
            auto setterIt = resolved.owner->setters.find(propName);
            if (setterIt != resolved.owner->setters.end()) {
              invokeFunction(setterIt->second, {right2}, privateReceiver);
              if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
            }
          } else {
            auto* storage = getPropertyStorageForPrivateAccess(obj);
            if (!storage) {
              throwError(ErrorType::TypeError,
                         "Cannot read private member " + propName +
                             " from an object whose class did not declare it");
              LIGHTJS_RETURN(Value(Undefined{}));
            }
            (*storage)[mangledName] = right2;
          }
        }
        LIGHTJS_RETURN(right2);
      }

      // Get current value (with getter support)
      Value current(Undefined{});
      bool hasGetter = false;
      bool hasSetter = true; // assume settable by default
      bool isWritable = true;
      bool isExtensible = true;
      bool propExists = false;
      if (obj.isObject()) {
        auto objPtr = obj.getGC<Object>();
        // Check for getter
        auto getterIt = objPtr->properties.find("__get_" + propName);
        if (getterIt != objPtr->properties.end() && getterIt->second.isFunction()) {
          hasGetter = true;
          current = callFunction(getterIt->second, {}, obj);
          propExists = true;
        } else {
          auto it = objPtr->properties.find(propName);
          if (it != objPtr->properties.end()) {
            current = it->second;
            propExists = true;
          }
        }
        // Check setter status
        auto setterIt = objPtr->properties.find("__set_" + propName);
        if (hasGetter && (setterIt == objPtr->properties.end() ||
            !setterIt->second.isFunction())) {
          // Getter exists but no callable setter - assignment will fail
          hasSetter = false;
        }
        // Check non-writable marker
        auto nwIt = objPtr->properties.find("__non_writable_" + propName);
        if (nwIt != objPtr->properties.end() && nwIt->second.toBool()) {
          isWritable = false;
        }
        // Check extensible
        if (objPtr->sealed || objPtr->frozen || objPtr->nonExtensible) {
          isExtensible = false;
        }
      } else if (obj.isFunction()) {
        auto fnPtr = obj.getGC<Function>();
        auto it = fnPtr->properties.find(propName);
        if (it != fnPtr->properties.end()) {
          current = it->second;
          propExists = true;
        }
      } else if (obj.isArray()) {
        auto arrPtr = obj.getGC<Array>();
        auto getterIt = arrPtr->properties.find("__get_" + propName);
        if (getterIt != arrPtr->properties.end() && getterIt->second.isFunction()) {
          current = callFunction(getterIt->second, {}, obj);
          propExists = true;
        }
        size_t idx = 0;
        bool isIdx = false;
        try { idx = std::stoull(propName); isIdx = true; } catch (...) {}
        if (!propExists && isIdx && idx < arrPtr->elements.size()) {
          current = arrPtr->elements[idx];
          propExists = true;
        }
      }

      // Short-circuit check
      bool shouldAssign = false;
      if (expr.op == AssignmentExpr::Op::AndAssign) {
        shouldAssign = current.toBool();
      } else if (expr.op == AssignmentExpr::Op::OrAssign) {
        shouldAssign = !current.toBool();
      } else if (expr.op == AssignmentExpr::Op::NullishAssign) {
        shouldAssign = (current.isNull() || current.isUndefined());
      }

      if (!shouldAssign) {
        LIGHTJS_RETURN(current);
      }

      // Check if assignment is allowed before evaluating RHS
      if (!isWritable && propExists) {
        // Evaluate RHS first (spec requires it)
        auto rightTask2 = evaluate(*expr.right);
        LIGHTJS_RUN_TASK_VOID(rightTask2);
        throwError(ErrorType::TypeError, "Cannot assign to read only property '" + propName + "'");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (!hasSetter && propExists) {
        auto rightTask2 = evaluate(*expr.right);
        LIGHTJS_RUN_TASK_VOID(rightTask2);
        throwError(ErrorType::TypeError, "Cannot set property " + propName + " which has only a getter");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (!isExtensible && !propExists) {
        auto rightTask2 = evaluate(*expr.right);
        LIGHTJS_RUN_TASK_VOID(rightTask2);
        throwError(ErrorType::TypeError, "Cannot add property " + propName + ", object is not extensible");
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      // Evaluate RHS only if we need to assign
      auto rightTask2 = evaluate(*expr.right);
      Value right2;
      LIGHTJS_RUN_TASK(rightTask2, right2);
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));

      // Assign (with setter support)
      if (obj.isObject()) {
        auto objPtr = obj.getGC<Object>();
        auto setterIt = objPtr->properties.find("__set_" + propName);
        if (setterIt != objPtr->properties.end() && setterIt->second.isFunction()) {
          callFunction(setterIt->second, {right2}, obj);
        } else {
          objPtr->properties[propName] = right2;
        }
      } else if (obj.isFunction()) {
        auto fnPtr = obj.getGC<Function>();
        fnPtr->properties[propName] = right2;
      } else if (obj.isArray()) {
        auto arrPtr = obj.getGC<Array>();
        auto setterIt = arrPtr->properties.find("__set_" + propName);
        if (setterIt != arrPtr->properties.end() && setterIt->second.isFunction()) {
          callFunction(setterIt->second, {right2}, obj);
        } else {
          size_t idx = 0;
          try { idx = std::stoull(propName); } catch (...) {}
          if (idx < arrPtr->elements.size()) arrPtr->elements[idx] = right2;
        }
      }
      LIGHTJS_RETURN(right2);
    }
  }

  auto computeCompoundValue = [&](AssignmentExpr::Op op,
                                  const Value& current,
                                  const Value& rhsValue,
                                  Value& result) -> bool {
    auto toNumericOperand = [&](const Value& operand, Value& out) -> bool {
      out = isObjectLike(operand) ? toPrimitiveValue(operand, false) : operand;
      if (hasError()) return false;
      if (out.isSymbol()) {
        throwError(ErrorType::TypeError, "Cannot convert Symbol to number");
        return false;
      }
      return true;
    };

    switch (op) {
      case AssignmentExpr::Op::AddAssign: {
        Value lhs = isObjectLike(current) ? toPrimitiveValue(current, false, true) : current;
        Value rhs = isObjectLike(rhsValue) ? toPrimitiveValue(rhsValue, false, true) : rhsValue;
        if (hasError()) return false;
        if (lhs.isSymbol() || rhs.isSymbol()) {
          throwError(ErrorType::TypeError, "Cannot convert Symbol to string");
          return false;
        }
        if (lhs.isString() || rhs.isString()) {
          result = Value(lhs.toString() + rhs.toString());
        } else if (lhs.isBigInt() && rhs.isBigInt()) {
          result = Value(BigInt(lhs.toBigInt() + rhs.toBigInt()));
        } else if (lhs.isBigInt() != rhs.isBigInt()) {
          throwError(ErrorType::TypeError, "Cannot mix BigInt and other types");
          return false;
        } else {
          result = Value(lhs.toNumber() + rhs.toNumber());
        }
        return true;
      }
      case AssignmentExpr::Op::SubAssign:
      case AssignmentExpr::Op::MulAssign:
      case AssignmentExpr::Op::DivAssign:
      case AssignmentExpr::Op::ModAssign:
      case AssignmentExpr::Op::ExpAssign:
      case AssignmentExpr::Op::BitwiseAndAssign:
      case AssignmentExpr::Op::BitwiseOrAssign:
      case AssignmentExpr::Op::BitwiseXorAssign:
      case AssignmentExpr::Op::LeftShiftAssign:
      case AssignmentExpr::Op::RightShiftAssign:
      case AssignmentExpr::Op::UnsignedRightShiftAssign: {
        Value lhs;
        Value rhs;
        if (!toNumericOperand(current, lhs) || !toNumericOperand(rhsValue, rhs)) {
          return false;
        }
        switch (op) {
          case AssignmentExpr::Op::SubAssign:
            if (lhs.isBigInt() && rhs.isBigInt()) {
              result = Value(BigInt(lhs.toBigInt() - rhs.toBigInt()));
            } else if (lhs.isBigInt() != rhs.isBigInt()) {
              throwError(ErrorType::TypeError, "Cannot mix BigInt and other types");
              return false;
            } else {
              result = Value(lhs.toNumber() - rhs.toNumber());
            }
            return true;
          case AssignmentExpr::Op::MulAssign:
            if (lhs.isBigInt() && rhs.isBigInt()) {
              result = Value(BigInt(lhs.toBigInt() * rhs.toBigInt()));
            } else if (lhs.isBigInt() != rhs.isBigInt()) {
              throwError(ErrorType::TypeError, "Cannot mix BigInt and other types");
              return false;
            } else {
              result = Value(lhs.toNumber() * rhs.toNumber());
            }
            return true;
          case AssignmentExpr::Op::DivAssign:
            if (lhs.isBigInt() && rhs.isBigInt()) {
              auto divisor = rhs.toBigInt();
              if (divisor == 0) {
                throwError(ErrorType::RangeError, "Division by zero");
                return false;
              }
              result = Value(BigInt(lhs.toBigInt() / divisor));
            } else if (lhs.isBigInt() != rhs.isBigInt()) {
              throwError(ErrorType::TypeError, "Cannot mix BigInt and other types");
              return false;
            } else {
              result = Value(lhs.toNumber() / rhs.toNumber());
            }
            return true;
          case AssignmentExpr::Op::ModAssign:
            if (lhs.isBigInt() && rhs.isBigInt()) {
              auto divisor = rhs.toBigInt();
              if (divisor == 0) {
                throwError(ErrorType::RangeError, "Division by zero");
                return false;
              }
              result = Value(BigInt(lhs.toBigInt() % divisor));
            } else if (lhs.isBigInt() != rhs.isBigInt()) {
              throwError(ErrorType::TypeError, "Cannot mix BigInt and other types");
              return false;
            } else {
              result = Value(std::fmod(lhs.toNumber(), rhs.toNumber()));
            }
            return true;
          case AssignmentExpr::Op::ExpAssign:
            if (lhs.isBigInt() && rhs.isBigInt()) {
              auto base = lhs.toBigInt();
              auto exp = rhs.toBigInt();
              if (exp < 0) {
                throwError(ErrorType::RangeError, "BigInt negative exponent");
                return false;
              }
              result = Value(BigInt(powBigInt(base, exp)));
            } else if (lhs.isBigInt() != rhs.isBigInt()) {
              throwError(ErrorType::TypeError, "Cannot mix BigInt and other types");
              return false;
            } else {
              result = Value(std::pow(lhs.toNumber(), rhs.toNumber()));
            }
            return true;
          case AssignmentExpr::Op::BitwiseAndAssign:
            if (lhs.isBigInt() && rhs.isBigInt()) {
              result = Value(BigInt(lhs.toBigInt() & rhs.toBigInt()));
            } else if (lhs.isBigInt() != rhs.isBigInt()) {
              throwError(ErrorType::TypeError, "Cannot mix BigInt and other types in bitwise operations");
              return false;
            } else {
              result = Value(static_cast<double>(toInt32(lhs.toNumber()) & toInt32(rhs.toNumber())));
            }
            return true;
          case AssignmentExpr::Op::BitwiseOrAssign:
            if (lhs.isBigInt() && rhs.isBigInt()) {
              result = Value(BigInt(lhs.toBigInt() | rhs.toBigInt()));
            } else if (lhs.isBigInt() != rhs.isBigInt()) {
              throwError(ErrorType::TypeError, "Cannot mix BigInt and other types in bitwise operations");
              return false;
            } else {
              result = Value(static_cast<double>(toInt32(lhs.toNumber()) | toInt32(rhs.toNumber())));
            }
            return true;
          case AssignmentExpr::Op::BitwiseXorAssign:
            if (lhs.isBigInt() && rhs.isBigInt()) {
              result = Value(BigInt(lhs.toBigInt() ^ rhs.toBigInt()));
            } else if (lhs.isBigInt() != rhs.isBigInt()) {
              throwError(ErrorType::TypeError, "Cannot mix BigInt and other types in bitwise operations");
              return false;
            } else {
              result = Value(static_cast<double>(toInt32(lhs.toNumber()) ^ toInt32(rhs.toNumber())));
            }
            return true;
          case AssignmentExpr::Op::LeftShiftAssign:
            if (lhs.isBigInt() && rhs.isBigInt()) {
              bool ok = false;
              auto shifted = applyBigIntShiftLeft(lhs.toBigInt(), rhs.toBigInt(), &ok);
              if (!ok) {
                throwError(ErrorType::RangeError, "Invalid BigInt shift count");
                return false;
              }
              result = Value(BigInt(shifted));
            } else if (lhs.isBigInt() != rhs.isBigInt()) {
              throwError(ErrorType::TypeError, "Cannot mix BigInt and other types in bitwise operations");
              return false;
            } else {
              result = Value(static_cast<double>(toInt32(lhs.toNumber()) << (toInt32(rhs.toNumber()) & 0x1f)));
            }
            return true;
          case AssignmentExpr::Op::RightShiftAssign:
            if (lhs.isBigInt() && rhs.isBigInt()) {
              bool ok = false;
              auto shifted = applyBigIntShiftRight(lhs.toBigInt(), rhs.toBigInt(), &ok);
              if (!ok) {
                throwError(ErrorType::RangeError, "Invalid BigInt shift count");
                return false;
              }
              result = Value(BigInt(shifted));
            } else if (lhs.isBigInt() != rhs.isBigInt()) {
              throwError(ErrorType::TypeError, "Cannot mix BigInt and other types in bitwise operations");
              return false;
            } else {
              result = Value(static_cast<double>(toInt32(lhs.toNumber()) >> (toInt32(rhs.toNumber()) & 0x1f)));
            }
            return true;
          case AssignmentExpr::Op::UnsignedRightShiftAssign:
            if (lhs.isBigInt() || rhs.isBigInt()) {
              throwError(ErrorType::TypeError, "Cannot use unsigned right shift on BigInt");
              return false;
            }
            result = Value(static_cast<double>(static_cast<uint32_t>(toInt32(lhs.toNumber())) >> (toInt32(rhs.toNumber()) & 0x1f)));
            return true;
          default:
            break;
        }
        break;
      }
      default:
        break;
    }

    result = rhsValue;
    return true;
  };

  if (auto* id = std::get_if<Identifier>(&expr.left->node)) {
    if (env_->isTDZ(id->name)) {
      throwError(ErrorType::ReferenceError,
                 "Cannot access '" + id->name + "' before initialization");
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    auto withScopeTarget = env_->resolveWithScopeValue(id->name);
    Environment* bindingTargetEnv = withScopeTarget ? nullptr : env_->resolveBindingEnvironment(id->name);
    GCPtr<Object> globalPropertyTarget;
    bool isResolvable = withScopeTarget.has_value() || bindingTargetEnv != nullptr;
    if (!withScopeTarget && !bindingTargetEnv) {
      auto rootEnv = env_->getRoot();
      auto globalThisVal = rootEnv->get("globalThis");
      if (globalThisVal && globalThisVal->isObject()) {
        auto globalObj = globalThisVal->getGC<Object>();
        if (globalObj->properties.count(id->name) > 0 ||
            globalObj->properties.count("__get_" + id->name) > 0 ||
            globalObj->properties.count("__set_" + id->name) > 0) {
          globalPropertyTarget = globalObj;
          isResolvable = true;
        }
      }
    }

    auto putIdentifierValue = [&](const Value& value, bool allowCreateGlobal) -> bool {
      if (withScopeTarget) {
        if (env_->setWithScopeBindingValue(*withScopeTarget, id->name, value, strictMode_)) {
          return true;
        }
        if (!hasError() && strictMode_) {
          throwError(ErrorType::ReferenceError, "'" + id->name + "' is not defined");
        }
        return false;
      }
      if (bindingTargetEnv) {
        if (bindingTargetEnv->set(id->name, value)) {
          return true;
        }
        if (bindingTargetEnv->isConst(id->name)) {
          throwError(ErrorType::TypeError, "Assignment to constant variable '" + id->name + "'");
          return false;
        }
        if (strictMode_ && bindingTargetEnv->isSilentImmutable(id->name)) {
          throwError(ErrorType::TypeError, "Assignment to constant variable '" + id->name + "'");
          return false;
        }
      }
      if (globalPropertyTarget) {
        auto setterIt = globalPropertyTarget->properties.find("__set_" + id->name);
        bool hasSetter = setterIt != globalPropertyTarget->properties.end() && setterIt->second.isFunction();
        bool hasGetter = globalPropertyTarget->properties.count("__get_" + id->name) > 0;
        bool stillExists =
          globalPropertyTarget->properties.count(id->name) > 0 ||
          globalPropertyTarget->properties.count("__get_" + id->name) > 0 ||
          globalPropertyTarget->properties.count("__set_" + id->name) > 0;

        if (!stillExists && strictMode_) {
          throwError(ErrorType::ReferenceError, "'" + id->name + "' is not defined");
          return false;
        }
        if (globalPropertyTarget->properties.count("__non_writable_" + id->name) > 0) {
          if (strictMode_) {
            throwError(ErrorType::TypeError,
                       "Cannot assign to read only property '" + id->name + "'");
            return false;
          }
          return true;
        }
        if (hasGetter && !hasSetter) {
          if (strictMode_) {
            throwError(ErrorType::TypeError,
                       "Cannot set property " + id->name + " which has only a getter");
            return false;
          }
          return true;
        }
        if (hasSetter) {
          callFunction(setterIt->second, {value}, Value(globalPropertyTarget));
          return !hasError();
        }
        globalPropertyTarget->properties[id->name] = value;
        return true;
      }
      // In strict mode, if the reference was unresolvable when initially evaluated,
      // throw ReferenceError even if a later side effect made it resolvable
      if (strictMode_ && !isResolvable) {
        throwError(ErrorType::ReferenceError, "'" + id->name + "' is not defined");
        return false;
      }
      if (env_->set(id->name, value)) {
        return true;
      }
      if (!allowCreateGlobal || strictMode_) {
        throwError(ErrorType::ReferenceError, "'" + id->name + "' is not defined");
        return false;
      }
      if (auto globalObj = env_->getRoot()->getGlobal()) {
        globalObj->properties[id->name] = value;
        return true;
      }
      return true;
    };

    if (expr.op == AssignmentExpr::Op::Assign) {
      auto previousPendingAnonymousClassName = pendingAnonymousClassName_;
      struct PendingAnonymousClassNameGuard {
        Interpreter* interpreter;
        std::optional<std::string> previous;
        ~PendingAnonymousClassNameGuard() {
          interpreter->pendingAnonymousClassName_ = previous;
        }
      } pendingAnonymousClassNameGuard{this, previousPendingAnonymousClassName};
      if (auto* clsExpr = std::get_if<ClassExpr>(&expr.right->node);
          clsExpr && clsExpr->name.empty()) {
        pendingAnonymousClassName_ = id->name;
      }

      Value right;
      { auto task = evaluate(*expr.right); LIGHTJS_RUN_TASK(task, right); }
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));

      bool isAnonFnDef = false;
      if (auto* fnExpr = std::get_if<FunctionExpr>(&expr.right->node)) {
        isAnonFnDef = fnExpr->name.empty();
      } else if (auto* clsExpr = std::get_if<ClassExpr>(&expr.right->node)) {
        isAnonFnDef = clsExpr->name.empty();
      }
      if (!expr.left->parenthesized && isAnonFnDef && right.isFunction()) {
        auto fn = right.getGC<Function>();
        auto nameIt = fn->properties.find("name");
        if (nameIt != fn->properties.end() && nameIt->second.isString() &&
            nameIt->second.toString().empty()) {
          fn->properties["name"] = Value(id->name);
          fn->properties["__non_writable_name"] = Value(true);
          fn->properties["__non_enum_name"] = Value(true);
        }
      } else if (!expr.left->parenthesized && isAnonFnDef && right.isClass()) {
        auto cls = right.getGC<Class>();
        auto nameIt = cls->properties.find("name");
        bool shouldSet = nameIt == cls->properties.end() ||
                         (nameIt->second.isString() && nameIt->second.toString().empty());
        if (shouldSet) {
          cls->name = id->name;
          cls->properties["name"] = Value(id->name);
          cls->properties["__non_writable_name"] = Value(true);
          cls->properties["__non_enum_name"] = Value(true);
        }
      }

      if (!putIdentifierValue(right, true)) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(right);
    }

    Value current;
    if (withScopeTarget) {
      auto currentValue = env_->getWithScopeBindingValue(*withScopeTarget, id->name, strictMode_);
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      if (!currentValue.has_value()) {
        if (strictMode_) {
          throwError(ErrorType::ReferenceError, "'" + id->name + "' is not defined");
        }
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      current = *currentValue;
    } else {
      { auto task = evaluate(*expr.left); LIGHTJS_RUN_TASK(task, current); }
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
    }

    Value right;
    { auto task = evaluate(*expr.right); LIGHTJS_RUN_TASK(task, right); }
    if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));

    Value result;
    if (!computeCompoundValue(expr.op, current, right, result)) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (!putIdentifierValue(result, false)) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    LIGHTJS_RETURN(result);
  }

  auto rightTask = evaluate(*expr.right);
  Value right;
  LIGHTJS_RUN_TASK(rightTask, right);

  if (std::get_if<ArrayPattern>(&expr.left->node) ||
      std::get_if<ObjectPattern>(&expr.left->node) ||
      std::get_if<AssignmentPattern>(&expr.left->node)) {
    { auto t = bindDestructuringPattern(*expr.left, right, false, true); LIGHTJS_RUN_TASK_VOID(t); }
    LIGHTJS_RETURN(right);
  }

  if (auto* member = std::get_if<MemberExpr>(&expr.left->node)) {
    auto objTask = evaluate(*member->object);
    Value obj;
    LIGHTJS_RUN_TASK(objTask, obj);
    if (flow_.type != ControlFlow::Type::None || hasError()) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    bool isSuperTarget = member->object &&
                         std::holds_alternative<SuperExpr>(member->object->node);
    Value superReceiver = Value(Undefined{});
    if (isSuperTarget) {
      if (auto superCalled = env_->get("__super_called__");
          superCalled && superCalled->isBool() && !superCalled->toBool()) {
        throwError(ErrorType::ReferenceError,
                   "Must call super constructor in derived class before accessing 'this'");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (auto thisVal = env_->get("this")) {
        superReceiver = *thisVal;
      }
    }

    std::string propName;
    std::optional<size_t> typedArrayNumericIndex;
    if (member->computed) {
      auto propTask = evaluate(*member->property);
      LIGHTJS_RUN_TASK_VOID(propTask);
      Value keyValue = propTask.result();
      if (isObjectLike(keyValue)) {
        keyValue = toPrimitiveValue(keyValue, true);
        if (hasError()) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      }
      if (keyValue.isNumber()) {
        double numericKey = keyValue.toNumber();
        if (std::isfinite(numericKey) && numericKey >= 0.0 && numericKey <= 4294967294.0) {
          double integerKey = std::trunc(numericKey);
          if (integerKey == numericKey) {
            typedArrayNumericIndex = static_cast<size_t>(integerKey);
          }
        }
      }
      propName = toPropertyKeyString(keyValue);
    } else {
      if (auto* id = std::get_if<Identifier>(&member->property->node)) {
        propName = id->name;
      }
    }

    // PutValue on a property reference coerces the base with ToObject,
    // which throws on null/undefined. (RHS has already been evaluated above.)
    if (obj.isNull() || obj.isUndefined()) {
      throwError(ErrorType::TypeError,
                 "Cannot set properties of " + std::string(obj.isNull() ? "null" : "undefined"));
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    if (!member->privateIdentifier && expr.op == AssignmentExpr::Op::Assign &&
        typedArrayNumericIndex.has_value() && obj.isTypedArray()) {
      auto taPtr = obj.getGC<TypedArray>();
      size_t index = *typedArrayNumericIndex;
      if (taPtr->type == TypedArrayType::BigInt64 || taPtr->type == TypedArrayType::BigUint64) {
        if (taPtr->type == TypedArrayType::BigUint64) {
          taPtr->setBigUintElement(index, bigint::toUint64Trunc(right.toBigInt()));
        } else {
          taPtr->setBigIntElement(index, bigint::toInt64Trunc(right.toBigInt()));
        }
      } else {
        taPtr->setElement(index, right.toNumber());
      }
      LIGHTJS_RETURN(right);
    }

    if (member->privateIdentifier) {
      if (!activePrivateOwnerClass_) {
        throwError(ErrorType::TypeError,
                   "Cannot read private member " + propName +
                       " from an object whose class did not declare it");
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      Value privateReceiver = obj;
      int proxyDepth = 0;
      while (obj.isProxy() && proxyDepth < 16) {
        auto proxyPtr = obj.getGC<Proxy>();
        if (!proxyPtr->target) {
          throwError(ErrorType::TypeError,
                     "Cannot read private member " + propName +
                         " from an object whose class did not declare it");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        obj = *proxyPtr->target;
        proxyDepth++;
      }
      if (obj.isProxy()) {
        throwError(ErrorType::TypeError,
                   "Cannot read private member " + propName +
                       " from an object whose class did not declare it");
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      PrivateNameOwner resolved = resolvePrivateNameOwnerClass(activePrivateOwnerClass_, propName);
      if (!resolved.owner || resolved.kind == PrivateNameKind::None) {
        throwError(ErrorType::TypeError,
                   "Cannot read private member " + propName +
                       " from an object whose class did not declare it");
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      Value current(Undefined{});
      bool isMethod = false;
      bool hasPrivateGetter = false;
      bool hasPrivateSetter = false;
      bool hasFieldValue = false;
      std::string mangledName = privateStorageKey(resolved.owner, propName);

      if (resolved.kind == PrivateNameKind::Static) {
        if (!obj.isClass() || obj.getGC<Class>().get() != resolved.owner.get()) {
          throwError(ErrorType::TypeError,
                     "Cannot read private member " + propName +
                         " from an object whose class did not declare it");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        auto clsPtr = obj.getGC<Class>();
        auto getterIt = clsPtr->properties.find("__get_" + mangledName);
        if (getterIt != clsPtr->properties.end() && getterIt->second.isFunction()) {
          hasPrivateGetter = true;
        }
        auto setterIt = clsPtr->properties.find("__set_" + mangledName);
        if (setterIt != clsPtr->properties.end() && setterIt->second.isFunction()) {
          hasPrivateSetter = true;
        }
        auto methodIt = clsPtr->staticMethods.find(mangledName);
        if (methodIt != clsPtr->staticMethods.end()) {
          isMethod = true;
          current = Value(methodIt->second);
        }
        auto valueIt = clsPtr->properties.find(mangledName);
        if (valueIt != clsPtr->properties.end()) {
          hasFieldValue = true;
          current = valueIt->second;
        }

        if (expr.op != AssignmentExpr::Op::Assign) {
          if (hasPrivateGetter) {
            current = callFunction(getterIt->second, {}, privateReceiver);
            if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
          } else if (isMethod) {
            // method already assigned to current
          } else if (hasFieldValue) {
            // field value already assigned to current
          } else {
            throwError(ErrorType::TypeError,
                       "Cannot read private member " + propName +
                           " from an object whose class did not declare it");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
        }

        if (isMethod) {
          throwError(ErrorType::TypeError, "Cannot assign to private method " + propName);
          LIGHTJS_RETURN(Value(Undefined{}));
        }

        if (expr.op == AssignmentExpr::Op::Assign) {
          if (hasPrivateGetter && !hasPrivateSetter) {
            throwError(ErrorType::TypeError, "Cannot set property " + propName + " which has only a getter");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          if (hasPrivateSetter) {
            callFunction(setterIt->second, {right}, privateReceiver);
            if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
          } else {
            if (!hasFieldValue) {
              throwError(ErrorType::TypeError,
                         "Cannot write private member " + propName +
                             " to an object whose class did not declare it");
              LIGHTJS_RETURN(Value(Undefined{}));
            }
            clsPtr->properties[mangledName] = right;
          }
          LIGHTJS_RETURN(right);
        }

        Value result;
        if (!computeCompoundValue(expr.op, current, right, result)) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (hasPrivateGetter && !hasPrivateSetter) {
          throwError(ErrorType::TypeError, "Cannot set property " + propName + " which has only a getter");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (hasPrivateSetter) {
          callFunction(setterIt->second, {result}, privateReceiver);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
        } else {
          if (!hasFieldValue) {
            throwError(ErrorType::TypeError,
                       "Cannot write private member " + propName +
                           " to an object whose class did not declare it");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          clsPtr->properties[mangledName] = result;
        }
        LIGHTJS_RETURN(result);
      }

      if (!isObjectLike(obj)) {
        throwError(ErrorType::TypeError,
                   "Cannot read private member " + propName +
                       " from an object whose class did not declare it");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      GCPtr<Class> targetClass = getConstructorClassForPrivateAccess(obj);
      if (!targetClass || !isOwnerInClassChain(targetClass, resolved.owner)) {
        throwError(ErrorType::TypeError,
                   "Cannot read private member " + propName +
                       " from an object whose class did not declare it");
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      auto getterIt = resolved.owner->getters.find(propName);
      if (getterIt != resolved.owner->getters.end()) {
        hasPrivateGetter = true;
      }
      auto setterIt = resolved.owner->setters.find(propName);
      if (setterIt != resolved.owner->setters.end()) {
        hasPrivateSetter = true;
      }
      auto methodIt = resolved.owner->methods.find(propName);
      if (methodIt != resolved.owner->methods.end()) {
        isMethod = true;
        current = Value(methodIt->second);
      }

      auto* storage = getPropertyStorageForPrivateAccess(obj);
      if (storage) {
        auto valueIt = storage->find(mangledName);
        if (valueIt != storage->end()) {
          hasFieldValue = true;
          current = valueIt->second;
        }
      }

      if (expr.op != AssignmentExpr::Op::Assign) {
        if (hasPrivateGetter) {
          current = invokeFunction(getterIt->second, {}, privateReceiver);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
        } else if (isMethod) {
          // method already assigned to current
        } else if (hasFieldValue) {
          // field value already assigned to current
        } else {
          throwError(ErrorType::TypeError,
                     "Cannot read private member " + propName +
                         " from an object whose class did not declare it");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      }

      if (isMethod) {
        throwError(ErrorType::TypeError, "Cannot assign to private method " + propName);
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      if (expr.op == AssignmentExpr::Op::Assign) {
        if (hasPrivateGetter && !hasPrivateSetter) {
          throwError(ErrorType::TypeError, "Cannot set property " + propName + " which has only a getter");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (hasPrivateSetter) {
          invokeFunction(setterIt->second, {right}, privateReceiver);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
        } else {
          if (!storage || !hasFieldValue) {
            throwError(ErrorType::TypeError,
                       "Cannot write private member " + propName +
                           " to an object whose class did not declare it");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          (*storage)[mangledName] = right;
        }
        LIGHTJS_RETURN(right);
      }

      Value result;
      if (!computeCompoundValue(expr.op, current, right, result)) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (hasPrivateGetter && !hasPrivateSetter) {
        throwError(ErrorType::TypeError, "Cannot set property " + propName + " which has only a getter");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (hasPrivateSetter) {
        invokeFunction(setterIt->second, {result}, privateReceiver);
        if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      } else {
        if (!storage || !hasFieldValue) {
          throwError(ErrorType::TypeError,
                     "Cannot write private member " + propName +
                         " to an object whose class did not declare it");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        (*storage)[mangledName] = result;
      }
      LIGHTJS_RETURN(result);
    }

    bool usedPrimitiveWrapper = false;

    // PutValue on a primitive base: ToObject(base) then [[Set]].
    if (obj.isNumber() || obj.isString() || obj.isBool() || obj.isSymbol() || obj.isBigInt()) {
      const char* ctorName = nullptr;
      if (obj.isNumber()) ctorName = "Number";
      else if (obj.isString()) ctorName = "String";
      else if (obj.isBool()) ctorName = "Boolean";
      else if (obj.isSymbol()) ctorName = "Symbol";
      else if (obj.isBigInt()) ctorName = "BigInt";

      auto wrapper = GarbageCollector::makeGC<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      wrapper->properties["__primitive_value__"] = obj;
      if (ctorName) {
        if (auto ctor = env_->get(ctorName)) {
          auto [foundProto, proto] = getPropertyForPrimitive(*ctor, "prototype");
          if (foundProto && (proto.isObject() || proto.isNull())) {
            wrapper->properties["__proto__"] = proto;
          }
        }
      }
      obj = Value(wrapper);
      usedPrimitiveWrapper = true;
    }

    // Spec-ish [[Set]] behavior for plain assignment:
    // if no own property, delegate to prototype's [[Set]] (important for Proxy
    // prototypes used by Test262 PutValue tests).
    if (expr.op == AssignmentExpr::Op::Assign && usedPrimitiveWrapper) {
      Value receiver = obj;
      if (isSuperTarget && superReceiver.isObject()) {
        receiver = superReceiver;
      }

      auto ordinarySet = [&](auto&& self,
                             const Value& base,
                             const Value& recv,
                             const std::string& key,
                             const Value& v) -> bool {
        if (base.isProxy()) {
          auto proxyPtr = base.getGC<Proxy>();
          if (proxyPtr->handler && proxyPtr->handler->isObject()) {
            auto handlerObj = std::get<GCPtr<Object>>(proxyPtr->handler->data);
            auto trapIt = handlerObj->properties.find("set");
            if (trapIt != handlerObj->properties.end() && trapIt->second.isFunction()) {
              auto trap = trapIt->second.getGC<Function>();
              std::vector<Value> trapArgs = {
                proxyPtr->target ? *proxyPtr->target : Value(Undefined{}),
                toPropertyKeyValue(key),
                v,
                recv
              };
              Value result = trap->isNative ? trap->nativeFunc(trapArgs)
                                            : invokeFunction(trap, trapArgs, Value(Undefined{}));
              if (hasError()) return false;
              return result.toBool();
            }
          }
          // No trap: write through to target if possible.
          if (proxyPtr->target && proxyPtr->target->isObject() && recv.isObject()) {
            recv.getGC<Object>()->properties[key] = v;
            return true;
          }
          return false;
        }

        if (base.isObject()) {
          auto baseObj = base.getGC<Object>();
          std::string getterName = "__get_" + key;
          std::string setterName = "__set_" + key;
          auto setterIt = baseObj->properties.find(setterName);
          if (setterIt != baseObj->properties.end() && setterIt->second.isFunction()) {
            invokeFunction(setterIt->second.getGC<Function>(), {v}, recv);
            return !hasError();
          }
          auto getterIt = baseObj->properties.find(getterName);
          if (getterIt != baseObj->properties.end() && getterIt->second.isFunction() &&
              setterIt == baseObj->properties.end()) {
            // Getter-only property.
            return false;
          }

          // If property exists somewhere on the prototype chain, ordinary [[Set]]
          // still routes through the chain. We approximate by delegating first.
          auto itOwn = baseObj->properties.find(key);
          if (itOwn == baseObj->properties.end()) {
            auto protoIt = baseObj->properties.find("__proto__");
            if (protoIt != baseObj->properties.end() && !protoIt->second.isNull() && !protoIt->second.isUndefined()) {
              return self(self, protoIt->second, recv, key, v);
            }
          }

          if (!recv.isObject()) return false;
          auto recvObj = recv.getGC<Object>();
          if (recvObj->isModuleNamespace) return false;
          if (recvObj->frozen) return false;
          if (recvObj->properties.count("__non_writable_" + key)) return false;
          bool isNew = recvObj->properties.find(key) == recvObj->properties.end();
          if ((recvObj->sealed || recvObj->nonExtensible) && isNew) return false;

          recvObj->properties[key] = v;
          return true;
        }

        if (base.isClass()) {
          auto cls = base.getGC<Class>();
          auto itOwn = cls->properties.find(key);
          if (itOwn == cls->properties.end()) {
            auto protoIt = cls->properties.find("__proto__");
            if (protoIt != cls->properties.end() && !protoIt->second.isNull() && !protoIt->second.isUndefined()) {
              return self(self, protoIt->second, recv, key, v);
            }
          }
          if (!recv.isClass() && !recv.isObject()) return false;
          if (recv.isClass()) {
            recv.getGC<Class>()->properties[key] = v;
          } else {
            recv.getGC<Object>()->properties[key] = v;
          }
          return true;
        }

        return false;
      };

      bool ok = ordinarySet(ordinarySet, obj, receiver, propName, right);
      if (!ok && strictMode_) {
        throwError(ErrorType::TypeError, "Cannot assign to property '" + propName + "'");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(right);
    }

    // Handle Proxy set trap
    if (obj.isProxy()) {
      auto proxyPtr = obj.getGC<Proxy>();
      if (proxyPtr->handler && proxyPtr->handler->isObject()) {
        auto handlerObj = std::get<GCPtr<Object>>(proxyPtr->handler->data);
        auto trapIt = handlerObj->properties.find("set");
        if (trapIt != handlerObj->properties.end() && trapIt->second.isFunction()) {
          auto trap = trapIt->second.getGC<Function>();
          if (trap->isNative) {
            // Call set trap: handler.set(target, property, value, receiver)
            std::vector<Value> trapArgs = {*proxyPtr->target, toPropertyKeyValue(propName), right, obj};
            Value result = trap->nativeFunc(trapArgs);
            if (!result.toBool()) {
              // Set trap returned false - throw in strict mode, but we'll just return
            }
            LIGHTJS_RETURN(right);
          } else {
            // Non-native set trap - call as JS function
            std::vector<Value> trapArgs = {*proxyPtr->target, toPropertyKeyValue(propName), right, obj};
            Value result = invokeFunction(trap, trapArgs, Value(Undefined{}));
            if (!result.toBool()) {
              // Set trap returned false
            }
            LIGHTJS_RETURN(right);
          }
        }
      }
      // No set trap - fall through to set on target
      if (proxyPtr->target && proxyPtr->target->isObject()) {
        auto targetObj = std::get<GCPtr<Object>>(proxyPtr->target->data);
        targetObj->properties[propName] = right;
        LIGHTJS_RETURN(right);
      }
    }

    if (obj.isObject()) {
      auto objPtr = obj.getGC<Object>();
      auto writeObjPtr = objPtr;
      if (isSuperTarget && superReceiver.isObject()) {
        writeObjPtr = superReceiver.getGC<Object>();
      }

      // Handle private field assignment (#name)
      if (member->privateIdentifier) {
        GCPtr<Class> cls;
        auto ctorIt = objPtr->properties.find("__constructor__");
        if (ctorIt != objPtr->properties.end() && ctorIt->second.isClass()) {
          cls = ctorIt->second.getGC<Class>();
        }
        GCPtr<Class> ownerClass = activePrivateOwnerClass_ ? activePrivateOwnerClass_ : cls;
        auto isOwnerInClassChain = [&](GCPtr<Class> target, const GCPtr<Class>& owner) -> bool {
          int depth = 0;
          while (target && depth < 128) {
            if (target.get() == owner.get()) {
              return true;
            }
            target = target->superClass;
            depth++;
          }
          return false;
        };
        if (!ownerClass || !cls || !isOwnerInClassChain(cls, ownerClass)) {
          throwError(ErrorType::TypeError, "Cannot read private member " + propName + " from an object whose class did not declare it");
          LIGHTJS_RETURN(Value(Undefined{}));
        }

        Value current(Undefined{});
        bool isMethod = false;
        bool hasPrivateGetter = false;
        bool hasPrivateSetter = false;
        if (ownerClass) {
          auto getterIt = ownerClass->getters.find(propName);
          if (getterIt != ownerClass->getters.end()) {
            hasPrivateGetter = true;
            current = invokeFunction(getterIt->second, {}, obj);
            if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
          }
          auto setterIt = ownerClass->setters.find(propName);
          if (setterIt != ownerClass->setters.end()) {
            hasPrivateSetter = true;
          }
          auto methodIt = ownerClass->methods.find(propName);
          if (methodIt != ownerClass->methods.end()) {
            isMethod = true;
            current = Value(methodIt->second);
          }
        }

        std::string mangledName = privateStorageKey(ownerClass, propName);
        if (!hasPrivateGetter && !isMethod) {
          auto it = objPtr->properties.find(mangledName);
          if (it != objPtr->properties.end()) {
            current = it->second;
          }
        }

        if (isMethod) {
          throwError(ErrorType::TypeError, "Cannot assign to private method " + propName);
          LIGHTJS_RETURN(Value(Undefined{}));
        }

        if (expr.op == AssignmentExpr::Op::Assign) {
          if (hasPrivateGetter && !hasPrivateSetter) {
            throwError(ErrorType::TypeError, "Cannot set property " + propName + " which has only a getter");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          if (hasPrivateSetter && ownerClass) {
            auto setterIt = ownerClass->setters.find(propName);
            if (setterIt != ownerClass->setters.end()) {
              invokeFunction(setterIt->second, {right}, obj);
              if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
            }
          } else {
            objPtr->properties[mangledName] = right;
          }
          LIGHTJS_RETURN(right);
        }

        Value result;
        if (!computeCompoundValue(expr.op, current, right, result)) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (hasPrivateGetter && !hasPrivateSetter) {
          throwError(ErrorType::TypeError, "Cannot set property " + propName + " which has only a getter");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (hasPrivateSetter && ownerClass) {
          auto setterIt = ownerClass->setters.find(propName);
          if (setterIt != ownerClass->setters.end()) {
            invokeFunction(setterIt->second, {result}, obj);
            if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
          }
        } else {
          objPtr->properties[mangledName] = result;
        }
        LIGHTJS_RETURN(result);
      }

      if (writeObjPtr->isModuleNamespace) {
        // Module namespace exotic objects reject writes.
        if (strictMode_) {
          throwError(ErrorType::TypeError, "Cannot assign to property '" + propName + "' of module namespace object");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        LIGHTJS_RETURN(right);
      }

      std::string getterName = "__get_" + propName;
      std::string setterName = "__set_" + propName;
      auto getterIt = objPtr->properties.find(getterName);
      auto setterIt = objPtr->properties.find(setterName);
      bool hasGetter = getterIt != objPtr->properties.end() && getterIt->second.isFunction();
      bool hasSetter = setterIt != objPtr->properties.end() && setterIt->second.isFunction();

      // If there's no own accessor or data property on the base object, check the prototype chain
      // for an accessor. This is required for class-defined accessors (stored on the prototype)
      // to run on instance assignment.
      bool baseHasDataProp = objPtr->properties.count(propName) > 0;
      bool inheritedHasGetter = false;
      bool inheritedHasSetter = false;
      bool inheritedDataNonWritable = false;
      Value inheritedGetter(Undefined{});
      Value inheritedSetter(Undefined{});
      if (!hasGetter && !hasSetter && !baseHasDataProp) {
        auto protoIt = objPtr->properties.find("__proto__");
        if (protoIt != objPtr->properties.end()) {
          Value protoVal = protoIt->second;
          int depth = 0;
          while (depth < 50) {
            if (protoVal.isNull() || protoVal.isUndefined()) break;
            if (protoVal.isObject()) {
              auto proto = protoVal.getGC<Object>();
              auto gIt = proto->properties.find(getterName);
              auto sIt = proto->properties.find(setterName);
              bool g = gIt != proto->properties.end() && gIt->second.isFunction();
              bool s = sIt != proto->properties.end() && sIt->second.isFunction();
              if (g || s) {
                inheritedHasGetter = g;
                inheritedHasSetter = s;
                if (g) inheritedGetter = gIt->second;
                if (s) inheritedSetter = sIt->second;
                break;
              }
              // Data properties shadow accessors further up the chain.
              if (proto->properties.count(propName) > 0) {
                inheritedDataNonWritable =
                  proto->properties.count("__non_writable_" + propName) > 0;
                break;
              }
              auto nextProto = proto->properties.find("__proto__");
              if (nextProto == proto->properties.end()) break;
              protoVal = nextProto->second;
            } else if (protoVal.isClass()) {
              auto protoCls = protoVal.getGC<Class>();
              auto gIt = protoCls->properties.find(getterName);
              auto sIt = protoCls->properties.find(setterName);
              bool g = gIt != protoCls->properties.end() && gIt->second.isFunction();
              bool s = sIt != protoCls->properties.end() && sIt->second.isFunction();
              if (g || s) {
                inheritedHasGetter = g;
                inheritedHasSetter = s;
                if (g) inheritedGetter = gIt->second;
                if (s) inheritedSetter = sIt->second;
                break;
              }
              if (protoCls->properties.count(propName) > 0) {
                inheritedDataNonWritable =
                  protoCls->properties.count("__non_writable_" + propName) > 0;
                break;
              }
              auto nextProto = protoCls->properties.find("__proto__");
              if (nextProto == protoCls->properties.end()) break;
              protoVal = nextProto->second;
            } else {
              break;
            }
            depth++;
          }
        }
      }

      // Accessor writes should run the setter instead of creating an own data property.
      if (expr.op == AssignmentExpr::Op::Assign) {
        if (hasSetter) {
          auto setter = setterIt->second.getGC<Function>();
          Value setterReceiver = (isSuperTarget && superReceiver.isObject()) ? superReceiver : obj;
          invokeFunction(setter, {right}, setterReceiver);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
          LIGHTJS_RETURN(right);
        }
        if (inheritedHasSetter) {
          auto setter = inheritedSetter.getGC<Function>();
          Value setterReceiver = (isSuperTarget && superReceiver.isObject()) ? superReceiver : obj;
          invokeFunction(setter, {right}, setterReceiver);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
          LIGHTJS_RETURN(right);
        }
        if ((hasGetter && !hasSetter) || (inheritedHasGetter && !inheritedHasSetter)) {
          if (strictMode_) {
            throwError(ErrorType::TypeError, "Cannot set property " + propName + " which has only a getter");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          LIGHTJS_RETURN(right);
        }
        if (inheritedDataNonWritable) {
          if (strictMode_) {
            throwError(ErrorType::TypeError, "Cannot assign to read only property '" + propName + "'");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          LIGHTJS_RETURN(right);
        }
      }

      // Check if object is frozen (can't modify own properties / add new ones).
      if (writeObjPtr->frozen) {
        if (strictMode_) {
          throwError(ErrorType::TypeError, "Cannot assign to read only property '" + propName + "' of object");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        LIGHTJS_RETURN(right);
      }

      // Check __non_writable_ marker on the receiver.
      if (writeObjPtr->properties.count("__non_writable_" + propName)) {
        if (strictMode_) {
          throwError(ErrorType::TypeError, "Cannot assign to read only property '" + propName + "'");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        LIGHTJS_RETURN(right);
      }

      // Setting [[Prototype]] of a non-extensible object must not succeed.
      if (propName == "__proto__" && (writeObjPtr->sealed || writeObjPtr->nonExtensible)) {
        if (strictMode_) {
          throwError(ErrorType::TypeError, "Cannot set prototype of non-extensible object");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        LIGHTJS_RETURN(right);
      }

      // Check if object is sealed (can't add new properties)
      bool isNewProperty = writeObjPtr->properties.find(propName) == writeObjPtr->properties.end();
      if ((writeObjPtr->sealed || writeObjPtr->nonExtensible) && isNewProperty) {
        if (strictMode_) {
          throwError(ErrorType::TypeError, "Cannot add property " + propName + ", object is not extensible");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        LIGHTJS_RETURN(right);
      }

      // Shape tracking disabled - using direct hash map access for simplicity

      if (expr.op == AssignmentExpr::Op::Assign) {
        writeObjPtr->properties[propName] = right;
      } else {
        Value current(Undefined{});
        if (hasGetter) {
          Value getterReceiver = (isSuperTarget && superReceiver.isObject()) ? superReceiver : obj;
          current = callFunction(getterIt->second, {}, getterReceiver);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
        } else if (inheritedHasGetter) {
          Value getterReceiver = (isSuperTarget && superReceiver.isObject()) ? superReceiver : obj;
          current = callFunction(inheritedGetter, {}, getterReceiver);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
        } else {
          auto curIt = objPtr->properties.find(propName);
          if (curIt != objPtr->properties.end()) {
            current = curIt->second;
          }
        }

        Value result;
        if (!computeCompoundValue(expr.op, current, right, result)) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }

        if (hasGetter && !hasSetter) {
          if (strictMode_) {
            throwError(ErrorType::TypeError, "Cannot set property " + propName + " which has only a getter");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          LIGHTJS_RETURN(result);
        }

        if (hasSetter) {
          auto setter = setterIt->second.getGC<Function>();
          Value setterReceiver = (isSuperTarget && superReceiver.isObject()) ? superReceiver : obj;
          invokeFunction(setter, {result}, setterReceiver);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
          LIGHTJS_RETURN(result);
        }
        if (inheritedHasSetter) {
          auto setter = inheritedSetter.getGC<Function>();
          Value setterReceiver = (isSuperTarget && superReceiver.isObject()) ? superReceiver : obj;
          invokeFunction(setter, {result}, setterReceiver);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
          LIGHTJS_RETURN(result);
        }
        if ((inheritedHasGetter && !inheritedHasSetter) || (hasGetter && !hasSetter)) {
          if (strictMode_) {
            throwError(ErrorType::TypeError, "Cannot set property " + propName + " which has only a getter");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          LIGHTJS_RETURN(result);
        }
        if (inheritedDataNonWritable) {
          if (strictMode_) {
            throwError(ErrorType::TypeError, "Cannot assign to read only property '" + propName + "'");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          LIGHTJS_RETURN(result);
        }

        writeObjPtr->properties[propName] = result;
      }
      // Sync globalThis property changes back to root environment bindings.
      // In JS, global variables ARE properties of the global object, so
      // this['x'] = 5 must also update the variable binding for x.
      auto rootEnv = env_->getRoot();
      auto globalThisVal = rootEnv->get("globalThis");
      if (globalThisVal && globalThisVal->isObject()) {
        auto globalObj = std::get<GCPtr<Object>>(globalThisVal->data);
        if (globalObj.get() == writeObjPtr.get()) {
          rootEnv->set(propName, writeObjPtr->properties[propName]);
        }
      }
      LIGHTJS_RETURN(writeObjPtr->properties[propName]);
    }

    if (obj.isFunction()) {
      auto funcPtr = obj.getGC<Function>();
      if (propName == "caller" || propName == "arguments") {
        // Restricted properties live on %FunctionPrototype% and must not be
        // created as own data properties.
        throwError(ErrorType::TypeError, "'caller', 'callee', and 'arguments' properties may not be accessed");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      // name and length are non-writable on functions
      if (propName == "name" || propName == "length") {
        if (strictMode_) {
          throwError(ErrorType::TypeError, "Cannot assign to read only property '" + propName + "'");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        LIGHTJS_RETURN(right);
      }
      // Check __non_writable_ marker
      if (funcPtr->properties.count("__non_writable_" + propName)) {
        if (strictMode_) {
          throwError(ErrorType::TypeError, "Cannot assign to read only property '" + propName + "'");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        LIGHTJS_RETURN(right);
      }
      auto getterIt = funcPtr->properties.find("__get_" + propName);
      auto setterIt = funcPtr->properties.find("__set_" + propName);
      bool hasGetter = getterIt != funcPtr->properties.end() && getterIt->second.isFunction();
      bool hasSetter = setterIt != funcPtr->properties.end() && setterIt->second.isFunction();
      if (expr.op == AssignmentExpr::Op::Assign) {
        if (hasSetter) {
          invokeFunction(setterIt->second.getGC<Function>(), {right}, obj);
          if (hasError()) {
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          LIGHTJS_RETURN(right);
        }
        if (hasGetter && !hasSetter) {
          if (strictMode_) {
            throwError(ErrorType::TypeError,
                       "Cannot set property " + propName + " which has only a getter");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          LIGHTJS_RETURN(right);
        }
      }
      if (expr.op == AssignmentExpr::Op::Assign) {
        funcPtr->properties[propName] = right;
      } else {
        Value current = Value(Undefined{});
        if (hasGetter) {
          current = callFunction(getterIt->second, {}, obj);
          if (hasError()) {
            LIGHTJS_RETURN(Value(Undefined{}));
          }
        } else if (auto it = funcPtr->properties.find(propName); it != funcPtr->properties.end()) {
          current = it->second;
        }
        Value result;
        if (!computeCompoundValue(expr.op, current, right, result)) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (hasSetter) {
          invokeFunction(setterIt->second.getGC<Function>(), {result}, obj);
          if (hasError()) {
            LIGHTJS_RETURN(Value(Undefined{}));
          }
        } else if (hasGetter && !hasSetter) {
          if (strictMode_) {
            throwError(ErrorType::TypeError,
                       "Cannot set property " + propName + " which has only a getter");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
        } else {
          funcPtr->properties[propName] = result;
        }
        LIGHTJS_RETURN(result);
      }
      LIGHTJS_RETURN(right);
    }

    if (obj.isPromise()) {
      auto promisePtr = obj.getGC<Promise>();
      if (expr.op == AssignmentExpr::Op::Assign) {
        promisePtr->properties[propName] = right;
      } else {
        Value current = promisePtr->properties[propName];
        Value result;
        if (!computeCompoundValue(expr.op, current, right, result)) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        promisePtr->properties[propName] = result;
        LIGHTJS_RETURN(result);
      }
      LIGHTJS_RETURN(right);
    }

    if (obj.isRegex()) {
      auto rxPtr = obj.getGC<Regex>();
      if (expr.op == AssignmentExpr::Op::Assign) {
        rxPtr->properties[propName] = right;
      } else {
        Value current = Value(Undefined{});
        auto it = rxPtr->properties.find(propName);
        if (it != rxPtr->properties.end()) current = it->second;
        Value result;
        if (!computeCompoundValue(expr.op, current, right, result)) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        rxPtr->properties[propName] = result;
        LIGHTJS_RETURN(result);
      }
      LIGHTJS_RETURN(right);
    }

    if (obj.isError()) {
      auto errPtr = obj.getGC<Error>();
      auto syncMessage = [&](const Value& v) {
        if (propName == "message") {
          errPtr->message = v.isUndefined() ? "" : v.toString();
        }
      };
      if (expr.op == AssignmentExpr::Op::Assign) {
        errPtr->properties[propName] = right;
        syncMessage(right);
      } else {
        Value current = Value(Undefined{});
        auto it = errPtr->properties.find(propName);
        if (it != errPtr->properties.end()) current = it->second;
        Value result;
        if (!computeCompoundValue(expr.op, current, right, result)) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        errPtr->properties[propName] = result;
        syncMessage(result);
        LIGHTJS_RETURN(result);
      }
      LIGHTJS_RETURN(right);
    }

    if (obj.isArray()) {
      auto arrPtr = obj.getGC<Array>();
      bool nonExtensible = false;
      if (auto itNonExt = arrPtr->properties.find("__non_extensible__");
          itNonExt != arrPtr->properties.end() &&
          itNonExt->second.isBool() &&
          itNonExt->second.toBool()) {
        nonExtensible = true;
      }
      if (expr.op == AssignmentExpr::Op::Assign && propName == "length") {
        if (arrPtr->properties.find("__non_writable_length") != arrPtr->properties.end()) {
          if (strictMode_) {
            throwError(ErrorType::TypeError, "Cannot assign to read only property 'length'");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          LIGHTJS_RETURN(Value(static_cast<double>(arrPtr->elements.size())));
        }
        // Arguments objects allow arbitrary length override (configurable data property)
        bool isArgumentsObject = false;
        auto isArgsIt = arrPtr->properties.find("__is_arguments_object__");
        if (isArgsIt != arrPtr->properties.end() && isArgsIt->second.isBool() && isArgsIt->second.toBool()) {
          isArgumentsObject = true;
        }
        if (isArgumentsObject) {
          arrPtr->properties["__overridden_length__"] = right;
          LIGHTJS_RETURN(right);
        }
        double lenNum = right.toNumber();
        if (!std::isfinite(lenNum) || lenNum < 0 || std::floor(lenNum) != lenNum) {
          throwError(ErrorType::RangeError, "Invalid array length");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        size_t newLen = static_cast<size_t>(lenNum);
        if (newLen < arrPtr->elements.size()) {
          arrPtr->elements.resize(newLen);
        } else if (newLen > arrPtr->elements.size()) {
          arrPtr->elements.resize(newLen, Value(Undefined{}));
        }
        LIGHTJS_RETURN(Value(static_cast<double>(arrPtr->elements.size())));
      }
      size_t idx = 0;
      if (parseArrayIndex(propName, idx)) {
        auto setterIt = arrPtr->properties.find("__set_" + propName);
        auto getterIt = arrPtr->properties.find("__get_" + propName);
        if (expr.op == AssignmentExpr::Op::Assign) {
          if (setterIt != arrPtr->properties.end() && setterIt->second.isFunction()) {
            callFunction(setterIt->second, {right}, obj);
          } else {
            if (nonExtensible && idx >= arrPtr->elements.size()) {
              if (strictMode_) {
                throwError(ErrorType::TypeError, "Cannot add property '" + propName + "', object is not extensible");
                LIGHTJS_RETURN(Value(Undefined{}));
              }
              LIGHTJS_RETURN(right);
            }
            if (arrPtr->properties.count("__non_writable_" + propName)) {
              if (strictMode_) {
                throwError(ErrorType::TypeError, "Cannot assign to read only property '" + propName + "'");
                LIGHTJS_RETURN(Value(Undefined{}));
              }
              LIGHTJS_RETURN(right);
            }
            if (idx >= arrPtr->elements.size()) {
              size_t oldSize = arrPtr->elements.size();
              arrPtr->elements.resize(idx + 1, Value(Undefined{}));
              // Mark intermediate indices as holes
              for (size_t hi = oldSize; hi < idx; hi++) {
                arrPtr->properties["__hole_" + std::to_string(hi) + "__"] = Value(true);
              }
            }
            arrPtr->elements[idx] = right;
            // Clear deleted/hole marker if re-assigning to a deleted index
            arrPtr->properties.erase("__deleted_" + propName + "__");
            arrPtr->properties.erase("__hole_" + propName + "__");
          }
          LIGHTJS_RETURN(right);
        }
        // Compound assignment: read current, compute, write back
        Value current = Value(Undefined{});
        if (getterIt != arrPtr->properties.end() && getterIt->second.isFunction()) {
          current = callFunction(getterIt->second, {}, obj);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
        } else if (idx < arrPtr->elements.size()) {
          current = arrPtr->elements[idx];
        }
        Value result;
        if (!computeCompoundValue(expr.op, current, right, result)) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (setterIt == arrPtr->properties.end() &&
            arrPtr->properties.count("__non_writable_" + propName)) {
          if (strictMode_) {
            throwError(ErrorType::TypeError, "Cannot assign to read only property '" + propName + "'");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          LIGHTJS_RETURN(result);
        }
        // Only extend array if index is in bounds (compound assignment on
        // out-of-bounds index matches arguments object behavior where length
        // doesn't change)
        if (setterIt != arrPtr->properties.end() && setterIt->second.isFunction()) {
          callFunction(setterIt->second, {result}, obj);
        } else if (idx < arrPtr->elements.size()) {
          arrPtr->elements[idx] = result;
        }
        LIGHTJS_RETURN(result);
      }
      if (expr.op == AssignmentExpr::Op::Assign) {
        auto getterIt = arrPtr->properties.find("__get_" + propName);
        auto setterIt = arrPtr->properties.find("__set_" + propName);
        bool hasGetter = getterIt != arrPtr->properties.end() && getterIt->second.isFunction();
        bool hasSetter = setterIt != arrPtr->properties.end() && setterIt->second.isFunction();

        // Accessor assignment should invoke the setter.
        if (hasSetter) {
          callFunction(setterIt->second, {right}, obj);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
          LIGHTJS_RETURN(right);
        }
        if (hasGetter && !hasSetter) {
          if (strictMode_) {
            throwError(ErrorType::TypeError, "Cannot set property " + propName + " which has only a getter");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          LIGHTJS_RETURN(right);
        }

        // For frozen template objects and similar, arrays also use __non_writable_<name> markers.
        if (arrPtr->properties.count("__non_writable_" + propName)) {
          if (strictMode_) {
            throwError(ErrorType::TypeError, "Cannot assign to read only property '" + propName + "'");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          LIGHTJS_RETURN(right);
        }

        if (nonExtensible && arrPtr->properties.find(propName) == arrPtr->properties.end()) {
          if (strictMode_) {
            throwError(ErrorType::TypeError, "Cannot add property '" + propName + "', object is not extensible");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          LIGHTJS_RETURN(right);
        }

        arrPtr->properties[propName] = right;
      } else {
        auto getterIt = arrPtr->properties.find("__get_" + propName);
        auto setterIt = arrPtr->properties.find("__set_" + propName);
        bool hasGetter = getterIt != arrPtr->properties.end() && getterIt->second.isFunction();
        bool hasSetter = setterIt != arrPtr->properties.end() && setterIt->second.isFunction();

        Value current = Value(Undefined{});
        if (hasGetter) {
          current = callFunction(getterIt->second, {}, obj);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
        } else if (auto it = arrPtr->properties.find(propName); it != arrPtr->properties.end()) {
          current = it->second;
        }
        Value result;
        if (!computeCompoundValue(expr.op, current, right, result)) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }

        if (hasGetter && !hasSetter) {
          if (strictMode_) {
            throwError(ErrorType::TypeError, "Cannot set property " + propName + " which has only a getter");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          LIGHTJS_RETURN(result);
        }

        if (hasSetter) {
          callFunction(setterIt->second, {result}, obj);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
          LIGHTJS_RETURN(result);
        }

        if (arrPtr->properties.count("__non_writable_" + propName)) {
          if (strictMode_) {
            throwError(ErrorType::TypeError, "Cannot assign to read only property '" + propName + "'");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          LIGHTJS_RETURN(result);
        }

        if (nonExtensible && arrPtr->properties.find(propName) == arrPtr->properties.end()) {
          if (strictMode_) {
            throwError(ErrorType::TypeError, "Cannot add property '" + propName + "', object is not extensible");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          LIGHTJS_RETURN(result);
        }

        arrPtr->properties[propName] = result;
      }
      LIGHTJS_RETURN(arrPtr->properties[propName]);
    }

    if (obj.isTypedArray()) {
      auto taPtr = obj.getGC<TypedArray>();
      size_t idx = 0;
      if (parseArrayIndex(propName, idx)) {
        if (taPtr->type == TypedArrayType::BigInt64 || taPtr->type == TypedArrayType::BigUint64) {
          if (taPtr->type == TypedArrayType::BigUint64) {
            taPtr->setBigUintElement(idx, bigint::toUint64Trunc(right.toBigInt()));
          } else {
            taPtr->setBigIntElement(idx, bigint::toInt64Trunc(right.toBigInt()));
          }
        } else {
          taPtr->setElement(idx, right.toNumber());
        }
        LIGHTJS_RETURN(right);
      }

      if (expr.op == AssignmentExpr::Op::Assign) {
        auto setterIt = taPtr->properties.find("__set_" + propName);
        if (setterIt != taPtr->properties.end() && setterIt->second.isFunction()) {
          callFunction(setterIt->second, {right}, obj);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
        } else {
          taPtr->properties[propName] = right;
        }
        LIGHTJS_RETURN(right);
      }

      Value current = Value(Undefined{});
      auto getterIt = taPtr->properties.find("__get_" + propName);
      if (getterIt != taPtr->properties.end() && getterIt->second.isFunction()) {
        current = callFunction(getterIt->second, {}, obj);
        if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      } else if (auto it = taPtr->properties.find(propName); it != taPtr->properties.end()) {
        current = it->second;
      }
      Value result;
      if (!computeCompoundValue(expr.op, current, right, result)) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      auto setterIt = taPtr->properties.find("__set_" + propName);
      if (setterIt != taPtr->properties.end() && setterIt->second.isFunction()) {
        callFunction(setterIt->second, {result}, obj);
        if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      } else {
        taPtr->properties[propName] = result;
      }
      LIGHTJS_RETURN(result);
    }

    if (obj.isArrayBuffer()) {
      auto bufferPtr = obj.getGC<ArrayBuffer>();
      if (expr.op == AssignmentExpr::Op::Assign) {
        auto setterIt = bufferPtr->properties.find("__set_" + propName);
        if (setterIt != bufferPtr->properties.end() && setterIt->second.isFunction()) {
          callFunction(setterIt->second, {right}, obj);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
        } else {
          bufferPtr->properties[propName] = right;
        }
        LIGHTJS_RETURN(right);
      }

      Value current = Value(Undefined{});
      auto getterIt = bufferPtr->properties.find("__get_" + propName);
      if (getterIt != bufferPtr->properties.end() && getterIt->second.isFunction()) {
        current = callFunction(getterIt->second, {}, obj);
        if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      } else if (auto it = bufferPtr->properties.find(propName); it != bufferPtr->properties.end()) {
        current = it->second;
      }
      Value result;
      if (!computeCompoundValue(expr.op, current, right, result)) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      auto setterIt = bufferPtr->properties.find("__set_" + propName);
      if (setterIt != bufferPtr->properties.end() && setterIt->second.isFunction()) {
        callFunction(setterIt->second, {result}, obj);
        if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      } else {
        bufferPtr->properties[propName] = result;
      }
      LIGHTJS_RETURN(result);
    }

    if (obj.isDataView()) {
      auto viewPtr = obj.getGC<DataView>();
      if (expr.op == AssignmentExpr::Op::Assign) {
        auto setterIt = viewPtr->properties.find("__set_" + propName);
        if (setterIt != viewPtr->properties.end() && setterIt->second.isFunction()) {
          callFunction(setterIt->second, {right}, obj);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
        } else {
          viewPtr->properties[propName] = right;
        }
        LIGHTJS_RETURN(right);
      }

      Value current = Value(Undefined{});
      auto getterIt = viewPtr->properties.find("__get_" + propName);
      if (getterIt != viewPtr->properties.end() && getterIt->second.isFunction()) {
        current = callFunction(getterIt->second, {}, obj);
        if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      } else if (auto it = viewPtr->properties.find(propName); it != viewPtr->properties.end()) {
        current = it->second;
      }
      Value result;
      if (!computeCompoundValue(expr.op, current, right, result)) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      auto setterIt = viewPtr->properties.find("__set_" + propName);
      if (setterIt != viewPtr->properties.end() && setterIt->second.isFunction()) {
        callFunction(setterIt->second, {result}, obj);
        if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      } else {
        viewPtr->properties[propName] = result;
      }
      LIGHTJS_RETURN(result);
    }

    if (obj.isRegex()) {
      auto regexPtr = obj.getGC<Regex>();
      if (expr.op == AssignmentExpr::Op::Assign) {
        regexPtr->properties[propName] = right;
      } else {
        Value current = regexPtr->properties[propName];
        Value result;
        if (!computeCompoundValue(expr.op, current, right, result)) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        regexPtr->properties[propName] = result;
        LIGHTJS_RETURN(result);
      }
      LIGHTJS_RETURN(right);
    }

    if (obj.isClass()) {
      auto clsPtr = obj.getGC<Class>();
      // Handle private static field assignment (#name)
      if (member->privateIdentifier) {
        GCPtr<Class> ownerClass = activePrivateOwnerClass_ ? activePrivateOwnerClass_ : clsPtr;
        if (!ownerClass || ownerClass.get() != clsPtr.get()) {
          throwError(ErrorType::TypeError,
                    "Cannot write private member " + propName +
                        " to an object whose class did not declare it");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        std::string mangledName = privateStorageKey(ownerClass, propName);
        auto getterIt = clsPtr->properties.find("__get_" + mangledName);
        auto setterIt = clsPtr->properties.find("__set_" + mangledName);
        bool hasGetter = getterIt != clsPtr->properties.end() && getterIt->second.isFunction();
        bool hasSetter = setterIt != clsPtr->properties.end() && setterIt->second.isFunction();
        bool isMethod = clsPtr->staticMethods.count(mangledName) > 0;
        bool hasPrivateDecl =
            clsPtr->properties.count(mangledName) > 0 ||
            clsPtr->properties.count("__get_" + mangledName) > 0 ||
            clsPtr->properties.count("__set_" + mangledName) > 0 ||
            isMethod;
        if (!hasPrivateDecl) {
          throwError(ErrorType::TypeError,
                    "Cannot write private member " + propName +
                        " to an object whose class did not declare it");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (isMethod) {
          throwError(ErrorType::TypeError, "Cannot assign to private method " + propName);
          LIGHTJS_RETURN(Value(Undefined{}));
        }

        Value current(Undefined{});
        if (hasGetter) {
          current = callFunction(getterIt->second, {}, obj);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
        } else if (auto it = clsPtr->properties.find(mangledName); it != clsPtr->properties.end()) {
          current = it->second;
        }

        if (expr.op == AssignmentExpr::Op::Assign) {
          if (hasGetter && !hasSetter) {
            throwError(ErrorType::TypeError, "Cannot set property " + propName + " which has only a getter");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          if (hasSetter) {
            callFunction(setterIt->second, {right}, obj);
            if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
          } else {
            clsPtr->properties[mangledName] = right;
          }
          LIGHTJS_RETURN(right);
        }

        Value result;
        if (!computeCompoundValue(expr.op, current, right, result)) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (hasGetter && !hasSetter) {
          throwError(ErrorType::TypeError, "Cannot set property " + propName + " which has only a getter");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (hasSetter) {
          callFunction(setterIt->second, {result}, obj);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
        } else {
          clsPtr->properties[mangledName] = result;
        }
        LIGHTJS_RETURN(result);
      }
      // Check __non_writable_ marker
      if (clsPtr->properties.count("__non_writable_" + propName)) {
        if (strictMode_) {
          throwError(ErrorType::TypeError, "Cannot assign to read only property '" + propName + "'");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        LIGHTJS_RETURN(right);
      }
      if (expr.op == AssignmentExpr::Op::Assign) {
        auto setterIt = clsPtr->properties.find("__set_" + propName);
        if (setterIt != clsPtr->properties.end() && setterIt->second.isFunction()) {
          callFunction(setterIt->second, {right}, obj);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
          LIGHTJS_RETURN(right);
        }
        // Inherited accessors (e.g. %FunctionPrototype%.caller) should run the
        // prototype setter instead of creating an own data property.
        if (clsPtr->properties.count(propName) == 0) {
          auto protoIt = clsPtr->properties.find("__proto__");
          if (protoIt != clsPtr->properties.end()) {
            Value protoVal = protoIt->second;
            int depth = 0;
            while (depth < 50) {
              if (protoVal.isObject()) {
                auto proto = protoVal.getGC<Object>();
                auto protoSetterIt = proto->properties.find("__set_" + propName);
                if (protoSetterIt != proto->properties.end() && protoSetterIt->second.isFunction()) {
                  callFunction(protoSetterIt->second, {right}, obj);
                  if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
                  LIGHTJS_RETURN(right);
                }
                auto nextProto = proto->properties.find("__proto__");
                if (nextProto == proto->properties.end()) break;
                protoVal = nextProto->second;
              } else if (protoVal.isClass()) {
                auto protoCls = protoVal.getGC<Class>();
                auto protoSetterIt = protoCls->properties.find("__set_" + propName);
                if (protoSetterIt != protoCls->properties.end() && protoSetterIt->second.isFunction()) {
                  callFunction(protoSetterIt->second, {right}, obj);
                  if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
                  LIGHTJS_RETURN(right);
                }
                auto nextProto = protoCls->properties.find("__proto__");
                if (nextProto == protoCls->properties.end()) break;
                protoVal = nextProto->second;
              } else if (protoVal.isFunction()) {
                auto protoFn = protoVal.getGC<Function>();
                auto protoSetterIt = protoFn->properties.find("__set_" + propName);
                if (protoSetterIt != protoFn->properties.end() && protoSetterIt->second.isFunction()) {
                  callFunction(protoSetterIt->second, {right}, obj);
                  if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
                  LIGHTJS_RETURN(right);
                }
                auto nextProto = protoFn->properties.find("__proto__");
                if (nextProto == protoFn->properties.end()) break;
                protoVal = nextProto->second;
              } else {
                break;
              }
              depth++;
            }
          }
        }
        clsPtr->properties[propName] = right;
      } else {
        auto getterIt = clsPtr->properties.find("__get_" + propName);
        auto setterIt = clsPtr->properties.find("__set_" + propName);
        bool hasGetter = getterIt != clsPtr->properties.end() && getterIt->second.isFunction();
        bool hasSetter = setterIt != clsPtr->properties.end() && setterIt->second.isFunction();
        Value current(Undefined{});
        if (hasGetter) {
          current = callFunction(getterIt->second, {}, obj);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
        } else if (auto it = clsPtr->properties.find(propName); it != clsPtr->properties.end()) {
          current = it->second;
        }
        Value result;
        if (!computeCompoundValue(expr.op, current, right, result)) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (hasGetter && !hasSetter) {
          if (strictMode_) {
            throwError(ErrorType::TypeError, "Cannot set property " + propName + " which has only a getter");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          LIGHTJS_RETURN(result);
        }
        if (hasSetter) {
          callFunction(setterIt->second, {result}, obj);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
          LIGHTJS_RETURN(result);
        }
        clsPtr->properties[propName] = result;
      }
      LIGHTJS_RETURN(clsPtr->properties[propName]);
    }
  }

  LIGHTJS_RETURN(right);
}

Task Interpreter::evaluateUpdate(const UpdateExpr& expr) {
  auto applyNumericUpdate = [&](const Value& currentValue,
                                Value& oldValue,
                                Value& newValue) -> bool {
    Value numeric = isObjectLike(currentValue) ? toPrimitiveValue(currentValue, false) : currentValue;
    if (hasError()) {
      return false;
    }
    if (numeric.isSymbol()) {
      throwError(ErrorType::TypeError, "Cannot convert Symbol to number");
      return false;
    }
    if (numeric.isBigInt()) {
      auto oldBigInt = numeric.toBigInt();
      auto nextBigInt = oldBigInt;
      if (expr.op == UpdateExpr::Op::Increment) {
        nextBigInt += 1;
      } else {
        nextBigInt -= 1;
      }
      oldValue = Value(BigInt(oldBigInt));
      newValue = Value(BigInt(nextBigInt));
      return true;
    }

    double oldNumber = numeric.toNumber();
    double nextNumber =
      (expr.op == UpdateExpr::Op::Increment) ? oldNumber + 1.0 : oldNumber - 1.0;
    oldValue = Value(oldNumber);
    newValue = Value(nextNumber);
    return true;
  };

  if (auto* id = std::get_if<Identifier>(&expr.argument->node)) {
    // Check const before update
    if (env_->isConst(id->name)) {
      throwError(ErrorType::TypeError, "Assignment to constant variable '" + id->name + "'");
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (env_->isSilentImmutable(id->name)) {
      if (strictMode_) {
        throwError(ErrorType::TypeError, "Assignment to constant variable '" + id->name + "'");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      // Non-strict: evaluate the operand but discard the update
      auto val = env_->get(id->name);
      Value currentVal = val.value_or(Value(Undefined{}));
      Value oldValue, newValue;
      applyNumericUpdate(currentVal, oldValue, newValue);
      LIGHTJS_RETURN(expr.prefix ? newValue : oldValue);
    }

    // Capture with-scope target early so PutValue uses the original
    // object environment reference even if getters mutate bindings.
    auto withScopeTarget = env_->resolveWithScopeValue(id->name);
    GCPtr<Object> globalPropertyTarget;
    if (!withScopeTarget && !env_->resolveBindingEnvironment(id->name)) {
      auto rootEnv = env_->getRoot();
      auto globalThisVal = rootEnv->get("globalThis");
      if (globalThisVal && globalThisVal->isObject()) {
        auto globalObj = globalThisVal->getGC<Object>();
        if (globalObj->properties.count(id->name) > 0 ||
            globalObj->properties.count("__get_" + id->name) > 0 ||
            globalObj->properties.count("__set_" + id->name) > 0) {
          globalPropertyTarget = globalObj;
        }
      }
    }
    if (hasError()) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    Value currentValue;
    if (withScopeTarget) {
      auto withValue = env_->getWithScopeBindingValue(*withScopeTarget, id->name, strictMode_);
      if (hasError()) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (!withValue.has_value()) {
        if (strictMode_) {
          throwError(ErrorType::ReferenceError, "'" + id->name + "' is not defined");
        }
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      currentValue = *withValue;
    } else {
      { auto _t = evaluate(*expr.argument); LIGHTJS_RUN_TASK(_t, currentValue); }
      if (hasError()) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
    }

    Value oldValue;
    Value newValue;
    if (!applyNumericUpdate(currentValue, oldValue, newValue)) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    if (withScopeTarget) {
      if (!env_->setWithScopeBindingValue(*withScopeTarget, id->name, newValue, strictMode_)) {
        if (!hasError() && strictMode_) {
          throwError(ErrorType::ReferenceError, "'" + id->name + "' is not defined");
        }
        LIGHTJS_RETURN(Value(Undefined{}));
      }
    } else if (globalPropertyTarget) {
      auto setterIt = globalPropertyTarget->properties.find("__set_" + id->name);
      bool hasSetter = setterIt != globalPropertyTarget->properties.end() && setterIt->second.isFunction();
      bool hasGetter = globalPropertyTarget->properties.count("__get_" + id->name) > 0;
      bool stillExists =
        globalPropertyTarget->properties.count(id->name) > 0 ||
        globalPropertyTarget->properties.count("__get_" + id->name) > 0 ||
        globalPropertyTarget->properties.count("__set_" + id->name) > 0;

      if (!stillExists) {
        if (strictMode_) {
          throwError(ErrorType::ReferenceError, "'" + id->name + "' is not defined");
        }
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (globalPropertyTarget->properties.count("__non_writable_" + id->name) > 0) {
        if (strictMode_) {
          throwError(ErrorType::TypeError,
                     "Cannot assign to read only property '" + id->name + "'");
        }
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (hasGetter && !hasSetter) {
        if (strictMode_) {
          throwError(ErrorType::TypeError,
                     "Cannot set property " + id->name + " which has only a getter");
        }
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (hasSetter) {
        callFunction(setterIt->second, {newValue}, Value(globalPropertyTarget));
        if (hasError()) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      } else {
        globalPropertyTarget->properties[id->name] = newValue;
      }
      if (hasError()) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
    } else if (!env_->set(id->name, newValue)) {
      if (strictMode_) {
        throwError(ErrorType::ReferenceError, "'" + id->name + "' is not defined");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
    }
    LIGHTJS_RETURN(expr.prefix ? newValue : oldValue);
  }

  if (auto* member = std::get_if<MemberExpr>(&expr.argument->node)) {
    Value obj;
    { auto _t = evaluate(*member->object); LIGHTJS_RUN_TASK(_t, obj); }
    if (hasError()) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    std::string propName;
    if (member->computed) {
      Value prop;
      { auto _t = evaluate(*member->property); LIGHTJS_RUN_TASK(_t, prop); }
      if (hasError()) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      Value key = isObjectLike(prop) ? toPrimitiveValue(prop, true) : prop;
      if (hasError()) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      propName = toPropertyKeyString(key);
    } else if (auto* idProp = std::get_if<Identifier>(&member->property->node)) {
      propName = idProp->name;
    } else {
      Value prop;
      { auto _t = evaluate(*member->property); LIGHTJS_RUN_TASK(_t, prop); }
      if (hasError()) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      Value key = isObjectLike(prop) ? toPrimitiveValue(prop, true) : prop;
      if (hasError()) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      propName = toPropertyKeyString(key);
    }

    if (obj.isNull() || obj.isUndefined()) {
      throwError(ErrorType::TypeError,
                 "Cannot read properties of " + std::string(obj.isNull() ? "null" : "undefined"));
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    if (obj.isObject()) {
      auto objPtr = obj.getGC<Object>();
      // Handle private field update (#name)
      std::string lookupName = propName;
      if (member->privateIdentifier) {
        GCPtr<Class> cls;
        auto ctorIt = objPtr->properties.find("__constructor__");
        if (ctorIt != objPtr->properties.end() && ctorIt->second.isClass()) {
          cls = ctorIt->second.getGC<Class>();
        }
        GCPtr<Class> ownerClass = activePrivateOwnerClass_ ? activePrivateOwnerClass_ : cls;
        auto isOwnerInClassChain = [&](GCPtr<Class> target, const GCPtr<Class>& owner) -> bool {
          int depth = 0;
          while (target && depth < 128) {
            if (target.get() == owner.get()) {
              return true;
            }
            target = target->superClass;
            depth++;
          }
          return false;
        };
        if (!ownerClass || !cls || !isOwnerInClassChain(cls, ownerClass)) {
          throwError(ErrorType::TypeError,
                     "Cannot read private member " + propName +
                     " from an object whose class did not declare it");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        lookupName = privateStorageKey(ownerClass, propName);
      }
      Value currentValue(Undefined{});
      auto getterIt = objPtr->properties.find("__get_" + lookupName);
      auto setterIt = objPtr->properties.find("__set_" + lookupName);
      if (getterIt != objPtr->properties.end() && getterIt->second.isFunction()) {
        currentValue = callFunction(getterIt->second, {}, obj);
        if (hasError()) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      } else if (auto it = objPtr->properties.find(lookupName); it != objPtr->properties.end()) {
        currentValue = it->second;
      }

      Value oldValue;
      Value newValue;
      if (!applyNumericUpdate(currentValue, oldValue, newValue)) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      if (objPtr->properties.count("__non_writable_" + lookupName)) {
        if (strictMode_) {
          throwError(ErrorType::TypeError,
                     "Cannot assign to read only property '" + lookupName + "'");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      } else if (setterIt != objPtr->properties.end() && setterIt->second.isFunction()) {
        callFunction(setterIt->second, {newValue}, obj);
        if (hasError()) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      } else {
        objPtr->properties[lookupName] = newValue;
      }
      LIGHTJS_RETURN(expr.prefix ? newValue : oldValue);
    }

    if (obj.isClass()) {
      auto clsPtr = obj.getGC<Class>();
      std::string lookupName = propName;
      if (member->privateIdentifier) {
        GCPtr<Class> ownerClass = activePrivateOwnerClass_ ? activePrivateOwnerClass_ : clsPtr;
        if (!ownerClass || ownerClass.get() != clsPtr.get()) {
          throwError(ErrorType::TypeError,
                     "Cannot read private member " + propName +
                     " from an object whose class did not declare it");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        lookupName = privateStorageKey(ownerClass, propName);
      }
      Value currentValue(Undefined{});
      auto getterIt = clsPtr->properties.find("__get_" + lookupName);
      auto setterIt = clsPtr->properties.find("__set_" + lookupName);
      if (getterIt != clsPtr->properties.end() && getterIt->second.isFunction()) {
        currentValue = callFunction(getterIt->second, {}, obj);
        if (hasError()) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      } else if (auto it = clsPtr->properties.find(lookupName); it != clsPtr->properties.end()) {
        currentValue = it->second;
      }

      Value oldValue;
      Value newValue;
      if (!applyNumericUpdate(currentValue, oldValue, newValue)) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      if (clsPtr->properties.count("__non_writable_" + lookupName)) {
        if (strictMode_) {
          throwError(ErrorType::TypeError,
                     "Cannot assign to read only property '" + lookupName + "'");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      } else if (setterIt != clsPtr->properties.end() && setterIt->second.isFunction()) {
        callFunction(setterIt->second, {newValue}, obj);
        if (hasError()) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      } else {
        clsPtr->properties[lookupName] = newValue;
      }
      LIGHTJS_RETURN(expr.prefix ? newValue : oldValue);
    }

    if (obj.isFunction()) {
      auto fnPtr = obj.getGC<Function>();
      Value currentValue(Undefined{});
      auto getterIt = fnPtr->properties.find("__get_" + propName);
      auto setterIt = fnPtr->properties.find("__set_" + propName);
      if (getterIt != fnPtr->properties.end() && getterIt->second.isFunction()) {
        currentValue = callFunction(getterIt->second, {}, obj);
        if (hasError()) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      } else if (auto it = fnPtr->properties.find(propName); it != fnPtr->properties.end()) {
        currentValue = it->second;
      }

      Value oldValue;
      Value newValue;
      if (!applyNumericUpdate(currentValue, oldValue, newValue)) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      if (propName == "name" || propName == "length" ||
          fnPtr->properties.count("__non_writable_" + propName)) {
        if (strictMode_) {
          throwError(ErrorType::TypeError,
                     "Cannot assign to read only property '" + propName + "'");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      } else if (setterIt != fnPtr->properties.end() && setterIt->second.isFunction()) {
        callFunction(setterIt->second, {newValue}, obj);
        if (hasError()) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      } else {
        fnPtr->properties[propName] = newValue;
      }
      LIGHTJS_RETURN(expr.prefix ? newValue : oldValue);
    }

    if (obj.isArray()) {
      auto arrPtr = obj.getGC<Array>();
      size_t index = 0;
      if (parseArrayIndex(propName, index)) {
        Value currentValue(Undefined{});
        auto getterIt = arrPtr->properties.find("__get_" + propName);
        auto setterIt = arrPtr->properties.find("__set_" + propName);
        if (getterIt != arrPtr->properties.end() && getterIt->second.isFunction()) {
          currentValue = callFunction(getterIt->second, {}, obj);
          if (hasError()) {
            LIGHTJS_RETURN(Value(Undefined{}));
          }
        } else if (index < arrPtr->elements.size()) {
          currentValue = arrPtr->elements[index];
        }

        Value oldValue;
        Value newValue;
        if (!applyNumericUpdate(currentValue, oldValue, newValue)) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }

        if (setterIt != arrPtr->properties.end() && setterIt->second.isFunction()) {
          callFunction(setterIt->second, {newValue}, obj);
          if (hasError()) {
            LIGHTJS_RETURN(Value(Undefined{}));
          }
        } else {
          if (index >= arrPtr->elements.size()) {
            size_t oldSize = arrPtr->elements.size();
            arrPtr->elements.resize(index + 1, Value(Undefined{}));
            for (size_t hi = oldSize; hi < index; hi++) {
              arrPtr->properties["__hole_" + std::to_string(hi) + "__"] = Value(true);
            }
          }
          arrPtr->elements[index] = newValue;
          arrPtr->properties.erase("__hole_" + std::to_string(index) + "__");
        }
        LIGHTJS_RETURN(expr.prefix ? newValue : oldValue);
      }

      Value currentValue(Undefined{});
      auto getterIt = arrPtr->properties.find("__get_" + propName);
      auto setterIt = arrPtr->properties.find("__set_" + propName);
      if (getterIt != arrPtr->properties.end() && getterIt->second.isFunction()) {
        currentValue = callFunction(getterIt->second, {}, obj);
        if (hasError()) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      } else if (auto it = arrPtr->properties.find(propName); it != arrPtr->properties.end()) {
        currentValue = it->second;
      }

      Value oldValue;
      Value newValue;
      if (!applyNumericUpdate(currentValue, oldValue, newValue)) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (setterIt != arrPtr->properties.end() && setterIt->second.isFunction()) {
        callFunction(setterIt->second, {newValue}, obj);
        if (hasError()) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      } else {
        arrPtr->properties[propName] = newValue;
      }
      LIGHTJS_RETURN(expr.prefix ? newValue : oldValue);
    }

    if (obj.isTypedArray()) {
      auto taPtr = obj.getGC<TypedArray>();
      size_t index = 0;
      if (!parseArrayIndex(propName, index)) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      Value currentValue;
      if (taPtr->type == TypedArrayType::BigInt64 ||
          taPtr->type == TypedArrayType::BigUint64) {
        if (taPtr->type == TypedArrayType::BigUint64) {
          currentValue = Value(BigInt(bigint::BigIntValue(taPtr->getBigUintElement(index))));
        } else {
          currentValue = Value(BigInt(bigint::BigIntValue(taPtr->getBigIntElement(index))));
        }
      } else {
        currentValue = Value(taPtr->getElement(index));
      }
      Value oldValue;
      Value newValue;
      if (!applyNumericUpdate(currentValue, oldValue, newValue)) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (taPtr->type == TypedArrayType::BigInt64 ||
          taPtr->type == TypedArrayType::BigUint64) {
        if (taPtr->type == TypedArrayType::BigUint64) {
          taPtr->setBigUintElement(index, bigint::toUint64Trunc(newValue.toBigInt()));
        } else {
          taPtr->setBigIntElement(index, bigint::toInt64Trunc(newValue.toBigInt()));
        }
      } else {
        taPtr->setElement(index, newValue.toNumber());
      }
      LIGHTJS_RETURN(expr.prefix ? newValue : oldValue);
    }
  }

  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::evaluateCall(const CallExpr& expr) {
  // Stack overflow protection
  StackGuard guard(stackDepth_, MAX_STACK_DEPTH);
  if (guard.overflowed()) {
    throwError(ErrorType::RangeError, "Maximum call stack size exceeded");
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  auto initializePromiseIntrinsics = [this](const GCPtr<Promise>& p) {
    if (!p) return;
    auto promiseCtor = env_->getRoot()->get("__intrinsic_Promise__");
    if (!promiseCtor || !promiseCtor->isFunction()) {
      promiseCtor = env_->getRoot()->get("Promise");
    }
    if (promiseCtor && promiseCtor->isFunction()) {
      p->properties["__constructor__"] = *promiseCtor;
      auto promiseCtorFn = promiseCtor->getGC<Function>();
      auto protoIt = promiseCtorFn->properties.find("prototype");
      if (protoIt != promiseCtorFn->properties.end() && protoIt->second.isObject()) {
        p->properties["__proto__"] = protoIt->second;
      }
    }
  };

  // Special handling for dynamic import()
  if (auto* id = std::get_if<Identifier>(&expr.callee->node)) {
    if (id->name == "import") {
      // This is a dynamic import - call the global import function
      auto importFunc = env_->get("import");

      if (importFunc && importFunc->isFunction()) {
        std::vector<Value> args;
        for (const auto& arg : expr.arguments) {
          auto argTask = evaluate(*arg);
          LIGHTJS_RUN_TASK_VOID(argTask);
          if (flow_.type != ControlFlow::Type::None) {
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          args.push_back(argTask.result());
        }

        auto func = std::get<GCPtr<Function>>(importFunc->data);
	  if (func->isNative) {
          LIGHTJS_RETURN(func->nativeFunc(args));
        }
      }

      // If we couldn't find the import function, return an error Promise
      auto promise = GarbageCollector::makeGC<Promise>();
      initializePromiseIntrinsics(promise);
      auto err = GarbageCollector::makeGC<Error>(ErrorType::ReferenceError, "import is not defined");
      promise->reject(Value(err));
      LIGHTJS_RETURN(Value(promise));
    }
  }

  // Track the 'this' value for method calls
  Value thisValue = Value(Undefined{});
  Value callee;

  // Check if this is a method call (obj.method())
  if (std::holds_alternative<MemberExpr>(expr.callee->node)) {
    // Evaluate the member expression once and capture the base object from evaluateMember.
    hasLastMemberBase_ = false;
    { auto _t = evaluate(*expr.callee); LIGHTJS_RUN_TASK(_t, callee); }
    if (flow_.type != ControlFlow::Type::None) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (hasLastMemberBase_) {
      thisValue = lastMemberBase_;
    }
  } else {
    if (auto* id = std::get_if<Identifier>(&expr.callee->node)) {
      auto withBaseValue = env_->resolveWithScopeValue(id->name);
      if (withBaseValue) {
        auto withValue = env_->getWithScopeBindingValue(*withBaseValue, id->name, strictMode_);
        if (flow_.type != ControlFlow::Type::None) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (withValue.has_value()) {
          callee = *withValue;
          thisValue = *withBaseValue;
        } else {
          throwError(ErrorType::ReferenceError, "'" + id->name + "' is not defined");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      } else {
        auto val = env_->getIgnoringWith(id->name);
        if (val.has_value()) {
          if (val->isModuleBinding()) {
            const auto& binding = std::get<ModuleBinding>(val->data);
            auto module = binding.module.lock();
            if (!module) {
              throwError(ErrorType::ReferenceError,
                         "Cannot access '" + id->name + "' before initialization");
              LIGHTJS_RETURN(Value(Undefined{}));
            }
            auto exportValue = module->getExport(binding.exportName);
            if (!exportValue) {
              throwError(ErrorType::ReferenceError,
                         "Cannot access '" + id->name + "' before initialization");
              LIGHTJS_RETURN(Value(Undefined{}));
            }
            callee = *exportValue;
          } else {
            callee = *val;
          }
        } else {
          bool foundNamedExpression = false;
          for (auto it = activeNamedExpressionStack_.rbegin();
               it != activeNamedExpressionStack_.rend();
               ++it) {
            const auto& fn = *it;
            auto nameIt = fn->properties.find("name");
            if (nameIt != fn->properties.end() &&
                nameIt->second.isString() &&
                nameIt->second.toString() == id->name) {
              callee = Value(fn);
              foundNamedExpression = true;
              break;
            }
          }
          if (!foundNamedExpression) {
            throwError(ErrorType::ReferenceError, "'" + id->name + "' is not defined");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
        }
      }
    } else {
      { auto _t = evaluate(*expr.callee); LIGHTJS_RUN_TASK(_t, callee); }
      if (flow_.type != ControlFlow::Type::None) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
    }
  }

  // Optional call short-circuits without evaluating arguments.
  // inOptionalChain propagates through the entire chain (e.g., a?.b.c(x))
  if ((expr.optional || expr.inOptionalChain) && (callee.isNull() || callee.isUndefined())) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  std::vector<Value> args;
  for (const auto& arg : expr.arguments) {
    // Check if this is a spread element
    if (auto* spread = std::get_if<SpreadElement>(&arg->node)) {
      // Evaluate the argument
      Value val;
      { auto _t = evaluate(*spread->argument); LIGHTJS_RUN_TASK(_t, val); }
      if (flow_.type != ControlFlow::Type::None) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      auto iterRecOpt = getIterator(val);
      if (!iterRecOpt) {
        throwError(ErrorType::TypeError, val.toString() + " is not iterable");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      auto& iterRec = *iterRecOpt;
      while (true) {
        Value step = iteratorNext(iterRec);
        if (flow_.type == ControlFlow::Type::Throw) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (!step.isObject()) {
          throwError(ErrorType::TypeError, "Iterator result is not an object");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        auto stepObj = step.getGC<Object>();
        bool done = false;
        auto doneGetterIt = stepObj->properties.find("__get_done");
        if (doneGetterIt != stepObj->properties.end() && doneGetterIt->second.isFunction()) {
          Value doneVal = callFunction(doneGetterIt->second, {}, step);
          if (flow_.type == ControlFlow::Type::Throw) {
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          done = doneVal.toBool();
        } else {
          auto doneIt = stepObj->properties.find("done");
          done = (doneIt != stepObj->properties.end() && doneIt->second.toBool());
        }
        if (done) {
          break;
        }
        Value nextArg = Value(Undefined{});
        auto valueGetterIt = stepObj->properties.find("__get_value");
        if (valueGetterIt != stepObj->properties.end() && valueGetterIt->second.isFunction()) {
          nextArg = callFunction(valueGetterIt->second, {}, step);
          if (flow_.type == ControlFlow::Type::Throw) {
            LIGHTJS_RETURN(Value(Undefined{}));
          }
        } else {
          auto valueIt = stepObj->properties.find("value");
          if (valueIt != stepObj->properties.end()) {
            nextArg = valueIt->second;
          }
        }
        args.push_back(nextArg);
      }
    } else {
      Value argVal;
      { auto _t = evaluate(*arg); LIGHTJS_RUN_TASK(_t, argVal); }
      if (flow_.type != ControlFlow::Type::None) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      args.push_back(argVal);
    }
  }

  // Handle super() calls as construction (ES2020: super() calls [[Construct]])
  if (std::holds_alternative<SuperExpr>(expr.callee->node)) {
    // super() uses the current constructor's [[Prototype]], not newTarget's.
    // That preserves nested derived-constructor behavior while still observing
    // dynamic prototype changes (e.g. Object.setPrototypeOf(C, ...)).
    bool resolvedSuperCtor = false;
    if (auto currentCtor = env_->get("__current_constructor__")) {
      if (currentCtor->isClass()) {
        auto cls = currentCtor->getGC<Class>();
        auto protoIt = cls->properties.find("__proto__");
        if (protoIt != cls->properties.end()) {
          callee = protoIt->second;
          resolvedSuperCtor = true;
        }
      }
    }
    if (!resolvedSuperCtor) {
      if (auto superCtor = env_->get("__super_constructor__")) {
        callee = *superCtor;
      }
    }
    bool hasSuperCalledSlot = false;
    bool superAlreadyCalled = false;
    if (auto superCalled = env_->get("__super_called__");
        superCalled && superCalled->isBool()) {
      hasSuperCalledSlot = true;
      superAlreadyCalled = superCalled->toBool();
    }
    Value newTarget = Value(Undefined{});
    if (auto nt = env_->get("__new_target__")) {
      newTarget = *nt;
    }
    Value result;
    { auto _t = constructValue(callee, args, newTarget); LIGHTJS_RUN_TASK(_t, result); }
    if (flow_.type != ControlFlow::Type::None) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    // In derived constructors, a second super() call is a ReferenceError,
    // but only after constructing the super result.
    if (hasSuperCalledSlot && superAlreadyCalled) {
      throwError(ErrorType::ReferenceError, "Super constructor may only be called once");
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    // Update 'this' binding to the super-constructed value.
    env_->set("this", result);
    if (hasSuperCalledSlot) {
      env_->set("__super_called__", Value(true));
    }

    // For derived constructors, initialize this class's instance elements
    // immediately after the first super() bind.
    if (hasSuperCalledSlot && activePrivateOwnerClass_) {
      Value ctorTag(activePrivateOwnerClass_);
      defineClassElementOnValue(result, "__constructor__", ctorTag, false);
      auto initTask = initializeClassInstanceElements(activePrivateOwnerClass_, result);
      LIGHTJS_RUN_TASK_VOID(initTask);
      if (flow_.type != ControlFlow::Type::None) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
    }
    LIGHTJS_RETURN(result);
  }

  if (inTailPosition_ && strictMode_ && callee.isFunction() && activeFunction_) {
    auto calleeFunc = callee.getGC<Function>();
    if (!calleeFunc->isNative &&
        !calleeFunc->isAsync &&
        !calleeFunc->isGenerator &&
        calleeFunc.get() == activeFunction_.get()) {
      pendingSelfTailCall_ = true;
      pendingSelfTailArgs_ = std::move(args);
      pendingSelfTailThis_ = thisValue;
      LIGHTJS_RETURN(Value(Undefined{}));
    }
  }

  bool isDirectEvalCall = false;
  if (!expr.optional &&
      !expr.inOptionalChain &&
      callee.isFunction() &&
      std::holds_alternative<Identifier>(expr.callee->node)) {
    auto* id = std::get_if<Identifier>(&expr.callee->node);
    if (id && id->name == "eval") {
      auto evalFn = callee.getGC<Function>();
      auto intrinsicEvalIt = evalFn->properties.find("__is_intrinsic_eval__");
      isDirectEvalCall = intrinsicEvalIt != evalFn->properties.end() &&
                         intrinsicEvalIt->second.isBool() &&
                         intrinsicEvalIt->second.toBool();
      if (isDirectEvalCall) {
        auto* rootEnv = env_ ? env_->getRoot() : nullptr;
        auto rootEval = rootEnv ? rootEnv->get("eval") : std::nullopt;
        if (!rootEval || !rootEval->isFunction() ||
            rootEval->getGC<Function>().get() != evalFn.get()) {
          isDirectEvalCall = false;
        }
      }
    }
  }

  // Handle Proxy apply trap for callable proxies
  if (callee.isProxy()) {
    auto proxyPtr = callee.getGC<Proxy>();
    if (proxyPtr->handler && proxyPtr->handler->isObject()) {
      auto handlerObj = std::get<GCPtr<Object>>(proxyPtr->handler->data);
      auto trapIt = handlerObj->properties.find("apply");
      if (trapIt != handlerObj->properties.end() && trapIt->second.isFunction()) {
        auto trap = trapIt->second.getGC<Function>();
        // Create args array
        auto argsArray = GarbageCollector::makeGC<Array>();
        argsArray->elements = args;
        // Call apply trap: handler.apply(target, thisArg, argumentsList)
        std::vector<Value> trapArgs = {*proxyPtr->target, thisValue, Value(argsArray)};
        if (trap->isNative) {
          LIGHTJS_RETURN(trap->nativeFunc(trapArgs));
        } else {
          LIGHTJS_RETURN(invokeFunction(trap, trapArgs, Value(Undefined{})));
        }
      }
    }
    // No apply trap - call the target directly if it's a function
    if (proxyPtr->target && proxyPtr->target->isFunction()) {
      LIGHTJS_RETURN(callFunction(*proxyPtr->target, args, thisValue));
    }
  }

  if (callee.isFunction()) {
    bool prevPendingDirectEvalCall = pendingDirectEvalCall_;
    pendingDirectEvalCall_ = isDirectEvalCall;
    Value callResult = callFunction(callee, args, thisValue);
    pendingDirectEvalCall_ = prevPendingDirectEvalCall;
    LIGHTJS_RETURN(callResult);
  }

  // Some built-ins are represented as wrapper objects with a callable constructor.
  if (callee.isObject()) {
    auto objPtr = callee.getGC<Object>();
    auto callableIt = objPtr->properties.find("__callable_object__");
    bool isCallableWrapper = callableIt != objPtr->properties.end() &&
                             callableIt->second.isBool() &&
                             callableIt->second.toBool();
    if (isCallableWrapper) {
      auto ctorIt = objPtr->properties.find("constructor");
      if (ctorIt != objPtr->properties.end() && ctorIt->second.isFunction()) {
        LIGHTJS_RETURN(callFunction(ctorIt->second, args, thisValue));
      }
    }
  }

  // Throw TypeError if trying to call a non-function
  throwError(ErrorType::TypeError, callee.toString() + " is not a function");
  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::evaluateMember(const MemberExpr& expr) {
  auto objTask = evaluate(*expr.object);
  Value obj;
  LIGHTJS_RUN_TASK(objTask, obj);

  // Propagate errors (e.g. ReferenceError from undeclared variable)
  if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));

  // Optional chaining: if object is null or undefined, return undefined.
  // This check must happen before evaluating computed keys to preserve short-circuiting.
  // inOptionalChain propagates short-circuiting through the entire chain (e.g., a?.b.c.d)
  if ((expr.optional || expr.inOptionalChain) && (obj.isNull() || obj.isUndefined())) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  // TypeError for property access on null/undefined (non-optional)
  if (!expr.optional && !expr.inOptionalChain && (obj.isNull() || obj.isUndefined())) {
    // Evaluate computed property key first per spec (LHS before RHS)
    if (expr.computed) {
      auto propTask = evaluate(*expr.property);
      LIGHTJS_RUN_TASK_VOID(propTask);
    }
    std::string propName;
    if (expr.computed) {
      // Already evaluated above, re-evaluate to get name for error message
    } else if (auto* id = std::get_if<Identifier>(&expr.property->node)) {
      propName = id->name;
    }
    throwError(ErrorType::TypeError,
      "Cannot read properties of " + std::string(obj.isNull() ? "null" : "undefined") +
      (propName.empty() ? "" : " (reading '" + propName + "')"));
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  std::string propName;
  std::optional<size_t> typedArrayNumericIndex;
  if (expr.computed) {
    auto propTask = evaluate(*expr.property);
    LIGHTJS_RUN_TASK_VOID(propTask);
    Value key = propTask.result();
    if (isObjectLike(key)) {
      key = toPrimitiveValue(key, true);
      if (hasError()) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
    }
    if (key.isNumber()) {
      double numericKey = key.toNumber();
      if (std::isfinite(numericKey) && numericKey >= 0.0 && numericKey <= 4294967294.0) {
        double integerKey = std::trunc(numericKey);
        if (integerKey == numericKey) {
          typedArrayNumericIndex = static_cast<size_t>(integerKey);
        }
      }
    }
    propName = toPropertyKeyString(key);
  } else {
    if (auto* id = std::get_if<Identifier>(&expr.property->node)) {
      propName = id->name;
    }
  }

  // Remember the current base object so method calls can bind `this` without
  // re-evaluating side-effectful member objects.
  bool isSuperAccess = expr.object && std::holds_alternative<SuperExpr>(expr.object->node);
  if (isSuperAccess) {
    // In derived constructors, `this` is uninitialized before super().
    if (auto superCalled = env_->get("__super_called__");
        superCalled && superCalled->isBool() && !superCalled->toBool()) {
      throwError(ErrorType::ReferenceError,
                 "Must call super constructor in derived class before accessing 'this'");
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (auto thisValue = env_->get("this")) {
      lastMemberBase_ = *thisValue;
    } else {
      lastMemberBase_ = Value(Undefined{});
    }
  } else {
    lastMemberBase_ = obj;
  }
  hasLastMemberBase_ = true;

  Value privateAccessReceiver = obj;
  if (expr.privateIdentifier) {
    int proxyDepth = 0;
    while (obj.isProxy() && proxyDepth < 16) {
      auto proxyPtr = obj.getGC<Proxy>();
      if (!proxyPtr->target) {
        throwError(ErrorType::TypeError,
                   "Cannot read private member " + propName +
                       " from an object whose class did not declare it");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      obj = *proxyPtr->target;
      proxyDepth++;
    }
    if (obj.isProxy()) {
      throwError(ErrorType::TypeError,
                 "Cannot read private member " + propName +
                     " from an object whose class did not declare it");
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (!obj.isClass() && !isObjectLike(obj)) {
      throwError(ErrorType::TypeError,
                 "Cannot read private member " + propName +
                     " from an object whose class did not declare it");
      LIGHTJS_RETURN(Value(Undefined{}));
    }
  }

  if (!expr.privateIdentifier && typedArrayNumericIndex.has_value() && obj.isTypedArray()) {
    auto ta = obj.getGC<TypedArray>();
    size_t index = *typedArrayNumericIndex;
    if (index >= ta->currentLength()) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (ta->type == TypedArrayType::BigInt64 || ta->type == TypedArrayType::BigUint64) {
      if (ta->type == TypedArrayType::BigUint64) {
        LIGHTJS_RETURN(Value(BigInt(bigint::BigIntValue(ta->getBigUintElement(index)))));
      }
      LIGHTJS_RETURN(Value(BigInt(ta->getBigIntElement(index))));
    }
    LIGHTJS_RETURN(Value(ta->getElement(index)));
  }

  // BigInt primitive member access
  if (obj.isBigInt()) {
    auto bigintValue = obj.toBigInt();

    if (propName == "valueOf") {
      auto valueOfFn = GarbageCollector::makeGC<Function>();
      valueOfFn->isNative = true;
      valueOfFn->properties["__throw_on_new__"] = Value(true);
      valueOfFn->nativeFunc = [bigintValue](const std::vector<Value>&) -> Value {
        return Value(BigInt(bigintValue));
      };
      LIGHTJS_RETURN(Value(valueOfFn));
    }

    if (propName == "toString") {
      auto toStringFn = GarbageCollector::makeGC<Function>();
      toStringFn->isNative = true;
      toStringFn->properties["__throw_on_new__"] = Value(true);
      toStringFn->nativeFunc = [bigintValue](const std::vector<Value>& args) -> Value {
        int radix = 10;
        if (!args.empty() && !args[0].isUndefined()) {
          int r = static_cast<int>(std::trunc(args[0].toNumber()));
          if (r < 2 || r > 36) {
            throw std::runtime_error("RangeError: radix must be between 2 and 36");
          }
          radix = r;
        }
        return Value(bigint::toString(bigintValue, radix));
      };
      LIGHTJS_RETURN(Value(toStringFn));
    }

    // Fallback to BigInt.prototype for user-defined properties.
    if (auto bigIntCtor = env_->get("BigInt")) {
      auto wrapper = GarbageCollector::makeGC<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      wrapper->properties["__primitive_value__"] = obj;
      auto [foundProto, proto] = getPropertyForPrimitive(*bigIntCtor, "prototype");
      if (foundProto && (proto.isObject() || proto.isNull())) {
        wrapper->properties["__proto__"] = proto;
      }
      auto [found, value] = getPropertyForPrimitive(Value(wrapper), propName);
      if (found) {
        LIGHTJS_RETURN(value);
      }
    }
  }

  // Symbol primitive member access - delegate to Symbol.prototype via boxing
  if (obj.isSymbol()) {
    if (auto symbolCtor = env_->get("Symbol")) {
      auto wrapper = GarbageCollector::makeGC<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      wrapper->properties["__primitive_value__"] = obj;
      auto [foundProto, proto] = getPropertyForPrimitive(*symbolCtor, "prototype");
      if (foundProto && (proto.isObject() || proto.isNull())) {
        wrapper->properties["__proto__"] = proto;
      }
      auto [found, value] = getPropertyForPrimitive(Value(wrapper), propName);
      if (found) {
        LIGHTJS_RETURN(value);
      }
    }
  }

  // Proxy trap handling - intercept get operations.
  // Private field access bypasses Proxy [[Get]] and is handled below.
  if (obj.isProxy() && !expr.privateIdentifier) {
    auto proxyPtr = obj.getGC<Proxy>();

    // Check if handler has a 'get' trap
    if (proxyPtr->handler && proxyPtr->handler->isObject()) {
      auto handlerObj = std::get<GCPtr<Object>>(proxyPtr->handler->data);
      auto getTrapIt = handlerObj->properties.find("get");

      if (getTrapIt != handlerObj->properties.end() && getTrapIt->second.isFunction()) {
        // Call the get trap: handler.get(target, property, receiver)
        auto getTrap = getTrapIt->second.getGC<Function>();
        std::vector<Value> trapArgs = {
          *proxyPtr->target,
          toPropertyKeyValue(propName),
          obj  // receiver is the proxy itself
        };
        if (getTrap->isNative) {
          LIGHTJS_RETURN(getTrap->nativeFunc(trapArgs));
        } else {
          LIGHTJS_RETURN(invokeFunction(getTrap, trapArgs, Value(Undefined{})));
        }
      }
    }

    // No trap, fall through to default behavior on target
    if (proxyPtr->target && proxyPtr->target->isObject()) {
      auto targetObj = std::get<GCPtr<Object>>(proxyPtr->target->data);
      auto it = targetObj->properties.find(propName);
      if (it != targetObj->properties.end()) {
        LIGHTJS_RETURN(it->second);
      }
    }
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  const auto& iteratorKey = WellKnownSymbols::iteratorKey();

  if (obj.isPromise()) {
    auto promisePtr = obj.getGC<Promise>();

    // Promise own accessor/data properties can override built-in behavior.
    std::string promiseGetterName = "__get_" + propName;
    auto promiseGetterIt = promisePtr->properties.find(promiseGetterName);
    if (promiseGetterIt != promisePtr->properties.end() && promiseGetterIt->second.isFunction()) {
      LIGHTJS_RETURN(callFunction(promiseGetterIt->second, {}, obj));
    }
    auto promiseOwnIt = promisePtr->properties.find(propName);
    if (promiseOwnIt != promisePtr->properties.end()) {
      LIGHTJS_RETURN(promiseOwnIt->second);
    }

    auto getIntrinsicPromisePrototype = [&]() -> GCPtr<Object> {
      Value promiseCtorValue = Value(Undefined{});
      if (auto intrinsicPromise = env_->get("__intrinsic_Promise__")) {
        promiseCtorValue = *intrinsicPromise;
      } else if (auto promiseCtor = env_->get("Promise")) {
        promiseCtorValue = *promiseCtor;
      }
      if (!promiseCtorValue.isFunction()) {
        return nullptr;
      }
      auto ctorFn = promiseCtorValue.getGC<Function>();
      auto protoIt = ctorFn->properties.find("prototype");
      if (protoIt == ctorFn->properties.end() || !protoIt->second.isObject()) {
        return nullptr;
      }
      return protoIt->second.getGC<Object>();
    };

    if (auto promiseProto = getIntrinsicPromisePrototype()) {
      auto getterIt = promiseProto->properties.find("__get_" + propName);
      if (getterIt != promiseProto->properties.end() && getterIt->second.isFunction()) {
        LIGHTJS_RETURN(callFunction(getterIt->second, {}, obj));
      }
      auto protoIt = promiseProto->properties.find(propName);
      if (protoIt != promiseProto->properties.end()) {
        LIGHTJS_RETURN(protoIt->second);
      }
    }

    if (propName == "constructor") {
      auto ctorIt = promisePtr->properties.find("__constructor__");
      if (ctorIt != promisePtr->properties.end()) {
        LIGHTJS_RETURN(ctorIt->second);
      }
      if (auto intrinsic = env_->get("__intrinsic_Promise__")) {
        LIGHTJS_RETURN(*intrinsic);
      }
      if (auto ctor = env_->get("Promise")) {
        LIGHTJS_RETURN(*ctor);
      }
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    // Add toString method for Promise
    if (propName == "toString") {
      auto toStringFn = GarbageCollector::makeGC<Function>();
      toStringFn->isNative = true;
      toStringFn->nativeFunc = [](const std::vector<Value>&) -> Value {
        return Value("[Promise]");
      };
      LIGHTJS_RETURN(Value(toStringFn));
    }

    // Promise.prototype.then(onFulfilled, onRejected)
    if (propName == "then") {
      auto thenFn = GarbageCollector::makeGC<Function>();
      thenFn->isNative = true;
      thenFn->nativeFunc = [this, promisePtr](const std::vector<Value>& args) -> Value {
        std::function<Value(Value)> onFulfilled = nullptr;
        std::function<Value(Value)> onRejected = nullptr;

        // Get onFulfilled callback if provided
        if (!args.empty() && args[0].isFunction()) {
          auto callback = args[0].getGC<Function>();
          onFulfilled = [this, callback](Value val) -> Value {
            Value out = invokeFunction(callback, {val}, Value(Undefined{}));
            if (hasError()) {
              Value err = getError();
              clearError();
              throw std::runtime_error(err.toString());
            }
            return out;
          };
        }

        // Get onRejected callback if provided
        if (args.size() > 1 && args[1].isFunction()) {
          auto callback = args[1].getGC<Function>();
          onRejected = [this, callback](Value val) -> Value {
            Value out = invokeFunction(callback, {val}, Value(Undefined{}));
            if (hasError()) {
              Value err = getError();
              clearError();
              throw std::runtime_error(err.toString());
            }
            return out;
          };
        }

        auto chainedPromise = promisePtr->then(onFulfilled, onRejected);
        return Value(chainedPromise);
      };
      LIGHTJS_RETURN(Value(thenFn));
    }

    // Promise.prototype.catch(onRejected)
    if (propName == "catch") {
      auto catchFn = GarbageCollector::makeGC<Function>();
      catchFn->isNative = true;
      catchFn->nativeFunc = [this, promisePtr](const std::vector<Value>& args) -> Value {
        std::function<Value(Value)> onRejected = nullptr;

        if (!args.empty() && args[0].isFunction()) {
          auto callback = args[0].getGC<Function>();
          onRejected = [this, callback](Value val) -> Value {
            Value out = invokeFunction(callback, {val}, Value(Undefined{}));
            if (hasError()) {
              Value err = getError();
              clearError();
              throw std::runtime_error(err.toString());
            }
            return out;
          };
        }

        auto chainedPromise = promisePtr->catch_(onRejected);
        return Value(chainedPromise);
      };
      LIGHTJS_RETURN(Value(catchFn));
    }

    // Promise.prototype.finally(onFinally)
    if (propName == "finally") {
      auto finallyFn = GarbageCollector::makeGC<Function>();
      finallyFn->isNative = true;
      finallyFn->nativeFunc = [this, promisePtr](const std::vector<Value>& args) -> Value {
        std::function<Value()> onFinally = nullptr;

        if (!args.empty() && args[0].isFunction()) {
          auto callback = args[0].getGC<Function>();
          onFinally = [this, callback]() -> Value {
            Value out = invokeFunction(callback, {}, Value(Undefined{}));
            if (hasError()) {
              Value err = getError();
              clearError();
              throw std::runtime_error(err.toString());
            }
            return out;
          };
        }

        auto chainedPromise = promisePtr->finally(onFinally);
        return Value(chainedPromise);
      };
      LIGHTJS_RETURN(Value(finallyFn));
    }

    if (promisePtr->state == PromiseState::Fulfilled) {
      Value resolvedValue = promisePtr->result;
      if (resolvedValue.isObject()) {
        auto objPtr = resolvedValue.getGC<Object>();
        auto it = objPtr->properties.find(propName);
        if (it != objPtr->properties.end()) {
          LIGHTJS_RETURN(it->second);
        }
      }
    }
  }

  // ArrayBuffer property access
  if (obj.isArrayBuffer()) {
    auto [found, value] = getPropertyForPrimitive(obj, propName);
    if (hasError()) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (found) {
      LIGHTJS_RETURN(value);
    }
  }

  // DataView property and method access
  if (obj.isDataView()) {
    auto [found, value] = getPropertyForPrimitive(obj, propName);
    if (hasError()) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (found) {
      LIGHTJS_RETURN(value);
    }

    auto viewPtr = obj.getGC<DataView>();

    if (propName == "buffer") {
      LIGHTJS_RETURN(Value(viewPtr->buffer));
    }
    if (propName == "byteOffset") {
      LIGHTJS_RETURN(Value(static_cast<double>(viewPtr->byteOffset)));
    }
    if (propName == "byteLength") {
      LIGHTJS_RETURN(Value(static_cast<double>(viewPtr->byteLength)));
    }

    // DataView get methods
    if (propName == "getInt8") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "getInt8 requires offset"));
        return Value(static_cast<double>(viewPtr->getInt8(static_cast<size_t>(args[0].toNumber()))));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getUint8") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "getUint8 requires offset"));
        return Value(static_cast<double>(viewPtr->getUint8(static_cast<size_t>(args[0].toNumber()))));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getInt16") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "getInt16 requires offset"));
        bool littleEndian = args.size() > 1 ? args[1].toBool() : false;
        return Value(static_cast<double>(viewPtr->getInt16(static_cast<size_t>(args[0].toNumber()), littleEndian)));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getUint16") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "getUint16 requires offset"));
        bool littleEndian = args.size() > 1 ? args[1].toBool() : false;
        return Value(static_cast<double>(viewPtr->getUint16(static_cast<size_t>(args[0].toNumber()), littleEndian)));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getInt32") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "getInt32 requires offset"));
        bool littleEndian = args.size() > 1 ? args[1].toBool() : false;
        return Value(static_cast<double>(viewPtr->getInt32(static_cast<size_t>(args[0].toNumber()), littleEndian)));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getUint32") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "getUint32 requires offset"));
        bool littleEndian = args.size() > 1 ? args[1].toBool() : false;
        return Value(static_cast<double>(viewPtr->getUint32(static_cast<size_t>(args[0].toNumber()), littleEndian)));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getFloat32") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "getFloat32 requires offset"));
        bool littleEndian = args.size() > 1 ? args[1].toBool() : false;
        return Value(static_cast<double>(viewPtr->getFloat32(static_cast<size_t>(args[0].toNumber()), littleEndian)));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getFloat64") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "getFloat64 requires offset"));
        bool littleEndian = args.size() > 1 ? args[1].toBool() : false;
        return Value(viewPtr->getFloat64(static_cast<size_t>(args[0].toNumber()), littleEndian));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getBigInt64") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "getBigInt64 requires offset"));
        bool littleEndian = args.size() > 1 ? args[1].toBool() : false;
        return Value(BigInt(viewPtr->getBigInt64(static_cast<size_t>(args[0].toNumber()), littleEndian)));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getBigUint64") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "getBigUint64 requires offset"));
        bool littleEndian = args.size() > 1 ? args[1].toBool() : false;
        return Value(BigInt(bigint::BigIntValue(viewPtr->getBigUint64(static_cast<size_t>(args[0].toNumber()), littleEndian))));
      };
      LIGHTJS_RETURN(Value(fn));
    }

    // DataView set methods
    if (propName == "setInt8") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "setInt8 requires offset and value"));
        viewPtr->setInt8(static_cast<size_t>(args[0].toNumber()), static_cast<int8_t>(args[1].toNumber()));
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "setUint8") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "setUint8 requires offset and value"));
        viewPtr->setUint8(static_cast<size_t>(args[0].toNumber()), static_cast<uint8_t>(args[1].toNumber()));
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "setInt16") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "setInt16 requires offset and value"));
        bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
        viewPtr->setInt16(static_cast<size_t>(args[0].toNumber()), static_cast<int16_t>(args[1].toNumber()), littleEndian);
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "setUint16") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "setUint16 requires offset and value"));
        bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
        viewPtr->setUint16(static_cast<size_t>(args[0].toNumber()), static_cast<uint16_t>(args[1].toNumber()), littleEndian);
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "setInt32") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "setInt32 requires offset and value"));
        bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
        viewPtr->setInt32(static_cast<size_t>(args[0].toNumber()), static_cast<int32_t>(args[1].toNumber()), littleEndian);
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "setUint32") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "setUint32 requires offset and value"));
        bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
        viewPtr->setUint32(static_cast<size_t>(args[0].toNumber()), static_cast<uint32_t>(args[1].toNumber()), littleEndian);
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "setFloat32") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "setFloat32 requires offset and value"));
        bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
        viewPtr->setFloat32(static_cast<size_t>(args[0].toNumber()), static_cast<float>(args[1].toNumber()), littleEndian);
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "setFloat64") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "setFloat64 requires offset and value"));
        bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
        viewPtr->setFloat64(static_cast<size_t>(args[0].toNumber()), args[1].toNumber(), littleEndian);
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "setBigInt64") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "setBigInt64 requires offset and value"));
        bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
        viewPtr->setBigInt64(static_cast<size_t>(args[0].toNumber()), bigint::toInt64Trunc(args[1].toBigInt()), littleEndian);
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "setBigUint64") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "setBigUint64 requires offset and value"));
        bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
        viewPtr->setBigUint64(static_cast<size_t>(args[0].toNumber()), bigint::toUint64Trunc(args[1].toBigInt()), littleEndian);
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
  }

  // ReadableStream property and method access
  if (obj.isReadableStream()) {
    auto streamPtr = obj.getGC<ReadableStream>();

    if (propName == "locked") {
      LIGHTJS_RETURN(Value(streamPtr->locked));
    }

    if (propName == "getReader") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [streamPtr](const std::vector<Value>& args) -> Value {
        auto reader = streamPtr->getReader();
        if (!reader) {
          return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "ReadableStream is already locked"));
        }
        // Return reader wrapped in an Object for now
        auto readerObj = GarbageCollector::makeGC<Object>();
        readerObj->properties["__reader__"] = Value(true);

        // Add read method
        auto readFn = GarbageCollector::makeGC<Function>();
        readFn->isNative = true;
        readFn->nativeFunc = [reader](const std::vector<Value>& args) -> Value {
          return Value(reader->read());
        };
        readerObj->properties["read"] = Value(readFn);

        // Add releaseLock method
        auto releaseFn = GarbageCollector::makeGC<Function>();
        releaseFn->isNative = true;
        releaseFn->nativeFunc = [reader](const std::vector<Value>& args) -> Value {
          reader->releaseLock();
          return Value(Undefined{});
        };
        readerObj->properties["releaseLock"] = Value(releaseFn);

        // Add closed property (Promise)
        readerObj->properties["closed"] = Value(reader->closedPromise);

        return Value(readerObj);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "cancel") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [streamPtr](const std::vector<Value>& args) -> Value {
        Value reason = args.empty() ? Value(Undefined{}) : args[0];
        return Value(streamPtr->cancel(reason));
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "pipeTo") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [streamPtr](const std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].isWritableStream()) {
          return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "pipeTo requires a WritableStream"));
        }
        auto dest = args[0].getGC<WritableStream>();
        return Value(streamPtr->pipeTo(dest));
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "pipeThrough") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [streamPtr](const std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].isTransformStream()) {
          return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "pipeThrough requires a TransformStream"));
        }
        auto transform = args[0].getGC<TransformStream>();
        auto result = streamPtr->pipeThrough(transform);
        return result ? Value(result) : Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "tee") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [streamPtr](const std::vector<Value>& args) -> Value {
        auto [branch1, branch2] = streamPtr->tee();
        auto result = GarbageCollector::makeGC<Array>();
        result->elements.push_back(Value(branch1));
        result->elements.push_back(Value(branch2));
        return Value(result);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    // Symbol.asyncIterator - returns async iterator for for-await-of
    const std::string& asyncIteratorKey = WellKnownSymbols::asyncIteratorKey();
    if (propName == asyncIteratorKey) {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [streamPtr](const std::vector<Value>& args) -> Value {
        // Return an async iterator object
        auto iteratorObj = GarbageCollector::makeGC<Object>();

        // Get reader for the stream
        auto reader = streamPtr->getReader();
        if (!reader) {
          return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "ReadableStream is already locked"));
        }

        // next() method returns Promise<{value, done}>
        auto nextFn = GarbageCollector::makeGC<Function>();
        nextFn->isNative = true;
        nextFn->nativeFunc = [reader](const std::vector<Value>& args) -> Value {
          return Value(reader->read());
        };
        iteratorObj->properties["next"] = Value(nextFn);

        // return() method for early termination
        auto returnFn = GarbageCollector::makeGC<Function>();
        returnFn->isNative = true;
        returnFn->nativeFunc = [reader](const std::vector<Value>& args) -> Value {
          reader->releaseLock();
          auto promise = GarbageCollector::makeGC<Promise>();
          auto resultObj = GarbageCollector::makeGC<Object>();
          resultObj->properties["value"] = args.empty() ? Value(Undefined{}) : args[0];
          resultObj->properties["done"] = true;
          promise->resolve(Value(resultObj));
          return Value(promise);
        };
        iteratorObj->properties["return"] = Value(returnFn);

        return Value(iteratorObj);
      };
      LIGHTJS_RETURN(Value(fn));
    }
  }

  // WritableStream property and method access
  if (obj.isWritableStream()) {
    auto streamPtr = obj.getGC<WritableStream>();

    if (propName == "locked") {
      LIGHTJS_RETURN(Value(streamPtr->locked));
    }

    if (propName == "getWriter") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [streamPtr](const std::vector<Value>& args) -> Value {
        auto writer = streamPtr->getWriter();
        if (!writer) {
          return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "WritableStream is already locked"));
        }
        // Return writer wrapped in an Object
        auto writerObj = GarbageCollector::makeGC<Object>();
        writerObj->properties["__writer__"] = Value(true);

        // Add write method
        auto writeFn = GarbageCollector::makeGC<Function>();
        writeFn->isNative = true;
        writeFn->nativeFunc = [writer](const std::vector<Value>& args) -> Value {
          Value chunk = args.empty() ? Value(Undefined{}) : args[0];
          return Value(writer->write(chunk));
        };
        writerObj->properties["write"] = Value(writeFn);

        // Add close method
        auto closeFn = GarbageCollector::makeGC<Function>();
        closeFn->isNative = true;
        closeFn->nativeFunc = [writer](const std::vector<Value>& args) -> Value {
          return Value(writer->close());
        };
        writerObj->properties["close"] = Value(closeFn);

        // Add abort method
        auto abortFn = GarbageCollector::makeGC<Function>();
        abortFn->isNative = true;
        abortFn->nativeFunc = [writer](const std::vector<Value>& args) -> Value {
          Value reason = args.empty() ? Value(Undefined{}) : args[0];
          return Value(writer->abort(reason));
        };
        writerObj->properties["abort"] = Value(abortFn);

        // Add releaseLock method
        auto releaseFn = GarbageCollector::makeGC<Function>();
        releaseFn->isNative = true;
        releaseFn->nativeFunc = [writer](const std::vector<Value>& args) -> Value {
          writer->releaseLock();
          return Value(Undefined{});
        };
        writerObj->properties["releaseLock"] = Value(releaseFn);

        // Add closed property (Promise)
        writerObj->properties["closed"] = Value(writer->closedPromise);

        // Add ready property (Promise)
        writerObj->properties["ready"] = Value(writer->readyPromise);

        // Add desiredSize property
        writerObj->properties["desiredSize"] = Value(writer->desiredSize());

        return Value(writerObj);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "abort") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [streamPtr](const std::vector<Value>& args) -> Value {
        Value reason = args.empty() ? Value(Undefined{}) : args[0];
        return Value(streamPtr->abort(reason));
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "close") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [streamPtr](const std::vector<Value>& args) -> Value {
        return Value(streamPtr->close());
      };
      LIGHTJS_RETURN(Value(fn));
    }
  }

  // TransformStream property access
  if (obj.isTransformStream()) {
    auto streamPtr = obj.getGC<TransformStream>();

    if (propName == "readable") {
      LIGHTJS_RETURN(Value(streamPtr->readable));
    }

    if (propName == "writable") {
      LIGHTJS_RETURN(Value(streamPtr->writable));
    }
  }

  if (obj.isClass()) {
    auto clsPtr = obj.getGC<Class>();
    // hasOwnProperty from Object.prototype / Function.prototype chain.
    if (propName == "hasOwnProperty") {
      auto hopFn = GarbageCollector::makeGC<Function>();
      hopFn->isNative = true;
      hopFn->nativeFunc = [clsPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(false);
        std::string key = valueToPropertyKey(args[0]);
        // Internal properties are not visible
        if (key.size() >= 4 && key.substr(0, 2) == "__" && key.substr(key.size() - 2) == "__") return Value(false);
        if (key.size() > 6 && (key.substr(0, 6) == "__get_" || key.substr(0, 6) == "__set_")) return Value(false);
        if (key.size() > 10 && key.substr(0, 10) == "__non_enum") return Value(false);
        if (key.size() > 14 && key.substr(0, 14) == "__non_writable") return Value(false);
        if (key.size() > 18 && key.substr(0, 18) == "__non_configurable") return Value(false);
        if (clsPtr->properties.count(key) > 0) return Value(true);
        if (clsPtr->properties.count("__get_" + key) > 0) return Value(true);
        if (clsPtr->properties.count("__set_" + key) > 0) return Value(true);
        return Value(false);
      };
      LIGHTJS_RETURN(Value(hopFn));
    }
    // Handle private static field/method access (#name)
    if (expr.privateIdentifier) {
      PrivateNameOwner resolved;
      if (activePrivateOwnerClass_) {
        resolved = resolvePrivateNameOwnerClass(activePrivateOwnerClass_, propName);
      } else if (classDeclaresStaticPrivateName(clsPtr, propName)) {
        resolved.owner = clsPtr;
        resolved.kind = PrivateNameKind::Static;
      }
      if (!resolved.owner ||
          resolved.kind != PrivateNameKind::Static ||
          resolved.owner.get() != clsPtr.get()) {
        throwError(ErrorType::TypeError,
                   "Cannot read private member " + propName +
                   " from an object whose class did not declare it");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      std::string mangledName = privateStorageKey(resolved.owner, propName);
      auto privateGetterIt = clsPtr->properties.find("__get_" + mangledName);
      if (privateGetterIt != clsPtr->properties.end() && privateGetterIt->second.isFunction()) {
        LIGHTJS_RETURN(callFunction(privateGetterIt->second, {}, privateAccessReceiver));
      }
      auto it = clsPtr->properties.find(mangledName);
      if (it != clsPtr->properties.end()) {
        LIGHTJS_RETURN(it->second);
      }
      auto staticMethodIt = clsPtr->staticMethods.find(mangledName);
      if (staticMethodIt != clsPtr->staticMethods.end()) {
        LIGHTJS_RETURN(Value(staticMethodIt->second));
      }
      throwError(ErrorType::TypeError, "Cannot read private member " + propName + " from an object whose class did not declare it");
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    auto getterIt = clsPtr->properties.find("__get_" + propName);
    if (getterIt != clsPtr->properties.end() && getterIt->second.isFunction()) {
      LIGHTJS_RETURN(callFunction(getterIt->second, {}, obj));
    }
    // If only a setter exists (no getter), return undefined (accessor with no get)
    auto setterOnlyIt = clsPtr->properties.find("__set_" + propName);
    if (setterOnlyIt != clsPtr->properties.end() && getterIt == clsPtr->properties.end()) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    // Check own properties first (name, length, static methods, etc.)
    auto propIt = clsPtr->properties.find(propName);
    if (propIt != clsPtr->properties.end()) {
      LIGHTJS_RETURN(propIt->second);
    }
    auto staticIt = clsPtr->staticMethods.find(propName);
    if (staticIt != clsPtr->staticMethods.end()) {
      LIGHTJS_RETURN(Value(staticIt->second));
    }
    if (clsPtr->superClass) {
      auto superGetterIt = clsPtr->superClass->properties.find("__get_" + propName);
      if (superGetterIt != clsPtr->superClass->properties.end() &&
          superGetterIt->second.isFunction()) {
        LIGHTJS_RETURN(callFunction(superGetterIt->second, {}, obj));
      }
      auto superPropIt = clsPtr->superClass->properties.find(propName);
      if (superPropIt != clsPtr->superClass->properties.end()) {
        LIGHTJS_RETURN(superPropIt->second);
      }
      auto superStaticIt = clsPtr->superClass->staticMethods.find(propName);
      if (superStaticIt != clsPtr->superClass->staticMethods.end()) {
        LIGHTJS_RETURN(Value(superStaticIt->second));
      }
    }

    // Walk prototype chain (__proto__) for classes (class objects behave like functions).
    {
      auto protoIt = clsPtr->properties.find("__proto__");
      if (protoIt != clsPtr->properties.end()) {
        Value protoVal = protoIt->second;
        int depth = 0;
        while (depth < 50) {
          if (protoVal.isObject()) {
            auto proto = protoVal.getGC<Object>();
            auto protoGetterIt = proto->properties.find("__get_" + propName);
            if (protoGetterIt != proto->properties.end() && protoGetterIt->second.isFunction()) {
              LIGHTJS_RETURN(callFunction(protoGetterIt->second, {}, obj));
            }
            auto found = proto->properties.find(propName);
            if (found != proto->properties.end()) {
              LIGHTJS_RETURN(found->second);
            }
            auto nextProto = proto->properties.find("__proto__");
            if (nextProto == proto->properties.end()) break;
            protoVal = nextProto->second;
          } else if (protoVal.isClass()) {
            auto protoCls = protoVal.getGC<Class>();
            auto protoGetterIt = protoCls->properties.find("__get_" + propName);
            if (protoGetterIt != protoCls->properties.end() && protoGetterIt->second.isFunction()) {
              LIGHTJS_RETURN(callFunction(protoGetterIt->second, {}, obj));
            }
            auto found = protoCls->properties.find(propName);
            if (found != protoCls->properties.end()) {
              LIGHTJS_RETURN(found->second);
            }
            auto foundStatic = protoCls->staticMethods.find(propName);
            if (foundStatic != protoCls->staticMethods.end()) {
              LIGHTJS_RETURN(Value(foundStatic->second));
            }
            auto nextProto = protoCls->properties.find("__proto__");
            if (nextProto == protoCls->properties.end()) break;
            protoVal = nextProto->second;
          } else if (protoVal.isFunction()) {
            auto protoFn = protoVal.getGC<Function>();
            auto protoGetterIt = protoFn->properties.find("__get_" + propName);
            if (protoGetterIt != protoFn->properties.end() && protoGetterIt->second.isFunction()) {
              LIGHTJS_RETURN(callFunction(protoGetterIt->second, {}, obj));
            }
            auto found = protoFn->properties.find(propName);
            if (found != protoFn->properties.end()) {
              LIGHTJS_RETURN(found->second);
            }
            auto nextProto = protoFn->properties.find("__proto__");
            if (nextProto == protoFn->properties.end()) break;
            protoVal = nextProto->second;
          } else {
            break;
          }
          depth++;
        }
      }
    }
  }

  if (expr.privateIdentifier && !obj.isClass()) {
    if (!activePrivateOwnerClass_) {
      throwError(ErrorType::TypeError,
                 "Cannot read private member " + propName +
                     " from an object whose class did not declare it");
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    PrivateNameOwner resolved = resolvePrivateNameOwnerClass(activePrivateOwnerClass_, propName);
    if (!resolved.owner || resolved.kind != PrivateNameKind::Instance) {
      throwError(ErrorType::TypeError,
                 "Cannot read private member " + propName +
                     " from an object whose class did not declare it");
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    GCPtr<Class> targetClass = getConstructorClassForPrivateAccess(obj);
    if (!targetClass || !isOwnerInClassChain(targetClass, resolved.owner)) {
      throwError(ErrorType::TypeError,
                 "Cannot read private member " + propName +
                     " from an object whose class did not declare it");
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    auto privateGetterIt = resolved.owner->getters.find(propName);
    if (privateGetterIt != resolved.owner->getters.end()) {
      LIGHTJS_RETURN(invokeFunction(privateGetterIt->second, {}, privateAccessReceiver));
    }
    auto privateMethodIt = resolved.owner->methods.find(propName);
    if (privateMethodIt != resolved.owner->methods.end()) {
      LIGHTJS_RETURN(Value(privateMethodIt->second));
    }

    std::string mangledName = privateStorageKey(resolved.owner, propName);
    if (const auto* storage = getPropertyStorageForPrivateAccess(obj)) {
      auto it = storage->find(mangledName);
      if (it != storage->end()) {
        LIGHTJS_RETURN(it->second);
      }
    }

    throwError(ErrorType::TypeError,
               "Cannot read private member " + propName +
                   " from an object whose class did not declare it");
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  if (obj.isObject()) {
    auto objPtr = obj.getGC<Object>();
    Value accessorReceiver = (isSuperAccess && !lastMemberBase_.isUndefined()) ? lastMemberBase_ : obj;
    if (propName == "__proto__") {
      auto protoGetterIt = objPtr->properties.find("__get___own_prop___proto__");
      if (protoGetterIt != objPtr->properties.end() && protoGetterIt->second.isFunction()) {
        auto getter = protoGetterIt->second.getGC<Function>();
        LIGHTJS_RETURN(invokeFunction(getter, {}, accessorReceiver));
      }
      auto ownProtoIt = objPtr->properties.find("__own_prop___proto__");
      if (ownProtoIt != objPtr->properties.end()) {
        LIGHTJS_RETURN(ownProtoIt->second);
      }
    }

    // Deferred dynamic import namespace: trigger evaluation on first external property access.
    if (propName.rfind("__", 0) != 0) {
      auto pendingIt = objPtr->properties.find("__deferred_pending__");
      auto evalIt = objPtr->properties.find("__deferred_eval__");
      if (pendingIt != objPtr->properties.end() &&
          pendingIt->second.isBool() &&
          pendingIt->second.toBool() &&
          evalIt != objPtr->properties.end() &&
          evalIt->second.isFunction()) {
        auto deferredEvalFn = evalIt->second.getGC<Function>();
        invokeFunction(deferredEvalFn, {}, obj);
        if (hasError()) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        objPtr->properties["__deferred_pending__"] = Value(false);
      }
    }

    // Check for getter first
    std::string getterName = "__get_" + propName;
    auto getterIt = objPtr->properties.find(getterName);
    if (getterIt != objPtr->properties.end() && getterIt->second.isFunction()) {
      auto getter = getterIt->second.getGC<Function>();
      // Call the getter with receiver semantics (super uses current this-value).
      LIGHTJS_RETURN(invokeFunction(getter, {}, accessorReceiver));
    }

    // Direct property lookup
    auto it = objPtr->properties.find(propName);
    if (it != objPtr->properties.end()) {
      LIGHTJS_RETURN(it->second);
    }

    // Walk prototype chain (__proto__)
    {
      auto proto = objPtr;
      int depth = 0;
      while (depth < 50) {
        auto protoIt = proto->properties.find("__proto__");
        if (protoIt == proto->properties.end() || !isObjectLike(protoIt->second)) break;
        if (!protoIt->second.isObject()) {
          auto [found, value] = getPropertyForPrimitive(protoIt->second, propName);
          if (found) {
            LIGHTJS_RETURN(value);
          }
          break;
        }
        proto = protoIt->second.getGC<Object>();
        depth++;
        // Check getter on prototype
        auto protoGetterIt = proto->properties.find("__get_" + propName);
        if (protoGetterIt != proto->properties.end() && protoGetterIt->second.isFunction()) {
          auto getter = protoGetterIt->second.getGC<Function>();
          LIGHTJS_RETURN(invokeFunction(getter, {}, accessorReceiver));
        }
        auto propIt = proto->properties.find(propName);
        if (propIt != proto->properties.end()) {
          LIGHTJS_RETURN(propIt->second);
        }
      }
    }

    // Primitive wrapper objects created by native constructors (e.g. new String(), new Number()).
    // After ordinary prototype lookup fails, fall back to the primitive's intrinsic methods.
    if (auto primIt = objPtr->properties.find("__primitive_value__"); primIt != objPtr->properties.end()) {
      Value prim = primIt->second;
      if (prim.isString()) {
        std::string str = std::get<std::string>(prim.data);
        if (propName == "trim") {
          auto trimFn = GarbageCollector::makeGC<Function>();
          trimFn->isNative = true;
          trimFn->nativeFunc = [str](const std::vector<Value>&) -> Value {
            return Value(stripESWhitespace(str));
          };
          setNativeFnProps(trimFn, "trim", 0);
      LIGHTJS_RETURN(Value(trimFn));
        }
      } else if (prim.isNumber()) {
        double num = prim.toNumber();
        if (propName == "toFixed") {
          auto toFixedFn = GarbageCollector::makeGC<Function>();
          toFixedFn->isNative = true;
          toFixedFn->nativeFunc = [num](const std::vector<Value>& args) -> Value {
            int digits = args.empty() ? 0 : static_cast<int>(args[0].toNumber());
            if (digits < 0) digits = 0;
            if (digits > 100) digits = 100;
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(digits) << num;
            return Value(oss.str());
          };
          LIGHTJS_RETURN(Value(toFixedFn));
        }
        if (propName == "toExponential") {
          auto toExponentialFn = GarbageCollector::makeGC<Function>();
          toExponentialFn->isNative = true;
          toExponentialFn->nativeFunc = [num](const std::vector<Value>& args) -> Value {
            int digits = args.empty() ? 6 : static_cast<int>(args[0].toNumber());
            if (digits < 0) digits = 0;
            if (digits > 100) digits = 100;
            std::ostringstream oss;
            oss << std::scientific << std::setprecision(digits) << num;
            std::string out = oss.str();
            auto expPos = out.find_first_of("eE");
            if (expPos != std::string::npos) {
              std::string mantissa = out.substr(0, expPos);
              std::string exponent = out.substr(expPos + 1);
              char sign = '+';
              size_t idx = 0;
              if (!exponent.empty() && (exponent[0] == '+' || exponent[0] == '-')) {
                sign = exponent[0];
                idx = 1;
              }
              while (idx < exponent.size() && exponent[idx] == '0') idx++;
              std::string expDigits = (idx < exponent.size()) ? exponent.substr(idx) : "0";
              out = mantissa + "e";
              out += sign;
              out += expDigits;
            }
            return Value(out);
          };
          LIGHTJS_RETURN(Value(toExponentialFn));
        }
      }
    }

    // Object.prototype methods - hasOwnProperty / propertyIsEnumerable
    if (propName == "hasOwnProperty") {
      auto hopFn = GarbageCollector::makeGC<Function>();
      hopFn->isNative = true;
      hopFn->properties["__uses_this_arg__"] = Value(true);
      hopFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(false);
        Value receiver = args[0];
        std::string key = valueToPropertyKey(args[1]);
        auto isHiddenInternalKey = [&](const std::string& k) -> bool {
          if (k.size() >= 4 && k.substr(0, 2) == "__" && k.substr(k.size() - 2) == "__") return true;
          if (k.size() > 6 && (k.substr(0, 6) == "__get_" || k.substr(0, 6) == "__set_")) return true;
          if (k.size() > 10 && k.substr(0, 10) == "__non_enum") return true;
          if (k.size() > 14 && k.substr(0, 14) == "__non_writable") return true;
          if (k.size() > 18 && k.substr(0, 18) == "__non_configurable") return true;
          if (k.size() > 7 && k.substr(0, 7) == "__enum_") return true;
          return false;
        };
        if (isHiddenInternalKey(key)) return Value(false);

        if (receiver.isObject()) {
          auto objPtr = receiver.getGC<Object>();
          if (key == "__proto__" && objPtr->properties.count("__own_prop___proto__") > 0) {
            return Value(true);
          }
          return Value(objPtr->properties.count(key) > 0);
        }
        if (receiver.isFunction()) {
          auto fnPtr = receiver.getGC<Function>();
          return Value(fnPtr->properties.count(key) > 0);
        }
        if (receiver.isArray()) {
          auto arrPtr = receiver.getGC<Array>();
          if (key == "length") return Value(true);
          size_t idx = 0;
          if (parseArrayIndex(key, idx) && idx < arrPtr->elements.size()) return Value(true);
          return Value(arrPtr->properties.count(key) > 0);
        }
        if (receiver.isRegex()) {
          auto rxPtr = receiver.getGC<Regex>();
          if (key == "source" || key == "flags") return Value(true);
          return Value(rxPtr->properties.count(key) > 0);
        }
        if (receiver.isPromise()) {
          auto p = receiver.getGC<Promise>();
          return Value(p->properties.count(key) > 0);
        }
        if (receiver.isClass()) {
          auto cls = receiver.getGC<Class>();
          return Value(cls->properties.count(key) > 0);
        }
        if (receiver.isGenerator()) {
          auto gen = receiver.getGC<Generator>();
          return Value(gen->properties.count(key) > 0);
        }
        if (receiver.isMap()) {
          auto m = receiver.getGC<Map>();
          return Value(m->properties.count(key) > 0);
        }
        if (receiver.isSet()) {
          auto s = receiver.getGC<Set>();
          return Value(s->properties.count(key) > 0);
        }
        if (receiver.isWeakMap()) {
          auto wm = receiver.getGC<WeakMap>();
          return Value(wm->properties.count(key) > 0);
        }
        if (receiver.isWeakSet()) {
          auto ws = receiver.getGC<WeakSet>();
          return Value(ws->properties.count(key) > 0);
        }
        if (receiver.isTypedArray()) {
          auto ta = receiver.getGC<TypedArray>();
          if (key == "length" || key == "byteLength" || key == "buffer" || key == "byteOffset") {
            return Value(true);
          }
          size_t idx = 0;
          if (parseArrayIndex(key, idx) && idx < ta->currentLength()) return Value(true);
          return Value(ta->properties.count(key) > 0);
        }
        if (receiver.isArrayBuffer()) {
          auto b = receiver.getGC<ArrayBuffer>();
          if (key == "byteLength") return Value(true);
          return Value(b->properties.count(key) > 0);
        }
        if (receiver.isDataView()) {
          auto v = receiver.getGC<DataView>();
          return Value(v->properties.count(key) > 0);
        }
        if (receiver.isError()) {
          auto e = receiver.getGC<Error>();
          return Value(e->properties.count(key) > 0);
        }
        return Value(false);
      };
      LIGHTJS_RETURN(Value(hopFn));
    }

    if (propName == "propertyIsEnumerable") {
      auto pieFn = GarbageCollector::makeGC<Function>();
      pieFn->isNative = true;
      pieFn->properties["__uses_this_arg__"] = Value(true);
      pieFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(false);
        Value receiver = args[0];
        std::string key = valueToPropertyKey(args[1]);

        auto isEnumerableForMarkers = [&](const auto& props) -> bool {
          return props.find("__non_enum_" + key) == props.end();
        };

        if (receiver.isArray()) {
          auto arrPtr = receiver.getGC<Array>();
          if (key == "length") return Value(false);
          size_t idx = 0;
          if (parseArrayIndex(key, idx) && idx < arrPtr->elements.size()) return Value(true);
          if (arrPtr->properties.count(key) == 0) return Value(false);
          return Value(isEnumerableForMarkers(arrPtr->properties));
        }
        if (receiver.isObject()) {
          auto objPtr = receiver.getGC<Object>();
          if (objPtr->properties.count(key) == 0) return Value(false);
          return Value(isEnumerableForMarkers(objPtr->properties));
        }
        if (receiver.isFunction()) {
          auto fnPtr = receiver.getGC<Function>();
          if (fnPtr->properties.count(key) == 0) return Value(false);
          // name/length are non-enumerable by spec
          if (key == "name" || key == "length") return Value(false);
          return Value(isEnumerableForMarkers(fnPtr->properties));
        }
        if (receiver.isRegex()) {
          auto rxPtr = receiver.getGC<Regex>();
          if (key == "source" || key == "flags") return Value(true);
          if (rxPtr->properties.count(key) == 0) return Value(false);
          return Value(isEnumerableForMarkers(rxPtr->properties));
        }
        if (receiver.isError()) {
          auto e = receiver.getGC<Error>();
          if (e->properties.count(key) == 0) return Value(false);
          return Value(isEnumerableForMarkers(e->properties));
        }
        if (receiver.isTypedArray()) {
          auto ta = receiver.getGC<TypedArray>();
          if (key == "length" || key == "byteLength" || key == "buffer" || key == "byteOffset") {
            return Value(false);
          }
          size_t idx = 0;
          if (parseArrayIndex(key, idx) && idx < ta->currentLength()) return Value(true);
          if (ta->properties.count(key) == 0) return Value(false);
          return Value(isEnumerableForMarkers(ta->properties));
        }
        // Fallback: not enumerable.
        return Value(false);
      };
      LIGHTJS_RETURN(Value(pieFn));
    }

    // Some constructor singletons (notably Array) store prototype metadata
    // separately for runtime compatibility.
    if (propName == "prototype") {
      if (auto arrayValue = env_->get("Array");
          arrayValue && arrayValue->isObject() &&
          std::get<GCPtr<Object>>(arrayValue->data).get() == objPtr.get()) {
        if (auto hiddenArrayProto = env_->get("__array_prototype__")) {
          LIGHTJS_RETURN(*hiddenArrayProto);
        }
      }
    }
  }

  if (obj.isFunction()) {
    auto funcPtr = obj.getGC<Function>();
    if ((funcPtr->isStrict || funcPtr->isGenerator) &&
        (propName == "caller" || propName == "arguments")) {
      throwError(ErrorType::TypeError, "'caller', 'callee', and 'arguments' properties may not be accessed");
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    // Use %Function.prototype%.{call,apply,bind} so semantics match built-ins (esp. bound functions).
    auto resolveFunctionPrototypeMethod = [&](const std::string& name) -> std::optional<Value> {
      auto tryLookupOnProtoValue = [&](const Value& protoValue) -> std::optional<Value> {
        if (protoValue.isObject()) {
          auto protoObj = protoValue.getGC<Object>();
          if (!protoObj) return std::nullopt;
          if (auto it = protoObj->properties.find(name); it != protoObj->properties.end()) {
            return it->second;
          }
          return std::nullopt;
        }
        if (protoValue.isFunction()) {
          auto protoFn = protoValue.getGC<Function>();
          if (!protoFn) return std::nullopt;
          if (auto it = protoFn->properties.find(name); it != protoFn->properties.end()) {
            return it->second;
          }
          return std::nullopt;
        }
        return std::nullopt;
      };
      auto protoIt = funcPtr->properties.find("__proto__");
      if (protoIt != funcPtr->properties.end() &&
          (protoIt->second.isObject() || protoIt->second.isFunction())) {
        if (auto v = tryLookupOnProtoValue(protoIt->second)) return v;
      }
      // Some native/built-in functions may not have an explicit __proto__.
      // Default to %Function.prototype% so `Function.prototype.call.bind(...)`
      // and similar patterns work.
      if (auto funcCtor = env_->getRoot()->get("Function");
          funcCtor.has_value() && funcCtor->isFunction()) {
        auto funcCtorFn = std::get<GCPtr<Function>>(funcCtor->data);
        auto fpIt = funcCtorFn->properties.find("prototype");
        if (fpIt != funcCtorFn->properties.end() &&
            (fpIt->second.isObject() || fpIt->second.isFunction())) {
          if (auto v = tryLookupOnProtoValue(fpIt->second)) return v;
        }
      }
      return std::nullopt;
    };

    if (propName == "call" || propName == "apply" || propName == "bind") {
      if (auto method = resolveFunctionPrototypeMethod(propName); method && method->isFunction()) {
        LIGHTJS_RETURN(*method);
      }
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    // hasOwnProperty from Object.prototype
    if (propName == "hasOwnProperty") {
      auto hopFn = GarbageCollector::makeGC<Function>();
      hopFn->isNative = true;
      hopFn->properties["__uses_this_arg__"] = Value(true);
      hopFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(false);
        Value receiver = args[0];
        std::string key = valueToPropertyKey(args[1]);
        // Internal properties are not visible
        if (key.size() >= 4 && key.substr(0, 2) == "__" && key.substr(key.size() - 2) == "__") return Value(false);
        if (key.size() > 6 && (key.substr(0, 6) == "__get_" || key.substr(0, 6) == "__set_")) return Value(false);
        if (key.size() > 10 && key.substr(0, 10) == "__non_enum") return Value(false);
        if (key.size() > 14 && key.substr(0, 14) == "__non_writable") return Value(false);
        if (key.size() > 18 && key.substr(0, 18) == "__non_configurable") return Value(false);
        if (!receiver.isFunction()) return Value(false);
        auto funcPtr = receiver.getGC<Function>();
        return Value(funcPtr->properties.count(key) > 0 ||
                     funcPtr->properties.count("__get_" + key) > 0 ||
                     funcPtr->properties.count("__set_" + key) > 0);
      };
      LIGHTJS_RETURN(Value(hopFn));
    }

    auto getterIt = funcPtr->properties.find("__get_" + propName);
    if (getterIt != funcPtr->properties.end() && getterIt->second.isFunction()) {
      LIGHTJS_RETURN(callFunction(getterIt->second, {}, obj));
    }
    if (funcPtr->properties.find("__set_" + propName) != funcPtr->properties.end()) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    auto it = funcPtr->properties.find(propName);
    if (it != funcPtr->properties.end()) {
      LIGHTJS_RETURN(it->second);
    }

    // Walk prototype chain (__proto__) for functions
    {
      auto protoIt = funcPtr->properties.find("__proto__");
      Value protoValue(Undefined{});
      if (protoIt != funcPtr->properties.end() &&
          (protoIt->second.isObject() || protoIt->second.isFunction())) {
        protoValue = protoIt->second;
      } else {
        // Default to %Function.prototype% if no explicit __proto__ is set.
        if (auto funcCtor = env_->getRoot()->get("Function");
            funcCtor.has_value() && funcCtor->isFunction()) {
          auto funcCtorFn = std::get<GCPtr<Function>>(funcCtor->data);
          auto fpIt = funcCtorFn->properties.find("prototype");
          if (fpIt != funcCtorFn->properties.end() &&
              (fpIt->second.isObject() || fpIt->second.isFunction())) {
            protoValue = fpIt->second;
          }
        }
      }
      if (protoValue.isObject() || protoValue.isFunction()) {
        Value currentProto = protoValue;
        int depth = 0;
        while ((currentProto.isObject() || currentProto.isFunction()) && depth < 50) {
          OrderedMap<std::string, Value>* props = nullptr;
          if (currentProto.isObject()) {
            auto protoObj = currentProto.getGC<Object>();
            if (!protoObj) break;
            props = &protoObj->properties;
          } else {
            auto protoFn = currentProto.getGC<Function>();
            if (!protoFn) break;
            props = &protoFn->properties;
          }

          auto protoGetterIt = props->find("__get_" + propName);
          if (protoGetterIt != props->end() && protoGetterIt->second.isFunction()) {
            auto getter = protoGetterIt->second.getGC<Function>();
            LIGHTJS_RETURN(invokeFunction(getter, {}, obj));
          }
          auto found = props->find(propName);
          if (found != props->end()) {
            LIGHTJS_RETURN(found->second);
          }
          auto nextProto = props->find("__proto__");
          if (nextProto == props->end() ||
              (!nextProto->second.isObject() && !nextProto->second.isFunction())) {
            break;
          }
          currentProto = nextProto->second;
          depth++;
        }
      }
    }
  }

  // Generator methods
  if (obj.isGenerator()) {
    auto genPtr = obj.getGC<Generator>();
    bool isAsyncGenerator = genPtr->function && genPtr->function->isAsync;
    const auto& asyncIteratorKey = WellKnownSymbols::asyncIteratorKey();
    auto initializePromiseIntrinsics = [this](const GCPtr<Promise>& p) {
      if (!p) return;
      auto promiseCtor = env_->getRoot()->get("__intrinsic_Promise__");
      if (!promiseCtor || !promiseCtor->isFunction()) {
        promiseCtor = env_->getRoot()->get("Promise");
      }
      if (promiseCtor && promiseCtor->isFunction()) {
        p->properties["__constructor__"] = *promiseCtor;
        auto promiseCtorFn = promiseCtor->getGC<Function>();
        auto protoIt = promiseCtorFn->properties.find("prototype");
        if (protoIt != promiseCtorFn->properties.end() && protoIt->second.isObject()) {
          p->properties["__proto__"] = protoIt->second;
        }
      }
    };
    auto extractAwaitSuspendPromise = [](const Value& step, GCPtr<Promise>& out) -> bool {
      if (!step.isObject()) return false;
      auto stepObj = step.getGC<Object>();
      if (!stepObj) return false;
      auto doneIt = stepObj->properties.find("done");
      if (doneIt == stepObj->properties.end() || doneIt->second.toBool()) {
        return false;
      }
      auto valueIt = stepObj->properties.find("value");
      if (valueIt == stepObj->properties.end() || !valueIt->second.isPromise()) {
        return false;
      }
      auto awaited = valueIt->second.getGC<Promise>();
      if (!awaited) return false;
      auto markerIt = awaited->properties.find("__await_suspend__");
      if (markerIt == awaited->properties.end() ||
          !markerIt->second.isBool() ||
          !markerIt->second.toBool()) {
        return false;
      }
      out = awaited;
      return true;
    };
    auto enqueueAsyncGeneratorRequest =
      [this, genPtr, initializePromiseIntrinsics, extractAwaitSuspendPromise]
      (std::function<Value()> beginRequest) -> Value {
        auto resultPromise = GarbageCollector::makeGC<Promise>();
        initializePromiseIntrinsics(resultPromise);
        auto queueTail = GarbageCollector::makeGC<Promise>();
        initializePromiseIntrinsics(queueTail);

        GCPtr<Promise> prevTail;
        auto tailIt = genPtr->properties.find("__async_gen_tail__");
        if (tailIt != genPtr->properties.end() && tailIt->second.isPromise()) {
          prevTail = tailIt->second.getGC<Promise>();
        }
        if (!prevTail) {
          prevTail = GarbageCollector::makeGC<Promise>();
          initializePromiseIntrinsics(prevTail);
          prevTail->resolve(Value(Undefined{}));
        }
        genPtr->properties["__async_gen_tail__"] = Value(queueTail);

        auto runRequest = std::make_shared<std::function<void()>>();
        *runRequest = [this, genPtr, beginRequest, resultPromise, queueTail, extractAwaitSuspendPromise]() {
          Value step = beginRequest();
          auto settleAsyncStep = std::make_shared<std::function<void(const Value&)>>();
          *settleAsyncStep =
            [this, genPtr, resultPromise, queueTail, settleAsyncStep, extractAwaitSuspendPromise]
            (const Value& currentStep) {
              if (this->flow_.type == ControlFlow::Type::Throw) {
                Value rejection = this->flow_.value;
                this->clearError();
                resultPromise->reject(rejection);
                queueTail->resolve(Value(Undefined{}));
                return;
              }

              GCPtr<Promise> awaited;
              if (extractAwaitSuspendPromise(currentStep, awaited)) {
                awaited->then(
                  [this, genPtr, settleAsyncStep](Value fulfilled) -> Value {
                    Value resumedStep = this->runGeneratorNext(
                      genPtr, ControlFlow::ResumeMode::Next, fulfilled);
                    (*settleAsyncStep)(resumedStep);
                    return fulfilled;
                  },
                  [this, genPtr, settleAsyncStep](Value reason) -> Value {
                    Value resumedStep = this->runGeneratorNext(
                      genPtr, ControlFlow::ResumeMode::Throw, reason);
                    (*settleAsyncStep)(resumedStep);
                    return reason;
                  });
                return;
              }

              resultPromise->resolve(currentStep);
              queueTail->resolve(Value(Undefined{}));
            };
          (*settleAsyncStep)(step);
        };

        if (prevTail->state == PromiseState::Pending) {
          prevTail->then(
            [runRequest](Value) -> Value {
              (*runRequest)();
              return Value(Undefined{});
            },
            [runRequest](Value) -> Value {
              (*runRequest)();
              return Value(Undefined{});
            });
        } else {
          (*runRequest)();
        }

        return Value(resultPromise);
      };

    if (propName == iteratorKey) {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [genPtr](const std::vector<Value>&) -> Value {
        return Value(genPtr);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == asyncIteratorKey && isAsyncGenerator) {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [genPtr](const std::vector<Value>&) -> Value {
        return Value(genPtr);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "next") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [genPtr, this, initializePromiseIntrinsics, extractAwaitSuspendPromise, enqueueAsyncGeneratorRequest](const std::vector<Value>& args) -> Value {
        Value resumeValue = args.empty() ? Value(Undefined{}) : args[0];
        auto mode = ControlFlow::ResumeMode::Next;
        if (genPtr->state == GeneratorState::SuspendedStart) {
          resumeValue = Value(Undefined{});
        }
        if (!(genPtr->function && genPtr->function->isAsync)) {
          return this->runGeneratorNext(genPtr, mode, resumeValue);
        }
        return enqueueAsyncGeneratorRequest(
          [this, genPtr, mode, resumeValue]() -> Value {
            return this->runGeneratorNext(genPtr, mode, resumeValue);
          });
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "return") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [genPtr, this, initializePromiseIntrinsics, enqueueAsyncGeneratorRequest](const std::vector<Value>& args) -> Value {
        Value returnValue = args.empty() ? Value(Undefined{}) : args[0];
        bool isAsyncGenerator = genPtr->function && genPtr->function->isAsync;
        auto wrapAsyncResult = [isAsyncGenerator, initializePromiseIntrinsics](const Value& settledValue, bool reject) -> Value {
          if (!isAsyncGenerator) {
            return settledValue;
          }
          auto promise = GarbageCollector::makeGC<Promise>();
          initializePromiseIntrinsics(promise);
          if (reject) {
            promise->reject(settledValue);
          } else {
            promise->resolve(settledValue);
          }
          return Value(promise);
        };

        if (genPtr->state == GeneratorState::Completed) {
          return wrapAsyncResult(makeIteratorResult(returnValue, true), false);
        }
        if (genPtr->state == GeneratorState::SuspendedStart) {
          genPtr->state = GeneratorState::Completed;
          genPtr->currentValue = std::make_shared<Value>(returnValue);
          return wrapAsyncResult(makeIteratorResult(returnValue, true), false);
        }

        if (isAsyncGenerator) {
          return enqueueAsyncGeneratorRequest(
            [this, genPtr, returnValue]() -> Value {
              return this->runGeneratorNext(genPtr, ControlFlow::ResumeMode::Return, returnValue);
            });
        }

        // SuspendedYield: resume execution to run finally blocks
        Value step = this->runGeneratorNext(genPtr, ControlFlow::ResumeMode::Return, returnValue);

        // If a finally block yielded, the generator is still suspended
        if (genPtr->state == GeneratorState::SuspendedYield) {
          return wrapAsyncResult(step, false);
        }

        if (this->flow_.type == ControlFlow::Type::Throw) {
          Value rejection = this->flow_.value;
          if (isAsyncGenerator) {
            this->clearError();
          }
          return wrapAsyncResult(rejection, true);
        }

        // Generator completed (normally or via finally return)
        Value resultValue = returnValue;
        if (genPtr->currentValue) {
          resultValue = *genPtr->currentValue;
        }

        return wrapAsyncResult(makeIteratorResult(resultValue, true), false);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "throw") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [genPtr, this, initializePromiseIntrinsics, enqueueAsyncGeneratorRequest](const std::vector<Value>& args) -> Value {
        Value throwValue = args.empty() ? Value(GarbageCollector::makeGC<Error>(ErrorType::Error, "Generator error")) : args[0];
        bool isAsyncGenerator = genPtr->function && genPtr->function->isAsync;

        if (genPtr->state == GeneratorState::Completed ||
            genPtr->state == GeneratorState::SuspendedStart) {
          genPtr->state = GeneratorState::Completed;
          if (isAsyncGenerator) {
            auto promise = GarbageCollector::makeGC<Promise>();
            initializePromiseIntrinsics(promise);
            promise->reject(throwValue);
            return Value(promise);
          }
          // Re-throw: set flow to Throw so caller sees it
          this->flow_.type = ControlFlow::Type::Throw;
          this->flow_.value = throwValue;
          return Value(Undefined{});
        }

        if (isAsyncGenerator) {
          return enqueueAsyncGeneratorRequest(
            [this, genPtr, throwValue]() -> Value {
              return this->runGeneratorNext(genPtr, ControlFlow::ResumeMode::Throw, throwValue);
            });
        }

        // SuspendedYield: resume to let try/catch handle the error
        Value step = this->runGeneratorNext(genPtr, ControlFlow::ResumeMode::Throw, throwValue);

        // If the generator caught the error and yielded again
        if (genPtr->state == GeneratorState::SuspendedYield) {
          return step;
        }

        return step;
      };
      LIGHTJS_RETURN(Value(fn));
    }
  }

  // Array iterator helpers continue below
  if (obj.isArray()) {
    auto arrPtr = obj.getGC<Array>();
    if (propName == "length") {
      // Arguments objects may have overridden length
      auto overriddenIt = arrPtr->properties.find("__overridden_length__");
      if (overriddenIt != arrPtr->properties.end()) {
        LIGHTJS_RETURN(overriddenIt->second);
      }
      LIGHTJS_RETURN(Value(static_cast<double>(arrPtr->elements.size())));
    }

    if (propName == iteratorKey) {
      // Own properties must shadow prototype @@iterator.
      auto ownGetterIt = arrPtr->properties.find("__get_" + propName);
      if (ownGetterIt != arrPtr->properties.end() && ownGetterIt->second.isFunction()) {
        Value out = callFunction(ownGetterIt->second, {}, obj);
        if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
        LIGHTJS_RETURN(out);
      }
      auto ownIt = arrPtr->properties.find(propName);
      if (ownIt != arrPtr->properties.end()) {
        LIGHTJS_RETURN(ownIt->second);
      }

      // Expose the shared Array.prototype[@@iterator] function.
      // This is required so `[][Symbol.iterator]` is stable and can be compared.
      if (auto arrayProtoVal = env_->getRoot()->get("__array_prototype__");
          arrayProtoVal && arrayProtoVal->isObject()) {
        auto arrayProto = arrayProtoVal->getGC<Object>();
        auto it = arrayProto->properties.find(iteratorKey);
        if (it != arrayProto->properties.end()) {
          LIGHTJS_RETURN(it->second);
        }
      }
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    // Array higher-order methods (map, filter, forEach, reduce, etc.)
    // are now on Array.prototype and will be resolved via prototype chain lookup

    if (propName == "push") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        for (const auto& arg : args) {
          arrPtr->elements.push_back(arg);
        }
        return Value(static_cast<double>(arrPtr->elements.size()));
      };
      setNativeFnProps(fn, "push", 1);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "pop") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        if (arrPtr->elements.empty()) {
          return Value(Undefined{});
        }
        Value result = arrPtr->elements.back();
        arrPtr->elements.pop_back();
        return result;
      };
      setNativeFnProps(fn, "pop", 0);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "shift") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        if (arrPtr->elements.empty()) {
          return Value(Undefined{});
        }
        Value result = arrPtr->elements.front();
        arrPtr->elements.erase(arrPtr->elements.begin());
        return result;
      };
      setNativeFnProps(fn, "shift", 0);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "unshift") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        for (size_t i = 0; i < args.size(); ++i) {
          arrPtr->elements.insert(arrPtr->elements.begin() + i, args[i]);
        }
        return Value(static_cast<double>(arrPtr->elements.size()));
      };
      setNativeFnProps(fn, "unshift", 1);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "slice") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        auto result = makeArrayWithPrototype();

        int len = static_cast<int>(arrPtr->elements.size());
        int start = 0;
        int end = len;

        if (args.size() > 0 && args[0].isNumber()) {
          start = static_cast<int>(std::get<double>(args[0].data));
          if (start < 0) start = std::max(0, len + start);
          if (start > len) start = len;
        }

        if (args.size() > 1 && args[1].isNumber()) {
          end = static_cast<int>(std::get<double>(args[1].data));
          if (end < 0) end = std::max(0, len + end);
          if (end > len) end = len;
        }

        for (int i = start; i < end; ++i) {
          result->elements.push_back(arrPtr->elements[i]);
        }
        return Value(result);
      };
      setNativeFnProps(fn, "slice", 2);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "splice") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        auto removed = makeArrayWithPrototype();

        int len = static_cast<int>(arrPtr->elements.size());
        int start = 0;
        int deleteCount = len;

        if (args.size() > 0 && args[0].isNumber()) {
          start = static_cast<int>(std::get<double>(args[0].data));
          if (start < 0) start = std::max(0, len + start);
          if (start > len) start = len;
        }

        if (args.size() > 1 && args[1].isNumber()) {
          deleteCount = std::max(0, static_cast<int>(std::get<double>(args[1].data)));
          deleteCount = std::min(deleteCount, len - start);
        }

        // Remove elements
        for (int i = 0; i < deleteCount; ++i) {
          removed->elements.push_back(arrPtr->elements[start]);
          arrPtr->elements.erase(arrPtr->elements.begin() + start);
        }

        // Insert new elements
        for (size_t i = 2; i < args.size(); ++i) {
          arrPtr->elements.insert(arrPtr->elements.begin() + start + (i - 2), args[i]);
        }

        return Value(removed);
      };
      setNativeFnProps(fn, "splice", 2);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "toSpliced") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        auto result = makeArrayWithPrototype();

        // Copy all elements to new array
        for (const auto& elem : arrPtr->elements) {
          result->elements.push_back(elem);
        }

        int len = static_cast<int>(result->elements.size());
        int start = 0;
        int deleteCount = 0;

        if (args.size() > 0 && args[0].isNumber()) {
          start = static_cast<int>(std::get<double>(args[0].data));
          if (start < 0) start = std::max(0, len + start);
          if (start > len) start = len;
        }

        if (args.size() > 1 && args[1].isNumber()) {
          deleteCount = std::max(0, static_cast<int>(std::get<double>(args[1].data)));
          deleteCount = std::min(deleteCount, len - start);
        }

        // Remove elements
        for (int i = 0; i < deleteCount; ++i) {
          result->elements.erase(result->elements.begin() + start);
        }

        // Insert new elements
        for (size_t i = 2; i < args.size(); ++i) {
          result->elements.insert(result->elements.begin() + start + (i - 2), args[i]);
        }

        return Value(result);
      };
      setNativeFnProps(fn, "toSpliced", 2);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "join") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [this, arrPtr](const std::vector<Value>& args) -> Value {
        auto toStringForJoin = [this](const Value& input) -> std::string {
          Value primitive = isObjectLike(input) ? toPrimitiveValue(input, true) : input;
          if (flow_.type == ControlFlow::Type::Throw) {
            return "";
          }
          if (primitive.isSymbol()) {
            throwError(ErrorType::TypeError, "Cannot convert Symbol to string");
            return "";
          }
          return primitive.toString();
        };

        std::string separator = ",";
        if (args.size() > 0 && !args[0].isUndefined()) {
          separator = toStringForJoin(args[0]);
          if (flow_.type == ControlFlow::Type::Throw) {
            return Value(Undefined{});
          }
        }

        std::string result;
        for (size_t i = 0; i < arrPtr->elements.size(); ++i) {
          if (i > 0) result += separator;
          if (!arrPtr->elements[i].isUndefined() && !arrPtr->elements[i].isNull()) {
            result += toStringForJoin(arrPtr->elements[i]);
            if (flow_.type == ControlFlow::Type::Throw) {
              return Value(Undefined{});
            }
          }
        }
        return Value(result);
      };
      setNativeFnProps(fn, "join", 1);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "indexOf") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        auto strictEqual = [](const Value& left, const Value& right) -> bool {
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

        if (args.empty()) return Value(-1.0);

        Value searchElement = args[0];
        int fromIndex = 0;

        if (args.size() > 1) {
          double numericIndex = args[1].toNumber();
          if (std::isnan(numericIndex) || numericIndex == 0.0) {
            numericIndex = 0.0;
          } else if (std::isfinite(numericIndex)) {
            numericIndex = std::trunc(numericIndex);
          }
          fromIndex = static_cast<int>(numericIndex);
          int len = static_cast<int>(arrPtr->elements.size());
          if (fromIndex < 0) fromIndex = std::max(0, len + fromIndex);
        }

        for (size_t i = fromIndex; i < arrPtr->elements.size(); ++i) {
          if (strictEqual(arrPtr->elements[i], searchElement)) {
            return Value(static_cast<double>(i));
          }
        }
        return Value(-1.0);
      };
      setNativeFnProps(fn, "indexOf", 1);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "lastIndexOf") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        auto strictEqual = [](const Value& left, const Value& right) -> bool {
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

        if (args.empty()) return Value(-1.0);

        Value searchElement = args[0];
        int len = static_cast<int>(arrPtr->elements.size());
        int fromIndex = len - 1;

        if (args.size() > 1) {
          double numericIndex = args[1].toNumber();
          if (std::isnan(numericIndex) || numericIndex == 0.0) {
            numericIndex = 0.0;
          } else if (std::isfinite(numericIndex)) {
            numericIndex = std::trunc(numericIndex);
          }
          if (numericIndex == -std::numeric_limits<double>::infinity()) {
            return Value(-1.0);
          }
          if (numericIndex == std::numeric_limits<double>::infinity()) {
            numericIndex = static_cast<double>(len - 1);
          }
          fromIndex = static_cast<int>(numericIndex);
          if (fromIndex < 0) fromIndex = len + fromIndex;
          if (fromIndex >= len) fromIndex = len - 1;
        }

        for (int i = fromIndex; i >= 0; --i) {
          if (strictEqual(arrPtr->elements[i], searchElement)) {
            return Value(static_cast<double>(i));
          }
        }
        return Value(-1.0);
      };
      setNativeFnProps(fn, "lastIndexOf", 1);
      LIGHTJS_RETURN(Value(fn));
    }

    // includes is now on Array.prototype

    if (propName == "at") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(Undefined{});
        int index = static_cast<int>(args[0].toNumber());
        int len = static_cast<int>(arrPtr->elements.size());
        if (index < 0) index = len + index;
        if (index < 0 || index >= len) return Value(Undefined{});
        return arrPtr->elements[index];
      };
      setNativeFnProps(fn, "at", 1);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "reverse") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        std::reverse(arrPtr->elements.begin(), arrPtr->elements.end());
        return Value(arrPtr);
      };
      setNativeFnProps(fn, "reverse", 0);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "sort") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr, this](const std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].isFunction()) {
          // Default sort: convert to strings and compare lexicographically
          std::sort(arrPtr->elements.begin(), arrPtr->elements.end(),
            [](const Value& a, const Value& b) {
              return a.toString() < b.toString();
            });
        } else {
          // Sort with comparator function
          auto compareFn = args[0].getGC<Function>();
          std::sort(arrPtr->elements.begin(), arrPtr->elements.end(),
            [compareFn, this](const Value& a, const Value& b) {
              std::vector<Value> compareArgs = {a, b};
              Value result;
              if (compareFn->isNative) {
                result = compareFn->nativeFunc(compareArgs);
              } else {
                result = this->invokeFunction(compareFn, compareArgs);
              }
              return result.toNumber() < 0;
            });
        }
        return Value(arrPtr);
      };
      setNativeFnProps(fn, "sort", 1);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "toSorted") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr, this](const std::vector<Value>& args) -> Value {
        auto result = makeArrayWithPrototype();
        result->elements = arrPtr->elements;  // Copy

        if (args.empty() || !args[0].isFunction()) {
          std::sort(result->elements.begin(), result->elements.end(),
            [](const Value& a, const Value& b) {
              return a.toString() < b.toString();
            });
        } else {
          auto compareFn = args[0].getGC<Function>();
          std::sort(result->elements.begin(), result->elements.end(),
            [compareFn, this](const Value& a, const Value& b) {
              std::vector<Value> compareArgs = {a, b};
              Value r;
              if (compareFn->isNative) {
                r = compareFn->nativeFunc(compareArgs);
              } else {
                r = this->invokeFunction(compareFn, compareArgs);
              }
              return r.toNumber() < 0;
            });
        }
        return Value(result);
      };
      setNativeFnProps(fn, "toSorted", 1);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "toReversed") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        auto result = makeArrayWithPrototype();
        result->elements = arrPtr->elements;
        std::reverse(result->elements.begin(), result->elements.end());
        return Value(result);
      };
      setNativeFnProps(fn, "toReversed", 0);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "at") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(Undefined{});
        int index = static_cast<int>(args[0].toNumber());
        int size = static_cast<int>(arrPtr->elements.size());
        if (index < 0) index = size + index;
        if (index < 0 || index >= size) return Value(Undefined{});
        return arrPtr->elements[index];
      };
      setNativeFnProps(fn, "at", 1);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "with") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(arrPtr);
        int index = static_cast<int>(args[0].toNumber());
        int size = static_cast<int>(arrPtr->elements.size());
        if (index < 0) index = size + index;
        if (index < 0 || index >= size) {
          return Value(GarbageCollector::makeGC<Error>(ErrorType::RangeError, "Invalid index"));
        }
        auto result = makeArrayWithPrototype();
        result->elements = arrPtr->elements;
        result->elements[index] = args[1];
        return Value(result);
      };
      setNativeFnProps(fn, "with", 2);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "concat") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        auto result = makeArrayWithPrototype();

        // Copy original array elements
        result->elements = arrPtr->elements;

        // Add all arguments
        for (const auto& arg : args) {
          if (arg.isArray()) {
            auto otherArr = arg.getGC<Array>();
            result->elements.insert(result->elements.end(),
                                  otherArr->elements.begin(),
                                  otherArr->elements.end());
          } else {
            result->elements.push_back(arg);
          }
        }

        return Value(result);
      };
      setNativeFnProps(fn, "concat", 1);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "flat") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        int depth = 1;
        if (args.size() > 0 && args[0].isNumber()) {
          depth = static_cast<int>(std::get<double>(args[0].data));
        }

        std::function<void(const std::vector<Value>&, int, std::vector<Value>&)> flattenImpl;
        flattenImpl = [&flattenImpl](const std::vector<Value>& src, int d, std::vector<Value>& dest) {
          for (const auto& elem : src) {
            if (d > 0 && elem.isArray()) {
              auto inner = elem.getGC<Array>();
              flattenImpl(inner->elements, d - 1, dest);
            } else {
              dest.push_back(elem);
            }
          }
        };

        auto result = makeArrayWithPrototype();
        flattenImpl(arrPtr->elements, depth, result->elements);
        return Value(result);
      };
      setNativeFnProps(fn, "flat", 0);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "flatMap") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [this, arrPtr](const std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].isFunction()) {
          return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "flatMap requires a callback function"));
        }
        auto callback = args[0].getGC<Function>();
        Value thisArg = args.size() > 1 ? args[1] : Value(Undefined{});

        auto result = makeArrayWithPrototype();

        for (size_t i = 0; i < arrPtr->elements.size(); ++i) {
          std::vector<Value> callArgs = {arrPtr->elements[i], Value(static_cast<double>(i)), Value(arrPtr)};
          Value mapped = invokeFunction(callback, callArgs, thisArg);

          if (mapped.isArray()) {
            auto inner = mapped.getGC<Array>();
            result->elements.insert(result->elements.end(),
                                  inner->elements.begin(),
                                  inner->elements.end());
          } else {
            result->elements.push_back(mapped);
          }
        }
        return Value(result);
      };
      setNativeFnProps(fn, "flatMap", 1);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "fill") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(arrPtr);

        Value fillValue = args[0];
        int len = static_cast<int>(arrPtr->elements.size());
        int start = 0;
        int end = len;

        if (args.size() > 1 && args[1].isNumber()) {
          start = static_cast<int>(std::get<double>(args[1].data));
          if (start < 0) start = std::max(0, len + start);
          if (start > len) start = len;
        }

        if (args.size() > 2 && args[2].isNumber()) {
          end = static_cast<int>(std::get<double>(args[2].data));
          if (end < 0) end = std::max(0, len + end);
          if (end > len) end = len;
        }

        for (int i = start; i < end; ++i) {
          arrPtr->elements[i] = fillValue;
        }
        return Value(arrPtr);
      };
      setNativeFnProps(fn, "fill", 1);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "copyWithin") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(arrPtr);

        int len = static_cast<int>(arrPtr->elements.size());
        int target = static_cast<int>(std::get<double>(args[0].data));
        if (target < 0) target = std::max(0, len + target);

        int start = 0;
        if (args.size() > 1 && args[1].isNumber()) {
          start = static_cast<int>(std::get<double>(args[1].data));
          if (start < 0) start = std::max(0, len + start);
        }

        int end = len;
        if (args.size() > 2 && args[2].isNumber()) {
          end = static_cast<int>(std::get<double>(args[2].data));
          if (end < 0) end = std::max(0, len + end);
        }

        int count = std::min(end - start, len - target);
        std::vector<Value> temp(arrPtr->elements.begin() + start, arrPtr->elements.begin() + start + count);

        for (int i = 0; i < count && target + i < len; ++i) {
          arrPtr->elements[target + i] = temp[i];
        }
        return Value(arrPtr);
      };
      setNativeFnProps(fn, "copyWithin", 2);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "keys") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        auto iterObj = GarbageCollector::makeGC<Object>();
        auto indexPtr = std::make_shared<size_t>(0);
        auto nextFn = GarbageCollector::makeGC<Function>();
        nextFn->isNative = true;
        nextFn->nativeFunc = [arrPtr, indexPtr](const std::vector<Value>&) -> Value {
          auto result = GarbageCollector::makeGC<Object>();
          if (*indexPtr >= arrPtr->elements.size()) {
            result->properties["value"] = Value(Undefined{});
            result->properties["done"] = Value(true);
          } else {
            result->properties["value"] = Value(static_cast<double>((*indexPtr)++));
            result->properties["done"] = Value(false);
          }
          return Value(result);
        };
        iterObj->properties["next"] = Value(nextFn);
        return Value(iterObj);
      };
      setNativeFnProps(fn, "keys", 0);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "entries") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        auto iterObj = GarbageCollector::makeGC<Object>();
        auto indexPtr = std::make_shared<size_t>(0);
        auto nextFn = GarbageCollector::makeGC<Function>();
        nextFn->isNative = true;
        nextFn->nativeFunc = [arrPtr, indexPtr](const std::vector<Value>&) -> Value {
          auto result = GarbageCollector::makeGC<Object>();
          if (*indexPtr >= arrPtr->elements.size()) {
            result->properties["value"] = Value(Undefined{});
            result->properties["done"] = Value(true);
          } else {
            auto pair = GarbageCollector::makeGC<Array>();
            pair->elements.push_back(Value(static_cast<double>(*indexPtr)));
            pair->elements.push_back(arrPtr->elements[*indexPtr]);
            (*indexPtr)++;
            result->properties["value"] = Value(pair);
            result->properties["done"] = Value(false);
          }
          return Value(result);
        };
        iterObj->properties["next"] = Value(nextFn);
        return Value(iterObj);
      };
      setNativeFnProps(fn, "entries", 0);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "values") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        auto iterObj = GarbageCollector::makeGC<Object>();
        auto indexPtr = std::make_shared<size_t>(0);
        auto nextFn = GarbageCollector::makeGC<Function>();
        nextFn->isNative = true;
        nextFn->nativeFunc = [arrPtr, indexPtr](const std::vector<Value>&) -> Value {
          auto result = GarbageCollector::makeGC<Object>();
          if (*indexPtr >= arrPtr->elements.size()) {
            result->properties["value"] = Value(Undefined{});
            result->properties["done"] = Value(true);
          } else {
            result->properties["value"] = arrPtr->elements[(*indexPtr)++];
            result->properties["done"] = Value(false);
          }
          return Value(result);
        };
        iterObj->properties["next"] = Value(nextFn);
        return Value(iterObj);
      };
      setNativeFnProps(fn, "values", 0);
      LIGHTJS_RETURN(Value(fn));
    }

    auto getterIt = arrPtr->properties.find("__get_" + propName);
    if (getterIt != arrPtr->properties.end() && getterIt->second.isFunction()) {
      LIGHTJS_RETURN(callFunction(getterIt->second, {}, obj));
    }
    // Accessor without getter: return undefined (do not fall back to indexed element).
    auto setterIt = arrPtr->properties.find("__set_" + propName);
    if (getterIt != arrPtr->properties.end() || setterIt != arrPtr->properties.end()) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    size_t idx = 0;
    if (parseArrayIndex(propName, idx) && idx < arrPtr->elements.size()) {
      LIGHTJS_RETURN(arrPtr->elements[idx]);
    }
    auto propIt = arrPtr->properties.find(propName);
    if (propIt != arrPtr->properties.end()) {
      LIGHTJS_RETURN(propIt->second);
    }

    // Walk prototype chain (__proto__) for arrays
    {
      auto protoIt = arrPtr->properties.find("__proto__");
      if (protoIt != arrPtr->properties.end() && protoIt->second.isObject()) {
        auto proto = protoIt->second.getGC<Object>();
        int depth = 0;
        while (proto && depth < 50) {
          // Check getter on prototype
          auto protoGetterIt = proto->properties.find("__get_" + propName);
          if (protoGetterIt != proto->properties.end() && protoGetterIt->second.isFunction()) {
            auto getter = protoGetterIt->second.getGC<Function>();
            LIGHTJS_RETURN(invokeFunction(getter, {}, obj));
          }
          auto found = proto->properties.find(propName);
          if (found != proto->properties.end()) {
            LIGHTJS_RETURN(found->second);
          }
          auto nextProto = proto->properties.find("__proto__");
          if (nextProto == proto->properties.end() || !nextProto->second.isObject()) break;
          proto = nextProto->second.getGC<Object>();
          depth++;
        }
      }
    }
  }

  if (obj.isMap()) {
    auto mapPtr = obj.getGC<Map>();
    if (propName == "size") {
      LIGHTJS_RETURN(Value(static_cast<double>(mapPtr->size())));
    }
    // Check Map own properties first
    auto ownIt = mapPtr->properties.find(propName);
    if (ownIt != mapPtr->properties.end()) {
      LIGHTJS_RETURN(ownIt->second);
    }
    // Look up from Map.prototype
    if (auto mapCtor = env_->get("Map"); mapCtor && mapCtor->isFunction()) {
      auto protoIt = mapCtor->getGC<Function>()->properties.find("prototype");
      if (protoIt != mapCtor->getGC<Function>()->properties.end() && protoIt->second.isObject()) {
        auto proto = protoIt->second.getGC<Object>();
        auto methIt = proto->properties.find(propName);
        if (methIt != proto->properties.end()) {
          LIGHTJS_RETURN(methIt->second);
        }
      }
    }
  }

  if (obj.isWeakMap()) {
    auto wmPtr = obj.getGC<WeakMap>();
    auto ownIt = wmPtr->properties.find(propName);
    if (ownIt != wmPtr->properties.end()) {
      LIGHTJS_RETURN(ownIt->second);
    }
    if (auto wmCtor = env_->get("WeakMap"); wmCtor && wmCtor->isFunction()) {
      auto protoIt = wmCtor->getGC<Function>()->properties.find("prototype");
      if (protoIt != wmCtor->getGC<Function>()->properties.end() && protoIt->second.isObject()) {
        auto proto = protoIt->second.getGC<Object>();
        auto methIt = proto->properties.find(propName);
        if (methIt != proto->properties.end()) {
          LIGHTJS_RETURN(methIt->second);
        }
      }
    }
  }

  if (obj.isWeakSet()) {
    auto wsPtr = obj.getGC<WeakSet>();
    auto ownIt = wsPtr->properties.find(propName);
    if (ownIt != wsPtr->properties.end()) {
      LIGHTJS_RETURN(ownIt->second);
    }
    if (auto wsCtor = env_->get("WeakSet"); wsCtor && wsCtor->isFunction()) {
      auto protoIt = wsCtor->getGC<Function>()->properties.find("prototype");
      if (protoIt != wsCtor->getGC<Function>()->properties.end() && protoIt->second.isObject()) {
        auto proto = protoIt->second.getGC<Object>();
        auto methIt = proto->properties.find(propName);
        if (methIt != proto->properties.end()) {
          LIGHTJS_RETURN(methIt->second);
        }
      }
    }
  }

  if (obj.isSet()) {
    auto setPtr = obj.getGC<Set>();
    if (propName == "size") {
      LIGHTJS_RETURN(Value(static_cast<double>(setPtr->size())));
    }
    // Check Set own properties first
    auto ownIt = setPtr->properties.find(propName);
    if (ownIt != setPtr->properties.end()) {
      LIGHTJS_RETURN(ownIt->second);
    }
    // Look up from Set.prototype
    if (auto setCtor = env_->get("Set"); setCtor && setCtor->isFunction()) {
      auto protoIt = setCtor->getGC<Function>()->properties.find("prototype");
      if (protoIt != setCtor->getGC<Function>()->properties.end() && protoIt->second.isObject()) {
        auto proto = protoIt->second.getGC<Object>();
        auto methIt = proto->properties.find(propName);
        if (methIt != proto->properties.end()) {
          LIGHTJS_RETURN(methIt->second);
        }
      }
    }
  }

  if (obj.isTypedArray()) {
    auto [found, value] = getPropertyForPrimitive(obj, propName);
    if (hasError()) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (found) {
      LIGHTJS_RETURN(value);
    }
  }

  if (obj.isRegex()) {
    auto regexPtr = obj.getGC<Regex>();

    // Own properties first (e.g. lastIndex)
    {
      auto getterIt = regexPtr->properties.find("__get_" + propName);
      if (getterIt != regexPtr->properties.end() && getterIt->second.isFunction()) {
        auto getter = getterIt->second.getGC<Function>();
        LIGHTJS_RETURN(invokeFunction(getter, {}, obj));
      }
      auto propIt = regexPtr->properties.find(propName);
      if (propIt != regexPtr->properties.end()) {
        LIGHTJS_RETURN(propIt->second);
      }
    }

    // RegExp flag getters (ES spec 21.2.5.*)
    if (propName == "global") {
      LIGHTJS_RETURN(Value(regexPtr->flags.find('g') != std::string::npos));
    }
    if (propName == "ignoreCase") {
      LIGHTJS_RETURN(Value(regexPtr->flags.find('i') != std::string::npos));
    }
    if (propName == "multiline") {
      LIGHTJS_RETURN(Value(regexPtr->flags.find('m') != std::string::npos));
    }
    if (propName == "dotAll") {
      LIGHTJS_RETURN(Value(regexPtr->flags.find('s') != std::string::npos));
    }
    if (propName == "unicode") {
      LIGHTJS_RETURN(Value(regexPtr->flags.find('u') != std::string::npos));
    }
    if (propName == "sticky") {
      LIGHTJS_RETURN(Value(regexPtr->flags.find('y') != std::string::npos));
    }
    if (propName == "hasIndices") {
      LIGHTJS_RETURN(Value(regexPtr->flags.find('d') != std::string::npos));
    }

    if (propName == "flags") {
      // Sort flags in canonical order: dgimsuy
      std::string sortedFlags;
      const char* canonical = "dgimsuy";
      for (const char* p = canonical; *p; ++p) {
        if (regexPtr->flags.find(*p) != std::string::npos) {
          sortedFlags += *p;
        }
      }
      LIGHTJS_RETURN(Value(sortedFlags));
    }
    if (propName == "source") {
      std::string src = regexPtr->pattern;
      if (src.empty()) src = "(?:)";
      LIGHTJS_RETURN(Value(src));
    }
    if (propName == "toString") {
      auto toStringFn = GarbageCollector::makeGC<Function>();
      toStringFn->isNative = true;
      toStringFn->nativeFunc = [regexPtr](const std::vector<Value>& /*args*/) -> Value {
        return Value(std::string("/") + regexPtr->pattern + "/" + regexPtr->flags);
      };
      LIGHTJS_RETURN(Value(toStringFn));
    }

    if (propName == "test") {
      auto testFn = GarbageCollector::makeGC<Function>();
      testFn->isNative = true;
      testFn->nativeFunc = [regexPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(false);
        std::string str = args[0].toString();
        bool global = regexPtr->flags.find('g') != std::string::npos;
        bool sticky = regexPtr->flags.find('y') != std::string::npos;
        size_t lastIndex = 0;
        if (global || sticky) {
          auto liIt = regexPtr->properties.find("lastIndex");
          if (liIt != regexPtr->properties.end()) {
            double li = liIt->second.toNumber();
            lastIndex = (li >= 0) ? static_cast<size_t>(li) : 0;
          }
          if (lastIndex > str.size()) {
            regexPtr->properties["lastIndex"] = Value(0.0);
            return Value(false);
          }
        }
        // For global/sticky, search from lastIndex but preserve anchors
        // by using match_prev_avail and iterating from the correct position
#if USE_SIMPLE_REGEX
        std::vector<simple_regex::Regex::Match> matches;
        bool found = regexPtr->regex->search(str, matches);
        if (found && (global || sticky)) {
          // Find the first match at or after lastIndex
          bool matchOk = false;
          for (const auto& m : matches) {
            if (global && m.position >= lastIndex) { matchOk = true; break; }
            if (sticky && m.position == lastIndex) { matchOk = true; break; }
          }
          found = matchOk;
        }
        if (found && (global || sticky) && !matches.empty()) {
          for (const auto& m : matches) {
            size_t pos = m.position;
            if ((sticky && pos == lastIndex) || (global && !sticky && pos >= lastIndex)) {
              regexPtr->properties["lastIndex"] = Value(static_cast<double>(pos + m.length));
              break;
            }
          }
        } else if (!found && (global || sticky)) {
          regexPtr->properties["lastIndex"] = Value(0.0);
        }
        return Value(found);
#else
        std::smatch match;
        bool found = false;
        if (global || sticky) {
          // Search starting from lastIndex position using iterators
          auto searchStart = str.cbegin() + lastIndex;
          auto flags = std::regex_constants::match_default;
          if (lastIndex > 0) {
            flags |= std::regex_constants::match_prev_avail;
          }
          found = std::regex_search(searchStart, str.cend(), match, regexPtr->regex, flags);
          if (found && sticky && match.position(0) != 0) {
            found = false;
          }
        } else {
          found = std::regex_search(str, match, regexPtr->regex);
        }
        if (found && (global || sticky)) {
          regexPtr->properties["lastIndex"] = Value(static_cast<double>(lastIndex + match.position(0) + match.length(0)));
        } else if (!found && (global || sticky)) {
          regexPtr->properties["lastIndex"] = Value(0.0);
        }
        return Value(found);
#endif
      };
      LIGHTJS_RETURN(Value(testFn));
    }

    if (propName == "exec") {
      auto execFn = GarbageCollector::makeGC<Function>();
      execFn->isNative = true;
      execFn->nativeFunc = [regexPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(Null{});
        std::string str = args[0].toString();
        bool global = regexPtr->flags.find('g') != std::string::npos;
        bool sticky = regexPtr->flags.find('y') != std::string::npos;
        size_t lastIndex = 0;
        if (global || sticky) {
          auto liIt = regexPtr->properties.find("lastIndex");
          if (liIt != regexPtr->properties.end()) {
            double li = liIt->second.toNumber();
            lastIndex = (li >= 0) ? static_cast<size_t>(li) : 0;
          }
          if (lastIndex > str.size()) {
            regexPtr->properties["lastIndex"] = Value(0.0);
            return Value(Null{});
          }
        }
#if USE_SIMPLE_REGEX
        std::vector<simple_regex::Regex::Match> matches;
        bool found = regexPtr->regex->search(str, matches);
        if (found && (global || sticky)) {
          bool matchOk = false;
          for (const auto& m : matches) {
            if (global && m.position >= lastIndex) { matchOk = true; break; }
            if (sticky && m.position == lastIndex) { matchOk = true; break; }
          }
          found = matchOk;
        }
        if (found && !matches.empty()) {
          auto arr = makeArrayWithPrototype();
          size_t matchPos = 0;
          size_t matchLen = 0;
          for (const auto& m : matches) {
            if ((global || sticky) && m.position < lastIndex) continue;
            if (sticky && m.position != lastIndex) { found = false; break; }
            arr->elements.push_back(Value(m.str));
            if (arr->elements.size() == 1) { matchPos = m.position; matchLen = m.length; }
          }
          if (!found || arr->elements.empty()) {
            if (global || sticky) regexPtr->properties["lastIndex"] = Value(0.0);
            return Value(Null{});
          }
          arr->properties["index"] = Value(static_cast<double>(matchPos));
          arr->properties["input"] = Value(str);
          if (global || sticky) {
            regexPtr->properties["lastIndex"] = Value(static_cast<double>(matchPos + matchLen));
          }
          return Value(arr);
        }
#else
        std::smatch match;
        bool found = false;
        if (global || sticky) {
          auto searchStart = str.cbegin() + lastIndex;
          auto flags = std::regex_constants::match_default;
          if (lastIndex > 0) {
            flags |= std::regex_constants::match_prev_avail;
          }
          found = std::regex_search(searchStart, str.cend(), match, regexPtr->regex, flags);
          if (found && sticky && match.position(0) != 0) {
            found = false;
          }
        } else {
          found = std::regex_search(str, match, regexPtr->regex);
        }
        if (found) {
          auto arr = GarbageCollector::makeGC<Array>();
          for (const auto& m : match) {
            arr->elements.push_back(Value(m.str()));
          }
          arr->properties["index"] = Value(static_cast<double>(lastIndex + match.position(0)));
          arr->properties["input"] = Value(str);
          if (global || sticky) {
            regexPtr->properties["lastIndex"] = Value(static_cast<double>(lastIndex + match.position(0) + match.length(0)));
          }
          return Value(arr);
        }
#endif
        if (global || sticky) {
          regexPtr->properties["lastIndex"] = Value(0.0);
        }
        return Value(Null{});
      };
      LIGHTJS_RETURN(Value(execFn));
    }

    if (propName == "source") {
      LIGHTJS_RETURN(Value(regexPtr->pattern));
    }

    if (propName == "flags") {
      LIGHTJS_RETURN(Value(regexPtr->flags));
    }

    // Walk prototype chain for methods not handled above
    {
      auto protoIt = regexPtr->properties.find("__proto__");
      if (protoIt != regexPtr->properties.end() && protoIt->second.isObject()) {
        auto proto = protoIt->second.getGC<Object>();
        int depth = 0;
        while (proto && depth < 50) {
          auto protoGetterIt = proto->properties.find("__get_" + propName);
          if (protoGetterIt != proto->properties.end() && protoGetterIt->second.isFunction()) {
            auto getter = protoGetterIt->second.getGC<Function>();
            LIGHTJS_RETURN(invokeFunction(getter, {}, obj));
          }
          auto found = proto->properties.find(propName);
          if (found != proto->properties.end()) {
            LIGHTJS_RETURN(found->second);
          }
          auto nextProto = proto->properties.find("__proto__");
          if (nextProto == proto->properties.end() || !nextProto->second.isObject()) break;
          proto = nextProto->second.getGC<Object>();
          depth++;
        }
      }
    }
  }

  if (obj.isError()) {
    auto errorPtr = obj.getGC<Error>();

    if (propName == "hasOwnProperty" || propName == "propertyIsEnumerable") {
      // Reuse the Object.prototype-style implementations (receiver-based).
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->properties["__uses_this_arg__"] = Value(true);
      fn->properties["__throw_on_new__"] = Value(true);
      if (propName == "hasOwnProperty") {
        fn->nativeFunc = [](const std::vector<Value>& args) -> Value {
          if (args.size() < 2) return Value(false);
          Value receiver = args[0];
          std::string key = valueToPropertyKey(args[1]);
          if (receiver.isError()) {
            auto e = receiver.getGC<Error>();
            return Value(e->properties.count(key) > 0);
          }
          return Value(false);
        };
      } else {
        fn->nativeFunc = [](const std::vector<Value>& args) -> Value {
          if (args.size() < 2) return Value(false);
          Value receiver = args[0];
          std::string key = valueToPropertyKey(args[1]);
          if (!receiver.isError()) return Value(false);
          auto e = receiver.getGC<Error>();
          if (e->properties.count(key) == 0) return Value(false);
          bool enumerable = e->properties.find("__non_enum_" + key) == e->properties.end();
          return Value(enumerable);
        };
      }
      LIGHTJS_RETURN(Value(fn));
    }

    // Own properties + prototype chain
    {
      auto getterIt = errorPtr->properties.find("__get_" + propName);
      if (getterIt != errorPtr->properties.end() && getterIt->second.isFunction()) {
        auto getter = getterIt->second.getGC<Function>();
        LIGHTJS_RETURN(invokeFunction(getter, {}, obj));
      }
      auto it = errorPtr->properties.find(propName);
      if (it != errorPtr->properties.end()) {
        LIGHTJS_RETURN(it->second);
      }
      auto protoIt = errorPtr->properties.find("__proto__");
      if (protoIt != errorPtr->properties.end() && protoIt->second.isObject()) {
        auto proto = protoIt->second.getGC<Object>();
        int depth = 0;
        while (proto && depth < 50) {
          auto protoGetterIt = proto->properties.find("__get_" + propName);
          if (protoGetterIt != proto->properties.end() && protoGetterIt->second.isFunction()) {
            auto getter = protoGetterIt->second.getGC<Function>();
            LIGHTJS_RETURN(invokeFunction(getter, {}, obj));
          }
          auto found = proto->properties.find(propName);
          if (found != proto->properties.end()) {
            LIGHTJS_RETURN(found->second);
          }
          auto nextProto = proto->properties.find("__proto__");
          if (nextProto == proto->properties.end() || !nextProto->second.isObject()) break;
          proto = nextProto->second.getGC<Object>();
          depth++;
        }
      }
    }

    if (propName == "toString") {
      auto toStringFn = GarbageCollector::makeGC<Function>();
      toStringFn->isNative = true;
      toStringFn->nativeFunc = [errorPtr](const std::vector<Value>&) -> Value {
        return Value(errorPtr->toString());
      };
      LIGHTJS_RETURN(Value(toStringFn));
    }

    if (propName == "name") {
      LIGHTJS_RETURN(Value(errorPtr->getName()));
    }

    if (propName == "constructor") {
      if (auto ctor = env_->get(errorPtr->getName())) {
        LIGHTJS_RETURN(*ctor);
      }
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    if (propName == "message") {
      LIGHTJS_RETURN(Value(errorPtr->message));
    }
  }

  if (obj.isNumber()) {
    double num = std::get<double>(obj.data);

    if (propName == "toString") {
      auto toStringFn = GarbageCollector::makeGC<Function>();
      toStringFn->isNative = true;
      toStringFn->nativeFunc = [num](const std::vector<Value>& args) -> Value {
        if (args.empty() || args[0].isUndefined()) {
          return Value(ecmaNumberToString(num));
        }
        int radix = static_cast<int>(args[0].toNumber());
        if (radix < 2 || radix > 36) {
          throw std::runtime_error("RangeError: toString() radix must be between 2 and 36");
        }
        if (std::isnan(num)) return Value(std::string("NaN"));
        if (std::isinf(num)) return Value(num < 0 ? std::string("-Infinity") : std::string("Infinity"));
        if (radix == 10) return Value(ecmaNumberToString(num));
        bool negative = num < 0;
        double absNum = negative ? -num : num;
        std::string result;
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
        if (fracPart > 0) {
          result += '.';
          for (int i = 0; i < 20 && fracPart > 0; ++i) {
            fracPart *= radix;
            int digit = static_cast<int>(fracPart);
            result += (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
            fracPart -= digit;
          }
          while (!result.empty() && result.back() == '0') result.pop_back();
          if (!result.empty() && result.back() == '.') result.pop_back();
        }
        if (negative) result = "-" + result;
        return Value(result);
      };
      LIGHTJS_RETURN(Value(toStringFn));
    }

    // Fallback to Number.prototype / Object.prototype for user-defined properties.
    // Walk the prototype chain: Number.prototype → Object.prototype
    // Use original primitive as receiver for getters (spec: Reference base = primitive)
    if (auto numberCtor = env_->get("Number")) {
      auto [foundProto, proto] = getPropertyForPrimitive(*numberCtor, "prototype");
      if (foundProto && proto.isObject()) {
        auto current = proto.getGC<Object>();
        int depth = 0;
        while (current && depth < 20) {
          depth++;
          auto getterIt = current->properties.find("__get_" + propName);
          if (getterIt != current->properties.end() && getterIt->second.isFunction()) {
            LIGHTJS_RETURN(callFunction(getterIt->second, {}, obj));
          }
          auto propIt = current->properties.find(propName);
          if (propIt != current->properties.end()) {
            LIGHTJS_RETURN(propIt->second);
          }
          auto nextProto = current->properties.find("__proto__");
          if (nextProto == current->properties.end() || !nextProto->second.isObject()) break;
          current = nextProto->second.getGC<Object>();
        }
      }
    }
  }

  if (obj.isBool()) {
    // Fallback to Boolean.prototype / Object.prototype for user-defined properties.
    if (auto boolCtor = env_->get("Boolean")) {
      auto [foundProto, proto] = getPropertyForPrimitive(*boolCtor, "prototype");
      if (foundProto && proto.isObject()) {
        auto current = proto.getGC<Object>();
        int depth = 0;
        while (current && depth < 20) {
          depth++;
          auto getterIt = current->properties.find("__get_" + propName);
          if (getterIt != current->properties.end() && getterIt->second.isFunction()) {
            LIGHTJS_RETURN(callFunction(getterIt->second, {}, obj));
          }
          auto propIt = current->properties.find(propName);
          if (propIt != current->properties.end()) {
            LIGHTJS_RETURN(propIt->second);
          }
          auto nextProto = current->properties.find("__proto__");
          if (nextProto == current->properties.end() || !nextProto->second.isObject()) break;
          current = nextProto->second.getGC<Object>();
        }
      }
      // Fallback: try wrapper approach
      auto wrapper = GarbageCollector::makeGC<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      wrapper->properties["__primitive_value__"] = obj;
      if (foundProto && (proto.isObject() || proto.isNull())) {
        wrapper->properties["__proto__"] = proto;
      }
      auto [found, value] = getPropertyForPrimitive(Value(wrapper), propName);
      if (found) {
        LIGHTJS_RETURN(value);
      }
    }
  }

  if (obj.isString()) {
    std::string str = std::get<std::string>(obj.data);

    if (propName == "toString" || propName == "valueOf") {
      auto toStringFn = GarbageCollector::makeGC<Function>();
      toStringFn->isNative = true;
      toStringFn->nativeFunc = [str](const std::vector<Value>&) -> Value {
        return Value(str);
      };
      LIGHTJS_RETURN(Value(toStringFn));
    }

    if (propName == "length") {
      // JavaScript string length is measured in UTF-16 code units.
      LIGHTJS_RETURN(Value(static_cast<double>(String_utf16Length(str))));
    }

    // Support numeric indexing for strings (e.g., str[0])
    // Check if propName is a valid non-negative integer
    bool isNumericIndex = !propName.empty() && std::all_of(propName.begin(), propName.end(), ::isdigit);
    if (isNumericIndex) {
      size_t index = std::stoul(propName);
      size_t strLen = String_utf16Length(str);
      if (index < strLen) {
        std::vector<Value> args = {obj, Value(static_cast<double>(index))};
        LIGHTJS_RETURN(String_charAt(args));
      }
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    if (propName == iteratorKey) {
      if (auto strCtor = env_->get("String"); strCtor && strCtor->isObject()) {
        auto strObj = std::get<GCPtr<Object>>(strCtor->data);
        auto protoIt = strObj->properties.find("prototype");
        if (protoIt != strObj->properties.end() && protoIt->second.isObject()) {
          auto protoObj = protoIt->second.getGC<Object>();
          auto methodIt = protoObj->properties.find(iteratorKey);
          if (methodIt != protoObj->properties.end() && methodIt->second.isFunction()) {
            LIGHTJS_RETURN(methodIt->second);
          }
        }
      }
      auto charArray = GarbageCollector::makeGC<Array>();
      GarbageCollector::instance().reportAllocation(sizeof(Array));
      size_t byteIndex = 0;
      while (byteIndex < str.size()) {
        size_t start = byteIndex;
        unicode::decodeUTF8(str, byteIndex);
        charArray->elements.push_back(Value(str.substr(start, byteIndex - start)));
      }
      LIGHTJS_RETURN(createIteratorFactory(charArray));
    }

    if (propName == "includes") {
      auto includesFn = GarbageCollector::makeGC<Function>();
      includesFn->isNative = true;
      includesFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        // IsRegExp check via Symbol.match
        if (!args.empty() && !args[0].isUndefined() && !args[0].isNull()) {
          auto* interp = getGlobalInterpreter();
          if (interp && (args[0].isObject() || args[0].isRegex() || args[0].isArray() || args[0].isFunction())) {
            const std::string& matchKey = WellKnownSymbols::matchKey();
            auto [found, matchProp] = interp->getPropertyForExternal(args[0], matchKey);
            if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
            if (found && !matchProp.isUndefined()) {
              if (matchProp.isBool() ? std::get<bool>(matchProp.data) : true) {
                throw std::runtime_error("TypeError: First argument to String.prototype.includes must not be a regular expression");
              }
            } else if (args[0].isRegex()) {
              throw std::runtime_error("TypeError: First argument to String.prototype.includes must not be a regular expression");
            }
          } else if (args[0].isRegex()) {
            throw std::runtime_error("TypeError: First argument to String.prototype.includes must not be a regular expression");
          }
        }
        std::string searchStr = args.empty() ? "undefined" : toStringForStringBuiltinArg(args[0]);
        double pos = 0;
        if (args.size() > 1 && !args[1].isUndefined()) {
          pos = toIntegerForStringBuiltinArg(args[1]);
          if (pos < 0) pos = 0;
        }
        size_t position = static_cast<size_t>(std::min(pos, static_cast<double>(str.length())));
        return Value(str.find(searchStr, position) != std::string::npos);
      };
      setNativeFnProps(includesFn, "includes", 1);
      LIGHTJS_RETURN(Value(includesFn));
    }

    if (propName == "repeat") {
      auto repeatFn = GarbageCollector::makeGC<Function>();
      repeatFn->isNative = true;
      repeatFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        double count = args.empty() ? 0 : toNumberForStringBuiltinArg(args[0]);
        if (std::isnan(count)) count = 0;
        if (count < 0 || count == std::numeric_limits<double>::infinity()) {
          throw std::runtime_error("RangeError: Invalid count value");
        }
        size_t n = static_cast<size_t>(count);
        static constexpr size_t kMaxRepeatBytes = 256 * 1024 * 1024;
        if (str.size() > 0 && n > kMaxRepeatBytes / str.size()) {
          throw std::runtime_error("RangeError: Invalid count value");
        }
        std::string result;
        result.reserve(str.size() * n);
        for (size_t i = 0; i < n; ++i) result += str;
        return Value(result);
      };
      setNativeFnProps(repeatFn, "repeat", 1);
      LIGHTJS_RETURN(Value(repeatFn));
    }

    if (propName == "padStart") {
      auto padStartFn = GarbageCollector::makeGC<Function>();
      padStartFn->isNative = true;
      padStartFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(str);
        double maxLen = toIntegerForStringBuiltinArg(args[0]);
        if (std::isnan(maxLen) || maxLen < 0) maxLen = 0;
        size_t targetLength = static_cast<size_t>(maxLen);
        if (targetLength <= str.length()) return Value(str);

        std::string padString = (args.size() > 1 && !args[1].isUndefined()) ? toStringForStringBuiltinArg(args[1]) : " ";
        if (padString.empty()) return Value(str);

        size_t padLength = targetLength - str.length();
        std::string result;
        while (result.length() < padLength) {
          result += padString;
        }
        result = result.substr(0, padLength) + str;
        return Value(result);
      };
      setNativeFnProps(padStartFn, "padStart", 1);
      LIGHTJS_RETURN(Value(padStartFn));
    }

    if (propName == "padEnd") {
      auto padEndFn = GarbageCollector::makeGC<Function>();
      padEndFn->isNative = true;
      padEndFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(str);
        double maxLen = toIntegerForStringBuiltinArg(args[0]);
        if (std::isnan(maxLen) || maxLen < 0) maxLen = 0;
        size_t targetLength = static_cast<size_t>(maxLen);
        if (targetLength <= str.length()) return Value(str);

        std::string padString = (args.size() > 1 && !args[1].isUndefined()) ? toStringForStringBuiltinArg(args[1]) : " ";
        if (padString.empty()) return Value(str);

        size_t padLength = targetLength - str.length();
        std::string result = str;
        while (result.length() < targetLength) {
          result += padString;
        }
        result = result.substr(0, targetLength);
        return Value(result);
      };
      setNativeFnProps(padEndFn, "padEnd", 1);
      LIGHTJS_RETURN(Value(padEndFn));
    }

    if (propName == "trim") {
      auto trimFn = GarbageCollector::makeGC<Function>();
      trimFn->isNative = true;
      trimFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        return Value(stripESWhitespace(str));
      };
      setNativeFnProps(trimFn, "trim", 0);
      LIGHTJS_RETURN(Value(trimFn));
    }

    if (propName == "trimStart") {
      auto trimStartFn = GarbageCollector::makeGC<Function>();
      trimStartFn->isNative = true;
      trimStartFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        return Value(stripLeadingESWhitespace(str));
      };
      setNativeFnProps(trimStartFn, "trimStart", 0);
      LIGHTJS_RETURN(Value(trimStartFn));
    }

    if (propName == "trimEnd") {
      auto trimEndFn = GarbageCollector::makeGC<Function>();
      trimEndFn->isNative = true;
      trimEndFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        return Value(stripTrailingESWhitespace(str));
      };
      setNativeFnProps(trimEndFn, "trimEnd", 0);
      LIGHTJS_RETURN(Value(trimEndFn));
    }

    if (propName == "split") {
      auto splitFn = GarbageCollector::makeGC<Function>();
      splitFn->isNative = true;
      splitFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        // ES2020 21.1.3.17: If separator is not null/undefined, check for @@split
        if (!args.empty() && !args[0].isUndefined() && !args[0].isNull() && !args[0].isString() && !args[0].isBool() && !args[0].isNumber() && !args[0].isBigInt() && !args[0].isSymbol()) {
          auto* interp = getGlobalInterpreter();
          if (interp) {
            const std::string& splitKey = WellKnownSymbols::splitKey();
            auto [found, splitter] = interp->getPropertyForExternal(args[0], splitKey);
            if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
            if (found && !splitter.isUndefined() && !splitter.isNull()) {
              if (!splitter.isFunction()) throw std::runtime_error("TypeError: @@split is not a function");
              Value limitVal = args.size() > 1 ? args[1] : Value(Undefined{});
              Value result = interp->callForHarness(splitter, {Value(str), limitVal}, args[0]);
              if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
              return result;
            }
          }
        }
        auto result = makeArrayWithPrototype();

        // Handle limit parameter (ToUint32 per spec)
        uint32_t limit = 0xFFFFFFFF;
        if (args.size() > 1 && !args[1].isUndefined()) {
          double lim = toNumberForStringBuiltinArg(args[1]);
          if (std::isnan(lim) || lim == 0.0) {
            limit = 0;
          } else if (!std::isfinite(lim)) {
            limit = (lim > 0) ? 0xFFFFFFFF : 0;
          } else {
            // ToUint32: truncate and modulo 2^32
            double integer = std::trunc(lim);
            double mod = std::fmod(integer, 4294967296.0);
            if (mod < 0) mod += 4294967296.0;
            limit = static_cast<uint32_t>(mod);
          }
        }

        if (limit == 0) return Value(result);

        if (args.empty() || args[0].isUndefined()) {
          // No separator: return array with entire string
          result->elements.push_back(Value(str));
          return Value(result);
        }

        std::string separator = toStringForStringBuiltinArg(args[0]);

        if (separator.empty()) {
          // Split into individual characters
          size_t len = unicode::utf8Length(str);
          for (size_t i = 0; i < len && result->elements.size() < limit; ++i) {
            result->elements.push_back(Value(unicode::charAt(str, i)));
          }
          return Value(result);
        }

        size_t start = 0;
        size_t pos;
        while ((pos = str.find(separator, start)) != std::string::npos) {
          if (result->elements.size() >= limit) break;
          result->elements.push_back(Value(str.substr(start, pos - start)));
          start = pos + separator.length();
        }
        if (result->elements.size() < limit) {
          result->elements.push_back(Value(str.substr(start)));
        }
        return Value(result);
      };
      setNativeFnProps(splitFn, "split", 2);
      LIGHTJS_RETURN(Value(splitFn));
    }

    if (propName == "startsWith") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        // IsRegExp check: access Symbol.match, throw if getter throws or returns truthy
        if (!args.empty() && !args[0].isUndefined() && !args[0].isNull()) {
          auto* interp = getGlobalInterpreter();
          if (interp && (args[0].isObject() || args[0].isRegex() || args[0].isArray() || args[0].isFunction())) {
            const std::string& matchKey = WellKnownSymbols::matchKey();
            auto [found, matchProp] = interp->getPropertyForExternal(args[0], matchKey);
            if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
            if (found && !matchProp.isUndefined()) {
              if (matchProp.isBool() ? std::get<bool>(matchProp.data) : true) {
                throw std::runtime_error("TypeError: First argument to String.prototype.startsWith must not be a regular expression");
              }
            } else if (args[0].isRegex()) {
              throw std::runtime_error("TypeError: First argument to String.prototype.startsWith must not be a regular expression");
            }
          } else if (args[0].isRegex()) {
            throw std::runtime_error("TypeError: First argument to String.prototype.startsWith must not be a regular expression");
          }
        }
        std::string searchStr = args.empty() ? "undefined" : toStringForStringBuiltinArg(args[0]);
        double pos = 0;
        if (args.size() > 1 && !args[1].isUndefined()) {
          pos = toIntegerForStringBuiltinArg(args[1]);
        }
        size_t position = static_cast<size_t>(std::max(0.0, std::min(pos, static_cast<double>(str.length()))));
        if (position + searchStr.length() > str.length()) return Value(false);
        return Value(str.compare(position, searchStr.length(), searchStr) == 0);
      };
      setNativeFnProps(fn, "startsWith", 1);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "endsWith") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        // IsRegExp check
        if (!args.empty() && !args[0].isUndefined() && !args[0].isNull()) {
          auto* interp = getGlobalInterpreter();
          if (interp && (args[0].isObject() || args[0].isRegex() || args[0].isArray() || args[0].isFunction())) {
            const std::string& matchKey = WellKnownSymbols::matchKey();
            auto [found, matchProp] = interp->getPropertyForExternal(args[0], matchKey);
            if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
            if (found && !matchProp.isUndefined()) {
              if (matchProp.isBool() ? std::get<bool>(matchProp.data) : true) {
                throw std::runtime_error("TypeError: First argument to String.prototype.endsWith must not be a regular expression");
              }
            } else if (args[0].isRegex()) {
              throw std::runtime_error("TypeError: First argument to String.prototype.endsWith must not be a regular expression");
            }
          } else if (args[0].isRegex()) {
            throw std::runtime_error("TypeError: First argument to String.prototype.endsWith must not be a regular expression");
          }
        }
        std::string searchStr = args.empty() ? "undefined" : toStringForStringBuiltinArg(args[0]);
        double e = static_cast<double>(str.length());
        if (args.size() > 1 && !args[1].isUndefined()) {
          e = toIntegerForStringBuiltinArg(args[1]);
        }
        size_t endPos = static_cast<size_t>(std::max(0.0, std::min(e, static_cast<double>(str.length()))));
        if (searchStr.length() > endPos) return Value(false);
        return Value(str.compare(endPos - searchStr.length(), searchStr.length(), searchStr) == 0);
      };
      setNativeFnProps(fn, "endsWith", 1);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "normalize") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        // Basic implementation - just returns the string as-is
        // Full Unicode normalization (NFC, NFD, etc.) is complex
        return Value(str);
      };
      setNativeFnProps(fn, "normalize", 0);
      LIGHTJS_RETURN(Value(fn));
    }

    // String.prototype.isWellFormed (ES2024) - check for lone surrogates
    if (propName == "isWellFormed") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        // Decode UTF-8 code points, check for lone surrogates in UTF-16 model
        std::vector<int32_t> cps;
        size_t i = 0;
        while (i < str.size()) {
          unsigned char ch = static_cast<unsigned char>(str[i]);
          int32_t cp = 0; int extra = 0;
          if (ch < 0x80) { cp = ch; extra = 0; }
          else if ((ch & 0xE0) == 0xC0) { cp = ch & 0x1F; extra = 1; }
          else if ((ch & 0xF0) == 0xE0) { cp = ch & 0x0F; extra = 2; }
          else if ((ch & 0xF8) == 0xF0) { cp = ch & 0x07; extra = 3; }
          else { return Value(false); }
          if (i + extra >= str.size()) return Value(false);
          for (int j = 0; j < extra; j++) {
            i++;
            if ((static_cast<unsigned char>(str[i]) & 0xC0) != 0x80) return Value(false);
            cp = (cp << 6) | (static_cast<unsigned char>(str[i]) & 0x3F);
          }
          i++;
          cps.push_back(cp);
        }
        for (size_t k = 0; k < cps.size(); k++) {
          int32_t cp = cps[k];
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (k + 1 < cps.size() && cps[k + 1] >= 0xDC00 && cps[k + 1] <= 0xDFFF) {
              k++; continue; // Valid pair
            }
            return Value(false); // Lone high surrogate
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return Value(false); // Lone low surrogate
          }
        }
        return Value(true);
      };
      setNativeFnProps(fn, "isWellFormed", 0);
      LIGHTJS_RETURN(Value(fn));
    }

    // String.prototype.toWellFormed (ES2024) - replace lone surrogates with U+FFFD
    if (propName == "toWellFormed") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        // First decode all code points with byte spans
        struct CpSpan { int32_t cp; size_t start; size_t end; };
        std::vector<CpSpan> cps;
        size_t i = 0;
        while (i < str.size()) {
          unsigned char ch = static_cast<unsigned char>(str[i]);
          size_t start = i;
          int32_t cp = 0; int extra = 0; bool valid = true;
          if (ch < 0x80) { cp = ch; extra = 0; }
          else if ((ch & 0xE0) == 0xC0) { cp = ch & 0x1F; extra = 1; }
          else if ((ch & 0xF0) == 0xE0) { cp = ch & 0x0F; extra = 2; }
          else if ((ch & 0xF8) == 0xF0) { cp = ch & 0x07; extra = 3; }
          else { valid = false; }
          if (valid && i + extra < str.size()) {
            for (int j = 0; j < extra; j++) {
              i++;
              if ((static_cast<unsigned char>(str[i]) & 0xC0) != 0x80) { valid = false; break; }
              cp = (cp << 6) | (static_cast<unsigned char>(str[i]) & 0x3F);
            }
            i++;
          } else { valid = false; i++; }
          if (!valid) cp = 0xFFFD;
          cps.push_back({cp, start, i});
        }
        // Build result, combining paired surrogates and replacing lone ones
        std::string result;
        for (size_t k = 0; k < cps.size(); k++) {
          int32_t cp = cps[k].cp;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (k + 1 < cps.size() && cps[k + 1].cp >= 0xDC00 && cps[k + 1].cp <= 0xDFFF) {
              int32_t combined = 0x10000 + ((cp - 0xD800) << 10) + (cps[k + 1].cp - 0xDC00);
              result += unicode::encodeUTF8(static_cast<uint32_t>(combined));
              k++; continue;
            }
            result += "\xEF\xBF\xBD";
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            result += "\xEF\xBF\xBD";
          } else if (cp == 0xFFFD) {
            result += "\xEF\xBF\xBD";
          } else {
            for (size_t j = cps[k].start; j < cps[k].end; j++) result += str[j];
          }
        }
        return Value(result);
      };
      setNativeFnProps(fn, "toWellFormed", 0);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "localeCompare") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        std::string other = args.empty() ? "undefined" : toStringForStringBuiltinArg(args[0]);
        int result = str.compare(other);
        return Value(static_cast<double>(result < 0 ? -1 : (result > 0 ? 1 : 0)));
      };
      setNativeFnProps(fn, "localeCompare", 1);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "concat") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [this, str](const std::vector<Value>& args) -> Value {
        std::string result = str;
        for (const auto& arg : args) {
          Value primitive = arg;
          if (isObjectLike(primitive)) {
            primitive = toPrimitiveValue(primitive, true);
            if (hasError()) {
              return Value(Undefined{});
            }
          }
          result += primitive.toString();
        }
        return Value(result);
      };
      setNativeFnProps(fn, "concat", 1);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "indexOf") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        std::string searchStr = args.empty() ? "undefined" : toStringForStringBuiltinArg(args[0]);
        double fromIndex = 0;
        if (args.size() > 1 && !args[1].isUndefined()) {
          fromIndex = toIntegerForStringBuiltinArg(args[1]);
          if (fromIndex < 0) fromIndex = 0;
        }
        if (fromIndex >= static_cast<double>(str.length())) {
          if (searchStr.empty()) return Value(static_cast<double>(str.length()));
          return Value(-1.0);
        }
        size_t pos = str.find(searchStr, static_cast<size_t>(fromIndex));
        if (pos == std::string::npos) return Value(-1.0);
        return Value(static_cast<double>(pos));
      };
      setNativeFnProps(fn, "indexOf", 1);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "lastIndexOf") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        std::string searchStr = args.empty() ? "undefined" : toStringForStringBuiltinArg(args[0]);
        double numPos = std::numeric_limits<double>::quiet_NaN();
        if (args.size() > 1 && !args[1].isUndefined()) {
          numPos = toNumberForStringBuiltinArg(args[1]);
        }
        size_t fromIndex;
        if (std::isnan(numPos)) {
          fromIndex = str.length();
        } else {
          double pos = std::trunc(numPos);
          if (pos < 0) pos = 0;
          fromIndex = static_cast<size_t>(std::min(pos, static_cast<double>(str.length())));
        }
        size_t pos = str.rfind(searchStr, fromIndex);
        if (pos == std::string::npos) return Value(-1.0);
        return Value(static_cast<double>(pos));
      };
      setNativeFnProps(fn, "lastIndexOf", 1);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "search") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        // ES2020 21.1.3.15: If regexp is not null/undefined, check for @@search
        if (!args.empty() && !args[0].isUndefined() && !args[0].isNull() && !args[0].isString() && !args[0].isBool() && !args[0].isNumber() && !args[0].isBigInt() && !args[0].isSymbol()) {
          auto* interp = getGlobalInterpreter();
          if (interp) {
            const std::string& searchKey = WellKnownSymbols::searchKey();
            auto [found, searcher] = interp->getPropertyForExternal(args[0], searchKey);
            if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
            if (found && !searcher.isUndefined() && !searcher.isNull()) {
              if (!searcher.isFunction()) throw std::runtime_error("TypeError: @@search is not a function");
              Value result = interp->callForHarness(searcher, {Value(str)}, args[0]);
              if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
              return result;
            }
          }
        }
        if (!args.empty() && args[0].isRegex()) {
          auto regexPtr = args[0].getGC<Regex>();
#if USE_SIMPLE_REGEX
          std::vector<simple_regex::Regex::Match> matches;
          if (regexPtr->regex->search(str, matches) && !matches.empty()) {
            return Value(static_cast<double>(matches[0].start));
          }
#else
          std::smatch match;
          if (std::regex_search(str, match, regexPtr->regex)) {
            return Value(static_cast<double>(match.position(0)));
          }
#endif
          return Value(-1.0);
        }
        // RegExpCreate fallback: construct RegExp via constructor, invoke @@search
        std::string searchStr = args.empty() ? "undefined" : toStringForStringBuiltinArg(args[0]);
        auto* interp2 = getGlobalInterpreter();
        if (interp2) {
          try {
            auto regexpCtor = interp2->resolveVariable("RegExp");
            if (regexpCtor.has_value() && regexpCtor->isFunction()) {
              Value rx = interp2->constructFromNative(*regexpCtor, {Value(searchStr)});
              if (!interp2->hasError() && rx.isRegex()) {
                const std::string& searchKey2 = WellKnownSymbols::searchKey();
                auto [found2, searcher2] = interp2->getPropertyForExternal(rx, searchKey2);
                if (found2 && searcher2.isFunction()) {
                  Value result = interp2->callForHarness(searcher2, {Value(str)}, rx);
                  if (interp2->hasError()) { Value err = interp2->getError(); interp2->clearError(); throw JsValueException(err); }
                  return result;
                }
              }
              if (interp2->hasError()) { Value err = interp2->getError(); interp2->clearError(); throw JsValueException(err); }
            }
          } catch (const JsValueException&) {
            throw;
          } catch (...) {}
        }
        // Fallback string search
        size_t pos = str.find(searchStr);
        if (pos == std::string::npos) return Value(-1.0);
        return Value(static_cast<double>(pos));
      };
      setNativeFnProps(fn, "search", 1);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "match") {
      auto matchFn = GarbageCollector::makeGC<Function>();
      matchFn->isNative = true;
      matchFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        // ES2020 21.1.3.11: If regexp is not null/undefined, check for @@match
        if (!args.empty() && !args[0].isUndefined() && !args[0].isNull() && !args[0].isString() && !args[0].isBool() && !args[0].isNumber() && !args[0].isBigInt() && !args[0].isSymbol()) {
          auto* interp = getGlobalInterpreter();
          if (interp) {
            const std::string& matchKey = WellKnownSymbols::matchKey();
            auto [found, matcher] = interp->getPropertyForExternal(args[0], matchKey);
            if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
            if (found && !matcher.isUndefined() && !matcher.isNull()) {
              if (!matcher.isFunction()) throw std::runtime_error("TypeError: @@match is not a function");
              Value result = interp->callForHarness(matcher, {Value(str)}, args[0]);
              if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
              return result;
            }
          }
        }
        if (args.empty() || !args[0].isRegex()) {
          // RegExpCreate fallback: construct RegExp via constructor, invoke @@match
          std::string pattern = args.empty() ? "" : (args[0].isUndefined() ? "" : toStringForStringBuiltinArg(args[0]));
          auto* interp2 = getGlobalInterpreter();
          if (interp2) {
            try {
              auto regexpCtor = interp2->resolveVariable("RegExp");
              if (regexpCtor.has_value() && regexpCtor->isFunction()) {
                Value rx = interp2->constructFromNative(*regexpCtor, {Value(pattern)});
                if (!interp2->hasError() && rx.isRegex()) {
                  const std::string& matchKey2 = WellKnownSymbols::matchKey();
                  auto [found2, matcher2] = interp2->getPropertyForExternal(rx, matchKey2);
                  if (found2 && matcher2.isFunction()) {
                    Value result = interp2->callForHarness(matcher2, {Value(str)}, rx);
                    if (interp2->hasError()) { Value err = interp2->getError(); interp2->clearError(); throw JsValueException(err); }
                    return result;
                  }
                }
                if (interp2->hasError()) { Value err = interp2->getError(); interp2->clearError(); throw JsValueException(err); }
              }
            } catch (const JsValueException&) {
              throw;
            } catch (...) {}
          }
          return Value(Null{});
        }
        auto regexPtr = args[0].getGC<Regex>();
#if USE_SIMPLE_REGEX
        std::vector<simple_regex::Regex::Match> matches;
        if (regexPtr->regex->search(str, matches)) {
          auto arr = GarbageCollector::makeGC<Array>();
          for (const auto& m : matches) {
            arr->elements.push_back(Value(m.str));
          }
          return Value(arr);
        }
#else
        std::smatch match;
        if (std::regex_search(str, match, regexPtr->regex)) {
          auto arr = GarbageCollector::makeGC<Array>();
          for (const auto& m : match) {
            arr->elements.push_back(Value(m.str()));
          }
          return Value(arr);
        }
#endif
        return Value(Null{});
      };
      setNativeFnProps(matchFn, "match", 1);
      LIGHTJS_RETURN(Value(matchFn));
    }

    // String.prototype.matchAll - ES2020
    if (propName == "matchAll") {
      if (auto strCtor = env_->get("String"); strCtor && strCtor->isObject()) {
        auto strObj = std::get<GCPtr<Object>>(strCtor->data);
        auto protoIt = strObj->properties.find("prototype");
        if (protoIt != strObj->properties.end() && protoIt->second.isObject()) {
          auto protoObj = protoIt->second.getGC<Object>();
          auto methodIt = protoObj->properties.find("matchAll");
          if (methodIt != protoObj->properties.end() && methodIt->second.isFunction()) {
            LIGHTJS_RETURN(methodIt->second);
          }
        }
      }
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    if (propName == "replace") {
      auto replaceFn = GarbageCollector::makeGC<Function>();
      replaceFn->isNative = true;
      auto interp = this;
      replaceFn->nativeFunc = [str, interp](const std::vector<Value>& args) -> Value {
        // ES2020 21.1.3.14: If searchValue is not null/undefined, check @@replace
        if (!args.empty() && !args[0].isUndefined() && !args[0].isNull() && !args[0].isString() && !args[0].isBool() && !args[0].isNumber() && !args[0].isBigInt() && !args[0].isSymbol()) {
          const std::string& replaceKey = WellKnownSymbols::replaceKey();
          auto [found, replacer] = interp->getPropertyForExternal(args[0], replaceKey);
          if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
          if (found && !replacer.isUndefined() && !replacer.isNull()) {
            if (!replacer.isFunction()) throw std::runtime_error("TypeError: @@replace is not a function");
            Value replaceValue = args.size() > 1 ? args[1] : Value(Undefined{});
            Value result = interp->callForHarness(replacer, {Value(str), replaceValue}, args[0]);
            if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
            return result;
          }
        }
        if (args.size() < 2) return Value(str);
        if (args[0].isRegex()) {
          auto regexPtr = args[0].getGC<Regex>();
          if (args[1].isFunction()) {
            // Function callback for regex replace
            auto callbackFn = args[1];
#if USE_SIMPLE_REGEX
            // For simple regex, do manual match+replace
            std::string result = str;
            // Simple single-match replacement
            auto matches = regexPtr->regex->match(str);
            if (!matches.empty()) {
              std::vector<Value> cbArgs;
              cbArgs.push_back(Value(matches[0]));  // match
              // TODO: add capture groups
              cbArgs.push_back(Value(static_cast<double>(str.find(matches[0]))));  // offset
              cbArgs.push_back(Value(str));  // string
              Value replacement = interp->callForHarness(callbackFn, cbArgs, Value(Undefined{}));
              std::string repStr = replacement.toString();
              size_t pos = result.find(matches[0]);
              if (pos != std::string::npos) {
                result.replace(pos, matches[0].length(), repStr);
              }
            }
            return Value(result);
#else
            // std::regex path
            std::smatch m;
            std::string result = str;
            if (std::regex_search(result, m, regexPtr->regex)) {
              std::vector<Value> cbArgs;
              cbArgs.push_back(Value(m[0].str()));  // match
              for (size_t i = 1; i < m.size(); ++i) {
                cbArgs.push_back(Value(m[i].str()));  // capture groups
              }
              cbArgs.push_back(Value(static_cast<double>(m.position())));  // offset
              cbArgs.push_back(Value(str));  // string
              Value replacement = interp->callForHarness(callbackFn, cbArgs, Value(Undefined{}));
              result = m.prefix().str() + replacement.toString() + m.suffix().str();
            }
            return Value(result);
#endif
          }
          std::string replacement = args[1].toString();
#if USE_SIMPLE_REGEX
          return Value(regexPtr->regex->replace(str, replacement));
#else
          return Value(std::regex_replace(str, regexPtr->regex, replacement));
#endif
        } else {
          std::string search = toStringForStringBuiltinArg(args[0]);
          if (args[1].isFunction()) {
            // Function callback for string replace
            std::string result = str;
            size_t pos = result.find(search);
            if (pos != std::string::npos) {
              std::vector<Value> cbArgs;
              cbArgs.push_back(Value(search));  // match
              cbArgs.push_back(Value(static_cast<double>(pos)));  // offset
              cbArgs.push_back(Value(str));  // string
              Value replacement = interp->callForHarness(args[1], cbArgs, Value(Undefined{}));
              result.replace(pos, search.length(), replacement.toString());
            }
            return Value(result);
          }
          std::string replaceTemplate = args[1].toString();
          std::string result = str;
          size_t pos = result.find(search);
          if (pos != std::string::npos) {
            // GetSubstitution: process $-patterns
            std::string replacement;
            for (size_t i = 0; i < replaceTemplate.size(); i++) {
              if (replaceTemplate[i] == '$' && i + 1 < replaceTemplate.size()) {
                char next = replaceTemplate[i + 1];
                if (next == '$') { replacement += '$'; i++; }
                else if (next == '&') { replacement += search; i++; }
                else if (next == '`') { replacement += str.substr(0, pos); i++; }
                else if (next == '\'') { replacement += str.substr(pos + search.size()); i++; }
                else { replacement += '$'; } // Unrecognized: keep $
              } else {
                replacement += replaceTemplate[i];
              }
            }
            result.replace(pos, search.length(), replacement);
          }
          return Value(result);
        }
      };
      setNativeFnProps(replaceFn, "replace", 2);
      LIGHTJS_RETURN(Value(replaceFn));
    }

    if (propName == "replaceAll") {
      auto replaceAllFn = GarbageCollector::makeGC<Function>();
      replaceAllFn->isNative = true;
      replaceAllFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        // ES2021 21.1.3.18: If searchValue is not null/undefined, check @@replace
        if (!args.empty() && !args[0].isUndefined() && !args[0].isNull() && !args[0].isString() && !args[0].isBool() && !args[0].isNumber() && !args[0].isBigInt() && !args[0].isSymbol()) {
          auto* interp = getGlobalInterpreter();
          if (interp) {
            // Check if searchValue is a RegExp (via @@match)
            const std::string& matchKey = WellKnownSymbols::matchKey();
            auto [hasMatch, matchProp] = interp->getPropertyForExternal(args[0], matchKey);
            if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
            bool isRegExp = false;
            if (hasMatch && !matchProp.isUndefined() && !matchProp.isNull()) {
              isRegExp = true;
            } else if (args[0].isRegex()) {
              isRegExp = true;
            }
            // If it's a RegExp, must have 'g' flag
            if (isRegExp) {
              if (args[0].isRegex()) {
                auto regexPtr = args[0].getGC<Regex>();
                if (regexPtr->flags.find('g') == std::string::npos) {
                  throw std::runtime_error("TypeError: String.prototype.replaceAll called with a non-global RegExp argument");
                }
              } else {
                // Check flags property for 'g'
                auto [hasFlags, flagsVal] = interp->getPropertyForExternal(args[0], "flags");
                if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
                std::string flags = hasFlags ? flagsVal.toString() : "";
                if (flags.find('g') == std::string::npos) {
                  throw std::runtime_error("TypeError: String.prototype.replaceAll called with a non-global RegExp argument");
                }
              }
            }
            // Check for @@replace
            const std::string& replaceKey = WellKnownSymbols::replaceKey();
            auto [found, replacer] = interp->getPropertyForExternal(args[0], replaceKey);
            if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
            if (found && !replacer.isUndefined() && !replacer.isNull()) {
              if (!replacer.isFunction()) throw std::runtime_error("TypeError: @@replace is not a function");
              Value replaceValue = args.size() > 1 ? args[1] : Value(Undefined{});
              Value result = interp->callForHarness(replacer, {Value(str), replaceValue}, args[0]);
              if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
              return result;
            }
          }
        }
        if (args.size() < 2) return Value(str);
        std::string search = toStringForStringBuiltinArg(args[0]);
        bool isFnReplacer = args[1].isFunction();
        std::string replacement = isFnReplacer ? "" : toStringForStringBuiltinArg(args[1]);

        // Helper: get replacement for a match
        auto getReplacement = [&](const std::string& matched, size_t matchPos) -> std::string {
          if (isFnReplacer) {
            auto* interp = getGlobalInterpreter();
            if (interp) {
              std::vector<Value> cbArgs;
              cbArgs.push_back(Value(matched));
              cbArgs.push_back(Value(static_cast<double>(matchPos)));
              cbArgs.push_back(Value(str));
              Value result = interp->callForHarness(args[1], cbArgs, Value(Undefined{}));
              if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
              return result.toString();
            }
            return "";
          }
          // GetSubstitution
          std::string sub;
          for (size_t i = 0; i < replacement.size(); i++) {
            if (replacement[i] == '$' && i + 1 < replacement.size()) {
              char next = replacement[i + 1];
              if (next == '$') { sub += '$'; i++; }
              else if (next == '&') { sub += matched; i++; }
              else if (next == '`') { sub += str.substr(0, matchPos); i++; }
              else if (next == '\'') { sub += str.substr(matchPos + matched.size()); i++; }
              else { sub += '$'; }
            } else {
              sub += replacement[i];
            }
          }
          return sub;
        };

        if (search.empty()) {
          // Insert replacement between every character and at start/end
          std::string result;
          result += getReplacement("", 0);
          for (size_t i = 0; i < str.size(); ++i) {
            result += str[i];
            result += getReplacement("", i + 1);
          }
          return Value(result);
        }
        std::string result;
        size_t pos = 0;
        while (true) {
          size_t found = str.find(search, pos);
          if (found == std::string::npos) {
            result += str.substr(pos);
            break;
          }
          result += str.substr(pos, found - pos);
          result += getReplacement(search, found);
          pos = found + search.size();
        }
        return Value(result);
      };
      setNativeFnProps(replaceAllFn, "replaceAll", 2);
      LIGHTJS_RETURN(Value(replaceAllFn));
    }

    if (propName == "repeat") {
      auto repeatFn = GarbageCollector::makeGC<Function>();
      repeatFn->isNative = true;
      repeatFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(std::string(""));
        int count = static_cast<int>(args[0].toNumber());
        if (count < 0) return Value(GarbageCollector::makeGC<Error>(ErrorType::RangeError, "Invalid count value"));
        if (count == 0) return Value(std::string(""));
        std::string result;
        result.reserve(str.length() * count);
        for (int i = 0; i < count; ++i) {
          result += str;
        }
        return Value(result);
      };
      setNativeFnProps(repeatFn, "repeat", 1);
      LIGHTJS_RETURN(Value(repeatFn));
    }

    if (propName == "padStart") {
      auto padStartFn = GarbageCollector::makeGC<Function>();
      padStartFn->isNative = true;
      padStartFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(str);
        int targetLength = static_cast<int>(args[0].toNumber());
        int currentLength = static_cast<int>(unicode::utf8Length(str));
        if (targetLength <= currentLength) return Value(str);
        std::string padString = args.size() > 1 ? args[1].toString() : " ";
        if (padString.empty()) return Value(str);
        int padLength = targetLength - currentLength;
        std::string padding;
        while (static_cast<int>(unicode::utf8Length(padding)) < padLength) {
          padding += padString;
        }
        // Trim to exact length
        std::string result;
        int count = 0;
        size_t i = 0;
        while (i < padding.size() && count < padLength) {
          std::string ch = unicode::charAt(padding, count);
          result += ch;
          count++;
        }
        return Value(result + str);
      };
      setNativeFnProps(padStartFn, "padStart", 1);
      LIGHTJS_RETURN(Value(padStartFn));
    }

    if (propName == "padEnd") {
      auto padEndFn = GarbageCollector::makeGC<Function>();
      padEndFn->isNative = true;
      padEndFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(str);
        int targetLength = static_cast<int>(args[0].toNumber());
        int currentLength = static_cast<int>(unicode::utf8Length(str));
        if (targetLength <= currentLength) return Value(str);
        std::string padString = args.size() > 1 ? args[1].toString() : " ";
        if (padString.empty()) return Value(str);
        int padLength = targetLength - currentLength;
        std::string padding;
        while (static_cast<int>(unicode::utf8Length(padding)) < padLength) {
          padding += padString;
        }
        // Trim to exact length
        std::string result;
        int count = 0;
        while (count < padLength) {
          std::string ch = unicode::charAt(padding, count);
          result += ch;
          count++;
        }
        return Value(str + result);
      };
      setNativeFnProps(padEndFn, "padEnd", 1);
      LIGHTJS_RETURN(Value(padEndFn));
    }

    if (propName == "trim") {
      auto trimFn = GarbageCollector::makeGC<Function>();
      trimFn->isNative = true;
      trimFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        return Value(stripESWhitespace(str));
      };
      setNativeFnProps(trimFn, "trim", 0);
      LIGHTJS_RETURN(Value(trimFn));
    }

    if (propName == "trimStart") {
      auto trimStartFn = GarbageCollector::makeGC<Function>();
      trimStartFn->isNative = true;
      trimStartFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        return Value(stripLeadingESWhitespace(str));
      };
      setNativeFnProps(trimStartFn, "trimStart", 0);
      LIGHTJS_RETURN(Value(trimStartFn));
    }

    if (propName == "trimEnd") {
      auto trimEndFn = GarbageCollector::makeGC<Function>();
      trimEndFn->isNative = true;
      trimEndFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        return Value(stripTrailingESWhitespace(str));
      };
      setNativeFnProps(trimEndFn, "trimEnd", 0);
      LIGHTJS_RETURN(Value(trimEndFn));
    }

    if (propName == "substring") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        int len = static_cast<int>(str.length());
        double intStart = 0;
        if (!args.empty() && !args[0].isUndefined()) {
          intStart = toIntegerForStringBuiltinArg(args[0]);
        }
        double intEnd = len;
        if (args.size() > 1 && !args[1].isUndefined()) {
          intEnd = toIntegerForStringBuiltinArg(args[1]);
        }
        int start = static_cast<int>(std::max(0.0, std::min(intStart, static_cast<double>(len))));
        int end = static_cast<int>(std::max(0.0, std::min(intEnd, static_cast<double>(len))));
        if (start > end) std::swap(start, end);
        return Value(str.substr(start, end - start));
      };
      setNativeFnProps(fn, "substring", 2);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "slice") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        int len = static_cast<int>(str.length());
        double intStart = 0;
        if (!args.empty() && !args[0].isUndefined()) {
          intStart = toIntegerForStringBuiltinArg(args[0]);
        }
        double intEnd = len;
        if (args.size() > 1 && !args[1].isUndefined()) {
          intEnd = toIntegerForStringBuiltinArg(args[1]);
        }
        int start, end;
        if (intStart < 0) start = std::max(0, len + static_cast<int>(std::max(intStart, static_cast<double>(-len))));
        else start = std::min(static_cast<int>(std::min(intStart, static_cast<double>(len))), len);
        if (intEnd < 0) end = std::max(0, len + static_cast<int>(std::max(intEnd, static_cast<double>(-len))));
        else end = std::min(static_cast<int>(std::min(intEnd, static_cast<double>(len))), len);
        if (start >= end) return Value(std::string(""));
        return Value(str.substr(start, end - start));
      };
      setNativeFnProps(fn, "slice", 2);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "substr") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        int len = static_cast<int>(str.length());
        int start = 0;
        if (!args.empty()) {
          start = static_cast<int>(args[0].toNumber());
          if (start < 0) start = std::max(0, len + start);
        }
        int length = len - start;
        if (args.size() > 1 && !args[1].isUndefined()) {
          length = static_cast<int>(args[1].toNumber());
          if (length < 0) length = 0;
        }
        if (start >= len || length <= 0) return Value(std::string(""));
        return Value(str.substr(start, length));
      };
      setNativeFnProps(fn, "substr", 2);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "toUpperCase" || propName == "toLocaleUpperCase") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        std::string result = str;
        std::transform(result.begin(), result.end(), result.begin(), ::toupper);
        return Value(result);
      };
      setNativeFnProps(fn, propName, 0);
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "toLowerCase" || propName == "toLocaleLowerCase") {
      auto fn = GarbageCollector::makeGC<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        std::string result = str;
        std::transform(result.begin(), result.end(), result.begin(), ::tolower);
        return Value(result);
      };
      setNativeFnProps(fn, propName, 0);
      LIGHTJS_RETURN(Value(fn));
    }

    // Fallback to String.prototype for user-defined properties.
    if (auto stringCtor = env_->get("String")) {
      auto wrapper = GarbageCollector::makeGC<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      wrapper->properties["__primitive_value__"] = obj;
      auto [foundProto, proto] = getPropertyForPrimitive(*stringCtor, "prototype");
      if (foundProto && (proto.isObject() || proto.isNull())) {
        wrapper->properties["__proto__"] = proto;
      }
      auto [found, value] = getPropertyForPrimitive(Value(wrapper), propName);
      if (found) {
        LIGHTJS_RETURN(value);
      }
    }
  }

  LIGHTJS_RETURN(Value(Undefined{}));
}

Value Interpreter::makeIteratorResult(const Value& value, bool done) {
  auto resultObj = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  resultObj->properties["value"] = value;
  resultObj->properties["done"] = Value(done);
  return Value(resultObj);
}

Value Interpreter::createIteratorFactory(const GCPtr<Array>& arrPtr) {
  auto iteratorFactory = GarbageCollector::makeGC<Function>();
  iteratorFactory->isNative = true;
  iteratorFactory->nativeFunc = [arrPtr](const std::vector<Value>&) -> Value {
    auto iteratorObj = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    auto state = std::make_shared<size_t>(0);
    auto nextFn = GarbageCollector::makeGC<Function>();
    nextFn->isNative = true;
    nextFn->nativeFunc = [arrPtr, state](const std::vector<Value>&) -> Value {
      if (!arrPtr || *state >= arrPtr->elements.size()) {
        return Interpreter::makeIteratorResult(Value(Undefined{}), true);
      }
      Value value = arrPtr->elements[(*state)++];
      return Interpreter::makeIteratorResult(value, false);
    };
    iteratorObj->properties["next"] = Value(nextFn);
    return Value(iteratorObj);
  };
  return Value(iteratorFactory);
}
Value Interpreter::runGeneratorNext(const GCPtr<Generator>& genPtr,
                                    ControlFlow::ResumeMode mode,
                                    const Value& resumeValue) {
  if (!genPtr) {
    return makeIteratorResult(Value(Undefined{}), true);
  }

  if (genPtr->state == GeneratorState::Completed) {
    return makeIteratorResult(Value(Undefined{}), true);
  }

  if (genPtr->state == GeneratorState::Executing) {
    throwError(ErrorType::TypeError, "Generator is already executing");
    return Value(Undefined{});
  }

  bool wasSuspendedYield = (genPtr->state == GeneratorState::SuspendedYield);
  genPtr->state = GeneratorState::Executing;

  if (genPtr->function && genPtr->context) {
    auto prevEnv = env_;
    env_ = genPtr->context;
    auto prevActiveFunction = activeFunction_;
    activeFunction_ = genPtr->function;
    auto prevPrivateOwnerClass = activePrivateOwnerClass_;
    if (auto ownerClassIt = genPtr->function->properties.find("__private_owner_class__");
        ownerClassIt != genPtr->function->properties.end() && ownerClassIt->second.isClass()) {
      activePrivateOwnerClass_ = ownerClassIt->second.getGC<Class>();
    }
    struct PrivateOwnerClassGuard {
      Interpreter* interpreter;
      GCPtr<Class> previousOwnerClass;
      GCPtr<Function> previousActiveFunction;
      ~PrivateOwnerClassGuard() {
        interpreter->activePrivateOwnerClass_ = previousOwnerClass;
        interpreter->activeFunction_ = previousActiveFunction;
      }
    } privateOwnerClassGuard{this, prevPrivateOwnerClass, prevActiveFunction};

    auto bodyPtr = std::static_pointer_cast<std::vector<StmtPtr>>(genPtr->function->body);
    
    // Resume or start the generator coroutine
    auto prevFlow = flow_;
    flow_.reset();
    if (mode != ControlFlow::ResumeMode::None && wasSuspendedYield) {
      flow_.prepareResume(mode, resumeValue);
    }

    Value result = Value(Undefined{});
    
    if (!genPtr->suspendedTask) {
      // First time starting the generator - evaluate the whole body
      auto task = evaluateGeneratorBody(*bodyPtr);
      
      // Store the task in genPtr->suspendedTask for future resumptions
      // We must store it as a pointer to Task to avoid slicing and preserve coroutine handle
      Task* heapTask = new Task(std::move(task));
      genPtr->suspendedTask = std::shared_ptr<void>(heapTask, [](void* p) {
        delete static_cast<Task*>(p);
      });
    }

    // Resume the stored task
    Task& task = *static_cast<Task*>(genPtr->suspendedTask.get());
    
    // Execute until it yields or completes
    LIGHTJS_RUN_TASK_SYNC(task, result);

    if (flow_.type == ControlFlow::Type::Yield) {
      genPtr->state = GeneratorState::SuspendedYield;
      genPtr->currentValue = std::make_shared<Value>(flow_.value);
      bool yieldIsIterResult = flow_.yieldIsIteratorResult;
      bool yieldSkipsAsyncAwait = flow_.yieldSkipsAsyncAwait;
      if (genPtr->function && genPtr->function->isAsync &&
          !yieldIsIterResult && !yieldSkipsAsyncAwait) {
        Value awaitedValue = awaitValue(*genPtr->currentValue);
        if (flow_.type == ControlFlow::Type::Throw) {
          Value thrownValue = flow_.value;
          flow_ = prevFlow;
          env_ = prevEnv;
          flow_.type = ControlFlow::Type::Throw;
          flow_.value = thrownValue;
          return Value(Undefined{});
        }
        genPtr->currentValue = std::make_shared<Value>(awaitedValue);
      }
      flow_ = prevFlow;
      env_ = prevEnv;
      if (yieldIsIterResult) {
        // `yield*` yields iterator result objects directly.
        return *genPtr->currentValue;
      }
      return makeIteratorResult(*genPtr->currentValue, false);
    }

    // Generator completed (returned or finished body)
    genPtr->state = GeneratorState::Completed;
    if (flow_.type == ControlFlow::Type::Return) {
      genPtr->currentValue = std::make_shared<Value>(flow_.value);
    } else if (flow_.type == ControlFlow::Type::Throw) {
      // Re-throw the error to the caller
      Value thrownValue = flow_.value;
      flow_ = prevFlow;
      env_ = prevEnv;
      flow_.type = ControlFlow::Type::Throw;
      flow_.value = thrownValue;
      return Value(Undefined{});
    } else {
      genPtr->currentValue = std::make_shared<Value>(Undefined{});
    }

    flow_ = prevFlow;
    env_ = prevEnv;
    return makeIteratorResult(*genPtr->currentValue, true);
  }

  genPtr->state = GeneratorState::Completed;
  return makeIteratorResult(Value(Undefined{}), true);
}

Value Interpreter::awaitValue(const Value& input) {
  Value val = input;
  auto initializePromiseIntrinsics = [&](const GCPtr<Promise>& p) {
    if (!p) return;
    auto promiseCtor = env_->getRoot()->get("__intrinsic_Promise__");
    if (!promiseCtor || !promiseCtor->isFunction()) {
      promiseCtor = env_->getRoot()->get("Promise");
    }
    if (promiseCtor && promiseCtor->isFunction()) {
      p->properties["__constructor__"] = *promiseCtor;
      auto promiseCtorFn = promiseCtor->getGC<Function>();
      auto protoIt = promiseCtorFn->properties.find("prototype");
      if (protoIt != promiseCtorFn->properties.end() && protoIt->second.isObject()) {
        p->properties["__proto__"] = protoIt->second;
      }
    }
  };

  if (!val.isPromise() && isObjectLike(val)) {
    auto [foundThen, thenValue] = getPropertyForPrimitive(val, "then");
    if (hasError()) {
      return Value(Undefined{});
    }
    if (foundThen && thenValue.isFunction()) {
      auto promise = GarbageCollector::makeGC<Promise>();
      initializePromiseIntrinsics(promise);

      auto resolveFunc = GarbageCollector::makeGC<Function>();
      resolveFunc->isNative = true;
      resolveFunc->properties["length"] = Value(1.0);
      resolveFunc->properties["name"] = Value(std::string(""));
      resolveFunc->nativeFunc = [promise](const std::vector<Value>& args) -> Value {
        if (!args.empty()) {
          promise->resolve(args[0]);
        } else {
          promise->resolve(Value(Undefined{}));
        }
        return Value(Undefined{});
      };

      auto rejectFunc = GarbageCollector::makeGC<Function>();
      rejectFunc->isNative = true;
      rejectFunc->properties["length"] = Value(1.0);
      rejectFunc->properties["name"] = Value(std::string(""));
      rejectFunc->nativeFunc = [promise](const std::vector<Value>& args) -> Value {
        if (!args.empty()) {
          promise->reject(args[0]);
        } else {
          promise->reject(Value(Undefined{}));
        }
        return Value(Undefined{});
      };

      callFunction(thenValue, {Value(resolveFunc), Value(rejectFunc)}, val);
      if (hasError()) {
        Value err = getError();
        clearError();
        promise->reject(err);
      }

      val = Value(promise);
    }
  }

  if (val.isPromise()) {
    auto promise = val.getGC<Promise>();
    if (promise->state == PromiseState::Pending) {
      auto& loop = EventLoopContext::instance().getLoop();
      constexpr size_t kMaxAwaitTicks = 10000;
      size_t ticks = 0;
      while (promise->state == PromiseState::Pending &&
             loop.hasPendingWork() &&
             ticks < kMaxAwaitTicks) {
        loop.runOnce();
        ticks++;
      }
    }

    if (promise->state == PromiseState::Fulfilled) {
      return promise->result;
    }
    if (promise->state == PromiseState::Rejected) {
      flow_.type = ControlFlow::Type::Throw;
      flow_.value = promise->result;
      return Value(Undefined{});
    }
    return Value(Undefined{});
  }

  if (!suppressMicrotasks_) {
    auto& loop = EventLoopContext::instance().getLoop();
    if (loop.pendingMicrotaskCount() > 0) {
      loop.runOnce();
    }
  }

  return val;
}

std::optional<Interpreter::IteratorRecord> Interpreter::getIterator(const Value& iterable) {
  auto buildRecord = [&](const Value& value) -> std::optional<IteratorRecord> {
    IteratorRecord record;
    if (value.isGenerator()) {
      record.kind = IteratorRecord::Kind::Generator;
      record.generator = value.getGC<Generator>();
      return record;
    }
    if (value.isArray()) {
      // Check if Array.prototype[Symbol.iterator] has been deleted
      auto arrayProtoOpt = env_->get("__array_prototype__");
      if (arrayProtoOpt.has_value() && arrayProtoOpt->isObject()) {
        auto protoObj = std::get<GCPtr<Object>>(arrayProtoOpt->data);
        const auto& iterKey = WellKnownSymbols::iteratorKey();
        if (protoObj->properties.find(iterKey) == protoObj->properties.end()) {
          // Symbol.iterator deleted from Array.prototype - not iterable
          return std::nullopt;
        }
      }
      record.kind = IteratorRecord::Kind::Array;
      record.array = value.getGC<Array>();
      record.index = 0;
      return record;
    }
    if (value.isString()) {
      record.kind = IteratorRecord::Kind::String;
      record.stringValue = std::get<std::string>(value.data);
      record.index = 0;
      return record;
    }
    if (value.isTypedArray()) {
      record.kind = IteratorRecord::Kind::TypedArray;
      record.typedArray = value.getGC<TypedArray>();
      record.index = 0;
      return record;
    }
    if (value.isMap()) {
      auto mapPtr = value.getGC<Map>();
      auto iterObj = GarbageCollector::makeGC<Object>();
      auto indexPtr = std::make_shared<size_t>(0);
      auto nextFn = GarbageCollector::makeGC<Function>();
      nextFn->isNative = true;
      nextFn->nativeFunc = [mapPtr, indexPtr](const std::vector<Value>&) -> Value {
        if (*indexPtr >= mapPtr->entries.size()) {
          return makeIteratorResult(Value(Undefined{}), true);
        }
        auto& entry = mapPtr->entries[*indexPtr];
        auto pair = GarbageCollector::makeGC<Array>();
        pair->elements.push_back(entry.first);
        pair->elements.push_back(entry.second);
        (*indexPtr)++;
        return makeIteratorResult(Value(pair), false);
      };
      iterObj->properties["next"] = Value(nextFn);
      record.kind = IteratorRecord::Kind::IteratorObject;
      record.iteratorValue = Value(iterObj);
      record.nextMethod = Value(nextFn);
      return record;
    }
    if (value.isSet()) {
      auto setPtr = value.getGC<Set>();
      auto iterObj = GarbageCollector::makeGC<Object>();
      auto indexPtr = std::make_shared<size_t>(0);
      auto nextFn = GarbageCollector::makeGC<Function>();
      nextFn->isNative = true;
      nextFn->nativeFunc = [setPtr, indexPtr](const std::vector<Value>&) -> Value {
        if (*indexPtr >= setPtr->values.size()) {
          return makeIteratorResult(Value(Undefined{}), true);
        }
        Value val = setPtr->values[*indexPtr];
        (*indexPtr)++;
        return makeIteratorResult(val, false);
      };
      iterObj->properties["next"] = Value(nextFn);
      record.kind = IteratorRecord::Kind::IteratorObject;
      record.iteratorValue = Value(iterObj);
      record.nextMethod = Value(nextFn);
      return record;
    }
    if (value.isObject()) {
      // Only treat as IteratorObject if it has a 'next' method (i.e., it's already an iterator)
      // Otherwise, fall through to check for Symbol.iterator
      auto obj = value.getGC<Object>();
      // Per GetIterator spec (7.4.1): cache next method (supports getters)
      auto getterIt = obj->properties.find("__get_next");
      if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
        // Call the getter to get the next method
        Value nextMethod = callFunction(getterIt->second, {}, value);
        if (flow_.type == ControlFlow::Type::Throw) {
          return std::nullopt;
        }
        if (nextMethod.isFunction()) {
          record.kind = IteratorRecord::Kind::IteratorObject;
          record.iteratorValue = value;
          record.nextMethod = nextMethod;
          return record;
        }
      }
      auto nextIt = obj->properties.find("next");
      if (nextIt != obj->properties.end()) {
        record.kind = IteratorRecord::Kind::IteratorObject;
        record.iteratorValue = value;
        record.nextMethod = nextIt->second;  // Cache next() per GetIterator spec (7.4.1)
        return record;
      }
    }
    return std::nullopt;
  };

  const auto& iteratorKey = WellKnownSymbols::iteratorKey();

  // First, try to get Symbol.iterator method from the iterable
  auto tryObjectIterator = [&](const Value& target) -> std::optional<IteratorRecord> {
    Value method;
    bool hasMethod = false;

    if (target.isProxy()) {
      // Handle Proxy: resolve Symbol.iterator through the proxy's get trap
      auto proxyPtr = target.getGC<Proxy>();
      if (proxyPtr->handler && proxyPtr->handler->isObject()) {
        auto handlerObj = std::get<GCPtr<Object>>(proxyPtr->handler->data);
        auto getTrapIt = handlerObj->properties.find("get");
        if (getTrapIt != handlerObj->properties.end() && getTrapIt->second.isFunction()) {
          Value resolved = callFunction(getTrapIt->second,
            {proxyPtr->target ? *proxyPtr->target : Value(Undefined{}), Value(iteratorKey), target},
            Value(Undefined{}));
          if (resolved.isFunction()) {
            method = resolved;
            hasMethod = true;
          }
        } else if (proxyPtr->target && proxyPtr->target->isObject()) {
          auto targetInner = std::get<GCPtr<Object>>(proxyPtr->target->data);
          auto it = targetInner->properties.find(iteratorKey);
          if (it != targetInner->properties.end()) {
            method = it->second;
            hasMethod = method.isFunction();
          }
        }
      } else if (proxyPtr->target && proxyPtr->target->isObject()) {
        auto targetInner = std::get<GCPtr<Object>>(proxyPtr->target->data);
        auto it = targetInner->properties.find(iteratorKey);
        if (it != targetInner->properties.end()) {
          method = it->second;
          hasMethod = method.isFunction();
        }
      }
    } else {
      // GetMethod(target, @@iterator) with getter/prototype-chain support.
      auto [found, prop] = getPropertyForPrimitive(target, iteratorKey);
      if (flow_.type == ControlFlow::Type::Throw) {
        return std::nullopt;
      }
      if (found) {
        if (prop.isNull() || prop.isUndefined()) {
          hasMethod = false;
        } else if (!prop.isFunction()) {
          throwError(ErrorType::TypeError, "@@iterator is not callable");
          return std::nullopt;
        } else {
          method = prop;
          hasMethod = true;
        }
      }
    }

    if (hasMethod) {
      // GetIterator: call @@iterator with receiver = iterable.
      auto iterValue = callFunction(method, {}, target);
      if (flow_.type == ControlFlow::Type::Throw) {
        return std::nullopt;
      }
      if (iterValue.isGenerator()) {
        IteratorRecord record;
        record.kind = IteratorRecord::Kind::Generator;
        record.generator = iterValue.getGC<Generator>();
        return record;
      }
      if (isObjectLike(iterValue) && !iterValue.isProxy()) {
        IteratorRecord record;
        record.kind = IteratorRecord::Kind::IteratorObject;
        record.iteratorValue = iterValue;
        // Cache next() method per GetIterator (supports getters).
        auto [foundNext, nextMethod] = getPropertyForPrimitive(iterValue, "next");
        if (flow_.type == ControlFlow::Type::Throw) {
          return std::nullopt;
        }
        if (foundNext) {
          record.nextMethod = nextMethod;
        }
        return record;
      }
      // Handle Proxy as iterator result
      if (iterValue.isProxy()) {
        auto proxyPtr = iterValue.getGC<Proxy>();
        // Resolve 'next' through the Proxy get trap
        Value nextMethod;
        if (proxyPtr->handler && proxyPtr->handler->isObject()) {
          auto handlerObj = std::get<GCPtr<Object>>(proxyPtr->handler->data);
          auto getTrapIt = handlerObj->properties.find("get");
          if (getTrapIt != handlerObj->properties.end() && getTrapIt->second.isFunction()) {
            nextMethod = callFunction(getTrapIt->second,
              {proxyPtr->target ? *proxyPtr->target : Value(Undefined{}), Value(std::string("next")), iterValue},
              Value(Undefined{}));
          } else if (proxyPtr->target && proxyPtr->target->isObject()) {
            // No get trap - fall through to target
            auto targetObj = std::get<GCPtr<Object>>(proxyPtr->target->data);
            auto nextIt = targetObj->properties.find("next");
            if (nextIt != targetObj->properties.end()) {
              nextMethod = nextIt->second;
            }
          }
        } else if (proxyPtr->target && proxyPtr->target->isObject()) {
          auto targetObj = std::get<GCPtr<Object>>(proxyPtr->target->data);
          auto nextIt = targetObj->properties.find("next");
          if (nextIt != targetObj->properties.end()) {
            nextMethod = nextIt->second;
          }
        }
        // Create a wrapper Object to act as the iterator
        auto iterObj = GarbageCollector::makeGC<Object>();
        iterObj->properties["__proxy__"] = iterValue;  // Keep proxy alive
        if (nextMethod.isFunction()) {
          // Create a next() wrapper that calls through the proxy
          auto nextFunc = GarbageCollector::makeGC<Function>();
          nextFunc->isNative = true;
          auto proxyCopy = proxyPtr;
          auto handlerCopy = proxyPtr->handler;
          nextFunc->nativeFunc = [this, proxyCopy, nextMethod](const std::vector<Value>& args) -> Value {
            return callFunction(nextMethod, {}, Value(proxyCopy->target ? *proxyCopy->target : Value(Undefined{})));
          };
          iterObj->properties["next"] = Value(nextFunc);
          IteratorRecord record;
          record.kind = IteratorRecord::Kind::IteratorObject;
          record.iteratorValue = Value(iterObj);
          record.nextMethod = Value(nextFunc);
          return record;
        }
      }
      // Per spec, @@iterator must return an Object.
      if (!isObjectLike(iterValue) && !iterValue.isProxy()) {
        throwError(ErrorType::TypeError, "Iterator is not an object");
        return std::nullopt;
      }
      if (auto nested = buildRecord(iterValue)) {
        return nested;
      }
    }

    return std::nullopt;
  };

  // Try Symbol.iterator first
  Value iteratorTarget = iterable;
  // GetIterator performs ToObject for primitives (except for our fast-path
  // built-ins like strings handled below).
  if (iterable.isBool() || iterable.isNumber() || iterable.isBigInt() || iterable.isSymbol()) {
    const char* ctorName = nullptr;
    if (iterable.isBool()) ctorName = "Boolean";
    else if (iterable.isNumber()) ctorName = "Number";
    else if (iterable.isBigInt()) ctorName = "BigInt";
    else if (iterable.isSymbol()) ctorName = "Symbol";

    auto wrapper = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    wrapper->properties["__primitive_value__"] = iterable;
    if (ctorName) {
      if (auto ctor = env_->get(ctorName)) {
        auto [foundProto, proto] = getPropertyForPrimitive(*ctor, "prototype");
        if (flow_.type == ControlFlow::Type::Throw) {
          return std::nullopt;
        }
        if (foundProto && (proto.isObject() || proto.isNull())) {
          wrapper->properties["__proto__"] = proto;
        }
      }
    }
    iteratorTarget = Value(wrapper);
  }

  if (auto custom = tryObjectIterator(iteratorTarget)) {
    return custom;
  }
  if (flow_.type == ControlFlow::Type::Throw) {
    return std::nullopt;
  }

  // Fall back to built-in iterators for arrays, strings, generators
  if (auto direct = buildRecord(iterable)) {
    return direct;
  }

  return std::nullopt;
}

// Remove duplicate tryObjectIterator definition below - it's now inline above
Value Interpreter::iteratorNext(IteratorRecord& record) {
  switch (record.kind) {
    case IteratorRecord::Kind::Generator:
      if (record.generator && record.generator->function && record.generator->function->isAsync) {
        auto promise = GarbageCollector::makeGC<Promise>();
        if (auto promiseCtor = env_->getRoot()->get("__intrinsic_Promise__");
            promiseCtor && promiseCtor->isFunction()) {
          promise->properties["__constructor__"] = *promiseCtor;
          auto promiseCtorFn = promiseCtor->getGC<Function>();
          auto protoIt = promiseCtorFn->properties.find("prototype");
          if (protoIt != promiseCtorFn->properties.end() && protoIt->second.isObject()) {
            promise->properties["__proto__"] = protoIt->second;
          }
        }
        Value step = runGeneratorNext(
          record.generator, ControlFlow::ResumeMode::Next, Value(Undefined{}));
        if (flow_.type == ControlFlow::Type::Throw) {
          Value rejection = flow_.value;
          clearError();
          promise->reject(rejection);
        } else {
          promise->resolve(step);
        }
        return Value(promise);
      }
      return runGeneratorNext(
        record.generator, ControlFlow::ResumeMode::Next, Value(Undefined{}));
    case IteratorRecord::Kind::Array: {
      if (!record.array || record.index >= record.array->elements.size()) {
        return makeIteratorResult(Value(Undefined{}), true);
      }
      // Check for getter on this index (e.g., Object.defineProperty(arr, '0', {get: ...}))
      std::string idxStr = std::to_string(record.index);
      auto getterIt = record.array->properties.find("__get_" + idxStr);
      if (getterIt != record.array->properties.end() && getterIt->second.isFunction()) {
        record.index++;
        Value value = callFunction(getterIt->second, {}, Value(record.array));
        return makeIteratorResult(value, false);
      }
      Value value = record.array->elements[record.index++];
      return makeIteratorResult(value, false);
    }
    case IteratorRecord::Kind::String: {
      size_t cpLen = unicode::utf8Length(record.stringValue);
      if (record.index >= cpLen) {
        return makeIteratorResult(Value(Undefined{}), true);
      }
      std::string ch = unicode::charAt(record.stringValue, record.index++);
      return makeIteratorResult(Value(ch), false);
    }
    case IteratorRecord::Kind::IteratorObject: {
      if (record.iteratorValue.isUndefined() || record.iteratorValue.isNull()) {
        return makeIteratorResult(Value(Undefined{}), true);
      }
      // Use cached nextMethod per GetIterator spec (7.4.1)
      if (!record.nextMethod.isFunction()) {
        throwError(ErrorType::TypeError, "iterator.next is not a function");
        return Value(Undefined{});
      }
      return callFunction(record.nextMethod, {}, record.iteratorValue);
    }
    case IteratorRecord::Kind::TypedArray: {
      if (!record.typedArray) {
        return makeIteratorResult(Value(Undefined{}), true);
      }
      if (record.typedArray->isOutOfBounds()) {
        throwError(ErrorType::TypeError, "TypedArray is out of bounds");
        return Value(Undefined{});
      }
      if (record.index >= record.typedArray->currentLength()) {
        return makeIteratorResult(Value(Undefined{}), true);
      }
      Value value;
      if (record.typedArray->type == TypedArrayType::BigInt64 ||
          record.typedArray->type == TypedArrayType::BigUint64) {
        if (record.typedArray->type == TypedArrayType::BigUint64) {
          value = Value(BigInt(bigint::BigIntValue(record.typedArray->getBigUintElement(record.index++))));
        } else {
          value = Value(BigInt(record.typedArray->getBigIntElement(record.index++)));
        }
      } else {
        value = Value(record.typedArray->getElement(record.index++));
      }
      return makeIteratorResult(value, false);
    }
  }

  return makeIteratorResult(Value(Undefined{}), true);
}

void Interpreter::iteratorClose(IteratorRecord& record) {
  if (record.kind == IteratorRecord::Kind::IteratorObject) {
    if (record.iteratorValue.isUndefined() || record.iteratorValue.isNull()) return;

    // GetMethod(iterator, "return") with getter/prototype-chain support.
    auto [found, returnMethod] = getPropertyForPrimitive(record.iteratorValue, "return");
    if (flow_.type == ControlFlow::Type::Throw) return;
    if (!found) return;
    if (returnMethod.isNull() || returnMethod.isUndefined()) return;
    if (!returnMethod.isFunction()) {
      throwError(ErrorType::TypeError, "iterator.return is not a function");
      return;
    }
    Value result = callFunction(returnMethod, {}, record.iteratorValue);
    // If return() threw, propagate that throw
    if (flow_.type == ControlFlow::Type::Throw) return;
    // Per spec 7.4.6 step 9: if return() result is not an Object, throw TypeError
    if (!isObjectLike(result)) {
      throwError(ErrorType::TypeError, "Iterator result is not an object");
    }
  } else if (record.kind == IteratorRecord::Kind::Generator) {
    if (!record.generator) return;
    if (record.generator->state == GeneratorState::Completed) return;
    // Call return on the generator to run finally blocks
    runGeneratorNext(record.generator, ControlFlow::ResumeMode::Return, Value(Undefined{}));
  }
  // For Array, String, TypedArray - no close needed
}

Value Interpreter::callFunction(const Value& callee, const std::vector<Value>& args, const Value& thisValue) {
  if (!callee.isFunction()) {
    return Value(Undefined{});
  }

  auto func = callee.getGC<Function>();
  std::vector<Value> currentArgs = args;
  Value currentThis = thisValue;
  auto namedExprIt = func->properties.find("__named_expression__");
  bool pushNamedExpr = namedExprIt != func->properties.end() &&
                       namedExprIt->second.isBool() &&
                       namedExprIt->second.toBool();
  if (pushNamedExpr) {
    activeNamedExpressionStack_.push_back(func);
  }
  struct NamedExprStackGuard {
    Interpreter* interpreter;
    bool active;
    ~NamedExprStackGuard() {
      if (active && !interpreter->activeNamedExpressionStack_.empty()) {
        interpreter->activeNamedExpressionStack_.pop_back();
      }
    }
  } namedExprGuard{this, pushNamedExpr};

  auto previousPrivateOwnerClass = activePrivateOwnerClass_;
  if (auto ownerClassIt = func->properties.find("__private_owner_class__");
      ownerClassIt != func->properties.end() && ownerClassIt->second.isClass()) {
    activePrivateOwnerClass_ = ownerClassIt->second.getGC<Class>();
  }
  struct PrivateOwnerClassGuard {
    Interpreter* interpreter;
    GCPtr<Class> previousOwnerClass;
    ~PrivateOwnerClassGuard() {
      interpreter->activePrivateOwnerClass_ = previousOwnerClass;
    }
  } privateOwnerClassGuard{this, previousPrivateOwnerClass};

  auto bindParameters = [&](GCPtr<Environment>& targetEnv) {
    bool isArrowFunction = false;
    auto arrowIt = func->properties.find("__is_arrow_function__");
    if (arrowIt != func->properties.end() &&
        arrowIt->second.isBool() &&
        arrowIt->second.toBool()) {
      isArrowFunction = true;
    }

    Value boundThis = currentThis;
    if (!isArrowFunction && !func->isStrict) {
      // OrdinaryCallBindThis: sloppy mode coerces this
      if (boundThis.isUndefined() || boundThis.isNull()) {
        if (auto globalThisValue = targetEnv->get("globalThis")) {
          boundThis = *globalThisValue;
        }
      } else if (boundThis.isString() || boundThis.isNumber() || boundThis.isBool() || boundThis.isBigInt()) {
        // ToObject: wrap primitive this in an object wrapper
        auto wrapper = GarbageCollector::makeGC<Object>();
        GarbageCollector::instance().reportAllocation(sizeof(Object));
        wrapper->properties["__primitive_value__"] = boundThis;
        if (boundThis.isString()) {
          const std::string& s = std::get<std::string>(boundThis.data);
          wrapper->properties["length"] = Value(static_cast<double>(String_utf16Length(s)));
          wrapper->properties["__non_writable_length"] = Value(true);
          wrapper->properties["__non_enum_length"] = Value(true);
          wrapper->properties["__non_configurable_length"] = Value(true);
        }
        // valueOf for the wrapper
        auto valueOfFn = GarbageCollector::makeGC<Function>();
        valueOfFn->isNative = true;
        Value capturedPrimitive = boundThis;
        valueOfFn->nativeFunc = [capturedPrimitive](const std::vector<Value>&) -> Value {
          return capturedPrimitive;
        };
        wrapper->properties["valueOf"] = Value(valueOfFn);
        boundThis = Value(wrapper);
      }
    }
    if (!isArrowFunction) {
      targetEnv->define("this", boundThis);
      // Ordinary [[Call]] must bind new.target to undefined.
      targetEnv->define("__new_target__", Value(Undefined{}));
    }
    if (!isArrowFunction) {
      // Class methods: compute `super` base from the owning class for static methods,
      // so Object.setPrototypeOf(C, ...) is observed.
      bool superSet = false;
      auto ownerIt = func->properties.find("__private_owner_class__");
      if (ownerIt != func->properties.end() && ownerIt->second.isClass() &&
          boundThis.isClass() &&
          boundThis.getGC<Class>().get() == ownerIt->second.getGC<Class>().get()) {
        // Static method called with `this` equal to its defining class.
        auto ownerCls = ownerIt->second.getGC<Class>();
        auto protoIt = ownerCls->properties.find("__proto__");
        if (protoIt != ownerCls->properties.end()) {
          targetEnv->define("__super__", protoIt->second);
          superSet = true;
        }
      }

      auto homeIt = func->properties.find("__home_object__");
      if (!superSet &&
          homeIt != func->properties.end() &&
          (homeIt->second.isObject() || homeIt->second.isClass())) {
        Value superBase(Undefined{});
        if (homeIt->second.isObject()) {
          auto homeObj = homeIt->second.getGC<Object>();
          auto homeProtoIt = homeObj->properties.find("__proto__");
          if (homeProtoIt != homeObj->properties.end()) {
            superBase = homeProtoIt->second;
          }
        } else {
          auto homeCls = homeIt->second.getGC<Class>();
          auto homeProtoIt = homeCls->properties.find("__proto__");
          if (homeProtoIt != homeCls->properties.end()) {
            superBase = homeProtoIt->second;
          }
        }
        if (superBase.isUndefined()) {
          auto objectCtor = targetEnv->get("Object");
          if (objectCtor && objectCtor->isFunction()) {
            auto ctor = objectCtor->getGC<Function>();
            auto protoIt = ctor->properties.find("prototype");
            if (protoIt != ctor->properties.end()) {
              superBase = protoIt->second;
            }
          }
        }
        if (!superBase.isUndefined()) {
          targetEnv->define("__super__", superBase);
        }
      } else if (!superSet) {
        auto superIt = func->properties.find("__super_class__");
        if (superIt != func->properties.end()) {
          targetEnv->define("__super__", superIt->second);
        }
      }
    }

    GCPtr<Array> argumentsArray;
    if (!isArrowFunction) {
      argumentsArray = GarbageCollector::makeGC<Array>();
      GarbageCollector::instance().reportAllocation(sizeof(Array));
      argumentsArray->elements = currentArgs;
      // Mark as an arguments object (internal, non-observable via builtins).
      argumentsArray->properties["__is_arguments_object__"] = Value(true);
      // Set arguments [[Prototype]] to Object.prototype
      if (auto objCtor = env_->get("Object"); objCtor.has_value() && objCtor->isFunction()) {
        auto protoIt = objCtor->getGC<Function>()->properties.find("prototype");
        if (protoIt != objCtor->getGC<Function>()->properties.end()) {
          argumentsArray->properties["__proto__"] = protoIt->second;
        }
      }
      if (func->isStrict) {
        auto throwTypeErrorAccessor = GarbageCollector::makeGC<Function>();
        throwTypeErrorAccessor->isNative = true;
        throwTypeErrorAccessor->nativeFunc = [](const std::vector<Value>&) -> Value {
          throw std::runtime_error("TypeError: 'caller', 'callee', and 'arguments' properties may not be accessed");
        };
        argumentsArray->properties["__get_callee"] = Value(throwTypeErrorAccessor);
        argumentsArray->properties["__set_callee"] = Value(throwTypeErrorAccessor);
        argumentsArray->properties["__non_configurable_callee"] = Value(true);
      } else {
        argumentsArray->properties["callee"] = callee;
      }
      argumentsArray->properties["__non_enum_callee"] = Value(true);

      // Mapped/unmapped arguments objects are iterable and expose Array's @@iterator
      // as an own, non-enumerable property (ES2015 9.4.4.7).
      {
        const auto& iterKey = WellKnownSymbols::iteratorKey();
        if (auto arrayProtoVal = targetEnv->getRoot()->get("__array_prototype__");
            arrayProtoVal && arrayProtoVal->isObject()) {
          auto arrayProto = arrayProtoVal->getGC<Object>();
          auto it = arrayProto->properties.find(iterKey);
          if (it != arrayProto->properties.end()) {
            argumentsArray->properties[iterKey] = it->second;
            argumentsArray->properties["__non_enum_" + iterKey] = Value(true);
          }
        }
      }
      // Arguments object [[Prototype]] is Object.prototype (not Array.prototype)
      if (auto objProtoVal = targetEnv->getRoot()->get("__object_prototype__");
          objProtoVal && objProtoVal->isObject()) {
        argumentsArray->properties["__proto__"] = *objProtoVal;
      }
      targetEnv->define("arguments", Value(argumentsArray));
    }

    // Parameter bindings are created before evaluating default initializers.
    for (const auto& param : func->params) {
      targetEnv->defineTDZ(param.name);
    }
    if (func->restParam.has_value()) {
      targetEnv->defineTDZ(*func->restParam);
    }

    for (size_t i = 0; i < func->params.size(); ++i) {
      Value paramValue = (i < currentArgs.size()) ? currentArgs[i] : Value(Undefined{});

      if (func->params[i].defaultValue && paramValue.isUndefined()) {
        auto prevParamInitEval = activeParameterInitializerEvaluation_;
        activeParameterInitializerEvaluation_ = true;

        auto defaultExpr = std::static_pointer_cast<Expression>(func->params[i].defaultValue);
        auto defaultTask = evaluate(*defaultExpr);
        LIGHTJS_RUN_TASK_SYNC(defaultTask, paramValue);
        activeParameterInitializerEvaluation_ = prevParamInitEval;
        if (flow_.type == ControlFlow::Type::Yield) return;
        if (flow_.type == ControlFlow::Type::Throw || hasError()) {
          return;
        }
      }

      targetEnv->define(func->params[i].name, paramValue);
    }

    // Mapped arguments: in sloppy mode with simple params, create getters
    // so formal param changes are reflected when iterating arguments
    if (!isArrowFunction && !func->isStrict && !func->restParam.has_value()) {
      bool hasSimpleParams = true;
      for (const auto& p : func->params) {
        if (p.defaultValue || p.name.empty() ||
            (p.name.size() > 8 && p.name.substr(0, 8) == "__param_")) {
          hasSimpleParams = false; break;
        }
      }
      if (hasSimpleParams) {
        for (size_t i = 0; i < func->params.size() && i < currentArgs.size(); ++i) {
          std::string paramName = func->params[i].name;
          std::string idxStr = std::to_string(i);
          argumentsArray->properties["__mapped_arg_index_" + idxStr + "__"] = Value(true);
          auto getter = GarbageCollector::makeGC<Function>();
          getter->isNative = true;
          getter->nativeFunc = [targetEnv, paramName](const std::vector<Value>&) -> Value {
            auto val = targetEnv->get(paramName);
            return val.has_value() ? *val : Value(Undefined{});
          };
          argumentsArray->properties["__get_" + idxStr] = Value(getter);
          auto setter = GarbageCollector::makeGC<Function>();
          setter->isNative = true;
          setter->nativeFunc = [targetEnv, paramName](const std::vector<Value>& args) -> Value {
            if (!args.empty()) {
              targetEnv->set(paramName, args[0]);
            }
            return Value(Undefined{});
          };
          argumentsArray->properties["__set_" + idxStr] = Value(setter);
        }
      }
    }

    if (func->restParam.has_value()) {
      auto restArr = GarbageCollector::makeGC<Array>();
      GarbageCollector::instance().reportAllocation(sizeof(Array));
      setArrayPrototype(restArr, env_.get());
      for (size_t i = func->params.size(); i < currentArgs.size(); ++i) {
        restArr->elements.push_back(currentArgs[i]);
      }
      targetEnv->define(*func->restParam, Value(restArr));
    }

    // Execute destructuring prologue if present
    if (func->destructurePrologue) {
      auto prologuePtr = std::static_pointer_cast<std::vector<StmtPtr>>(func->destructurePrologue);
      for (const auto& stmt : *prologuePtr) {
        auto stmtTask = evaluate(*stmt);
        Value stmtResult = Value(Undefined{});
        LIGHTJS_RUN_TASK_SYNC(stmtTask, stmtResult);
        if (flow_.type == ControlFlow::Type::Throw || hasError()) {
          return;
        }
      }
    }
  };

  if (func->isNative) {
    auto requireNewIt = func->properties.find("__require_new__");
    if (requireNewIt != func->properties.end() &&
        requireNewIt->second.isBool() &&
        requireNewIt->second.toBool()) {
      throwError(ErrorType::TypeError, "Constructor requires 'new'");
      return Value(Undefined{});
    }
    auto itReflectConstruct = func->properties.find("__reflect_construct__");
    if (itReflectConstruct != func->properties.end() &&
        itReflectConstruct->second.isBool() &&
        itReflectConstruct->second.toBool()) {
      if (args.size() < 2) {
        throwError(ErrorType::TypeError, "Reflect.construct target is not a function");
        return Value(Undefined{});
      }

      Value target = args[0];
      if (!target.isFunction() && !target.isClass() && !target.isProxy()) {
        throwError(ErrorType::TypeError, "Reflect.construct target is not a function");
        return Value(Undefined{});
      }

      std::vector<Value> constructArgs;
      if (args[1].isArray()) {
        auto arr = args[1].getGC<Array>();
        constructArgs = arr->elements;
      }

      Value newTarget = (args.size() >= 3) ? args[2] : target;
      auto constructTask = constructValue(target, constructArgs, newTarget);
      Value constructed;
      LIGHTJS_RUN_TASK_SYNC(constructTask, constructed);
      return constructed;
    }

    bool isIntrinsicEval = false;
    auto intrinsicEvalIt = func->properties.find("__is_intrinsic_eval__");
    if (intrinsicEvalIt != func->properties.end() &&
        intrinsicEvalIt->second.isBool() &&
        intrinsicEvalIt->second.toBool()) {
      isIntrinsicEval = true;
    }
    bool prevActiveDirectEvalInvocation = activeDirectEvalInvocation_;
    if (isIntrinsicEval) {
      activeDirectEvalInvocation_ = pendingDirectEvalCall_;
      pendingDirectEvalCall_ = false;
    }

    try {
      auto itUsesThis = func->properties.find("__uses_this_arg__");
      Value nativeResult = Value(Undefined{});
      if (itUsesThis != func->properties.end() && itUsesThis->second.isBool() && itUsesThis->second.toBool()) {
        std::vector<Value> nativeArgs;
        nativeArgs.reserve(args.size() + 1);
        nativeArgs.push_back(thisValue);
        nativeArgs.insert(nativeArgs.end(), args.begin(), args.end());
        nativeResult = func->nativeFunc(nativeArgs);
      } else {
        nativeResult = func->nativeFunc(args);
      }
      activeDirectEvalInvocation_ = prevActiveDirectEvalInvocation;
      return nativeResult;
    } catch (const JsValueException& e) {
      activeDirectEvalInvocation_ = prevActiveDirectEvalInvocation;
      flow_.type = ControlFlow::Type::Throw;
      flow_.value = e.value();
      return Value(Undefined{});
    } catch (const std::exception& e) {
      activeDirectEvalInvocation_ = prevActiveDirectEvalInvocation;
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
        errorType = ErrorType::Error;
      }
      throwError(errorType, message);
      return Value(Undefined{});
    }
  }

  if (func->isGenerator) {
    auto genEnv = func->closure;
    if (pushNamedExpr) {
      // Create intermediate scope for NFE name binding
      genEnv = genEnv->createChild();
      auto nameIt2 = func->properties.find("name");
      if (nameIt2 != func->properties.end() && nameIt2->second.isString()) {
        genEnv->defineImmutableNFE(nameIt2->second.toString(), Value(func));
      }
    }
    genEnv = genEnv->createChild();
    genEnv->define("__var_scope__", Value(true), true);
    auto prevEnv2 = env_;
    bool prevStrict2 = strictMode_;
    auto prevFlow2 = flow_;
    env_ = genEnv;
    strictMode_ = func->isStrict;
    flow_.reset();
    bindParameters(genEnv);
    if (flow_.type == ControlFlow::Type::Throw) {
      env_ = prevEnv2;
      strictMode_ = prevStrict2;
      return Value(Undefined{});
    }

    bool hasParameterExpressions = false;
    for (const auto& param : func->params) {
      if (param.defaultValue) {
        hasParameterExpressions = true;
        break;
      }
    }
    if (hasParameterExpressions) {
      genEnv = genEnv->createChild();
    }
    env_ = genEnv;

    auto bodyPtr = std::static_pointer_cast<std::vector<StmtPtr>>(func->body);
    for (const auto& s : *bodyPtr) {
      if (auto* varDecl = std::get_if<VarDeclaration>(&s->node)) {
        if (varDecl->kind == VarDeclaration::Kind::Let ||
            varDecl->kind == VarDeclaration::Kind::Const) {
          for (const auto& declarator : varDecl->declarations) {
            std::vector<std::string> names;
            collectVarHoistNames(*declarator.pattern, names);
            for (const auto& name : names) {
              env_->defineTDZ(name);
            }
          }
        }
      }
    }

    hoistVarDeclarations(*bodyPtr);
    for (const auto& hoistStmt : *bodyPtr) {
      if (std::holds_alternative<FunctionDeclaration>(hoistStmt->node)) {
        auto hoistTask = evaluate(*hoistStmt);
        LIGHTJS_RUN_TASK_VOID_SYNC(hoistTask);
      }
    }
    if (flow_.type == ControlFlow::Type::Throw) {
      env_ = prevEnv2;
      strictMode_ = prevStrict2;
      return Value(Undefined{});
    }

    auto generator = GarbageCollector::makeGC<Generator>(func, func->closure);
    generator->properties["__constructor__"] = callee;
    auto protoIt = func->properties.find("prototype");
    if (protoIt != func->properties.end() && (protoIt->second.isObject() || protoIt->second.isArray() || protoIt->second.isFunction() || protoIt->second.isGenerator() || protoIt->second.isProxy())) {
      generator->properties["__proto__"] = protoIt->second;
    } else {
      if (auto genProto = env_->getRoot()->get("__generator_prototype__"); genProto && genProto->isObject()) {
        generator->properties["__proto__"] = *genProto;
      }
    }
    GarbageCollector::instance().reportAllocation(sizeof(Generator));

    generator->context = genEnv;
    env_ = prevEnv2;
    strictMode_ = prevStrict2;
    flow_ = prevFlow2;
    return Value(generator);
  }

  if (func->isAsync) {
    // Push stack frame for async function calls
    auto stackFrame = pushStackFrame("<async>");

    auto initializePromiseIntrinsics = [&](const GCPtr<Promise>& p) {
      if (!p) return;
      auto promiseCtor = env_->getRoot()->get("__intrinsic_Promise__");
      if (!promiseCtor || !promiseCtor->isFunction()) {
        promiseCtor = env_->getRoot()->get("Promise");
      }
      if (promiseCtor && promiseCtor->isFunction()) {
        p->properties["__constructor__"] = *promiseCtor;
        auto promiseCtorFn = promiseCtor->getGC<Function>();
        auto protoIt = promiseCtorFn->properties.find("prototype");
        if (protoIt != promiseCtorFn->properties.end() && protoIt->second.isObject()) {
          p->properties["__proto__"] = protoIt->second;
        }
      }
    };

    auto promise = GarbageCollector::makeGC<Promise>();
    initializePromiseIntrinsics(promise);
    auto prevFlow = flow_;
    auto prevEnv = env_;
    env_ = func->closure;
    if (pushNamedExpr) {
      // Create intermediate scope for NFE name binding (spec: FunctionExpression evaluation)
      env_ = env_->createChild();
      auto nameIt2 = func->properties.find("name");
      if (nameIt2 != func->properties.end() && nameIt2->second.isString()) {
        env_->defineImmutableNFE(nameIt2->second.toString(), Value(func));
      }
    }
    env_ = env_->createChild();
    env_->define("__var_scope__", Value(true), true);

    flow_.reset();
    bindParameters(env_);
    if (flow_.type == ControlFlow::Type::Throw) {
      promise->reject(flow_.value);
      flow_ = prevFlow;
      env_ = prevEnv;
      return Value(promise);
    }
    if (hasError()) {
      Value err = getError();
      clearError();
      promise->reject(err);
      flow_ = prevFlow;
      env_ = prevEnv;
      return Value(promise);
    }
    flow_ = prevFlow;

    bool hasParameterExpressions = false;
    for (const auto& param : func->params) {
      if (param.defaultValue) {
        hasParameterExpressions = true;
        break;
      }
    }
    if (hasParameterExpressions) {
      env_ = env_->createChild();
    }

    auto bodyPtr = std::static_pointer_cast<std::vector<StmtPtr>>(func->body);
    bool previousStrictMode = strictMode_;
    strictMode_ = func->isStrict;

    // Initialize TDZ for lexical declarations in async function body
    for (const auto& s : *bodyPtr) {
      if (auto* varDecl = std::get_if<VarDeclaration>(&s->node)) {
        if (varDecl->kind == VarDeclaration::Kind::Let ||
            varDecl->kind == VarDeclaration::Kind::Const) {
          for (const auto& declarator : varDecl->declarations) {
            std::vector<std::string> names;
            collectVarHoistNames(*declarator.pattern, names);
            for (const auto& name : names) {
              env_->defineTDZ(name);
            }
          }
        }
      } else if (auto* classDecl = std::get_if<ClassDeclaration>(&s->node)) {
        env_->defineTDZ(classDecl->id.name);
      }
    }

    // Hoist var and function declarations in async function body
    hoistVarDeclarations(*bodyPtr);
    for (const auto& stmt : *bodyPtr) {
      if (std::holds_alternative<FunctionDeclaration>(stmt->node)) {
        auto task = evaluate(*stmt);
        LIGHTJS_RUN_TASK_VOID_SYNC(task);
        if (flow_.type == ControlFlow::Type::Yield) return Value(Undefined{});
      }
    }

    auto asyncEnv = env_;
    auto executeAsyncBody = std::make_shared<std::function<void(size_t)>>();
    *executeAsyncBody = [this, executeAsyncBody, promise, asyncEnv, bodyPtr, isStrict = func->isStrict, initializePromiseIntrinsics](size_t startIndex) {
      if (!promise || promise->state != PromiseState::Pending) {
        return;
      }

      auto savedEnv = env_;
      bool savedStrictMode = strictMode_;
      auto savedFlow = flow_;

      env_ = asyncEnv;
      strictMode_ = isStrict;
      flow_.reset();

      auto restoreState = [&]() {
        env_ = savedEnv;
        strictMode_ = savedStrictMode;
        flow_ = savedFlow;
      };

      try {
        for (size_t i = startIndex; i < bodyPtr->size(); ++i) {
          const auto& stmt = (*bodyPtr)[i];
          if (std::holds_alternative<FunctionDeclaration>(stmt->node)) {
            continue;
          }

          if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt->node)) {
            if (exprStmt->expression) {
              if (auto* awaitExpr = std::get_if<AwaitExpr>(&exprStmt->expression->node)) {
                auto awaitArgTask = evaluate(*awaitExpr->argument);
                Value awaitedValue = Value(Undefined{});
                LIGHTJS_RUN_TASK_SYNC(awaitArgTask, awaitedValue);
                if (flow_.type == ControlFlow::Type::Yield) return;

                if (flow_.type == ControlFlow::Type::Throw) {
                  promise->reject(flow_.value);
                  restoreState();
                  return;
                }
                if (hasError()) {
                  Value err = getError();
                  clearError();
                  promise->reject(err);
                  restoreState();
                  return;
                }

                if (!awaitedValue.isPromise() && isObjectLike(awaitedValue)) {
                  auto [foundThen, thenValue] = getPropertyForPrimitive(awaitedValue, "then");
                  if (hasError()) {
                    Value err = getError();
                    clearError();
                    promise->reject(err);
                    restoreState();
                    return;
                  }
                  if (foundThen && thenValue.isFunction()) {
                    auto thenablePromise = GarbageCollector::makeGC<Promise>();
                    initializePromiseIntrinsics(thenablePromise);

                    auto resolveFunc = GarbageCollector::makeGC<Function>();
                    resolveFunc->isNative = true;
                    resolveFunc->nativeFunc = [thenablePromise](const std::vector<Value>& args) -> Value {
                      if (!args.empty()) {
                        thenablePromise->resolve(args[0]);
                      } else {
                        thenablePromise->resolve(Value(Undefined{}));
                      }
                      return Value(Undefined{});
                    };

                    auto rejectFunc = GarbageCollector::makeGC<Function>();
                    rejectFunc->isNative = true;
                    rejectFunc->nativeFunc = [thenablePromise](const std::vector<Value>& args) -> Value {
                      if (!args.empty()) {
                        thenablePromise->reject(args[0]);
                      } else {
                        thenablePromise->reject(Value(Undefined{}));
                      }
                      return Value(Undefined{});
                    };

                    callFunction(thenValue, {Value(resolveFunc), Value(rejectFunc)}, awaitedValue);
                    if (flow_.type == ControlFlow::Type::Throw) {
                      thenablePromise->reject(flow_.value);
                      flow_.reset();
                    }
                    if (hasError()) {
                      Value err = getError();
                      clearError();
                      thenablePromise->reject(err);
                    }

                    awaitedValue = Value(thenablePromise);
                  }
                }

                GCPtr<Promise> awaitedPromise;
                if (awaitedValue.isPromise()) {
                  awaitedPromise = awaitedValue.getGC<Promise>();
                } else {
                  awaitedPromise = GarbageCollector::makeGC<Promise>();
                  initializePromiseIntrinsics(awaitedPromise);
                  awaitedPromise->resolve(awaitedValue);
                }

                size_t nextIndex = i + 1;
                awaitedPromise->then(
                  [executeAsyncBody, nextIndex](Value v) -> Value {
                    (*executeAsyncBody)(nextIndex);
                    return v;
                  },
                  [promise](Value reason) -> Value {
                    if (promise && promise->state == PromiseState::Pending) {
                      promise->reject(reason);
                    }
                    return reason;
                  }
                );

                restoreState();
                return;
              }
            }
          }

          auto stmtTask = evaluate(*stmt);
          Value stmtResult = Value(Undefined{});
          LIGHTJS_RUN_TASK_SYNC(stmtTask, stmtResult);
          if (flow_.type == ControlFlow::Type::Return) {
            Value returnValue = flow_.value;
            if (returnValue.isPromise()) {
              auto returnedPromise = returnValue.getGC<Promise>();
              returnedPromise->then(
                [promise](Value value) -> Value {
                  if (promise && promise->state == PromiseState::Pending) {
                    promise->resolve(value);
                  }
                  return value;
                },
                [promise](Value reason) -> Value {
                  if (promise && promise->state == PromiseState::Pending) {
                    promise->reject(reason);
                  }
                  return reason;
                }
              );
            } else {
              promise->resolve(returnValue);
            }
            restoreState();
            return;
          }
          if (flow_.type == ControlFlow::Type::Throw) {
            promise->reject(flow_.value);
            restoreState();
            return;
          }
        }

        promise->resolve(Value(Undefined{}));
      } catch (const std::exception& e) {
        promise->reject(Value(std::string(e.what())));
      }

      restoreState();
    };

    (*executeAsyncBody)(0);

    strictMode_ = previousStrictMode;
    env_ = prevEnv;
    return Value(promise);
  }

  // Push stack frame for JavaScript function calls
  auto stackFrame = pushStackFrame("<function>");

  auto prevEnv = env_;
  auto bodyPtr = std::static_pointer_cast<std::vector<StmtPtr>>(func->body);
  bool previousStrictMode = strictMode_;
  strictMode_ = func->isStrict;
  Value result = Value(Undefined{});

  auto prevFlow = flow_;
  auto prevActiveFunction = activeFunction_;
  bool prevPendingSelfTailCall = pendingSelfTailCall_;
  auto prevPendingSelfTailArgs = std::move(pendingSelfTailArgs_);
  Value prevPendingSelfTailThis = pendingSelfTailThis_;
  activeFunction_ = func;
  pendingSelfTailCall_ = false;
  pendingSelfTailArgs_.clear();
  pendingSelfTailThis_ = Value(Undefined{});

  while (true) {
    env_ = func->closure;
    // Named function expression: create intermediate scope for name binding
    if (pushNamedExpr) {
      env_ = env_->createChild();
      auto nameIt2 = func->properties.find("name");
      if (nameIt2 != func->properties.end() && nameIt2->second.isString()) {
        env_->defineImmutableNFE(nameIt2->second.toString(), Value(func));
      }
    }
    env_ = env_->createChild();
    env_->define("__var_scope__", Value(true), true);
    bindParameters(env_);
    if (flow_.type == ControlFlow::Type::Throw) {
      break;
    }
    bool hasParameterExpressions = false;
    for (const auto& param : func->params) {
      if (param.defaultValue) {
        hasParameterExpressions = true;
        break;
      }
    }
    if (hasParameterExpressions) {
      env_ = env_->createChild();
    }

    // Initialize TDZ for lexical declarations in function body
    for (const auto& s : *bodyPtr) {
      if (auto* varDecl = std::get_if<VarDeclaration>(&s->node)) {
        if (varDecl->kind == VarDeclaration::Kind::Let ||
            varDecl->kind == VarDeclaration::Kind::Const) {
          for (const auto& declarator : varDecl->declarations) {
            std::vector<std::string> names;
            collectVarHoistNames(*declarator.pattern, names);
            for (const auto& name : names) {
              env_->defineTDZ(name);
            }
          }
        }
      } else if (auto* classDecl = std::get_if<ClassDeclaration>(&s->node)) {
        env_->defineTDZ(classDecl->id.name);
      }
    }

    // Hoist var and function declarations in function body
    hoistVarDeclarations(*bodyPtr);
    for (const auto& hoistStmt : *bodyPtr) {
      if (std::holds_alternative<FunctionDeclaration>(hoistStmt->node)) {
        auto hoistTask = evaluate(*hoistStmt);
        LIGHTJS_RUN_TASK_VOID_SYNC(hoistTask);
      }
    }

    bool returned = false;
    bool tailCallSelf = false;
    flow_ = ControlFlow{};
    pendingSelfTailCall_ = false;
    pendingSelfTailArgs_.clear();
    pendingSelfTailThis_ = Value(Undefined{});

    for (const auto& stmt : *bodyPtr) {
      // Skip function declarations - already hoisted
      if (std::holds_alternative<FunctionDeclaration>(stmt->node)) {
        continue;
      }
      auto stmtTask = evaluate(*stmt);
      Value stmtResult = Value(Undefined{});
      LIGHTJS_RUN_TASK_SYNC(stmtTask, stmtResult);
      if (flow_.type == ControlFlow::Type::Yield) return Value(Undefined{});

      if (flow_.type == ControlFlow::Type::Return) {        if (strictMode_ && pendingSelfTailCall_) {
          currentArgs = std::move(pendingSelfTailArgs_);
          currentThis = pendingSelfTailThis_;
          pendingSelfTailCall_ = false;
          pendingSelfTailArgs_.clear();
          pendingSelfTailThis_ = Value(Undefined{});
          tailCallSelf = true;
        } else {
          result = flow_.value;
          returned = true;
        }
        break;
      }

      // Preserve throw flow control (errors)
      if (flow_.type == ControlFlow::Type::Throw) {
        break;
      }
    }

    if (tailCallSelf) {
      continue;
    }
    if (!returned && flow_.type != ControlFlow::Type::Throw) {
      result = Value(Undefined{});
    }
    break;
  }

  // Don't restore flow if an error was thrown - preserve the error
  if (flow_.type != ControlFlow::Type::Throw) {
    flow_ = prevFlow;
  }
  pendingSelfTailCall_ = prevPendingSelfTailCall;
  pendingSelfTailArgs_ = std::move(prevPendingSelfTailArgs);
  pendingSelfTailThis_ = prevPendingSelfTailThis;
  activeFunction_ = prevActiveFunction;
  strictMode_ = previousStrictMode;
  env_ = prevEnv;
  return result;
}

Task Interpreter::evaluateConditional(const ConditionalExpr& expr) {
  auto testTask = evaluate(*expr.test);
  LIGHTJS_RUN_TASK_VOID(testTask);

  if (testTask.result().toBool()) {
    auto consTask = evaluate(*expr.consequent);
    LIGHTJS_RUN_TASK_VOID(consTask);
    LIGHTJS_RETURN(consTask.result());
  } else {
    auto altTask = evaluate(*expr.alternate);
    LIGHTJS_RUN_TASK_VOID(altTask);
    LIGHTJS_RETURN(altTask.result());
  }
}

Task Interpreter::evaluateArray(const ArrayExpr& expr) {
  // Check memory limit before allocation
  if (!checkMemoryLimit(sizeof(Array))) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  auto arr = GarbageCollector::makeGC<Array>();
  GarbageCollector::instance().reportAllocation(sizeof(Array));

  // Set __proto__ to Array.prototype for prototype chain resolution
  auto arrCtor = env_->get("Array");
  if (arrCtor) {
    OrderedMap<std::string, Value>* ctorProps = nullptr;
    if (arrCtor->isFunction()) {
      ctorProps = &std::get<GCPtr<Function>>(arrCtor->data)->properties;
    } else if (arrCtor->isObject()) {
      ctorProps = &std::get<GCPtr<Object>>(arrCtor->data)->properties;
    }
    if (ctorProps) {
      auto protoIt = ctorProps->find("prototype");
      if (protoIt != ctorProps->end() && protoIt->second.isObject()) {
        arr->properties["__proto__"] = protoIt->second;
      }
    }
  }

  for (const auto& elem : expr.elements) {
    // Handle holes (elision) - nullptr elements are sparse holes
    if (!elem) {
      size_t holeIdx = arr->elements.size();
      arr->elements.push_back(Value(Undefined{}));
      arr->properties["__hole_" + std::to_string(holeIdx) + "__"] = Value(true);
      continue;
    }
    // Check if this is a spread element
    if (auto* spread = std::get_if<SpreadElement>(&elem->node)) {
      // Evaluate the argument
      auto task = evaluate(*spread->argument);
      Value val;
      LIGHTJS_RUN_TASK(task, val);
      if (flow_.type != ControlFlow::Type::None) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      auto iterRecOpt = getIterator(val);
      if (!iterRecOpt) {
        throwError(ErrorType::TypeError, val.toString() + " is not iterable");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      auto& iterRec = *iterRecOpt;
      while (true) {
        Value step = iteratorNext(iterRec);
        if (flow_.type == ControlFlow::Type::Throw) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (!step.isObject()) {
          throwError(ErrorType::TypeError, "Iterator result is not an object");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        auto stepObj = step.getGC<Object>();
        bool done = false;
        auto doneGetterIt = stepObj->properties.find("__get_done");
        if (doneGetterIt != stepObj->properties.end() && doneGetterIt->second.isFunction()) {
          Value doneVal = callFunction(doneGetterIt->second, {}, step);
          if (flow_.type == ControlFlow::Type::Throw) {
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          done = doneVal.toBool();
        } else {
          auto doneIt = stepObj->properties.find("done");
          done = (doneIt != stepObj->properties.end() && doneIt->second.toBool());
        }
        if (done) {
          break;
        }
        Value nextValue = Value(Undefined{});
        auto valueGetterIt = stepObj->properties.find("__get_value");
        if (valueGetterIt != stepObj->properties.end() && valueGetterIt->second.isFunction()) {
          nextValue = callFunction(valueGetterIt->second, {}, step);
          if (flow_.type == ControlFlow::Type::Throw) {
            LIGHTJS_RETURN(Value(Undefined{}));
          }
        } else {
          auto valueIt = stepObj->properties.find("value");
          if (valueIt != stepObj->properties.end()) {
            nextValue = valueIt->second;
          }
        }
        arr->elements.push_back(nextValue);
      }
    } else {
      auto task = evaluate(*elem);
      LIGHTJS_RUN_TASK_VOID(task);
      arr->elements.push_back(task.result());
    }
  }
  LIGHTJS_RETURN(Value(arr));
}

Task Interpreter::evaluateObject(const ObjectExpr& expr) {
  // Check memory limit before allocation
  if (!checkMemoryLimit(sizeof(Object))) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  auto obj = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // Set __proto__ to Object.prototype for prototype chain resolution
  auto objCtor = env_->get("Object");
  if (objCtor) {
    OrderedMap<std::string, Value>* ctorProps = nullptr;
    if (objCtor->isFunction()) {
      ctorProps = &std::get<GCPtr<Function>>(objCtor->data)->properties;
    } else if (objCtor->isObject()) {
      ctorProps = &std::get<GCPtr<Object>>(objCtor->data)->properties;
    }
    if (ctorProps) {
      auto protoIt = ctorProps->find("prototype");
      if (protoIt != ctorProps->end() && protoIt->second.isObject()) {
        obj->properties["__proto__"] = protoIt->second;
      }
    }
  }

  for (const auto& prop : expr.properties) {
    if (prop.isSpread) {
      // Handle spread syntax: ...sourceObj
      auto spreadTask = evaluate(*prop.value);
      Value spreadVal;
      LIGHTJS_RUN_TASK(spreadTask, spreadVal);

      // CopyDataProperties-like behavior for object spread
      if (spreadVal.isProxy()) {
        auto proxyPtr = spreadVal.getGC<Proxy>();
        if (!proxyPtr->target || !proxyPtr->target->isObject()) {
          continue;
        }
        auto targetObj = proxyPtr->target->getGC<Object>();
        GCPtr<Object> handlerObj = nullptr;
        if (proxyPtr->handler && proxyPtr->handler->isObject()) {
          handlerObj = proxyPtr->handler->getGC<Object>();
        }

        std::vector<std::string> keys;
        if (handlerObj) {
          auto ownKeysIt = handlerObj->properties.find("ownKeys");
          if (ownKeysIt != handlerObj->properties.end() && ownKeysIt->second.isFunction()) {
            Value ownKeysResult = callFunction(ownKeysIt->second, {*proxyPtr->target}, Value(handlerObj));
            if (hasError()) {
              LIGHTJS_RETURN(Value(Undefined{}));
            }
            if (ownKeysResult.isArray()) {
              auto keyArr = ownKeysResult.getGC<Array>();
              for (const auto& keyVal : keyArr->elements) {
                keys.push_back(toPropertyKeyString(keyVal));
              }
            }
          }
        }
        if (keys.empty()) {
          for (const auto& sourceKey : targetObj->properties.orderedKeys()) {
            if (sourceKey.rfind("__", 0) == 0) {
              continue;
            }
            keys.push_back(sourceKey);
          }
        }

        for (const auto& key : keys) {
          bool enumerable = false;
          bool hasDesc = false;
          if (handlerObj) {
            auto gopdIt = handlerObj->properties.find("getOwnPropertyDescriptor");
            if (gopdIt != handlerObj->properties.end() && gopdIt->second.isFunction()) {
              Value descVal = callFunction(
                  gopdIt->second, {*proxyPtr->target, toPropertyKeyValue(key)}, Value(handlerObj));
              if (hasError()) {
                LIGHTJS_RETURN(Value(Undefined{}));
              }
              if (descVal.isObject()) {
                hasDesc = true;
                auto descObj = descVal.getGC<Object>();
                auto enumIt = descObj->properties.find("enumerable");
                enumerable = (enumIt != descObj->properties.end()) && enumIt->second.toBool();
              }
            } else {
              hasDesc = targetObj->properties.count(key) > 0 ||
                        targetObj->properties.count("__get_" + key) > 0 ||
                        targetObj->properties.count("__set_" + key) > 0;
              enumerable = hasDesc && targetObj->properties.count("__non_enum_" + key) == 0;
            }
          } else {
            hasDesc = targetObj->properties.count(key) > 0 ||
                      targetObj->properties.count("__get_" + key) > 0 ||
                      targetObj->properties.count("__set_" + key) > 0;
            enumerable = hasDesc && targetObj->properties.count("__non_enum_" + key) == 0;
          }
          if (!hasDesc || !enumerable) {
            continue;
          }

          Value propValue(Undefined{});
          bool resolved = false;
          if (handlerObj) {
            auto getIt = handlerObj->properties.find("get");
            if (getIt != handlerObj->properties.end() && getIt->second.isFunction()) {
              propValue = callFunction(
                  getIt->second,
                  {*proxyPtr->target, toPropertyKeyValue(key), spreadVal},
                  Value(handlerObj));
              if (hasError()) {
                LIGHTJS_RETURN(Value(Undefined{}));
              }
              resolved = true;
            }
          }
          if (!resolved) {
            auto getterIt = targetObj->properties.find("__get_" + key);
            if (getterIt != targetObj->properties.end() && getterIt->second.isFunction()) {
              propValue = callFunction(getterIt->second, {}, Value(targetObj));
              if (hasError()) {
                LIGHTJS_RETURN(Value(Undefined{}));
              }
            } else {
              auto valueIt = targetObj->properties.find(key);
              if (valueIt != targetObj->properties.end()) {
                propValue = valueIt->second;
              }
            }
          }
          obj->properties[key] = propValue;
        }
      } else if (spreadVal.isObject()) {
        auto sourceObj = spreadVal.getGC<Object>();
        std::vector<std::string> numericKeys;
        std::vector<std::string> stringKeys;
        std::vector<std::string> symbolKeys;
        std::unordered_set<std::string> seenKeys;
        std::unordered_set<std::string> nonEnumerable;
        std::unordered_set<std::string> getterKeys;

        auto isArrayIndexKey = [](const std::string& key) -> bool {
          if (key.empty()) {
            return false;
          }
          for (char ch : key) {
            if (ch < '0' || ch > '9') {
              return false;
            }
          }
          return true;
        };
        auto isSymbolKey = [](const std::string& key) -> bool {
          return isSymbolPropertyKey(key);
        };

        for (const auto& sourceKey : sourceObj->properties.orderedKeys()) {
          if (sourceKey.rfind("__non_enum_", 0) == 0) {
            nonEnumerable.insert(sourceKey.substr(11));
            continue;
          }
          if (sourceKey.rfind("__get_", 0) == 0) {
            getterKeys.insert(sourceKey.substr(6));
          }
        }

        for (const auto& sourceKey : sourceObj->properties.orderedKeys()) {
          std::string key;
          if (sourceKey.rfind("__get_", 0) == 0 || sourceKey.rfind("__set_", 0) == 0) {
            key = sourceKey.substr(6);
          } else if (sourceKey.rfind("__", 0) == 0) {
            continue;
          } else {
            key = sourceKey;
          }

          if (key.empty() || seenKeys.find(key) != seenKeys.end()) {
            continue;
          }
          seenKeys.insert(key);

          if (nonEnumerable.find(key) != nonEnumerable.end()) {
            continue;
          }
          if (isArrayIndexKey(key)) {
            numericKeys.push_back(key);
          } else if (isSymbolKey(key)) {
            symbolKeys.push_back(key);
          } else {
            stringKeys.push_back(key);
          }
        }

        std::sort(numericKeys.begin(), numericKeys.end(), [](const std::string& a, const std::string& b) {
          return std::stoull(a) < std::stoull(b);
        });

        for (const auto& key : numericKeys) {
          if (getterKeys.find(key) != getterKeys.end()) {
            auto getterIt = sourceObj->properties.find("__get_" + key);
            if (getterIt != sourceObj->properties.end() && getterIt->second.isFunction()) {
              Value getterValue = callFunction(getterIt->second, {}, Value(sourceObj));
              if (hasError()) {
                LIGHTJS_RETURN(Value(Undefined{}));
              }
              obj->properties[key] = getterValue;
              continue;
            }
          }
          auto valueIt = sourceObj->properties.find(key);
          obj->properties[key] = (valueIt != sourceObj->properties.end()) ? valueIt->second : Value(Undefined{});
        }
        for (const auto& key : stringKeys) {
          if (getterKeys.find(key) != getterKeys.end()) {
            auto getterIt = sourceObj->properties.find("__get_" + key);
            if (getterIt != sourceObj->properties.end() && getterIt->second.isFunction()) {
              Value getterValue = callFunction(getterIt->second, {}, Value(sourceObj));
              if (hasError()) {
                LIGHTJS_RETURN(Value(Undefined{}));
              }
              obj->properties[key] = getterValue;
              continue;
            }
          }
          auto valueIt = sourceObj->properties.find(key);
          obj->properties[key] = (valueIt != sourceObj->properties.end()) ? valueIt->second : Value(Undefined{});
        }
        for (const auto& key : symbolKeys) {
          if (getterKeys.find(key) != getterKeys.end()) {
            auto getterIt = sourceObj->properties.find("__get_" + key);
            if (getterIt != sourceObj->properties.end() && getterIt->second.isFunction()) {
              Value getterValue = callFunction(getterIt->second, {}, Value(sourceObj));
              if (hasError()) {
                LIGHTJS_RETURN(Value(Undefined{}));
              }
              obj->properties[key] = getterValue;
              continue;
            }
          }
          auto valueIt = sourceObj->properties.find(key);
          obj->properties[key] = (valueIt != sourceObj->properties.end()) ? valueIt->second : Value(Undefined{});
        }
      }
    } else {
      // Regular property
      std::string key;

      // For identifier keys, use the identifier name directly (not its value from environment)
      if (prop.key) {
        if (prop.isComputed) {
          // For computed property names, evaluate the expression
          auto keyTask = evaluate(*prop.key);
          LIGHTJS_RUN_TASK_VOID(keyTask);
          Value keyValue = keyTask.result();
          if (isObjectLike(keyValue)) {
            keyValue = toPrimitiveValue(keyValue, true);
            if (hasError()) {
              LIGHTJS_RETURN(Value(Undefined{}));
            }
          }
          key = toPropertyKeyString(keyValue);
        } else if (auto* ident = std::get_if<Identifier>(&prop.key->node)) {
          key = ident->name;
        } else if (auto* str = std::get_if<StringLiteral>(&prop.key->node)) {
          key = str->value;
        } else if (auto* num = std::get_if<NumberLiteral>(&prop.key->node)) {
          key = numberToPropertyKey(num->value);
        } else if (auto* bigint = std::get_if<BigIntLiteral>(&prop.key->node)) {
          key = bigint::toString(bigint->value);
        } else {
          // Fallback: evaluate as expression
          auto keyTask = evaluate(*prop.key);
          LIGHTJS_RUN_TASK_VOID(keyTask);
          key = toPropertyKeyString(keyTask.result());
        }
      }

      auto valTask = evaluate(*prop.value);
      LIGHTJS_RUN_TASK_VOID(valTask);
      auto propValue = valTask.result();
      if (prop.isProtoSetter) {
        // Annex B __proto__ literal property: set [[Prototype]] only for Object/null, otherwise no-op.
        if (propValue.isObject() || propValue.isNull()) {
          obj->properties["__proto__"] = propValue;
        }
        continue;
      }
      std::string storageKey = key;
      if (!prop.isProtoSetter && key == "__proto__") {
        storageKey = "__own_prop___proto__";
      }
      if (prop.isComputed && (prop.isGetter || prop.isSetter)) {
        std::string accessorStorageKey = (prop.isGetter ? "__get_" : "__set_") + storageKey;
        obj->properties[accessorStorageKey] = propValue;
        if (obj->properties.find(storageKey) == obj->properties.end()) {
          obj->properties[storageKey] = Value(Undefined{});
        }
        if (propValue.isFunction()) {
          auto fn = propValue.getGC<Function>();
          auto nameIt = fn->properties.find("name");
          if (nameIt != fn->properties.end() &&
              nameIt->second.isString() &&
              nameIt->second.toString().empty()) {
            std::string keyName = key;
            Symbol symbolValue;
            if (propertyKeyToSymbol(key, symbolValue)) {
              std::string desc = symbolValue.description;
              keyName = desc.empty() ? "" : "[" + desc + "]";
            }
            fn->properties["name"] = Value(std::string(prop.isGetter ? "get " : "set ") + keyName);
            fn->properties["__non_writable_name"] = Value(true);
            fn->properties["__non_enum_name"] = Value(true);
          }
        }
      } else {
        obj->properties[storageKey] = propValue;
      }
      bool isAnonFnDef = false;
      bool isMethodDefinition = false;
      if (auto* fnExpr = std::get_if<FunctionExpr>(&prop.value->node)) {
        isAnonFnDef = fnExpr->name.empty();
        isMethodDefinition = fnExpr->isMethod;
      } else if (auto* clsExpr = std::get_if<ClassExpr>(&prop.value->node)) {
        isAnonFnDef = clsExpr->name.empty();
      }
      if (isMethodDefinition && propValue.isFunction()) {
        auto fn = propValue.getGC<Function>();
        fn->properties["__home_object__"] = Value(obj);
      }
      bool isInternalAccessorStorageKey =
          key.rfind("__get_", 0) == 0 || key.rfind("__set_", 0) == 0;
      bool isAnnexBProtoSetter = prop.isProtoSetter || (!prop.isComputed && key == "__proto__");
      if (!isInternalAccessorStorageKey && !isAnnexBProtoSetter && isAnonFnDef && propValue.isFunction()) {
        auto fn = propValue.getGC<Function>();
        auto nameIt = fn->properties.find("name");
        if (nameIt != fn->properties.end() &&
            nameIt->second.isString() &&
            nameIt->second.toString().empty()) {
          std::string inferred = key;
          Symbol symbolValue;
          if (propertyKeyToSymbol(key, symbolValue)) {
            std::string desc = symbolValue.description;
            inferred = desc.empty() ? "" : "[" + desc + "]";
          }
          fn->properties["name"] = Value(inferred);
          fn->properties["__non_writable_name"] = Value(true);
          fn->properties["__non_enum_name"] = Value(true);
        }
      } else if (!isInternalAccessorStorageKey && !isAnnexBProtoSetter && isAnonFnDef && propValue.isClass()) {
        auto cls = propValue.getGC<Class>();
        auto nameIt = cls->properties.find("name");
        bool shouldSet = nameIt == cls->properties.end() ||
                         (nameIt->second.isString() && nameIt->second.toString().empty());
        if (shouldSet) {
          std::string inferred = key;
          Symbol symbolValue;
          if (propertyKeyToSymbol(key, symbolValue)) {
            std::string desc = symbolValue.description;
            inferred = desc.empty() ? "" : "[" + desc + "]";
          }
          cls->name = inferred;
          cls->properties["name"] = Value(inferred);
          cls->properties["__non_writable_name"] = Value(true);
          cls->properties["__non_enum_name"] = Value(true);
        }
      }
      if (storageKey.rfind("__get_", 0) == 0 || storageKey.rfind("__set_", 0) == 0) {
        auto accessorName = storageKey.substr(6);
        if (obj->properties.find(accessorName) == obj->properties.end()) {
          obj->properties[accessorName] = Value(Undefined{});
        }
        if (propValue.isFunction()) {
          auto fn = propValue.getGC<Function>();
          auto nameIt = fn->properties.find("name");
          if (nameIt != fn->properties.end() &&
              nameIt->second.isString() &&
              nameIt->second.toString().empty()) {
            std::string prefix = (storageKey.rfind("__get_", 0) == 0) ? "get " : "set ";
            fn->properties["name"] = Value(prefix + accessorName);
            fn->properties["__non_writable_name"] = Value(true);
            fn->properties["__non_enum_name"] = Value(true);
          }
        }
      }
    }
  }
  LIGHTJS_RETURN(Value(obj));
}

Task Interpreter::evaluateFunction(const FunctionExpr& expr) {
  auto func = GarbageCollector::makeGC<Function>();
  func->isNative = false;
  func->isAsync = expr.isAsync;
  func->isGenerator = expr.isGenerator;
  func->isStrict = strictMode_ || hasUseStrictDirective(expr.body);

  for (const auto& param : expr.params) {
    FunctionParam funcParam;
    funcParam.name = param.name.name;
    if (param.defaultValue) {
      funcParam.defaultValue = std::shared_ptr<void>(const_cast<Expression*>(param.defaultValue.get()), [](void*){});
    }
    func->params.push_back(funcParam);
  }

  if (expr.restParam.has_value()) {
    func->restParam = expr.restParam->name;
  }

  func->body = std::shared_ptr<void>(const_cast<std::vector<StmtPtr>*>(&expr.body), [](void*){});
  func->destructurePrologue = std::shared_ptr<void>(const_cast<std::vector<StmtPtr>*>(&expr.destructurePrologue), [](void*){});
  // If evaluating in an eval context, keep the AST alive
  if (sourceKeepAlive_) {
    func->astOwner = sourceKeepAlive_;
  }
  func->closure = env_;
  if (activePrivateOwnerClass_) {
    func->properties["__private_owner_class__"] = Value(activePrivateOwnerClass_);
  }
  // Compute length: number of params before first default parameter
  size_t funcLength = 0;
  for (const auto& param : expr.params) {
    if (param.defaultValue) break;
    funcLength++;
  }
  func->properties["length"] = Value(static_cast<double>(funcLength));
  func->properties["__non_writable_length"] = Value(true);
  func->properties["__non_enum_length"] = Value(true);
  func->properties["name"] = Value(expr.name);
  func->properties["__non_writable_name"] = Value(true);
  func->properties["__non_enum_name"] = Value(true);
  func->sourceText = expr.sourceText;
  if (expr.isArrow) {
    func->properties["__is_arrow_function__"] = Value(true);
  }
  if (!expr.name.empty()) {
    func->properties["__named_expression__"] = Value(true);
  }
  // Regular functions (non-arrow, non-method) are constructors.
  // Generator methods still need a .prototype object for iterator instances.
  // Plain async functions (non-generator) are not constructors and don't get .prototype.
  bool isPlainAsync = func->isAsync && !func->isGenerator;
  if (!expr.isArrow && !isPlainAsync && (!expr.isMethod || expr.isGenerator || func->isGenerator)) {
    func->isConstructor = (!expr.isMethod && !func->isGenerator);
    // Create default prototype object
    auto proto = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    if (func->isGenerator) {
      if (auto genProto = env_->get("__generator_prototype__"); genProto && genProto->isObject()) {
        proto->properties["__proto__"] = *genProto;
      }
      func->properties["__non_enum_prototype"] = Value(true);
      func->properties["__non_configurable_prototype"] = Value(true);
    } else {
      proto->properties["constructor"] = Value(func);
      proto->properties["__non_enum_constructor"] = Value(true);
      // Ordinary function prototypes inherit from Object.prototype.
      if (auto objectCtorVal = env_->getRoot()->get("Object"); objectCtorVal.has_value()) {
        Value objectProto(Undefined{});
        bool hasObjectProto = false;
        if (objectCtorVal->isFunction()) {
          auto objectCtor = objectCtorVal->getGC<Function>();
          auto objectProtoIt = objectCtor->properties.find("prototype");
          if (objectProtoIt != objectCtor->properties.end()) {
            objectProto = objectProtoIt->second;
            hasObjectProto = true;
          }
        } else if (objectCtorVal->isObject()) {
          auto objectCtorObj = objectCtorVal->getGC<Object>();
          auto objectProtoIt = objectCtorObj->properties.find("prototype");
          if (objectProtoIt != objectCtorObj->properties.end()) {
            objectProto = objectProtoIt->second;
            hasObjectProto = true;
          }
        }
        if (hasObjectProto) {
          proto->properties["__proto__"] = objectProto;
        }
      }
    }
    func->properties["prototype"] = Value(proto);
    func->properties["__non_enum_prototype"] = Value(true);
    func->properties["__non_configurable_prototype"] = Value(true);
  } else {
    func->isConstructor = false;
  }

  // Set __proto__ to Function.prototype or GeneratorFunction.prototype for proper prototype chain
  std::string protoName = func->isGenerator ? "__generator_function_prototype__" : "Function";
  auto protoVal = env_->getRoot()->get(protoName);
  if (protoVal.has_value()) {
    if (func->isGenerator && protoVal->isObject()) {
      func->properties["__proto__"] = *protoVal;
    } else {
      Value functionProto(Undefined{});
      bool hasFunctionProto = false;
      if (protoVal->isFunction()) {
        auto protoCtor = std::get<GCPtr<Function>>(protoVal->data);
        auto protoIt = protoCtor->properties.find("prototype");
        if (protoIt != protoCtor->properties.end()) {
          functionProto = protoIt->second;
          hasFunctionProto = true;
        }
      } else if (protoVal->isObject()) {
        auto protoObj = protoVal->getGC<Object>();
        auto protoIt = protoObj->properties.find("prototype");
        if (protoIt != protoObj->properties.end()) {
          functionProto = protoIt->second;
          hasFunctionProto = true;
        }
      }
      if (hasFunctionProto) {
        func->properties["__proto__"] = functionProto;
      }
    }
  }

  LIGHTJS_RETURN(Value(func));
}

Task Interpreter::evaluateAwait(const AwaitExpr& expr) {
  auto task = evaluate(*expr.argument);
  Value val;
  LIGHTJS_RUN_TASK(task, val);
  if (activeFunction_ && activeFunction_->isGenerator && activeFunction_->isAsync) {
    auto initializePromiseIntrinsics = [&](const GCPtr<Promise>& p) {
      if (!p) return;
      auto promiseCtor = env_->getRoot()->get("__intrinsic_Promise__");
      if (!promiseCtor || !promiseCtor->isFunction()) {
        promiseCtor = env_->getRoot()->get("Promise");
      }
      if (promiseCtor && promiseCtor->isFunction()) {
        p->properties["__constructor__"] = *promiseCtor;
        auto promiseCtorFn = promiseCtor->getGC<Function>();
        auto protoIt = promiseCtorFn->properties.find("prototype");
        if (protoIt != promiseCtorFn->properties.end() && protoIt->second.isObject()) {
          p->properties["__proto__"] = protoIt->second;
        }
      }
    };

    Value awaitedInput = val;
    if (!awaitedInput.isPromise() && isObjectLike(awaitedInput)) {
      auto [foundThen, thenValue] = getPropertyForPrimitive(awaitedInput, "then");
      if (flow_.type == ControlFlow::Type::Throw) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (foundThen && thenValue.isFunction()) {
        auto thenablePromise = GarbageCollector::makeGC<Promise>();
        initializePromiseIntrinsics(thenablePromise);

        auto resolveFunc = GarbageCollector::makeGC<Function>();
        resolveFunc->isNative = true;
        resolveFunc->nativeFunc = [thenablePromise](const std::vector<Value>& args) -> Value {
          thenablePromise->resolve(args.empty() ? Value(Undefined{}) : args[0]);
          return Value(Undefined{});
        };
        auto rejectFunc = GarbageCollector::makeGC<Function>();
        rejectFunc->isNative = true;
        rejectFunc->nativeFunc = [thenablePromise](const std::vector<Value>& args) -> Value {
          thenablePromise->reject(args.empty() ? Value(Undefined{}) : args[0]);
          return Value(Undefined{});
        };

        callFunction(thenValue, {Value(resolveFunc), Value(rejectFunc)}, awaitedInput);
        if (flow_.type == ControlFlow::Type::Throw) {
          thenablePromise->reject(flow_.value);
          flow_.reset();
        }
        awaitedInput = Value(thenablePromise);
      }
    }

    GCPtr<Promise> awaitedPromise;
    if (awaitedInput.isPromise()) {
      awaitedPromise = awaitedInput.getGC<Promise>();
    } else {
      awaitedPromise = GarbageCollector::makeGC<Promise>();
      initializePromiseIntrinsics(awaitedPromise);
      awaitedPromise->resolve(awaitedInput);
    }
    awaitedPromise->properties["__await_suspend__"] = Value(true);
    flow_.setYieldWithoutAsyncAwait(Value(awaitedPromise));
    co_await YieldAwaiter{Value(awaitedPromise)};

    auto resumeMode = flow_.takeResumeMode();
    Value resumeValue = flow_.takeResumeValue();
    if (resumeMode == ControlFlow::ResumeMode::Throw) {
      flow_.type = ControlFlow::Type::Throw;
      flow_.value = resumeValue;
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    LIGHTJS_RETURN(resumeValue);
  }
  LIGHTJS_RETURN(awaitValue(val));
}

Task Interpreter::evaluateYield(const YieldExpr& expr) {
  // Evaluate the yielded value
  Value yieldedValue = Value(Undefined{});
  if (expr.argument) {
    auto task = evaluate(*expr.argument);
    LIGHTJS_RUN_TASK(task, yieldedValue);
    if (flow_.type == ControlFlow::Type::Yield) {
      // Nested yield propagation
      LIGHTJS_RETURN(yieldedValue);
    }
    if (flow_.type == ControlFlow::Type::Throw) {
      // YieldExpression evaluates its operand before yielding. If that throws,
      // propagate the abrupt completion without yielding.
      LIGHTJS_RETURN(Value(Undefined{}));
    }
  }

  if (expr.delegate) {
    bool isAsyncGeneratorDelegation =
      activeFunction_ && activeFunction_->isGenerator && activeFunction_->isAsync;
    auto iterRecOpt = getIterator(yieldedValue);
    if (!iterRecOpt) {
      // Propagate any abrupt completion produced by GetIterator.
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    IteratorRecord iterRec = std::move(*iterRecOpt);

    auto getMethod = [&](const Value& receiver, const std::string& name) -> Value {
      auto [found, v] = getPropertyForPrimitive(receiver, name);
      if (flow_.type == ControlFlow::Type::Throw) return Value(Undefined{});
      if (!found) return Value(Undefined{});
      if (v.isNull() || v.isUndefined()) return Value(Undefined{});
      if (!v.isFunction()) {
        throwError(ErrorType::TypeError, "Iterator method is not callable");
        return Value(Undefined{});
      }
      return v;
    };

    Value receivedValue = Value(Undefined{});
    ControlFlow::ResumeMode resumeMode = ControlFlow::ResumeMode::Next;

    while (true) {
      Value innerResult(Undefined{});

      if (resumeMode == ControlFlow::ResumeMode::Throw) {
        if (iterRec.kind == IteratorRecord::Kind::Generator) {
          innerResult = runGeneratorNext(iterRec.generator, ControlFlow::ResumeMode::Throw, receivedValue);
        } else if (iterRec.kind == IteratorRecord::Kind::IteratorObject) {
          Value throwMethod = getMethod(iterRec.iteratorValue, "throw");
          if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
          if (throwMethod.isUndefined()) {
            // Yield* protocol violation: iterator has no throw method.
            auto savedFlow = flow_;
            flow_.reset();
            iteratorClose(iterRec);
            if (flow_.type == ControlFlow::Type::Throw) {
              // IteratorClose threw.
              LIGHTJS_RETURN(Value(Undefined{}));
            }
            flow_ = savedFlow;
            throwError(ErrorType::TypeError, "Iterator does not have a throw method");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          innerResult = callFunction(throwMethod, {receivedValue}, iterRec.iteratorValue);
        } else {
          auto savedFlow = flow_;
          flow_.reset();
          iteratorClose(iterRec);
          if (flow_.type == ControlFlow::Type::Throw) {
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          flow_ = savedFlow;
          throwError(ErrorType::TypeError, "Iterator does not have a throw method");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      } else if (resumeMode == ControlFlow::ResumeMode::Return) {
        if (iterRec.kind == IteratorRecord::Kind::Generator) {
          innerResult = runGeneratorNext(iterRec.generator, ControlFlow::ResumeMode::Return, receivedValue);
        } else if (iterRec.kind == IteratorRecord::Kind::IteratorObject) {
          Value returnMethod = getMethod(iterRec.iteratorValue, "return");
          if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
          if (returnMethod.isUndefined()) {
            // No return method: propagate Return completion.
            if (isAsyncGeneratorDelegation) {
              receivedValue = awaitValue(receivedValue);
              if (flow_.type == ControlFlow::Type::Throw) {
                LIGHTJS_RETURN(Value(Undefined{}));
              }
            }
            flow_.type = ControlFlow::Type::Return;
            flow_.value = receivedValue;
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          innerResult = callFunction(returnMethod, {receivedValue}, iterRec.iteratorValue);
        } else {
          flow_.type = ControlFlow::Type::Return;
          flow_.value = receivedValue;
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      } else {
        // Next (including initial entry).
        if (iterRec.kind == IteratorRecord::Kind::Generator) {
          innerResult = runGeneratorNext(iterRec.generator, ControlFlow::ResumeMode::Next, receivedValue);
        } else if (iterRec.kind == IteratorRecord::Kind::IteratorObject) {
          if (!iterRec.nextMethod.isFunction()) {
            throwError(ErrorType::TypeError, "iterator.next is not a function");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          innerResult = callFunction(iterRec.nextMethod, {receivedValue}, iterRec.iteratorValue);
        } else {
          // Built-in iterators ignore the sent value.
          innerResult = iteratorNext(iterRec);
        }
      }

      if (flow_.type == ControlFlow::Type::Throw) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      if (!isObjectLike(innerResult)) {
        throwError(ErrorType::TypeError, "Iterator result is not an object");
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      bool done = false;
      {
        auto [foundDone, doneVal] = getPropertyForPrimitive(innerResult, "done");
        if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
        done = foundDone ? doneVal.toBool() : false;
      }

      if (done) {
        Value value = Value(Undefined{});
        auto [foundValue, valueVal] = getPropertyForPrimitive(innerResult, "value");
        if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
        if (foundValue) value = valueVal;

        if (resumeMode == ControlFlow::ResumeMode::Return) {
          flow_.type = ControlFlow::Type::Return;
          flow_.value = value;
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        LIGHTJS_RETURN(value);
      }

      if (isAsyncGeneratorDelegation) {
        Value value = Value(Undefined{});
        auto [foundValue, valueVal] = getPropertyForPrimitive(innerResult, "value");
        if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
        if (foundValue) {
          value = valueVal;
        }
        flow_.setYieldWithoutAsyncAwait(value);
        co_await YieldAwaiter{value};
      } else {
        // Delegation continues: yield the iterator result object itself (no eager
        // access of `.value`).
        flow_.setYieldIteratorResult(innerResult);
        co_await YieldAwaiter{innerResult};
      }

      resumeMode = flow_.takeResumeMode();
      if (resumeMode == ControlFlow::ResumeMode::None) {
        resumeMode = ControlFlow::ResumeMode::Next;
      }
      receivedValue = flow_.takeResumeValue();
    }
  }

  // Normal yield
  flow_.setYield(yieldedValue);
  co_await YieldAwaiter{yieldedValue};

  auto mode = flow_.takeResumeMode();
  Value resumeValue = flow_.takeResumeValue();

  if (mode == ControlFlow::ResumeMode::Return) {
    flow_.type = ControlFlow::Type::Return;
    flow_.value = resumeValue;
    LIGHTJS_RETURN(Value(Undefined{}));
  }
  if (mode == ControlFlow::ResumeMode::Throw) {
    flow_.type = ControlFlow::Type::Throw;
    flow_.value = resumeValue;
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  LIGHTJS_RETURN(resumeValue);
}

bool Interpreter::defineClassElementOnValue(Value& targetVal,
                                            const std::string& key,
                                            const Value& propertyValue,
                                            bool useProxyDefineTrapForPublic) {
  bool isPrivateKey = key.rfind("__private_", 0) == 0;

  if (targetVal.isObject()) {
    auto obj = targetVal.getGC<Object>();
    bool isNew = obj->properties.find(key) == obj->properties.end();
    if (isPrivateKey && !isNew) return false;
    if (obj->frozen) return false;
    if ((obj->sealed || obj->nonExtensible) && isNew) return false;
    obj->properties[key] = propertyValue;
    return true;
  }

  if (targetVal.isClass()) {
    auto cls = targetVal.getGC<Class>();
    bool isNew = cls->properties.find(key) == cls->properties.end();
    if (isPrivateKey && !isNew) return false;

    auto nonExtensibleIt = cls->properties.find("__non_extensible__");
    bool isNonExtensible = nonExtensibleIt != cls->properties.end() &&
                           nonExtensibleIt->second.isBool() &&
                           nonExtensibleIt->second.toBool();
    if (isNonExtensible && isNew) return false;

    if (!isPrivateKey &&
        key == "prototype" &&
        cls->properties.find("__non_writable_prototype") != cls->properties.end()) {
      return false;
    }

    cls->properties[key] = propertyValue;
    return true;
  }

  if (targetVal.isProxy()) {
    auto proxy = targetVal.getGC<Proxy>();
    if (!proxy->target) {
      return false;
    }

    if (!isPrivateKey &&
        useProxyDefineTrapForPublic &&
        proxy->handler &&
        proxy->handler->isObject()) {
      auto handlerObj = proxy->handler->getGC<Object>();
      auto trapIt = handlerObj->properties.find("defineProperty");
      if (trapIt != handlerObj->properties.end() && trapIt->second.isFunction()) {
        auto descObj = GarbageCollector::makeGC<Object>();
        descObj->properties["value"] = propertyValue;
        descObj->properties["writable"] = Value(true);
        descObj->properties["enumerable"] = Value(true);
        descObj->properties["configurable"] = Value(true);
        Value trapResult = invokeFunction(
            trapIt->second.getGC<Function>(),
            {*proxy->target, toPropertyKeyValue(key), Value(descObj)},
            Value(Undefined{}));
        if (hasError()) {
          return false;
        }
        return trapResult.toBool();
      }
    }

    Value target = *proxy->target;
    return defineClassElementOnValue(target, key, propertyValue, false);
  }

  if (targetVal.isArray()) {
    auto arr = targetVal.getGC<Array>();
    if (isPrivateKey && arr->properties.find(key) != arr->properties.end()) {
      return false;
    }
    arr->properties[key] = propertyValue;
    return true;
  }

  if (targetVal.isFunction()) {
    auto fn = targetVal.getGC<Function>();
    if (isPrivateKey && fn->properties.find(key) != fn->properties.end()) {
      return false;
    }
    fn->properties[key] = propertyValue;
    return true;
  }

  if (targetVal.isRegex()) {
    auto regex = targetVal.getGC<Regex>();
    if (isPrivateKey && regex->properties.find(key) != regex->properties.end()) {
      return false;
    }
    regex->properties[key] = propertyValue;
    return true;
  }

  if (targetVal.isPromise()) {
    auto promise = targetVal.getGC<Promise>();
    if (isPrivateKey && promise->properties.find(key) != promise->properties.end()) {
      return false;
    }
    promise->properties[key] = propertyValue;
    return true;
  }

  return false;
}

Value Interpreter::getPrototypeFromConstructorValue(const Value& ctorValue) {
  // ES: GetPrototypeFromConstructor uses ordinary property access, so `prototype`
  // may be an accessor. Keep this behavior consistent across Function/Class and
  // callable wrapper Objects.
  if (!isObjectLike(ctorValue)) {
    return Value(Undefined{});
  }
  auto [found, proto] = getPropertyForPrimitive(ctorValue, "prototype");
  if (hasError()) {
    return Value(Undefined{});
  }
  if (!found) {
    return Value(Undefined{});
  }
  if (isObjectLike(proto) || proto.isNull()) {
    return proto;
  }
  return Value(Undefined{});
}

GCPtr<Environment> Interpreter::getRealmRootEnvFromConstructorValue(const Value& ctorValue) {
  // Best-effort: infer realm from function/class closure. For other types, fall back
  // to current interpreter realm.
  if (ctorValue.isFunction()) {
    auto fn = ctorValue.getGC<Function>();
    if (fn && fn->closure) return fn->closure->getRoot();
  }
  if (ctorValue.isClass()) {
    auto cls = ctorValue.getGC<Class>();
    if (cls && cls->closure) return cls->closure->getRoot();
  }
  if (ctorValue.isObject()) {
    // Callable wrapper objects may carry an inner constructor.
    auto obj = ctorValue.getGC<Object>();
    auto it = obj->properties.find("constructor");
    if (it != obj->properties.end()) {
      return getRealmRootEnvFromConstructorValue(it->second);
    }
  }
  if (ctorValue.isProxy()) {
    auto proxy = ctorValue.getGC<Proxy>();
    if (proxy && proxy->target) return getRealmRootEnvFromConstructorValue(*proxy->target);
  }
  return env_->getRoot();
}

Value Interpreter::getIntrinsicObjectPrototypeForCtor(const Value& ctorValue) {
  auto realmRoot = getRealmRootEnvFromConstructorValue(ctorValue);
  if (!realmRoot) return Value(Undefined{});
  if (auto objectCtor = realmRoot->get("Object")) {
    Value proto = getPrototypeFromConstructorValue(*objectCtor);
    if (proto.isObject()) return proto;
  }
  return Value(Undefined{});
}

Value Interpreter::getIntrinsicPrototypeForCtorNamed(const Value& ctorValue,
                                                     const std::string& ctorName) {
  auto realmRoot = getRealmRootEnvFromConstructorValue(ctorValue);
  if (!realmRoot) return Value(Undefined{});
  if (auto ctor = realmRoot->get(ctorName)) {
    Value proto = getPrototypeFromConstructorValue(*ctor);
    if (proto.isObject()) return proto;
  }
  return Value(Undefined{});
}

Value Interpreter::getOrdinaryCreatePrototypeFromNewTarget(const Value& newTargetValue) {
  // OrdinaryCreateFromConstructor(newTarget, "%Object.prototype%"):
  // If newTarget.prototype is not an Object, use the realm's
  // intrinsic %Object.prototype%.
  if (isObjectLike(newTargetValue)) {
    auto [found, proto] = getPropertyForPrimitive(newTargetValue, "prototype");
    if (hasError()) return Value(Undefined{});
    if (found && isObjectLike(proto)) return proto;
  }
  return getIntrinsicObjectPrototypeForCtor(newTargetValue);
}

Value Interpreter::getInstanceFieldSuperBase(const GCPtr<Class>& cls) {
  auto objectPrototype = [&]() -> Value {
    if (auto objectCtor = env_->get("Object")) {
      Value proto = getPrototypeFromConstructorValue(*objectCtor);
      if (proto.isObject() || proto.isNull()) return proto;
    }
    return Value(Undefined{});
  };

  if (!cls) return objectPrototype();

  auto extendsNullIt = cls->properties.find("__extends_null__");
  bool extendsNull = extendsNullIt != cls->properties.end() &&
                     extendsNullIt->second.isBool() &&
                     extendsNullIt->second.toBool();
  if (extendsNull) {
    return Value(Null{});
  }

  if (cls->superClass) {
    auto superProtoIt = cls->superClass->properties.find("prototype");
    if (superProtoIt != cls->superClass->properties.end() &&
        (superProtoIt->second.isObject() || superProtoIt->second.isNull())) {
      return superProtoIt->second;
    }
  } else if (auto superCtorIt = cls->properties.find("__super_constructor__");
             superCtorIt != cls->properties.end()) {
    Value proto = getPrototypeFromConstructorValue(superCtorIt->second);
    if (proto.isObject() || proto.isNull()) return proto;
  }

  return objectPrototype();
}

Value Interpreter::getStaticFieldSuperBase(const GCPtr<Class>& cls) {
  if (!cls) return Value(Undefined{});
  if (cls->superClass) return Value(cls->superClass);
  if (auto superCtorIt = cls->properties.find("__super_constructor__");
      superCtorIt != cls->properties.end()) {
    return superCtorIt->second;
  }
  if (auto functionCtor = env_->get("Function")) {
    Value proto = getPrototypeFromConstructorValue(*functionCtor);
    if (proto.isObject() || proto.isNull()) return proto;
  }
  return Value(Undefined{});
}

void Interpreter::maybeInferAnonymousFieldInitializerName(const std::string& fieldName,
                                                          bool isPrivate,
                                                          const Expression* initExpr,
                                                          Value& fieldValue) {
  if (!initExpr) return;

  bool isAnonymousFnDef = false;
  if (auto* fnExpr = std::get_if<FunctionExpr>(&initExpr->node)) {
    isAnonymousFnDef = fnExpr->name.empty();
  } else if (auto* clsExpr = std::get_if<ClassExpr>(&initExpr->node)) {
    isAnonymousFnDef = clsExpr->name.empty();
  }
  if (!isAnonymousFnDef) return;

  std::string inferredName;
  if (isPrivate) {
    if (!fieldName.empty() && fieldName[0] == '#') {
      inferredName = fieldName;
    } else {
      inferredName = "#" + fieldName;
    }
  } else {
    inferredName = fieldName;
    Symbol symbolValue;
    if (propertyKeyToSymbol(fieldName, symbolValue)) {
      std::string desc = symbolValue.description;
      inferredName = desc.empty() ? "" : "[" + desc + "]";
    }
  }

  if (fieldValue.isFunction()) {
    auto fn = fieldValue.getGC<Function>();
    auto nameIt = fn->properties.find("name");
    bool shouldSet = nameIt == fn->properties.end() ||
                     (nameIt->second.isString() && nameIt->second.toString().empty());
    if (shouldSet) {
      fn->properties["name"] = Value(inferredName);
      fn->properties["__non_writable_name"] = Value(true);
      fn->properties["__non_enum_name"] = Value(true);
    }
  } else if (fieldValue.isClass()) {
    auto cls = fieldValue.getGC<Class>();
    auto nameIt = cls->properties.find("name");
    bool shouldSet = nameIt == cls->properties.end() ||
                     (nameIt->second.isString() && nameIt->second.toString().empty());
    if (shouldSet) {
      cls->name = inferredName;
      cls->properties["name"] = Value(inferredName);
      cls->properties["__non_writable_name"] = Value(true);
      cls->properties["__non_enum_name"] = Value(true);
    }
  }
}

Task Interpreter::initializeClassInstanceElements(const GCPtr<Class>& cls, Value receiver) {
  if (!cls) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }
  // If there are no instance elements (fields/private methods/accessors) to
  // initialize, do nothing even if the receiver is a non-Object internal type
  // (e.g. Map/TypedArray in subclass-builtins tests).
  if (cls->fieldInitializers.empty() &&
      cls->methods.empty() &&
      cls->getters.empty() &&
      cls->setters.empty()) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }
  if (!isObjectLike(receiver)) {
    throwError(ErrorType::TypeError, "Cannot initialize class fields on non-object receiver");
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  std::vector<std::string> privateMethodNames;
  privateMethodNames.reserve(cls->methods.size() + cls->getters.size() + cls->setters.size());
  std::unordered_set<std::string> seenPrivateMethodNames;
  for (const auto& [name, _] : cls->methods) {
    if (seenPrivateMethodNames.insert(name).second) {
      privateMethodNames.push_back(name);
    }
  }
  for (const auto& [name, _] : cls->getters) {
    if (seenPrivateMethodNames.insert(name).second) {
      privateMethodNames.push_back(name);
    }
  }
  for (const auto& [name, _] : cls->setters) {
    if (seenPrivateMethodNames.insert(name).second) {
      privateMethodNames.push_back(name);
    }
  }
  std::sort(privateMethodNames.begin(), privateMethodNames.end());
  for (const auto& privateName : privateMethodNames) {
    std::string markerKey = privateMethodMarkerKey(cls, privateName);
    if (!defineClassElementOnValue(receiver, markerKey, Value(Undefined{}), false)) {
      throwError(ErrorType::TypeError, "Cannot initialize class fields on non-object receiver");
      LIGHTJS_RETURN(Value(Undefined{}));
    }
  }

  auto prevEnv = env_;
  env_ = cls->closure;
  env_ = env_->createChild();
  env_->define("__var_scope__", Value(true), true);
  env_->define("this", receiver);
  env_->define("__in_class_field_initializer__", Value(true), true);
  Value instanceSuperBase = getInstanceFieldSuperBase(cls);
  if (instanceSuperBase.isObject() || instanceSuperBase.isNull() ||
      instanceSuperBase.isClass() || instanceSuperBase.isFunction()) {
    env_->define("__super__", instanceSuperBase);
  }

  bool previousFieldInitializerState = activeFieldInitializerEvaluation_;
  activeFieldInitializerEvaluation_ = true;
  for (const auto& fi : cls->fieldInitializers) {
    Value fieldVal(Undefined{});
    const Expression* initExpr = nullptr;
    if (fi.initExpr) {
      initExpr = std::static_pointer_cast<Expression>(fi.initExpr).get();
      auto initTask = evaluate(*initExpr);
      LIGHTJS_RUN_TASK(initTask, fieldVal);
      if (flow_.type != ControlFlow::Type::None) {
        activeFieldInitializerEvaluation_ = previousFieldInitializerState;
        env_ = prevEnv;
        LIGHTJS_RETURN(Value(Undefined{}));
      }
    }

    maybeInferAnonymousFieldInitializerName(fi.name, fi.isPrivate, initExpr, fieldVal);
    std::string fieldKey = fi.isPrivate ? privateStorageKey(cls, fi.name) : fi.name;
    if (!defineClassElementOnValue(receiver, fieldKey, fieldVal, true)) {
      activeFieldInitializerEvaluation_ = previousFieldInitializerState;
      env_ = prevEnv;
      throwError(ErrorType::TypeError, "Cannot initialize class fields on non-object receiver");
      LIGHTJS_RETURN(Value(Undefined{}));
    }
  }
  activeFieldInitializerEvaluation_ = previousFieldInitializerState;
  env_ = prevEnv;
  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::initializeClassStaticFields(const GCPtr<Class>& cls,
                                              const std::vector<Class::FieldInit>& staticFields) {
  if (!cls || staticFields.empty()) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  auto prevEnv = env_;
  env_ = cls->closure;
  env_ = env_->createChild();
  env_->define("__var_scope__", Value(true), true);
  env_->define("this", Value(cls));
  env_->define("__in_class_field_initializer__", Value(true), true);
  Value staticSuperBase = getStaticFieldSuperBase(cls);
  if (staticSuperBase.isObject() || staticSuperBase.isNull() ||
      staticSuperBase.isClass() || staticSuperBase.isFunction()) {
    env_->define("__super__", staticSuperBase);
  }

  bool previousFieldInitializerState = activeFieldInitializerEvaluation_;
  activeFieldInitializerEvaluation_ = true;
  for (const auto& fi : staticFields) {
    Value fieldVal(Undefined{});
    const Expression* initExpr = nullptr;
    if (fi.initExpr) {
      initExpr = std::static_pointer_cast<Expression>(fi.initExpr).get();
      auto initTask = evaluate(*initExpr);
      LIGHTJS_RUN_TASK(initTask, fieldVal);
      if (flow_.type != ControlFlow::Type::None) {
        activeFieldInitializerEvaluation_ = previousFieldInitializerState;
        env_ = prevEnv;
        LIGHTJS_RETURN(Value(Undefined{}));
      }
    }

    maybeInferAnonymousFieldInitializerName(fi.name, fi.isPrivate, initExpr, fieldVal);
    std::string fieldKey = fi.isPrivate ? privateStorageKey(cls, fi.name) : fi.name;
    Value classValue(cls);
    if (!defineClassElementOnValue(classValue, fieldKey, fieldVal, true)) {
      activeFieldInitializerEvaluation_ = previousFieldInitializerState;
      env_ = prevEnv;
      throwError(ErrorType::TypeError, "Cannot define class static field " + fi.name);
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (!fi.isPrivate) {
      cls->properties["__enum_" + fi.name] = Value(true);
      cls->properties.erase("__non_enum_" + fi.name);
    }
  }
  activeFieldInitializerEvaluation_ = previousFieldInitializerState;
  env_ = prevEnv;
  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::constructValue(Value callee, std::vector<Value> args, Value newTargetOverride) {
  // Check memory limit before potential allocation
  if (!checkMemoryLimit(sizeof(Object))) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  if (!newTargetOverride.isUndefined()) {
    bool validNewTarget = false;
    if (newTargetOverride.isClass()) {
      validNewTarget = true;
    } else if (newTargetOverride.isFunction()) {
      auto fn = newTargetOverride.getGC<Function>();
      validNewTarget = fn->isConstructor;
    } else if (newTargetOverride.isProxy()) {
      validNewTarget = true;
    }

    if (!validNewTarget) {
      throwError(ErrorType::TypeError, "newTarget is not a constructor");
      LIGHTJS_RETURN(Value(Undefined{}));
    }
  }
  Value effectiveNewTarget = newTargetOverride.isUndefined() ? callee : newTargetOverride;

  // Handle Proxy construct trap
  if (callee.isProxy()) {
    auto proxyPtr = callee.getGC<Proxy>();
    if (proxyPtr->handler && proxyPtr->handler->isObject()) {
      auto handlerObj = std::get<GCPtr<Object>>(proxyPtr->handler->data);
      auto trapIt = handlerObj->properties.find("construct");
      if (trapIt != handlerObj->properties.end() && trapIt->second.isFunction()) {
        auto trap = trapIt->second.getGC<Function>();
        // Create args array
        auto argsArray = GarbageCollector::makeGC<Array>();
        argsArray->elements = args;
        // Call construct trap: handler.construct(target, argumentsList, newTarget)
        std::vector<Value> trapArgs = {*proxyPtr->target, Value(argsArray), effectiveNewTarget};
        Value result;
        if (trap->isNative) {
          result = trap->nativeFunc(trapArgs);
        } else {
          result = invokeFunction(trap, trapArgs, Value(Undefined{}));
        }
        // construct trap must return an object
        if (result.isObject() || result.isArray() || result.isFunction()) {
          LIGHTJS_RETURN(result);
        }
        throwError(ErrorType::TypeError, "'construct' on proxy: trap returned non-object");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
    }
    // No construct trap - pass through to target
    if (proxyPtr->target) {
      callee = *proxyPtr->target;
    }
  }

    auto setConstructorTag = [&](Value& instanceVal) {
      Value constructorTag = newTargetOverride.isUndefined() ? callee : effectiveNewTarget;
      auto setTagOnObject = [&](const auto& container) {
        container->properties["__constructor__"] = constructorTag;
      };
      if (instanceVal.isObject()) {
        setTagOnObject(instanceVal.getGC<Object>());
      } else if (instanceVal.isArray()) {
        setTagOnObject(instanceVal.getGC<Array>());
      } else if (instanceVal.isFunction()) {
        setTagOnObject(instanceVal.getGC<Function>());
      } else if (instanceVal.isRegex()) {
        setTagOnObject(instanceVal.getGC<Regex>());
      } else if (instanceVal.isPromise()) {
        setTagOnObject(instanceVal.getGC<Promise>());
      } else if (instanceVal.isMap()) {
        setTagOnObject(instanceVal.getGC<Map>());
      } else if (instanceVal.isSet()) {
        setTagOnObject(instanceVal.getGC<Set>());
      } else if (instanceVal.isWeakMap()) {
        setTagOnObject(instanceVal.getGC<WeakMap>());
      } else if (instanceVal.isWeakSet()) {
        setTagOnObject(instanceVal.getGC<WeakSet>());
      } else if (instanceVal.isTypedArray()) {
        setTagOnObject(instanceVal.getGC<TypedArray>());
      } else if (instanceVal.isArrayBuffer()) {
        setTagOnObject(instanceVal.getGC<ArrayBuffer>());
      } else if (instanceVal.isDataView()) {
        setTagOnObject(instanceVal.getGC<DataView>());
      } else if (instanceVal.isError()) {
        setTagOnObject(instanceVal.getGC<Error>());
      }
    };

    auto setProtoOnValue = [&](Value& targetVal, const Value& protoVal) {
      if (!isObjectLike(protoVal) && !protoVal.isNull()) return;
      auto setProto = [&](const auto& container) {
        container->properties["__proto__"] = protoVal;
      };
      if (targetVal.isObject()) {
        setProto(targetVal.getGC<Object>());
      } else if (targetVal.isArray()) {
        setProto(targetVal.getGC<Array>());
      } else if (targetVal.isFunction()) {
        setProto(targetVal.getGC<Function>());
      } else if (targetVal.isRegex()) {
        setProto(targetVal.getGC<Regex>());
      } else if (targetVal.isPromise()) {
        setProto(targetVal.getGC<Promise>());
      } else if (targetVal.isMap()) {
        setProto(targetVal.getGC<Map>());
      } else if (targetVal.isSet()) {
        setProto(targetVal.getGC<Set>());
      } else if (targetVal.isWeakMap()) {
        setProto(targetVal.getGC<WeakMap>());
      } else if (targetVal.isWeakSet()) {
        setProto(targetVal.getGC<WeakSet>());
      } else if (targetVal.isTypedArray()) {
        setProto(targetVal.getGC<TypedArray>());
      } else if (targetVal.isArrayBuffer()) {
        setProto(targetVal.getGC<ArrayBuffer>());
      } else if (targetVal.isDataView()) {
        setProto(targetVal.getGC<DataView>());
      } else if (targetVal.isError()) {
        setProto(targetVal.getGC<Error>());
      } else if (targetVal.isProxy()) {
        auto proxy = targetVal.getGC<Proxy>();
        if (proxy->target) {
          if (proxy->target->isObject()) {
            setProto(proxy->target->getGC<Object>());
          } else if (proxy->target->isArray()) {
            setProto(proxy->target->getGC<Array>());
          } else if (proxy->target->isFunction()) {
            setProto(proxy->target->getGC<Function>());
          } else if (proxy->target->isRegex()) {
            setProto(proxy->target->getGC<Regex>());
          } else if (proxy->target->isPromise()) {
            setProto(proxy->target->getGC<Promise>());
          }
        }
      }
    };

    auto setProxyTargetConstructor = [&](Value& maybeProxy,
                                         const Value& ctorValue,
                                         bool setVisibleConstructor) {
      if (!maybeProxy.isProxy()) return;
      auto proxy = maybeProxy.getGC<Proxy>();
      if (!proxy->target) return;
      auto apply = [&](const auto& container) {
        container->properties["__constructor__"] = ctorValue;
        if (setVisibleConstructor) {
          container->properties["constructor"] = ctorValue;
        }
      };
      if (proxy->target->isObject()) {
        apply(proxy->target->getGC<Object>());
      } else if (proxy->target->isArray()) {
        apply(proxy->target->getGC<Array>());
      } else if (proxy->target->isFunction()) {
        apply(proxy->target->getGC<Function>());
      } else if (proxy->target->isRegex()) {
        apply(proxy->target->getGC<Regex>());
      } else if (proxy->target->isPromise()) {
        apply(proxy->target->getGC<Promise>());
      }
    };

  auto wrapPrimitiveValue = [&](const Value& primitive) -> Value {
    auto wrapper = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    wrapper->properties["__primitive_value__"] = primitive;
    if (primitive.isString()) {
      // String objects have an own, non-writable/non-enumerable/non-configurable `length`.
      const std::string& s = std::get<std::string>(primitive.data);
      wrapper->properties["length"] = Value(static_cast<double>(String_utf16Length(s)));
      wrapper->properties["__non_writable_length"] = Value(true);
      wrapper->properties["__non_enum_length"] = Value(true);
      wrapper->properties["__non_configurable_length"] = Value(true);
    }

    // valueOf comes from the prototype chain (Number.prototype.valueOf, etc.)
    // not as an own property on the wrapper
    return Value(wrapper);
  };

  if (callee.isObject()) {
    auto objPtr = callee.getGC<Object>();
    auto callableIt = objPtr->properties.find("__callable_object__");
    auto ctorWrapperIt = objPtr->properties.find("__constructor_wrapper__");
    bool isCallableWrapper = callableIt != objPtr->properties.end() &&
                             callableIt->second.isBool() &&
                             callableIt->second.toBool() &&
                             ctorWrapperIt != objPtr->properties.end() &&
                             ctorWrapperIt->second.isBool() &&
                             ctorWrapperIt->second.toBool();
    if (isCallableWrapper) {
      auto ctorIt = objPtr->properties.find("constructor");
      if (ctorIt != objPtr->properties.end()) {
        callee = ctorIt->second;
      }
    }
  }

  // Handle Class constructor
  if (callee.isClass()) {
    auto cls = callee.getGC<Class>();
    struct ActivePrivateOwnerClassGuard {
      Interpreter* interpreter;
      GCPtr<Class> previousOwnerClass;
      ~ActivePrivateOwnerClassGuard() {
        interpreter->activePrivateOwnerClass_ = previousOwnerClass;
      }
    } activePrivateOwnerClassGuard{this, activePrivateOwnerClass_};
    activePrivateOwnerClass_ = cls;

    bool derivedConstructor =
      cls->superClass ||
      (cls->properties.find("__super_constructor__") != cls->properties.end());

    // Create the new instance object
    auto instance = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));

    // OrdinaryCreateFromConstructor(newTarget, "%Object.prototype%")
    Value protoForInstance = getOrdinaryCreatePrototypeFromNewTarget(effectiveNewTarget);
    if (protoForInstance.isObject()) {
      instance->properties["__proto__"] = protoForInstance;
    } else if (auto objectCtor = env_->getRoot()->get("Object")) {
      // Last-resort fallback to current realm.
      Value objectProto = getPrototypeFromConstructorValue(*objectCtor);
      if (objectProto.isObject()) {
        instance->properties["__proto__"] = objectProto;
      }
    }

    // Execute constructor if it exists
    if (cls->constructor) {
      auto prevEnv = env_;
      bool previousStrictMode = strictMode_;
      struct StrictModeScopeGuard {
        Interpreter* interpreter;
        bool previous;
        ~StrictModeScopeGuard() {
          interpreter->strictMode_ = previous;
        }
      } strictModeScopeGuard{this, previousStrictMode};
      env_ = cls->closure;
      env_ = env_->createChild();
      env_->define("__var_scope__", Value(true), true);
      strictMode_ = cls->constructor->isStrict;
      if (derivedConstructor) {
        env_->define("__super_called__", Value(false));
      }

      // Bind 'this' to the new instance
      env_->define("this", Value(instance));
      env_->define("__new_target__", effectiveNewTarget);
      env_->define("__current_constructor__", callee, true);

      // Bind super constructor + super base separately.
      bool extendsNull = false;
      if (auto extendsNullIt = cls->properties.find("__extends_null__");
          extendsNullIt != cls->properties.end() &&
          extendsNullIt->second.isBool() &&
          extendsNullIt->second.toBool()) {
        extendsNull = true;
      }

      Value superCtor(Undefined{});
      if (cls->superClass) {
        superCtor = Value(cls->superClass);
      } else if (auto superCtorIt = cls->properties.find("__super_constructor__");
                 superCtorIt != cls->properties.end()) {
        superCtor = superCtorIt->second;
      }
      if (!superCtor.isUndefined()) {
        env_->define("__super_constructor__", superCtor);
      }

      if (extendsNull) {
        env_->define("__super__", Value(Null{}));
      } else if (!superCtor.isUndefined()) {
        Value superBase = getPrototypeFromConstructorValue(superCtor);
        if (!superBase.isUndefined()) {
          env_->define("__super__", superBase);
        }
      } else if (auto objectCtor = env_->get("Object")) {
        // Base class: SuperProperty is allowed and targets Object.prototype.
        Value objectProto = getPrototypeFromConstructorValue(*objectCtor);
        if (!objectProto.isUndefined()) {
          env_->define("__super__", objectProto);
        }
      }

      // Bind `arguments` for the constructor body.
      auto argumentsArray = GarbageCollector::makeGC<Array>();
      GarbageCollector::instance().reportAllocation(sizeof(Array));
      argumentsArray->elements = args;
      auto throwTypeErrorAccessor = GarbageCollector::makeGC<Function>();
      throwTypeErrorAccessor->isNative = true;
      throwTypeErrorAccessor->nativeFunc = [](const std::vector<Value>&) -> Value {
        throw std::runtime_error(
          "TypeError: 'caller', 'callee', and 'arguments' properties may not be accessed");
      };
      argumentsArray->properties["__get_callee"] = Value(throwTypeErrorAccessor);
      argumentsArray->properties["__set_callee"] = Value(throwTypeErrorAccessor);
      argumentsArray->properties["__non_enum_callee"] = Value(true);
      if (auto objProtoVal = env_->getRoot()->get("__object_prototype__");
          objProtoVal && objProtoVal->isObject()) {
        argumentsArray->properties["__proto__"] = *objProtoVal;
      }
      env_->define("arguments", Value(argumentsArray));

      // Bind parameters
      auto func = cls->constructor;
      for (size_t i = 0; i < func->params.size(); ++i) {
        if (i < args.size()) {
          env_->define(func->params[i].name, args[i]);
        } else if (func->params[i].defaultValue) {
          auto defaultExpr = std::static_pointer_cast<Expression>(func->params[i].defaultValue);
          auto defaultTask = evaluate(*defaultExpr);
          LIGHTJS_RUN_TASK_VOID(defaultTask);
          env_->define(func->params[i].name, defaultTask.result());
        } else {
          env_->define(func->params[i].name, Value(Undefined{}));
        }
      }

      // Handle rest parameter
      if (func->restParam.has_value()) {
        auto restArr = GarbageCollector::makeGC<Array>();
        GarbageCollector::instance().reportAllocation(sizeof(Array));
        setArrayPrototype(restArr, env_.get());
        for (size_t i = func->params.size(); i < args.size(); ++i) {
          restArr->elements.push_back(args[i]);
        }
        env_->define(*func->restParam, Value(restArr));
      }

      // Set __constructor__ before any instance element initialization.
      instance->properties["__constructor__"] = callee;

      // Base constructors initialize instance elements before constructor body.
      if (!derivedConstructor) {
        Value instanceValue(instance);
        auto initTask = initializeClassInstanceElements(cls, instanceValue);
        LIGHTJS_RUN_TASK_VOID(initTask);
        if (flow_.type != ControlFlow::Type::None) {
          env_ = prevEnv;
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      }

      // Execute constructor body
      auto bodyPtr = std::static_pointer_cast<std::vector<StmtPtr>>(func->body);

      // Initialize TDZ for lexical declarations in constructor body
      for (const auto& s : *bodyPtr) {
        if (auto* varDecl = std::get_if<VarDeclaration>(&s->node)) {
          if (varDecl->kind == VarDeclaration::Kind::Let ||
              varDecl->kind == VarDeclaration::Kind::Const) {
            for (const auto& declarator : varDecl->declarations) {
              std::vector<std::string> names;
              collectVarHoistNames(*declarator.pattern, names);
              for (const auto& name : names) {
                env_->defineTDZ(name);
              }
            }
          }
        } else if (auto* classDecl = std::get_if<ClassDeclaration>(&s->node)) {
          env_->defineTDZ(classDecl->id.name);
        }
      }

      // Hoist var and function declarations in the constructor body (same as callFunction()).
      hoistVarDeclarations(*bodyPtr);
      for (const auto& hoistStmt : *bodyPtr) {
        if (std::holds_alternative<FunctionDeclaration>(hoistStmt->node)) {
          auto hoistTask = evaluate(*hoistStmt);
          LIGHTJS_RUN_TASK_VOID(hoistTask);
        }
      }
      if (flow_.type == ControlFlow::Type::Throw) {
        env_ = prevEnv;
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      auto prevFlow = flow_;
      flow_ = ControlFlow{};

      for (const auto& stmt : *bodyPtr) {
        auto stmtTask = evaluate(*stmt);
        LIGHTJS_RUN_TASK_VOID(stmtTask);

        if (flow_.type == ControlFlow::Type::Return) {
          break;
        }
        // Preserve throw flow control (errors)
        if (flow_.type == ControlFlow::Type::Throw) {
          break;
        }
      }

      bool superCalled = true;
      if (derivedConstructor) {
        if (auto superCalledVal = env_->get("__super_called__")) {
          superCalled = superCalledVal->toBool();
        } else {
          superCalled = false;
        }
        if (!superCalled) {
          if (auto currentThis = env_->get("this")) {
            bool stillInitialInstance =
              currentThis->isObject() &&
              currentThis->getGC<Object>().get() == instance.get();
            superCalled = !stillInitialInstance;
          }
        }
      }

      if (flow_.type == ControlFlow::Type::Return) {
        Value returnValue = flow_.value;
        if (isObjectLike(returnValue)) {
          if (flow_.type != ControlFlow::Type::Throw) {
            flow_ = prevFlow;
          }
          env_ = prevEnv;
          LIGHTJS_RETURN(returnValue);
        }
        if (derivedConstructor && !returnValue.isUndefined()) {
          flow_ = prevFlow;
          env_ = prevEnv;
          throwError(ErrorType::TypeError,
                     "Derived constructors may only return object or undefined");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      }

      if (derivedConstructor && !superCalled) {
        flow_ = prevFlow;
        env_ = prevEnv;
        throwError(ErrorType::ReferenceError,
                   "Must call super constructor in derived class before returning");
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      // Get the final `this` value (super() may have replaced it)
      Value finalThis = Value(instance);
      if (auto currentThis = env_->get("this")) {
        finalThis = *currentThis;
      }

      // Don't restore flow if an error was thrown - preserve the error
      if (flow_.type != ControlFlow::Type::Throw) {
        flow_ = prevFlow;
      } else {
        env_ = prevEnv;
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      env_ = prevEnv;

      setConstructorTag(finalThis);
      setProxyTargetConstructor(finalThis, callee, true);
      // If super() replaced `this` with a different type, set `constructor` property
      // to point to this class (simulates prototype.constructor inheritance)
      if (finalThis.isPromise()) {
        auto p = finalThis.getGC<Promise>();
        p->properties["constructor"] = callee;
      } else if (finalThis.isObject() &&
                 finalThis.getGC<Object>().get() != instance) {
        auto obj = finalThis.getGC<Object>();
        obj->properties["constructor"] = callee;
      } else if (finalThis.isArray()) {
        auto arr = finalThis.getGC<Array>();
        arr->properties["constructor"] = callee;
      }
      LIGHTJS_RETURN(finalThis);
    }

    // No explicit constructor with Class super: implicitly call super(...args).
    if (cls->superClass) {
      Value result;
      { auto _t = constructValue(Value(cls->superClass), args, effectiveNewTarget); LIGHTJS_RUN_TASK(_t, result); }
      if (flow_.type != ControlFlow::Type::None) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      setConstructorTag(result);
      setProxyTargetConstructor(result, callee, true);
      {
        auto initTask = initializeClassInstanceElements(cls, result);
        LIGHTJS_RUN_TASK_VOID(initTask);
        if (flow_.type != ControlFlow::Type::None) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      }

      if (result.isPromise()) {
        auto p = result.getGC<Promise>();
        p->properties["constructor"] = callee;
      } else if (result.isObject()) {
        auto obj = result.getGC<Object>();
        obj->properties["constructor"] = callee;
      } else if (result.isArray()) {
        auto arr = result.getGC<Array>();
        arr->properties["constructor"] = callee;
      }
      LIGHTJS_RETURN(result);
    }

    // No explicit constructor with Function/callable super: implicitly call super(...args).
    // ES2020: default constructor for derived class is constructor(...args) { super(...args); }
    auto superCtorIt = cls->properties.find("__super_constructor__");
    if (superCtorIt != cls->properties.end()) {
      Value superVal = superCtorIt->second;
      Value result;
      { auto _t = constructValue(superVal, args, effectiveNewTarget); LIGHTJS_RUN_TASK(_t, result); }
      if (flow_.type != ControlFlow::Type::None) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      setConstructorTag(result);
      setProxyTargetConstructor(result, callee, true);
      {
        auto initTask = initializeClassInstanceElements(cls, result);
        LIGHTJS_RUN_TASK_VOID(initTask);
        if (flow_.type != ControlFlow::Type::None) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      }

      if (result.isPromise()) {
        auto p = result.getGC<Promise>();
        p->properties["constructor"] = callee;
      } else if (result.isObject()) {
        auto obj = result.getGC<Object>();
        obj->properties["constructor"] = callee;
      }
      LIGHTJS_RETURN(result);
    }

    instance->properties["__constructor__"] = callee;
    {
      Value instanceValue(instance);
      auto initTask = initializeClassInstanceElements(cls, instanceValue);
      LIGHTJS_RUN_TASK_VOID(initTask);
      if (flow_.type != ControlFlow::Type::None) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
    }
    LIGHTJS_RETURN(Value(instance));
  }

  // Handle Function constructor (regular constructor function)
  if (callee.isFunction()) {
    auto func = callee.getGC<Function>();

    if (func->isGenerator || func->isAsync) {
      throwError(ErrorType::TypeError, "Function is not a constructor");
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (!func->isConstructor) {
      throwError(ErrorType::TypeError, "Function is not a constructor");
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    if (func->isNative) {
      auto noNewIt = func->properties.find("__throw_on_new__");
      if (noNewIt != func->properties.end() && noNewIt->second.isBool() && noNewIt->second.toBool()) {
        throwError(ErrorType::TypeError, "Function is not a constructor");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      // Bound functions: [[Construct]] forwards to the bound target with bound arguments
      // and preserves the original newTarget.
      auto boundTargetIt = func->properties.find("__bound_target__");
      if (boundTargetIt != func->properties.end()) {
        std::vector<Value> finalArgs;
        if (auto boundArgsIt = func->properties.find("__bound_args__");
            boundArgsIt != func->properties.end() && boundArgsIt->second.isArray()) {
          auto boundArgsArr = boundArgsIt->second.getGC<Array>();
          finalArgs = boundArgsArr->elements;
        }
        finalArgs.insert(finalArgs.end(), args.begin(), args.end());
        // For `new boundFn(...)`, newTarget defaults to the bound function itself.
        // Bound functions should forward the default newTarget to the bound target so
        // derived default constructors use the target's prototype as the fallback.
        Value forwardedNewTarget = effectiveNewTarget;
        if (newTargetOverride.isUndefined()) {
          forwardedNewTarget = boundTargetIt->second;
        }
        Value constructed;
        { auto _t = constructValue(boundTargetIt->second, finalArgs, forwardedNewTarget); LIGHTJS_RUN_TASK(_t, constructed); }
        if (flow_.type != ControlFlow::Type::None) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        LIGHTJS_RETURN(constructed);
      }
      // Native constructors (e.g., Array, Object, Map, etc.)
      Value constructed(Undefined{});
      auto nativeConstructIt = func->properties.find("__native_construct__");
      if (nativeConstructIt != func->properties.end() && nativeConstructIt->second.isFunction()) {
        auto ctorFn = nativeConstructIt->second.getGC<Function>();
        constructed = ctorFn->nativeFunc(args);
      } else {
        constructed = func->nativeFunc(args);
      }
      if (constructed.isNumber() || constructed.isString() || constructed.isBool()) {
        constructed = wrapPrimitiveValue(constructed);
      }
      auto nameIt = func->properties.find("name");
      std::string ctorName =
        (nameIt != func->properties.end() && nameIt->second.isString())
          ? nameIt->second.toString()
          : std::string();
      // OrdinaryCreateFromConstructor(newTarget, ...) for native constructors.
      // Many internal objects (TypedArray, ArrayBuffer, Error, etc.) are not plain Objects,
      // so ensure they get the correct prototype for subclassing.
      Value protoFromNewTarget(Undefined{});
      if (newTargetOverride.isUndefined()) {
        auto protoIt = func->properties.find("prototype");
        if (protoIt != func->properties.end() &&
            (protoIt->second.isObject() || protoIt->second.isNull())) {
          protoFromNewTarget = protoIt->second;
        }
      } else {
        protoFromNewTarget = this->getPrototypeFromConstructorValue(effectiveNewTarget);
        if ((ctorName == "Promise" || ctorName == "ArrayBuffer" || ctorName == "SharedArrayBuffer" || ctorName == "DataView" ||
             isTypedArrayConstructorName(ctorName)) &&
            !protoFromNewTarget.isObject()) {
          protoFromNewTarget = getIntrinsicPrototypeForCtorNamed(effectiveNewTarget, ctorName);
        }
      }
      if (newTargetOverride.isUndefined() &&
          (ctorName == "Promise" || ctorName == "ArrayBuffer" || ctorName == "SharedArrayBuffer" || ctorName == "DataView" ||
           isTypedArrayConstructorName(ctorName)) &&
          !protoFromNewTarget.isObject()) {
        protoFromNewTarget = getIntrinsicPrototypeForCtorNamed(callee, ctorName);
      }
      if (ctorName == "DataView" && constructed.isDataView()) {
        auto view = constructed.getGC<DataView>();
        auto buffer = view ? view->buffer : GCPtr<ArrayBuffer>{};
        if (!buffer || buffer->detached) {
          throwError(ErrorType::TypeError, "DataView requires a non-detached ArrayBuffer");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (view->byteOffset > buffer->byteLength) {
          throwError(ErrorType::RangeError, "Invalid DataView offset");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (!view->lengthTracking &&
            view->byteLength > buffer->byteLength - view->byteOffset) {
          throwError(ErrorType::RangeError, "Invalid DataView length");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      }
      if (isObjectLike(protoFromNewTarget)) {
        setProtoOnValue(constructed, protoFromNewTarget);
      }
      setConstructorTag(constructed);
      LIGHTJS_RETURN(constructed);
    }

    // Create the new instance object
    auto instance = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));

    // OrdinaryCreateFromConstructor(newTarget, "%Object.prototype%")
    Value protoForInstance = getOrdinaryCreatePrototypeFromNewTarget(effectiveNewTarget);
    if (isObjectLike(protoForInstance)) {
      instance->properties["__proto__"] = protoForInstance;
    } else if (auto objectCtor = env_->getRoot()->get("Object")) {
      Value objectProto = getPrototypeFromConstructorValue(*objectCtor);
      if (objectProto.isObject()) {
        instance->properties["__proto__"] = objectProto;
      }
    }

    // Set up execution environment
    auto prevEnv = env_;
    env_ = func->closure;
    env_ = env_->createChild();
    env_->define("__var_scope__", Value(true), true);

    // Bind 'this' to the new instance
    env_->define("this", Value(instance));
    env_->define("__new_target__", effectiveNewTarget);

    // Bind `arguments` for the constructor body.
    auto argumentsArray = GarbageCollector::makeGC<Array>();
    GarbageCollector::instance().reportAllocation(sizeof(Array));
    argumentsArray->elements = args;
    if (func->isStrict) {
      auto throwTypeErrorAccessor = GarbageCollector::makeGC<Function>();
      throwTypeErrorAccessor->isNative = true;
      throwTypeErrorAccessor->nativeFunc = [](const std::vector<Value>&) -> Value {
        throw std::runtime_error("TypeError: 'caller', 'callee', and 'arguments' properties may not be accessed");
      };
      argumentsArray->properties["__get_callee"] = Value(throwTypeErrorAccessor);
      argumentsArray->properties["__set_callee"] = Value(throwTypeErrorAccessor);
    } else {
      argumentsArray->properties["callee"] = callee;
    }
    argumentsArray->properties["__non_enum_callee"] = Value(true);
    if (auto objProtoVal = env_->getRoot()->get("__object_prototype__");
        objProtoVal && objProtoVal->isObject()) {
      argumentsArray->properties["__proto__"] = *objProtoVal;
    }
    env_->define("arguments", Value(argumentsArray));

    // Bind parameters
    for (size_t i = 0; i < func->params.size(); ++i) {
      if (i < args.size()) {
        env_->define(func->params[i].name, args[i]);
      } else if (func->params[i].defaultValue) {
        auto defaultExpr = std::static_pointer_cast<Expression>(func->params[i].defaultValue);
        auto defaultTask = evaluate(*defaultExpr);
        LIGHTJS_RUN_TASK_VOID(defaultTask);
        env_->define(func->params[i].name, defaultTask.result());
      } else {
        env_->define(func->params[i].name, Value(Undefined{}));
      }
    }

    // Mapped arguments: in sloppy mode with simple params and no rest parameter,
    // keep `arguments[i]` and the corresponding formal parameter in sync.
    if (!func->isStrict && !func->restParam.has_value()) {
      bool hasSimpleParams = true;
      for (const auto& p : func->params) {
        if (p.defaultValue || p.name.empty() ||
            (p.name.size() > 8 && p.name.substr(0, 8) == "__param_")) {
          hasSimpleParams = false; break;
        }
      }
      if (hasSimpleParams) {
        for (size_t i = 0; i < func->params.size() && i < args.size(); ++i) {
          std::string paramName = func->params[i].name;
          std::string idxStr = std::to_string(i);
          argumentsArray->properties["__mapped_arg_index_" + idxStr + "__"] = Value(true);
          auto getter = GarbageCollector::makeGC<Function>();
          getter->isNative = true;
          getter->nativeFunc = [env = env_, paramName](const std::vector<Value>&) -> Value {
            auto val = env->get(paramName);
            return val.has_value() ? *val : Value(Undefined{});
          };
          argumentsArray->properties["__get_" + idxStr] = Value(getter);
          auto setter = GarbageCollector::makeGC<Function>();
          setter->isNative = true;
          setter->nativeFunc = [env = env_, paramName](const std::vector<Value>& a) -> Value {
            if (!a.empty()) {
              env->set(paramName, a[0]);
            }
            return Value(Undefined{});
          };
          argumentsArray->properties["__set_" + idxStr] = Value(setter);
        }
      }
    }

    // Handle rest parameter
    if (func->restParam.has_value()) {
      auto restArr = GarbageCollector::makeGC<Array>();
      GarbageCollector::instance().reportAllocation(sizeof(Array));
      setArrayPrototype(restArr, env_.get());
      for (size_t i = func->params.size(); i < args.size(); ++i) {
        restArr->elements.push_back(args[i]);
      }
      env_->define(*func->restParam, Value(restArr));
    }

    // Execute function body
    auto bodyPtr = std::static_pointer_cast<std::vector<StmtPtr>>(func->body);

    // Initialize TDZ for lexical declarations in constructor body
    for (const auto& s : *bodyPtr) {
      if (auto* varDecl = std::get_if<VarDeclaration>(&s->node)) {
        if (varDecl->kind == VarDeclaration::Kind::Let ||
            varDecl->kind == VarDeclaration::Kind::Const) {
          for (const auto& declarator : varDecl->declarations) {
            std::vector<std::string> names;
            collectVarHoistNames(*declarator.pattern, names);
            for (const auto& name : names) {
              env_->defineTDZ(name);
            }
          }
        }
      } else if (auto* classDecl = std::get_if<ClassDeclaration>(&s->node)) {
        env_->defineTDZ(classDecl->id.name);
      }
    }

    // Hoist var and function declarations in constructor body
    hoistVarDeclarations(*bodyPtr);
    for (const auto& hoistStmt : *bodyPtr) {
      if (std::holds_alternative<FunctionDeclaration>(hoistStmt->node)) {
        auto hoistTask = evaluate(*hoistStmt);
        LIGHTJS_RUN_TASK_VOID_SYNC(hoistTask);
      }
    }

    Value result = Value(Undefined{});
    auto prevFlow = flow_;
    flow_ = ControlFlow{};

    for (const auto& stmt : *bodyPtr) {
      // Skip function declarations - already hoisted
      if (std::holds_alternative<FunctionDeclaration>(stmt->node)) {
        continue;
      }
      auto stmtTask = evaluate(*stmt);
  LIGHTJS_RUN_TASK(stmtTask, result);

      if (flow_.type == ControlFlow::Type::Return) {
        // If constructor returns an object-like value, use that; otherwise use the instance
        // ES spec: "If Type(result) is Object, return result."
        if (flow_.value.isObject()) {
          instance = flow_.value.getGC<Object>();
        } else if (isObjectLike(flow_.value)) {
          // Constructor returned a non-plain-Object type (Promise, Array, Function, etc.)
          Value returnedVal = flow_.value;
          flow_ = prevFlow;
          env_ = prevEnv;
          setConstructorTag(returnedVal);
          LIGHTJS_RETURN(returnedVal);
        }
        break;
      }
      // Preserve throw flow control (errors)
      if (flow_.type == ControlFlow::Type::Throw) {
        break;
      }
    }

    // Don't restore flow if an error was thrown - preserve the error
    if (flow_.type != ControlFlow::Type::Throw) {
      flow_ = prevFlow;
    }
    env_ = prevEnv;

    Value instanceVal(instance);
    setConstructorTag(instanceVal);
    LIGHTJS_RETURN(Value(instance));
  }

  // Not a constructor
  throwError(ErrorType::TypeError, "Value is not a constructor");
  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::evaluateNew(const NewExpr& expr) {
  // Evaluate the callee (constructor)
  auto calleeTask = evaluate(*expr.callee);
  Value callee;
  LIGHTJS_RUN_TASK(calleeTask, callee);
  if (flow_.type != ControlFlow::Type::None) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  // Evaluate arguments
  std::vector<Value> args;
  for (const auto& arg : expr.arguments) {
    if (auto* spread = std::get_if<SpreadElement>(&arg->node)) {
      Value val;
      { auto _t = evaluate(*spread->argument); LIGHTJS_RUN_TASK(_t, val); }
      if (flow_.type != ControlFlow::Type::None) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      auto iterRecOpt = getIterator(val);
      if (!iterRecOpt) {
        throwError(ErrorType::TypeError, val.toString() + " is not iterable");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      auto& iterRec = *iterRecOpt;
      while (true) {
        Value step = iteratorNext(iterRec);
        if (flow_.type == ControlFlow::Type::Throw) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (!step.isObject()) {
          throwError(ErrorType::TypeError, "Iterator result is not an object");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        auto stepObj = step.getGC<Object>();
        bool done = false;
        auto doneGetterIt = stepObj->properties.find("__get_done");
        if (doneGetterIt != stepObj->properties.end() && doneGetterIt->second.isFunction()) {
          Value doneVal = callFunction(doneGetterIt->second, {}, step);
          if (flow_.type == ControlFlow::Type::Throw) {
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          done = doneVal.toBool();
        } else {
          auto doneIt = stepObj->properties.find("done");
          done = (doneIt != stepObj->properties.end() && doneIt->second.toBool());
        }
        if (done) {
          break;
        }
        Value nextArg = Value(Undefined{});
        auto valueGetterIt = stepObj->properties.find("__get_value");
        if (valueGetterIt != stepObj->properties.end() && valueGetterIt->second.isFunction()) {
          nextArg = callFunction(valueGetterIt->second, {}, step);
          if (flow_.type == ControlFlow::Type::Throw) {
            LIGHTJS_RETURN(Value(Undefined{}));
          }
        } else {
          auto valueIt = stepObj->properties.find("value");
          if (valueIt != stepObj->properties.end()) {
            nextArg = valueIt->second;
          }
        }
        args.push_back(nextArg);
      }
    } else {
      auto argTask = evaluate(*arg);
      LIGHTJS_RUN_TASK_VOID(argTask);
      if (flow_.type != ControlFlow::Type::None) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      args.push_back(argTask.result());
    }
  }

  { auto _t = constructValue(callee, args, Value(Undefined{})); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
}

Task Interpreter::evaluateClass(const ClassExpr& expr) {
  auto previousPendingAnonymousClassName = pendingAnonymousClassName_;
  struct PendingAnonymousClassNameGuard {
    Interpreter* interpreter;
    std::optional<std::string> previous;
    ~PendingAnonymousClassNameGuard() {
      interpreter->pendingAnonymousClassName_ = previous;
    }
  } pendingAnonymousClassNameGuard{this, previousPendingAnonymousClassName};

  std::string effectiveClassName = expr.name;
  if (effectiveClassName.empty() && pendingAnonymousClassName_.has_value()) {
    effectiveClassName = *pendingAnonymousClassName_;
  }
  pendingAnonymousClassName_.reset();

  auto prevEnv = env_;
  struct EnvironmentGuard {
    Interpreter* interpreter;
    GCPtr<Environment> previous;
    ~EnvironmentGuard() {
      interpreter->env_ = previous;
    }
  } envGuard{this, prevEnv};

  auto cls = GarbageCollector::makeGC<Class>(effectiveClassName);
  GarbageCollector::instance().reportAllocation(sizeof(Class));
  cls->privateBrandId = g_classPrivateBrandCounter.fetch_add(1, std::memory_order_relaxed);
  if (sourceKeepAlive_) {
    cls->astOwner = sourceKeepAlive_;
  }
  const bool hasLexicalNameBinding = !expr.name.empty();
  if (hasLexicalNameBinding) {
    auto classScopeEnv = prevEnv->createChild();
    classScopeEnv->defineLexical(expr.name, Value(cls), true);
    env_ = classScopeEnv;
    cls->closure = classScopeEnv;
  } else {
    cls->closure = env_;
  }
  cls->lexicalParentClass = activePrivateOwnerClass_;

  // Handle superclass
  if (expr.superClass) {
      struct StrictModeScopeGuard {
        Interpreter* interpreter;
        bool previous;
        ~StrictModeScopeGuard() {
          interpreter->strictMode_ = previous;
        }
      } strictModeScopeGuard{this, strictMode_};
      strictMode_ = true;
      auto superTask = evaluate(*expr.superClass);
      Value superVal;
      LIGHTJS_RUN_TASK(superTask, superVal);
      auto isCallableObject = [](const Value& v) -> bool {
      if (!v.isObject()) return false;
      auto obj = v.getGC<Object>();
      auto callableIt = obj->properties.find("__callable_object__");
      return callableIt != obj->properties.end() &&
             callableIt->second.isBool() &&
             callableIt->second.toBool();
    };
    if (superVal.isNull()) {
      cls->properties["__extends_null__"] = Value(true);
      if (auto functionCtor = env_->get("Function");
          functionCtor && functionCtor->isFunction()) {
        auto fn = functionCtor->getGC<Function>();
        auto protoIt = fn->properties.find("prototype");
        if (protoIt != fn->properties.end()) {
          cls->properties["__super_constructor__"] = protoIt->second;
        }
      }
      } else if (superVal.isClass()) {
        cls->superClass = superVal.getGC<Class>();
      } else if (superVal.isFunction() || isCallableObject(superVal)) {
        bool isConstructable = true;
        if (superVal.isFunction()) {
          auto superFunc = superVal.getGC<Function>();
          isConstructable = superFunc->isConstructor;
          if (superFunc->isGenerator || superFunc->isAsync) isConstructable = false;
          auto arrowIt = superFunc->properties.find("__is_arrow_function__");
          if (arrowIt != superFunc->properties.end() &&
              arrowIt->second.isBool() &&
              arrowIt->second.toBool()) {
            isConstructable = false;
          }
        } else if (superVal.isObject()) {
          auto superObj = superVal.getGC<Object>();
          auto ctorIt = superObj->properties.find("constructor");
          if (ctorIt == superObj->properties.end()) {
            isConstructable = false;
          } else if (ctorIt->second.isFunction()) {
            auto inner = ctorIt->second.getGC<Function>();
            isConstructable = inner && inner->isConstructor;
          } else if (ctorIt->second.isClass()) {
            isConstructable = true;
          } else if (ctorIt->second.isProxy()) {
            isConstructable = true;
          } else {
            isConstructable = false;
          }
        }
        if (!isConstructable) {
          throwError(ErrorType::TypeError, "Class extends value is not a constructor or null");
          LIGHTJS_RETURN(Value(Undefined{}));
        }

      cls->properties["__super_constructor__"] = superVal;
      if (superVal.isFunction()) {
        auto superFunc = superVal.getGC<Function>();
        for (const auto& [key, val] : superFunc->properties) {
          if (key.size() >= 2 && key[0] == '_' && key[1] == '_') continue;
          if (key == "name" || key == "length" || key == "prototype" ||
              key == "caller" || key == "arguments") continue;
          if (cls->properties.find(key) == cls->properties.end()) {
            cls->properties[key] = val;
          }
        }
      } else if (superVal.isObject()) {
        auto superObj = superVal.getGC<Object>();
        for (const auto& [key, val] : superObj->properties) {
          if (key.size() >= 2 && key[0] == '_' && key[1] == '_') continue;
          if (key == "name" || key == "length" || key == "prototype" ||
              key == "caller" || key == "arguments") continue;
          if (cls->properties.find(key) == cls->properties.end()) {
            cls->properties[key] = val;
          }
        }
      }
    } else {
      throwError(ErrorType::TypeError, "Class extends value is not a constructor or null");
      LIGHTJS_RETURN(Value(Undefined{}));
    }
  }

  auto getPrototypeFromConstructor = [&](const Value& ctorValue) -> Value {
    return getPrototypeFromConstructorValue(ctorValue);
  };

  // Create Class.prototype object and wire prototype inheritance.
  auto classPrototype = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  auto extendsNullIt = cls->properties.find("__extends_null__");
  bool extendsNull = extendsNullIt != cls->properties.end() &&
                     extendsNullIt->second.isBool() &&
                     extendsNullIt->second.toBool();
  if (extendsNull) {
    classPrototype->properties["__proto__"] = Value(Null{});
  } else if (cls->superClass) {
    Value superProto = getPrototypeFromConstructor(Value(cls->superClass));
    if (hasError()) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (isObjectLike(superProto) || superProto.isNull()) {
      classPrototype->properties["__proto__"] = superProto;
    } else {
      throwError(ErrorType::TypeError, "Class extends value does not have a valid prototype");
      LIGHTJS_RETURN(Value(Undefined{}));
    }
  } else if (auto superCtorIt = cls->properties.find("__super_constructor__");
             superCtorIt != cls->properties.end()) {
    Value superProto = getPrototypeFromConstructor(superCtorIt->second);
    if (hasError()) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (isObjectLike(superProto) || superProto.isNull()) {
      classPrototype->properties["__proto__"] = superProto;
    } else {
      throwError(ErrorType::TypeError, "Class extends value does not have a valid prototype");
      LIGHTJS_RETURN(Value(Undefined{}));
    }
  } else if (auto objectCtor = env_->get("Object")) {
    Value objectProto = getPrototypeFromConstructor(*objectCtor);
    if (hasError()) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (isObjectLike(objectProto) || objectProto.isNull()) {
      classPrototype->properties["__proto__"] = objectProto;
    }
  }
  cls->properties["prototype"] = Value(classPrototype);
  cls->properties["__non_writable_prototype"] = Value(true);
  cls->properties["__non_enum_prototype"] = Value(true);
  cls->properties["__non_configurable_prototype"] = Value(true);

  classPrototype->properties["constructor"] = Value(cls);
  classPrototype->properties["__non_enum_constructor"] = Value(true);

  auto resolveSuperForMethod = [&](const MethodDefinition& method) -> Value {
    if (method.kind == MethodDefinition::Kind::Constructor || method.isStatic) {
      if (cls->superClass) {
        return Value(cls->superClass);
      }
      auto superCtorIt = cls->properties.find("__super_constructor__");
      if (superCtorIt != cls->properties.end()) {
        return superCtorIt->second;
      }
      if (auto objectCtor = env_->get("Object")) {
        return *objectCtor;
      }
      return Value(Undefined{});
    }
    if (extendsNull) {
      return Value(Null{});
    }
    if (cls->superClass) {
      Value superProto = getPrototypeFromConstructor(Value(cls->superClass));
      if (hasError()) {
        return Value(Undefined{});
      }
      if (isObjectLike(superProto) || superProto.isNull()) {
        return superProto;
      }
    }
    auto superCtorIt = cls->properties.find("__super_constructor__");
    if (superCtorIt != cls->properties.end()) {
      Value superProto = getPrototypeFromConstructor(superCtorIt->second);
      if (hasError()) {
        return Value(Undefined{});
      }
      if (isObjectLike(superProto) || superProto.isNull()) {
        return superProto;
      }
    }
    if (auto objectCtor = env_->get("Object")) {
      Value objectProto = getPrototypeFromConstructor(*objectCtor);
      if (hasError()) {
        return Value(Undefined{});
      }
      if (isObjectLike(objectProto) || objectProto.isNull()) {
        return objectProto;
      }
    }
    return Value(Undefined{});
  };

  auto previousPrivateOwnerClass = activePrivateOwnerClass_;
  struct ActivePrivateOwnerClassScopeGuard {
    Interpreter* interpreter;
    GCPtr<Class> previous;
    ~ActivePrivateOwnerClassScopeGuard() {
      interpreter->activePrivateOwnerClass_ = previous;
    }
  } activePrivateOwnerClassScopeGuard{this, previousPrivateOwnerClass};
  activePrivateOwnerClass_ = cls;

  struct StaticInitStep {
    enum class Kind { Field, Block };
    Kind kind = Kind::Field;
    Class::FieldInit field;
    const std::vector<StmtPtr>* blockBody = nullptr;
  };
  std::vector<StaticInitStep> staticInitSteps;

  // Process methods and fields
  for (const auto& method : expr.methods) {
    if (method.kind == MethodDefinition::Kind::StaticBlock) {
      StaticInitStep step;
      step.kind = StaticInitStep::Kind::Block;
      step.blockBody = &method.body;
      staticInitSteps.push_back(std::move(step));
      continue;
    }

    std::string methodName = method.key.name;
    Value propKeyForName(methodName);
    if (method.computed) {
      if (!method.computedKey) {
        throwError(ErrorType::SyntaxError, "Invalid computed class element name");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      auto keyTask = evaluate(*method.computedKey);
      Value keyValue;
      LIGHTJS_RUN_TASK(keyTask, keyValue);
      if (isObjectLike(keyValue)) {
        keyValue = toPrimitiveValue(keyValue, true);
        if (hasError()) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      }
      propKeyForName = keyValue;
      methodName = toPropertyKeyString(keyValue);
    }

    // Handle field declarations
    if (method.kind == MethodDefinition::Kind::Field) {
      if (method.isStatic && !method.isPrivate && methodName == "prototype") {
        throwError(ErrorType::TypeError, "Cannot redefine property: prototype");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (method.isStatic) {
        Class::FieldInit fi;
        fi.name = methodName;
        fi.isPrivate = method.isPrivate;
        if (method.initializer) {
          fi.initExpr = std::shared_ptr<void>(
            const_cast<Expression*>(method.initializer.get()),
            [](void*){} // No-op deleter
          );
        }
        StaticInitStep step;
        step.kind = StaticInitStep::Kind::Field;
        step.field = std::move(fi);
        staticInitSteps.push_back(std::move(step));
      } else {
        Class::FieldInit fi;
        fi.name = methodName;
        fi.isPrivate = method.isPrivate;
        if (method.initializer) {
          fi.initExpr = std::shared_ptr<void>(
            const_cast<Expression*>(method.initializer.get()),
            [](void*){} // No-op deleter
          );
        }
        cls->fieldInitializers.push_back(std::move(fi));
      }
      continue;
    }

    if (method.isStatic && !method.isPrivate && methodName == "prototype") {
      throwError(ErrorType::TypeError, "Cannot redefine property: prototype");
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    // Create function from method definition
    auto func = GarbageCollector::makeGC<Function>();
    func->isNative = false;
    func->isAsync = method.isAsync;
    func->isGenerator = method.isGenerator;
    func->isStrict = true;  // Class bodies are always strict.
    if (sourceKeepAlive_) {
      func->astOwner = sourceKeepAlive_;
    }
    func->closure = env_;
    func->properties["__private_owner_class__"] = Value(cls);
    func->properties["__is_class_static_method__"] = Value(method.isStatic);
    // [[HomeObject]] drives `super` property access semantics and must observe
    // dynamic changes to the home object's [[Prototype]] (e.g. Object.setPrototypeOf).
    if (method.isStatic) {
      func->properties["__home_object__"] = Value(cls);
    } else {
      func->properties["__home_object__"] = Value(classPrototype);
    }

    size_t methodLength = 0;
    bool sawDefault = false;
    for (const auto& param : method.params) {
      FunctionParam funcParam;
      funcParam.name = param.name.name;
      if (param.defaultValue) {
        funcParam.defaultValue = std::shared_ptr<void>(
            const_cast<Expression*>(param.defaultValue.get()),
            [](void*) {});
        sawDefault = true;
      } else if (!sawDefault) {
        methodLength++;
      }
      func->params.push_back(funcParam);
    }
    if (method.restParam.has_value()) {
      func->restParam = method.restParam->name;
    }

    // Store the body - we need to cast away const for the shared_ptr
    func->body = std::shared_ptr<void>(
      const_cast<std::vector<StmtPtr>*>(&method.body),
      [](void*){} // No-op deleter since we don't own the memory
    );
    func->destructurePrologue = std::shared_ptr<void>(
      const_cast<std::vector<StmtPtr>*>(&method.destructurePrologue),
      [](void*){} // No-op deleter
    );
    func->properties["length"] = Value(static_cast<double>(methodLength));
    if (method.kind == MethodDefinition::Kind::Constructor) {
      func->properties["name"] = Value(std::string("constructor"));
    } else {
      std::string baseName;
      if (propKeyForName.isSymbol()) {
        const auto& sym = std::get<Symbol>(propKeyForName.data);
        baseName = sym.description.empty() ? "" : "[" + sym.description + "]";
      } else {
        baseName = toPropertyKeyString(propKeyForName);
      }
      if (method.kind == MethodDefinition::Kind::Get) {
        func->properties["name"] = Value(std::string("get ") + baseName);
      } else if (method.kind == MethodDefinition::Kind::Set) {
        func->properties["name"] = Value(std::string("set ") + baseName);
      } else {
        func->properties["name"] = Value(baseName);
      }
    }
    func->properties["__non_writable_name"] = Value(true);
    func->properties["__non_enum_name"] = Value(true);
    func->properties["__non_writable_length"] = Value(true);
    func->properties["__non_enum_length"] = Value(true);

    // Keep legacy super base for older code paths, but prefer [[HomeObject]].
    Value superBase = resolveSuperForMethod(method);
    if (!superBase.isUndefined()) {
      func->properties["__super_class__"] = superBase;
    }

    if (method.kind == MethodDefinition::Kind::Constructor) {
      cls->constructor = func;
    } else if (method.isStatic) {
      if (method.isPrivate) {
        std::string mangledName = privateStorageKey(cls, methodName);
        if (method.kind == MethodDefinition::Kind::Get) {
          cls->properties["__get_" + mangledName] = Value(func);
        } else if (method.kind == MethodDefinition::Kind::Set) {
          cls->properties["__set_" + mangledName] = Value(func);
        } else {
          cls->staticMethods[mangledName] = func;
          cls->properties[mangledName] = Value(func);
        }
      } else {
        if (method.kind == MethodDefinition::Kind::Get) {
          cls->properties["__get_" + methodName] = Value(func);
          cls->properties["__non_enum_" + methodName] = Value(true);
        } else if (method.kind == MethodDefinition::Kind::Set) {
          cls->properties["__set_" + methodName] = Value(func);
          cls->properties["__non_enum_" + methodName] = Value(true);
        } else {
          cls->staticMethods[methodName] = func;
          cls->properties[methodName] = Value(func);
          cls->properties["__non_enum_" + methodName] = Value(true);
        }
      }
    } else if (method.isPrivate) {
      if (method.kind == MethodDefinition::Kind::Get) {
        cls->getters[methodName] = func;
      } else if (method.kind == MethodDefinition::Kind::Set) {
        cls->setters[methodName] = func;
      } else {
        cls->methods[methodName] = func;
      }
    } else if (method.kind == MethodDefinition::Kind::Get) {
      classPrototype->properties["__get_" + methodName] = Value(func);
      classPrototype->properties["__non_enum_" + methodName] = Value(true);
    } else if (method.kind == MethodDefinition::Kind::Set) {
      classPrototype->properties["__set_" + methodName] = Value(func);
      classPrototype->properties["__non_enum_" + methodName] = Value(true);
    } else {
      classPrototype->properties[methodName] = Value(func);
      classPrototype->properties["__non_enum_" + methodName] = Value(true);
    }
  }

  // Set name as own property (per spec: SetFunctionName). Class static elements
  // may define an own "name" property (e.g. `static name() {}`) which should
  // override the built-in name property, so don't clobber if already present
  // (including accessor definitions).
  if (cls->properties.find("name") == cls->properties.end() &&
      cls->properties.find("__get_name") == cls->properties.end() &&
      cls->properties.find("__set_name") == cls->properties.end()) {
    cls->properties["name"] = Value(cls->name);
    cls->properties["__non_writable_name"] = Value(true);
    cls->properties["__non_enum_name"] = Value(true);
  }

  // Set length property (constructor parameter count)
  int ctorLen = cls->constructor ? (int)cls->constructor->params.size() : 0;
  if (cls->properties.find("length") == cls->properties.end() &&
      cls->properties.find("__get_length") == cls->properties.end() &&
      cls->properties.find("__set_length") == cls->properties.end()) {
    cls->properties["length"] = Value((double)ctorLen);
    cls->properties["__non_writable_length"] = Value(true);
    cls->properties["__non_enum_length"] = Value(true);
  }

  // Class constructor inheritance: [[Prototype]] is superclass when present.
  if (cls->superClass) {
    cls->properties["__proto__"] = Value(cls->superClass);
  } else if (auto superCtorIt = cls->properties.find("__super_constructor__");
             superCtorIt != cls->properties.end()) {
    cls->properties["__proto__"] = superCtorIt->second;
  } else if (auto funcVal = env_->get("Function"); funcVal && funcVal->isFunction()) {
    auto funcCtor = std::get<GCPtr<Function>>(funcVal->data);
    auto protoIt = funcCtor->properties.find("prototype");
    if (protoIt != funcCtor->properties.end()) {
      cls->properties["__proto__"] = protoIt->second;
    }
  }

  auto runStaticBlock = [&](const std::vector<StmtPtr>& body) -> Task {
    auto prevEnvStatic = env_;
    bool prevStrict = strictMode_;
    env_ = cls->closure;
    env_ = env_->createChild();
    env_->define("__var_scope__", Value(true), true);
    env_->define("this", Value(cls));
    env_->define("__new_target__", Value(Undefined{}));
    auto superIt = cls->properties.find("__proto__");
    if (superIt != cls->properties.end()) {
      env_->define("__super__", superIt->second);
    }

    // Hoist var declarations to this static block's var-scope environment.
    hoistVarDeclarations(body);

    // Lexical environment for the block.
    env_ = env_->createChild();

    // Initialize TDZ for lexical declarations in this block (non-recursive)
    for (const auto& s : body) {
      if (auto* varDecl = std::get_if<VarDeclaration>(&s->node)) {
        if (varDecl->kind == VarDeclaration::Kind::Let ||
            varDecl->kind == VarDeclaration::Kind::Const) {
          for (const auto& declarator : varDecl->declarations) {
            std::vector<std::string> names;
            collectVarHoistNames(*declarator.pattern, names);
            for (const auto& name : names) {
              env_->defineTDZ(name);
            }
          }
        }
      } else if (auto* classDecl = std::get_if<ClassDeclaration>(&s->node)) {
        env_->defineTDZ(classDecl->id.name);
      }
    }

    strictMode_ = true;  // Class bodies are always strict.
    for (const auto& s : body) {
      auto t = evaluate(*s);
      LIGHTJS_RUN_TASK_VOID(t);
      if (flow_.type != ControlFlow::Type::None) {
        break;
      }
    }

    strictMode_ = prevStrict;
    env_ = prevEnvStatic;
    LIGHTJS_RETURN(Value(Undefined{}));
  };

  for (const auto& step : staticInitSteps) {
    if (step.kind == StaticInitStep::Kind::Field) {
      std::vector<Class::FieldInit> one;
      one.push_back(step.field);
      auto staticInitTask = initializeClassStaticFields(cls, one);
      LIGHTJS_RUN_TASK_VOID(staticInitTask);
      if (flow_.type != ControlFlow::Type::None) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      continue;
    }
    if (step.blockBody) {
      auto t = runStaticBlock(*step.blockBody);
      LIGHTJS_RUN_TASK_VOID(t);
      if (flow_.type != ControlFlow::Type::None) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
    }
  }

  if (hasLexicalNameBinding) {
    env_->defineLexical(expr.name, Value(cls), true);
  }

  LIGHTJS_RETURN(Value(cls));
}

// Helper for recursively binding destructuring patterns
Task Interpreter::bindDestructuringPattern(const Expression& pattern, Value value, bool isConst, bool useSet) {
  auto toPropertyKeyForPattern = [&](const Value& keyValue) -> std::string {
    Value prim = isObjectLike(keyValue) ? toPrimitiveValue(keyValue, true) : keyValue;
    if (flow_.type == ControlFlow::Type::Throw) return "";
    return valueToPropertyKey(prim);
  };

  if (auto* assign = std::get_if<AssignmentPattern>(&pattern.node)) {
    Value boundValue = value;
    if (boundValue.isUndefined()) {
      auto initTask = evaluate(*assign->right);
      Value initValue = Value(Undefined{});
      LIGHTJS_RUN_TASK(initTask, initValue);
      boundValue = initValue;
      // Named evaluation: set function/class name from binding identifier
      // Only when the initializer is an anonymous function definition (not comma expression)
      if (auto* leftId = std::get_if<Identifier>(&assign->left->node)) {
        bool isAnonymousFnDef = !std::get_if<SequenceExpr>(&assign->right->node);
        if (isAnonymousFnDef && boundValue.isFunction()) {
          auto fn = boundValue.getGC<Function>();
          auto nameIt = fn->properties.find("name");
          if (nameIt != fn->properties.end() && nameIt->second.isString() && nameIt->second.toString().empty()) {
            fn->properties["name"] = Value(leftId->name);
          } else if (nameIt == fn->properties.end()) {
            fn->properties["name"] = Value(leftId->name);
          }
          } else if (isAnonymousFnDef) {
          if (auto* cls = std::get_if<GCPtr<Class>>(&boundValue.data)) {
            auto nameIt = (*cls)->properties.find("name");
            bool shouldSet = nameIt == (*cls)->properties.end() ||
                             (nameIt->second.isString() && nameIt->second.toString().empty());
            if (shouldSet) {
              (*cls)->name = leftId->name;
              (*cls)->properties["name"] = Value(leftId->name);
              (*cls)->properties["__non_writable_name"] = Value(true);
              (*cls)->properties["__non_enum_name"] = Value(true);
            }
          }
        }
      }
    }
    { auto t = bindDestructuringPattern(*assign->left, boundValue, isConst, useSet); LIGHTJS_RUN_TASK_VOID(t); }
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  if (auto* id = std::get_if<Identifier>(&pattern.node)) {
    // Check TDZ before assignment (e.g., assigning to `let` variable before its declaration)
    if (useSet && env_->isTDZ(id->name)) {
      throwError(ErrorType::ReferenceError,
                 "Cannot access '" + id->name + "' before initialization");
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    // Simple identifier
    if (useSet) {
      if (!env_->set(id->name, value)) {
        // Check if this is a const binding being reassigned
        if (env_->isConst(id->name)) {
          throwError(ErrorType::TypeError, "Assignment to constant variable '" + id->name + "'");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (strictMode_) {
          // In strict mode, assignment to undeclared variable is a ReferenceError
          throwError(ErrorType::ReferenceError, id->name + " is not defined");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        // In non-strict mode, assignment to undeclared variable creates global
        env_->getRoot()->define(id->name, value);
      }
    } else {
      env_->defineLexical(id->name, value, isConst);
    }
  } else if (auto* member = std::get_if<MemberExpr>(&pattern.node)) {
    // MemberExpression target in assignment destructuring (e.g., x.y, obj['key'])
    auto objTask = evaluate(*member->object);
    Value objVal = Value(Undefined{});
    LIGHTJS_RUN_TASK(objTask, objVal);
    if (flow_.type == ControlFlow::Type::Throw) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    std::string prop;
    if (member->computed) {
      auto propTask = evaluate(*member->property);
      Value propVal = Value(Undefined{});
      LIGHTJS_RUN_TASK(propTask, propVal);
      prop = toPropertyKeyForPattern(propVal);
      if (flow_.type == ControlFlow::Type::Throw) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
    } else if (auto* propId = std::get_if<Identifier>(&member->property->node)) {
      prop = propId->name;
    }

    if (member->privateIdentifier) {
      if (!activePrivateOwnerClass_) {
        throwError(ErrorType::TypeError,
                   "Cannot read private member " + prop +
                       " from an object whose class did not declare it");
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      Value privateReceiver = objVal;
      int proxyDepth = 0;
      while (objVal.isProxy() && proxyDepth < 16) {
        auto proxyPtr = objVal.getGC<Proxy>();
        if (!proxyPtr->target) {
          throwError(ErrorType::TypeError,
                     "Cannot read private member " + prop +
                         " from an object whose class did not declare it");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        objVal = *proxyPtr->target;
        proxyDepth++;
      }
      if (objVal.isProxy()) {
        throwError(ErrorType::TypeError,
                   "Cannot read private member " + prop +
                       " from an object whose class did not declare it");
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      PrivateNameOwner resolved = resolvePrivateNameOwnerClass(activePrivateOwnerClass_, prop);
      if (!resolved.owner || resolved.kind == PrivateNameKind::None) {
        throwError(ErrorType::TypeError,
                   "Cannot read private member " + prop +
                       " from an object whose class did not declare it");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      std::string mangledName = privateStorageKey(resolved.owner, prop);

      if (resolved.kind == PrivateNameKind::Static) {
        if (!objVal.isClass() || objVal.getGC<Class>().get() != resolved.owner.get()) {
          throwError(ErrorType::TypeError,
                     "Cannot read private member " + prop +
                         " from an object whose class did not declare it");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        auto clsPtr = objVal.getGC<Class>();
        auto getterIt = clsPtr->properties.find("__get_" + mangledName);
        auto setterIt = clsPtr->properties.find("__set_" + mangledName);
        bool hasGetter = getterIt != clsPtr->properties.end() && getterIt->second.isFunction();
        bool hasSetter = setterIt != clsPtr->properties.end() && setterIt->second.isFunction();
        bool isMethod = clsPtr->staticMethods.count(mangledName) > 0;
        bool hasField = clsPtr->properties.count(mangledName) > 0;
        if (isMethod) {
          throwError(ErrorType::TypeError, "Cannot assign to private method " + prop);
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (hasGetter && !hasSetter) {
          throwError(ErrorType::TypeError, "Cannot set property " + prop + " which has only a getter");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (hasSetter) {
          callFunction(setterIt->second, {value}, privateReceiver);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
        } else if (hasField) {
          clsPtr->properties[mangledName] = value;
        } else {
          throwError(ErrorType::TypeError,
                     "Cannot write private member " + prop +
                         " to an object whose class did not declare it");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      if (!isObjectLike(objVal)) {
        throwError(ErrorType::TypeError,
                   "Cannot read private member " + prop +
                       " from an object whose class did not declare it");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      GCPtr<Class> targetClass = getConstructorClassForPrivateAccess(objVal);
      if (!targetClass || !isOwnerInClassChain(targetClass, resolved.owner)) {
        throwError(ErrorType::TypeError,
                   "Cannot read private member " + prop +
                       " from an object whose class did not declare it");
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      auto getterIt = resolved.owner->getters.find(prop);
      auto setterIt = resolved.owner->setters.find(prop);
      bool hasGetter = getterIt != resolved.owner->getters.end();
      bool hasSetter = setterIt != resolved.owner->setters.end();
      bool isMethod = resolved.owner->methods.count(prop) > 0;
      if (isMethod) {
        throwError(ErrorType::TypeError, "Cannot assign to private method " + prop);
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (hasGetter && !hasSetter) {
        throwError(ErrorType::TypeError, "Cannot set property " + prop + " which has only a getter");
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      auto* storage = getPropertyStorageForPrivateAccess(objVal);
      bool hasField = storage && storage->find(mangledName) != storage->end();
      if (hasSetter) {
        invokeFunction(setterIt->second, {value}, privateReceiver);
        if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      } else if (hasField) {
        (*storage)[mangledName] = value;
      } else {
        throwError(ErrorType::TypeError,
                   "Cannot write private member " + prop +
                       " to an object whose class did not declare it");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    auto setMemberLike = [&](auto&& self,
                             const Value& base,
                             const Value& receiver,
                             const std::string& key,
                             const Value& assigned) -> bool {
      if (base.isObject()) {
        auto obj = base.getGC<Object>();
        auto setterIt = obj->properties.find("__set_" + key);
        if (setterIt != obj->properties.end() && setterIt->second.isFunction()) {
          callFunction(setterIt->second, {assigned}, receiver);
          return !hasError();
        }
        auto getterIt = obj->properties.find("__get_" + key);
        if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
          return false;
        }
        if (obj->properties.count(key)) {
          if (receiver.isObject()) {
            receiver.getGC<Object>()->properties[key] = assigned;
            return true;
          }
          if (receiver.isArray()) {
            receiver.getGC<Array>()->properties[key] = assigned;
            return true;
          }
          return false;
        }
        auto protoIt = obj->properties.find("__proto__");
        if (protoIt != obj->properties.end() &&
            (protoIt->second.isObject() || protoIt->second.isArray())) {
          return self(self, protoIt->second, receiver, key, assigned);
        }
        if (receiver.isObject()) {
          receiver.getGC<Object>()->properties[key] = assigned;
          return true;
        }
        if (receiver.isArray()) {
          receiver.getGC<Array>()->properties[key] = assigned;
          return true;
        }
        return false;
      }

      if (base.isArray()) {
        auto arr = base.getGC<Array>();
        auto setterIt = arr->properties.find("__set_" + key);
        if (setterIt != arr->properties.end() && setterIt->second.isFunction()) {
          callFunction(setterIt->second, {assigned}, receiver);
          return !hasError();
        }
        auto getterIt = arr->properties.find("__get_" + key);
        if (getterIt != arr->properties.end() && getterIt->second.isFunction()) {
          return false;
        }
        size_t index = 0;
        if (parseArrayIndex(key, index)) {
          if (index < arr->elements.size()) {
            if (receiver.isArray()) {
              auto recvArr = receiver.getGC<Array>();
              if (index >= recvArr->elements.size()) {
                recvArr->elements.resize(index + 1, Value(Undefined{}));
              }
              recvArr->elements[index] = assigned;
              return true;
            }
            return false;
          }
        } else if (arr->properties.count(key)) {
          if (receiver.isArray()) {
            receiver.getGC<Array>()->properties[key] = assigned;
            return true;
          }
          if (receiver.isObject()) {
            receiver.getGC<Object>()->properties[key] = assigned;
            return true;
          }
          return false;
        }
        auto protoIt = arr->properties.find("__proto__");
        if (protoIt != arr->properties.end() &&
            (protoIt->second.isObject() || protoIt->second.isArray())) {
          return self(self, protoIt->second, receiver, key, assigned);
        }
        if (receiver.isArray()) {
          auto recvArr = receiver.getGC<Array>();
          if (parseArrayIndex(key, index)) {
            if (index >= recvArr->elements.size()) {
              recvArr->elements.resize(index + 1, Value(Undefined{}));
            }
            recvArr->elements[index] = assigned;
          } else {
            recvArr->properties[key] = assigned;
          }
          return true;
        }
        if (receiver.isObject()) {
          receiver.getGC<Object>()->properties[key] = assigned;
          return true;
        }
      }
      return false;
    };

    setMemberLike(setMemberLike, objVal, objVal, prop, value);
    if (hasError()) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }
  } else if (auto* arrayPat = std::get_if<ArrayPattern>(&pattern.node)) {
    // Array destructuring - null/undefined are not iterable
    if (value.isNull() || value.isUndefined()) {
      throwError(ErrorType::TypeError, "Cannot destructure " + value.toString() + " as it is not iterable");
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    GCPtr<Array> arr;
    if (value.isArray()) {
      // Array destructuring must respect Array.prototype[Symbol.iterator] overrides.
      // Fast path: use direct indexing only when the builtin iterator is installed.
      auto arrayProtoOpt = env_->get("__array_prototype__");
      if (!arrayProtoOpt.has_value() || !arrayProtoOpt->isObject()) {
        // Fallback to the legacy behavior when the hidden prototype is missing.
        arr = value.getGC<Array>();
      } else {
        auto protoObj = std::get<GCPtr<Object>>(arrayProtoOpt->data);
        const auto& iterKey = WellKnownSymbols::iteratorKey();
        auto iterIt = protoObj->properties.find(iterKey);
        if (iterIt == protoObj->properties.end()) {
          throwError(ErrorType::TypeError, value.toString() + " is not iterable");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        Value iteratorMethod = iterIt->second;
        if (!iteratorMethod.isFunction()) {
          throwError(ErrorType::TypeError, "Symbol.iterator is not a function");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        bool isBuiltinIterator = false;
        if (iteratorMethod.isFunction()) {
          auto fn = iteratorMethod.getGC<Function>();
          auto builtinIt = fn->properties.find("__builtin_array_iterator__");
          isBuiltinIterator = builtinIt != fn->properties.end() &&
                              builtinIt->second.isBool() &&
                              builtinIt->second.toBool();
        }
        if (isBuiltinIterator) {
          arr = value.getGC<Array>();
        } else {
          // Use the overridden @@iterator to collect elements.
          Value iterResult = callFunction(iteratorMethod, {}, value);
          if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));

          IteratorRecord iterRec;
          if (iterResult.isGenerator()) {
            iterRec.kind = IteratorRecord::Kind::Generator;
            iterRec.generator = iterResult.getGC<Generator>();
          } else if (iterResult.isObject()) {
            auto iterObj = iterResult.getGC<Object>();
            // Cache next method (with getter support).
            Value nextMethod;
            auto getterIt = iterObj->properties.find("__get_next");
            if (getterIt != iterObj->properties.end() && getterIt->second.isFunction()) {
              nextMethod = callFunction(getterIt->second, {}, iterResult);
              if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
            } else {
              auto nextIt = iterObj->properties.find("next");
              if (nextIt != iterObj->properties.end()) nextMethod = nextIt->second;
            }
            if (!nextMethod.isFunction()) {
              throwError(ErrorType::TypeError, "Iterator next is not a function");
              LIGHTJS_RETURN(Value(Undefined{}));
            }
            iterRec.kind = IteratorRecord::Kind::IteratorObject;
            iterRec.iteratorValue = iterResult;
            iterRec.nextMethod = nextMethod;
          } else {
            throwError(ErrorType::TypeError, "Iterator result is not an object");
            LIGHTJS_RETURN(Value(Undefined{}));
          }

          arr = GarbageCollector::makeGC<Array>();
          GarbageCollector::instance().reportAllocation(sizeof(Array));
          size_t needed = arrayPat->elements.size();
          bool hasRest = (arrayPat->rest != nullptr);
          for (size_t i = 0; i < needed || hasRest; ++i) {
            Value stepResult = iteratorNext(iterRec);
            if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
            if (!stepResult.isObject()) {
              throwError(ErrorType::TypeError, "Iterator result is not an object");
              LIGHTJS_RETURN(Value(Undefined{}));
            }
            auto stepObj = stepResult.getGC<Object>();
            bool done = false;
            auto doneGetterIt = stepObj->properties.find("__get_done");
            if (doneGetterIt != stepObj->properties.end() && doneGetterIt->second.isFunction()) {
              Value doneVal = callFunction(doneGetterIt->second, {}, stepResult);
              if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
              done = doneVal.toBool();
            } else {
              auto doneIt = stepObj->properties.find("done");
              done = (doneIt != stepObj->properties.end() && doneIt->second.toBool());
            }
            if (done) break;

            Value elemVal;
            auto valGetterIt = stepObj->properties.find("__get_value");
            if (valGetterIt != stepObj->properties.end() && valGetterIt->second.isFunction()) {
              elemVal = callFunction(valGetterIt->second, {}, stepResult);
              if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
            } else {
              auto valIt = stepObj->properties.find("value");
              elemVal = (valIt != stepObj->properties.end()) ? valIt->second : Value(Undefined{});
            }
            arr->elements.push_back(elemVal);
            if (i >= needed && !hasRest) break;
          }
        }
      }
    } else if (value.isString()) {
      // Strings are iterable - convert to array of chars
      auto str = std::get<std::string>(value.data);
      arr = GarbageCollector::makeGC<Array>();
      for (size_t i = 0; i < str.size(); ++i) {
        arr->elements.push_back(Value(std::string(1, str[i])));
      }
    } else if (value.isGenerator()) {
      // Generators are iterable - lazily iterate via next()
      auto gen = value.getGC<Generator>();
      arr = GarbageCollector::makeGC<Array>();
      IteratorRecord genRecord;
      genRecord.kind = IteratorRecord::Kind::Generator;
      genRecord.generator = gen;
      size_t needed = arrayPat->elements.size();
      bool hasRest = (arrayPat->rest != nullptr);
      bool genExhausted = false;
      // Only pull as many elements as the pattern requires
      for (size_t i = 0; i < needed || hasRest; ++i) {
        Value stepResult = iteratorNext(genRecord);
        if (!stepResult.isObject()) { genExhausted = true; break; }
        auto stepObj = stepResult.getGC<Object>();
        auto doneIt2 = stepObj->properties.find("done");
        if (doneIt2 != stepObj->properties.end() && doneIt2->second.toBool()) { genExhausted = true; break; }
        auto valIt = stepObj->properties.find("value");
        arr->elements.push_back(valIt != stepObj->properties.end() ? valIt->second : Value(Undefined{}));
        if (i >= needed && !hasRest) break;
      }
      // Per spec: IteratorClose if iterator not exhausted after destructuring.
      // Only when elements were consumed ([] = empty pattern doesn't get iterator per spec).
      if (needed > 0 && !genExhausted && !hasRest &&
          gen->state != GeneratorState::Completed) {
        iteratorClose(genRecord);
      }
    } else if (value.isTypedArray()) {
      auto iterOpt = getIterator(value);
      if (!iterOpt.has_value()) {
        throwError(ErrorType::TypeError, value.toString() + " is not iterable");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      IteratorRecord iterRec = *iterOpt;
      arr = GarbageCollector::makeGC<Array>();
      size_t needed = arrayPat->elements.size();
      bool hasRest = (arrayPat->rest != nullptr);
      bool exhausted = false;
      for (size_t i = 0; i < needed || hasRest; ++i) {
        Value stepResult = iteratorNext(iterRec);
        if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
        if (!stepResult.isObject()) { exhausted = true; break; }
        auto stepObj = stepResult.getGC<Object>();
        auto doneIt2 = stepObj->properties.find("done");
        if (doneIt2 != stepObj->properties.end() && doneIt2->second.toBool()) {
          exhausted = true;
          break;
        }
        auto valIt = stepObj->properties.find("value");
        arr->elements.push_back(valIt != stepObj->properties.end() ? valIt->second : Value(Undefined{}));
        if (i >= needed && !hasRest) break;
      }
      if (needed > 0 && !hasRest && !exhausted) {
        iteratorClose(iterRec);
        if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
      }
    } else if (value.isObject()) {
      // Check for Symbol.iterator on objects
      auto obj = value.getGC<Object>();
      const auto& iteratorKey = WellKnownSymbols::iteratorKey();
      auto it = obj->properties.find(iteratorKey);
      if (it != obj->properties.end() && it->second.isFunction()) {
        Value iterResult = callFunction(it->second, {}, value);
        if (iterResult.isObject()) {
          auto iterObj = iterResult.getGC<Object>();
          auto nextIt = iterObj->properties.find("next");
          {
            bool hasDirectNext = (nextIt != iterObj->properties.end() && nextIt->second.isFunction());
            // Single-pass lazy destructuring with IteratorClose protocol
            IteratorRecord iterRec;
            iterRec.kind = IteratorRecord::Kind::IteratorObject;
            iterRec.iteratorValue = iterResult;
            auto [foundNext, nextMethod] = getPropertyForPrimitive(iterResult, "next");
            if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
            if (foundNext) iterRec.nextMethod = nextMethod;
            bool iteratorDone = false;

            // Helper lambda: advance iterator, return value or Undefined
            // If next() throws, marks iterator done and returns (no IteratorClose per spec)
            auto advanceIterator = [&]() -> Value {
              if (iteratorDone) return Value(Undefined{});
              Value stepResult = hasDirectNext
                               ? callFunction(nextIt->second, {}, iterResult)
                               : iteratorNext(iterRec);
              if (flow_.type == ControlFlow::Type::Throw) {
                iteratorDone = true;
                return Value(Undefined{});
              }
              if (!stepResult.isObject()) { iteratorDone = true; return Value(Undefined{}); }
              auto stepObj = stepResult.getGC<Object>();
              // Check done (with getter support)
              bool isDone = false;
              auto doneGetterIt2 = stepObj->properties.find("__get_done");
              if (doneGetterIt2 != stepObj->properties.end() && doneGetterIt2->second.isFunction()) {
                Value doneVal = callFunction(doneGetterIt2->second, {}, stepResult);
                if (flow_.type == ControlFlow::Type::Throw) { iteratorDone = true; return Value(Undefined{}); }
                isDone = doneVal.toBool();
              } else {
                auto doneIt2 = stepObj->properties.find("done");
                isDone = (doneIt2 != stepObj->properties.end() && doneIt2->second.toBool());
              }
              if (isDone) { iteratorDone = true; return Value(Undefined{}); }
              // Get value (with getter support)
              Value elemVal;
              auto valGetterIt2 = stepObj->properties.find("__get_value");
              if (valGetterIt2 != stepObj->properties.end() && valGetterIt2->second.isFunction()) {
                elemVal = callFunction(valGetterIt2->second, {}, stepResult);
                if (flow_.type == ControlFlow::Type::Throw) { iteratorDone = true; return Value(Undefined{}); }
              } else {
                auto valIt2 = stepObj->properties.find("value");
                elemVal = (valIt2 != stepObj->properties.end()) ? valIt2->second : Value(Undefined{});
              }
              return elemVal;
            };

            // Helper lambda: close iterator preserving original throw (spec 7.4.6 step 7)
            auto closeWithThrow = [&]() {
              if (iteratorDone) return;
              auto savedFlow = flow_;
              flow_.type = ControlFlow::Type::None;
              iteratorClose(iterRec);
              flow_ = savedFlow;  // Original throw always wins
            };
            auto closeOnAbrupt = [&]() {
              if (flow_.type == ControlFlow::Type::Throw) {
                closeWithThrow();
                return;
              }
              if (flow_.type == ControlFlow::Type::None ||
                  flow_.type == ControlFlow::Type::Yield) {
                return;
              }
              auto savedFlow = flow_;
              flow_.type = ControlFlow::Type::None;
              iteratorClose(iterRec);
              if (flow_.type == ControlFlow::Type::None) {
                flow_ = savedFlow;
              }
            };

            // Single-pass element processing
            for (size_t i = 0; i < arrayPat->elements.size(); ++i) {
              if (!arrayPat->elements[i]) {
                // Hole/elision: advance iterator to consume slot
                advanceIterator();
                if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
                continue;
              }

              auto& elem = *arrayPat->elements[i];

              // Unwrap AssignmentPattern to get target and default
              const Expression* target = &elem;
              const Expression* defaultExpr = nullptr;
              if (auto* assignPat = std::get_if<AssignmentPattern>(&elem.node)) {
                target = assignPat->left.get();
                defaultExpr = assignPat->right.get();
              }

              if (auto* memberTarget = std::get_if<MemberExpr>(&target->node)) {
                // MemberExpr target: evaluate target reference FIRST (spec order)
                auto objTask = evaluate(*memberTarget->object);
                Value objVal = Value(Undefined{});
                LIGHTJS_RUN_TASK(objTask, objVal);
                if (flow_.type != ControlFlow::Type::None && flow_.type != ControlFlow::Type::Yield) { closeOnAbrupt(); LIGHTJS_RETURN(Value(Undefined{})); }

                std::string prop;
                Value propKeyVal(Undefined{});
                if (memberTarget->computed) {
                  auto propTask = evaluate(*memberTarget->property);
                  Value propVal = Value(Undefined{});
                  LIGHTJS_RUN_TASK(propTask, propVal);
                  if (flow_.type != ControlFlow::Type::None && flow_.type != ControlFlow::Type::Yield) { closeOnAbrupt(); LIGHTJS_RETURN(Value(Undefined{})); }
                  propKeyVal = propVal;
                } else if (auto* propId = std::get_if<Identifier>(&memberTarget->property->node)) {
                  prop = propId->name;
                }

                // THEN advance iterator to get value
                Value elemValue = advanceIterator();
                if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{})); // next() threw, no close

                // Handle default value if undefined
                if (elemValue.isUndefined() && defaultExpr) {
                  auto defaultTask = evaluate(*defaultExpr);
                  LIGHTJS_RUN_TASK(defaultTask, elemValue);
                  if (flow_.type != ControlFlow::Type::None && flow_.type != ControlFlow::Type::Yield) { closeOnAbrupt(); LIGHTJS_RETURN(Value(Undefined{})); }
                }

                // Bind value to pre-evaluated reference
                if (memberTarget->computed) {
                  prop = toPropertyKeyForPattern(propKeyVal);
                  if (flow_.type == ControlFlow::Type::Throw) { closeWithThrow(); LIGHTJS_RETURN(Value(Undefined{})); }
                }
                if (objVal.isObject()) {
                  auto obj2 = objVal.getGC<Object>();
                  auto setterIt = obj2->properties.find("__set_" + prop);
                  if (setterIt != obj2->properties.end() && setterIt->second.isFunction()) {
                    callFunction(setterIt->second, {elemValue}, objVal);
                  } else {
                    obj2->properties[prop] = elemValue;
                  }
                } else if (objVal.isArray()) {
                  auto arrRef = objVal.getGC<Array>();
                  if (memberTarget->computed) {
                    size_t idx = static_cast<size_t>(std::stod(prop));
                    if (idx < arrRef->elements.size()) {
                      arrRef->elements[idx] = elemValue;
                    }
                  }
                }
                if (flow_.type != ControlFlow::Type::None && flow_.type != ControlFlow::Type::Yield) { closeOnAbrupt(); LIGHTJS_RETURN(Value(Undefined{})); }
              } else {
                // Identifier or nested pattern: advance iterator first, then bind
                Value elemValue = advanceIterator();
                if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{})); // next() threw, no close

                // Handle default value if undefined
                if (elemValue.isUndefined() && defaultExpr) {
                  auto defaultTask = evaluate(*defaultExpr);
                  LIGHTJS_RUN_TASK(defaultTask, elemValue);
                   if (flow_.type != ControlFlow::Type::None && flow_.type != ControlFlow::Type::Yield) { closeOnAbrupt(); LIGHTJS_RETURN(Value(Undefined{})); }
                }

                { auto t = bindDestructuringPattern(*target, elemValue, isConst, useSet); LIGHTJS_RUN_TASK_VOID(t); }
                if (flow_.type != ControlFlow::Type::None && flow_.type != ControlFlow::Type::Yield) { closeOnAbrupt(); LIGHTJS_RETURN(Value(Undefined{})); }
              }
            }

            // Handle rest element
            if (arrayPat->rest) {
              auto& restTarget = *arrayPat->rest;

              // Unwrap AssignmentPattern for rest (rare but possible)
              const Expression* restActualTarget = &restTarget;
              if (auto* assignPat = std::get_if<AssignmentPattern>(&restTarget.node)) {
                restActualTarget = assignPat->left.get();
              }

              if (auto* memberTarget = std::get_if<MemberExpr>(&restActualTarget->node)) {
                // MemberExpr rest target: evaluate target reference FIRST
                auto objTask = evaluate(*memberTarget->object);
                Value objVal = Value(Undefined{});
                LIGHTJS_RUN_TASK(objTask, objVal);
                if (flow_.type != ControlFlow::Type::None && flow_.type != ControlFlow::Type::Yield) { closeOnAbrupt(); LIGHTJS_RETURN(Value(Undefined{})); }

                std::string prop;
                Value propKeyVal(Undefined{});
                if (memberTarget->computed) {
                  auto propTask = evaluate(*memberTarget->property);
                  Value propVal = Value(Undefined{});
                  LIGHTJS_RUN_TASK(propTask, propVal);
                  if (flow_.type != ControlFlow::Type::None && flow_.type != ControlFlow::Type::Yield) { closeOnAbrupt(); LIGHTJS_RETURN(Value(Undefined{})); }
                  propKeyVal = propVal;
                } else if (auto* propId = std::get_if<Identifier>(&memberTarget->property->node)) {
                  prop = propId->name;
                }

                // Collect remaining values
                auto restArr = GarbageCollector::makeGC<Array>();
                setArrayPrototype(restArr, env_.get());
                while (!iteratorDone) {
                  Value v = advanceIterator();
                  if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
                  if (iteratorDone) break;
                  restArr->elements.push_back(v);
                }

                // Bind to pre-evaluated reference
                if (memberTarget->computed) {
                  prop = toPropertyKeyForPattern(propKeyVal);
                  if (flow_.type == ControlFlow::Type::Throw) { closeWithThrow(); LIGHTJS_RETURN(Value(Undefined{})); }
                }
                if (objVal.isObject()) {
                  auto obj2 = objVal.getGC<Object>();
                  auto setterIt = obj2->properties.find("__set_" + prop);
                  if (setterIt != obj2->properties.end() && setterIt->second.isFunction()) {
                    callFunction(setterIt->second, {Value(restArr)}, objVal);
                  } else {
                    obj2->properties[prop] = Value(restArr);
                  }
                } else if (objVal.isArray()) {
                  auto arrRef = objVal.getGC<Array>();
                  if (memberTarget->computed) {
                    size_t idx = static_cast<size_t>(std::stod(prop));
                    if (idx < arrRef->elements.size()) {
                      arrRef->elements[idx] = Value(restArr);
                    }
                  }
                }
                if (flow_.type != ControlFlow::Type::None && flow_.type != ControlFlow::Type::Yield) { closeOnAbrupt(); LIGHTJS_RETURN(Value(Undefined{})); }
              } else {
                // Identifier or nested pattern rest: collect remaining, then bind
                auto restArr = GarbageCollector::makeGC<Array>();
                setArrayPrototype(restArr, env_.get());
                while (!iteratorDone) {
                  Value v = advanceIterator();
                  if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
                  if (iteratorDone) break;
                  restArr->elements.push_back(v);
                }
                { auto t = bindDestructuringPattern(*restActualTarget, Value(restArr), isConst, useSet); LIGHTJS_RUN_TASK_VOID(t); }
                if (flow_.type != ControlFlow::Type::None && flow_.type != ControlFlow::Type::Yield) { closeOnAbrupt(); LIGHTJS_RETURN(Value(Undefined{})); }
              }
            }

            // Final IteratorClose if iterator not exhausted
            if (!iteratorDone) {
              iteratorClose(iterRec);
              if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
            }
            // Done - skip the default binding loop below
            LIGHTJS_RETURN(Value(Undefined{}));
          }
        } else {
          arr = GarbageCollector::makeGC<Array>();
        }
      } else {
        throwError(ErrorType::TypeError, value.toString() + " is not iterable");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
    } else {
      // Primitives (bool, number, bigint) are not iterable
      throwError(ErrorType::TypeError, value.toString() + " is not iterable");
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    for (size_t i = 0; i < arrayPat->elements.size(); ++i) {
      if (!arrayPat->elements[i]) continue;  // Skip holes

      Value elemValue = (i < arr->elements.size()) ? arr->elements[i] : Value(Undefined{});

      // Recursively bind (handles nested patterns)
      { auto t = bindDestructuringPattern(*arrayPat->elements[i], elemValue, isConst, useSet); LIGHTJS_RUN_TASK_VOID(t); }
      if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
    }

    // Handle rest element
    if (arrayPat->rest) {
      auto restArr = GarbageCollector::makeGC<Array>();
      setArrayPrototype(restArr, env_.get());
      for (size_t i = arrayPat->elements.size(); i < arr->elements.size(); ++i) {
        restArr->elements.push_back(arr->elements[i]);
      }
      { auto t = bindDestructuringPattern(*arrayPat->rest, Value(restArr), isConst, useSet); LIGHTJS_RUN_TASK_VOID(t); }
    }
  } else if (auto* objPat = std::get_if<ObjectPattern>(&pattern.node)) {
    // Object destructuring - null/undefined cannot be destructured
    if (value.isNull() || value.isUndefined()) {
      throwError(ErrorType::TypeError, "Cannot destructure " + value.toString() + " as it is not an object");
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    GCPtr<Object> obj;
    if (value.isObject()) {
      obj = value.getGC<Object>();
    } else if (value.isArray()) {
      // Convert array to object-like representation for destructuring
      auto arr = value.getGC<Array>();
      obj = GarbageCollector::makeGC<Object>();
      for (size_t i = 0; i < arr->elements.size(); ++i) {
        obj->properties[std::to_string(i)] = arr->elements[i];
      }
      obj->properties["length"] = Value(static_cast<double>(arr->elements.size()));
    } else if (value.isString()) {
      // Convert string to object-like representation for destructuring
      auto str = std::get<std::string>(value.data);
      obj = GarbageCollector::makeGC<Object>();
      for (size_t i = 0; i < str.size(); ++i) {
        obj->properties[std::to_string(i)] = Value(std::string(1, str[i]));
      }
      obj->properties["length"] = Value(static_cast<double>(str.size()));
    } else {
      // Create empty object for other primitive values
      obj = GarbageCollector::makeGC<Object>();
    }

    std::unordered_set<std::string> extractedKeys;

    for (const auto& prop : objPat->properties) {
      std::string keyName;
      if (prop.computed) {
        // Computed property key: {[expr]: pattern}
        auto keyTask = evaluate(*prop.key);
        Value keyVal;
        LIGHTJS_RUN_TASK(keyTask, keyVal);
        keyName = toPropertyKeyForPattern(keyVal);
        if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
      } else if (auto* keyId = std::get_if<Identifier>(&prop.key->node)) {
        keyName = keyId->name;
      } else if (auto* keyStr = std::get_if<StringLiteral>(&prop.key->node)) {
        keyName = keyStr->value;
      } else if (auto* keyNum = std::get_if<NumberLiteral>(&prop.key->node)) {
        keyName = numberToPropertyKey(keyNum->value);
      } else if (auto* keyBigInt = std::get_if<BigIntLiteral>(&prop.key->node)) {
        keyName = bigint::toString(keyBigInt->value);
      } else {
        continue;
      }

      extractedKeys.insert(keyName);

      const Expression* targetExpr = prop.value.get();
      const Expression* defaultExpr = nullptr;
      if (auto* assignPat = std::get_if<AssignmentPattern>(&prop.value->node)) {
        targetExpr = assignPat->left.get();
        defaultExpr = assignPat->right.get();
      }

      Value targetObjVal(Undefined{});
      std::string targetProp;
      Value targetPropKey(Undefined{});
      bool hasMemberTarget = false;
      bool memberComputed = false;
      const MemberExpr* memberTargetPtr = nullptr;
      if (targetExpr) {
        if (auto* memberTarget = std::get_if<MemberExpr>(&targetExpr->node)) {
          hasMemberTarget = true;
          memberComputed = memberTarget->computed;
          memberTargetPtr = memberTarget;
          auto objTask = evaluate(*memberTarget->object);
          LIGHTJS_RUN_TASK(objTask, targetObjVal);
          if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
          if (memberTarget->computed) {
            auto propTask = evaluate(*memberTarget->property);
            LIGHTJS_RUN_TASK(propTask, targetPropKey);
            if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
          } else if (auto* propId = std::get_if<Identifier>(&memberTarget->property->node)) {
            targetProp = propId->name;
          }
        }
      }

      Value propValue;
      auto [foundProp, fetchedPropValue] = getPropertyForPrimitive(value, keyName);
      if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
      if (foundProp) {
        propValue = fetchedPropValue;
      }
      if (hasMemberTarget && propValue.isUndefined() && defaultExpr) {
        auto defaultTask = evaluate(*defaultExpr);
        LIGHTJS_RUN_TASK(defaultTask, propValue);
        if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
      }

      if (hasMemberTarget) {
        if (memberComputed) {
          targetProp = toPropertyKeyForPattern(targetPropKey);
          if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (memberTargetPtr && memberTargetPtr->privateIdentifier) {
          if (!activePrivateOwnerClass_) {
            throwError(ErrorType::TypeError,
                       "Cannot read private member " + targetProp +
                           " from an object whose class did not declare it");
            LIGHTJS_RETURN(Value(Undefined{}));
          }

          Value privateReceiver = targetObjVal;
          int proxyDepth = 0;
          while (targetObjVal.isProxy() && proxyDepth < 16) {
            auto proxyPtr = targetObjVal.getGC<Proxy>();
            if (!proxyPtr->target) {
              throwError(ErrorType::TypeError,
                         "Cannot read private member " + targetProp +
                             " from an object whose class did not declare it");
              LIGHTJS_RETURN(Value(Undefined{}));
            }
            targetObjVal = *proxyPtr->target;
            proxyDepth++;
          }
          if (targetObjVal.isProxy()) {
            throwError(ErrorType::TypeError,
                       "Cannot read private member " + targetProp +
                           " from an object whose class did not declare it");
            LIGHTJS_RETURN(Value(Undefined{}));
          }

          PrivateNameOwner resolved =
              resolvePrivateNameOwnerClass(activePrivateOwnerClass_, targetProp);
          if (!resolved.owner || resolved.kind == PrivateNameKind::None) {
            throwError(ErrorType::TypeError,
                       "Cannot read private member " + targetProp +
                           " from an object whose class did not declare it");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          std::string mangledName = privateStorageKey(resolved.owner, targetProp);

          if (resolved.kind == PrivateNameKind::Static) {
            if (!targetObjVal.isClass() ||
                targetObjVal.getGC<Class>().get() != resolved.owner.get()) {
              throwError(ErrorType::TypeError,
                         "Cannot read private member " + targetProp +
                             " from an object whose class did not declare it");
              LIGHTJS_RETURN(Value(Undefined{}));
            }
            auto clsPtr = targetObjVal.getGC<Class>();
            auto getterIt = clsPtr->properties.find("__get_" + mangledName);
            auto setterIt = clsPtr->properties.find("__set_" + mangledName);
            bool hasGetter = getterIt != clsPtr->properties.end() && getterIt->second.isFunction();
            bool hasSetter = setterIt != clsPtr->properties.end() && setterIt->second.isFunction();
            bool isMethod = clsPtr->staticMethods.count(mangledName) > 0;
            bool hasField = clsPtr->properties.count(mangledName) > 0;
            if (isMethod) {
              throwError(ErrorType::TypeError, "Cannot assign to private method " + targetProp);
              LIGHTJS_RETURN(Value(Undefined{}));
            }
            if (hasGetter && !hasSetter) {
              throwError(ErrorType::TypeError, "Cannot set property " + targetProp + " which has only a getter");
              LIGHTJS_RETURN(Value(Undefined{}));
            }
            if (hasSetter) {
              callFunction(setterIt->second, {propValue}, privateReceiver);
              if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
            } else if (hasField) {
              clsPtr->properties[mangledName] = propValue;
            } else {
              throwError(ErrorType::TypeError,
                         "Cannot write private member " + targetProp +
                             " to an object whose class did not declare it");
              LIGHTJS_RETURN(Value(Undefined{}));
            }
          } else {
            if (!isObjectLike(targetObjVal)) {
              throwError(ErrorType::TypeError,
                         "Cannot read private member " + targetProp +
                             " from an object whose class did not declare it");
              LIGHTJS_RETURN(Value(Undefined{}));
            }
            GCPtr<Class> targetClass = getConstructorClassForPrivateAccess(targetObjVal);
            if (!targetClass || !isOwnerInClassChain(targetClass, resolved.owner)) {
              throwError(ErrorType::TypeError,
                         "Cannot read private member " + targetProp +
                             " from an object whose class did not declare it");
              LIGHTJS_RETURN(Value(Undefined{}));
            }
            auto getterIt = resolved.owner->getters.find(targetProp);
            auto setterIt = resolved.owner->setters.find(targetProp);
            bool hasGetter = getterIt != resolved.owner->getters.end();
            bool hasSetter = setterIt != resolved.owner->setters.end();
            bool isMethod = resolved.owner->methods.count(targetProp) > 0;
            if (isMethod) {
              throwError(ErrorType::TypeError, "Cannot assign to private method " + targetProp);
              LIGHTJS_RETURN(Value(Undefined{}));
            }
            if (hasGetter && !hasSetter) {
              throwError(ErrorType::TypeError, "Cannot set property " + targetProp + " which has only a getter");
              LIGHTJS_RETURN(Value(Undefined{}));
            }
            auto* storage = getPropertyStorageForPrivateAccess(targetObjVal);
            bool hasField = storage && storage->find(mangledName) != storage->end();
            if (hasSetter) {
              invokeFunction(setterIt->second, {propValue}, privateReceiver);
              if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
            } else if (hasField) {
              (*storage)[mangledName] = propValue;
            } else {
              throwError(ErrorType::TypeError,
                         "Cannot write private member " + targetProp +
                             " to an object whose class did not declare it");
              LIGHTJS_RETURN(Value(Undefined{}));
            }
          }
        } else if (targetObjVal.isObject()) {
          auto targetObj = targetObjVal.getGC<Object>();
          auto setterIt = targetObj->properties.find("__set_" + targetProp);
          if (setterIt != targetObj->properties.end() && setterIt->second.isFunction()) {
            callFunction(setterIt->second, {propValue}, targetObjVal);
          } else {
            targetObj->properties[targetProp] = propValue;
          }
        } else if (targetObjVal.isArray() && memberComputed) {
          auto targetArr = targetObjVal.getGC<Array>();
          size_t idx = static_cast<size_t>(std::stod(targetProp));
          if (idx < targetArr->elements.size()) {
            targetArr->elements[idx] = propValue;
          }
        }
        if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
      } else {
        { auto t = bindDestructuringPattern(*prop.value, propValue, isConst, useSet); LIGHTJS_RUN_TASK_VOID(t); }
        if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
      }
    }

    // Handle rest properties
    if (objPat->rest) {
      auto restObj = GarbageCollector::makeGC<Object>();
      if (value.isProxy()) {
        auto proxyPtr = value.getGC<Proxy>();
        if (proxyPtr->target && proxyPtr->target->isObject()) {
          auto targetObj = proxyPtr->target->getGC<Object>();
          GCPtr<Object> handlerObj = nullptr;
          if (proxyPtr->handler && proxyPtr->handler->isObject()) {
            handlerObj = proxyPtr->handler->getGC<Object>();
          }

          std::vector<std::string> keys;
          if (handlerObj) {
            auto ownKeysIt = handlerObj->properties.find("ownKeys");
            if (ownKeysIt != handlerObj->properties.end() && ownKeysIt->second.isFunction()) {
              Value ownKeysResult = callFunction(ownKeysIt->second, {*proxyPtr->target}, Value(handlerObj));
              if (hasError()) {
                LIGHTJS_RETURN(Value(Undefined{}));
              }
              if (ownKeysResult.isArray()) {
                auto keyArr = ownKeysResult.getGC<Array>();
                for (const auto& keyVal : keyArr->elements) {
                  keys.push_back(toPropertyKeyString(keyVal));
                }
              }
            }
          }
          if (keys.empty()) {
            for (const auto& sourceKey : targetObj->properties.orderedKeys()) {
              if (sourceKey.rfind("__", 0) == 0) {
                continue;
              }
              keys.push_back(sourceKey);
            }
          }

          for (const auto& key : keys) {
            if (extractedKeys.count(key) > 0) {
              continue;
            }

            bool enumerable = false;
            bool hasDesc = false;
            if (handlerObj) {
              auto gopdIt = handlerObj->properties.find("getOwnPropertyDescriptor");
              if (gopdIt != handlerObj->properties.end() && gopdIt->second.isFunction()) {
                Value descVal = callFunction(
                    gopdIt->second, {*proxyPtr->target, toPropertyKeyValue(key)}, Value(handlerObj));
                if (hasError()) {
                  LIGHTJS_RETURN(Value(Undefined{}));
                }
                if (descVal.isObject()) {
                  hasDesc = true;
                  auto descObj = descVal.getGC<Object>();
                  auto enumIt = descObj->properties.find("enumerable");
                  enumerable = (enumIt != descObj->properties.end()) && enumIt->second.toBool();
                }
              } else {
                hasDesc = targetObj->properties.count(key) > 0 ||
                          targetObj->properties.count("__get_" + key) > 0 ||
                          targetObj->properties.count("__set_" + key) > 0;
                enumerable = hasDesc && targetObj->properties.count("__non_enum_" + key) == 0;
              }
            } else {
              hasDesc = targetObj->properties.count(key) > 0 ||
                        targetObj->properties.count("__get_" + key) > 0 ||
                        targetObj->properties.count("__set_" + key) > 0;
              enumerable = hasDesc && targetObj->properties.count("__non_enum_" + key) == 0;
            }
            if (!hasDesc || !enumerable) {
              continue;
            }

            Value propValue(Undefined{});
            bool resolved = false;
            if (handlerObj) {
              auto getIt = handlerObj->properties.find("get");
              if (getIt != handlerObj->properties.end() && getIt->second.isFunction()) {
                propValue = callFunction(
                    getIt->second,
                    {*proxyPtr->target, toPropertyKeyValue(key), value},
                    Value(handlerObj));
                if (hasError()) {
                  LIGHTJS_RETURN(Value(Undefined{}));
                }
                resolved = true;
              }
            }
            if (!resolved) {
              auto getterIt = targetObj->properties.find("__get_" + key);
              if (getterIt != targetObj->properties.end() && getterIt->second.isFunction()) {
                propValue = callFunction(getterIt->second, {}, *proxyPtr->target);
                if (hasError()) {
                  LIGHTJS_RETURN(Value(Undefined{}));
                }
              } else {
                auto valueIt = targetObj->properties.find(key);
                if (valueIt != targetObj->properties.end()) {
                  propValue = valueIt->second;
                }
              }
            }
            restObj->properties[key] = propValue;
          }
          { auto t = bindDestructuringPattern(*objPat->rest, Value(restObj), isConst, useSet); LIGHTJS_RUN_TASK_VOID(t); }
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      }
      std::vector<std::string> ownKeys = obj->properties.orderedKeys();
      std::vector<std::string> numericKeys;
      std::vector<std::string> stringKeys;
      std::vector<std::string> symbolKeys;
      std::unordered_set<std::string> seenKeys;
      for (const auto& rawKey : ownKeys) {
        std::string key = rawKey;
        if (rawKey.rfind("__get_", 0) == 0) {
          key = rawKey.substr(6);
        } else if (rawKey.rfind("__set_", 0) == 0 || rawKey.rfind("__non_enum_", 0) == 0 ||
                   rawKey.rfind("__non_writable_", 0) == 0 ||
                   rawKey.rfind("__non_configurable_", 0) == 0 ||
                   rawKey.rfind("__enum_", 0) == 0 ||
                   (rawKey.size() >= 4 && rawKey.substr(0, 2) == "__" &&
                    rawKey.substr(rawKey.size() - 2) == "__")) {
          continue;
        }
        if (seenKeys.count(key) > 0) continue;
        seenKeys.insert(key);
        if (extractedKeys.count(key) > 0) continue;
        if (obj->properties.count("__non_enum_" + key)) continue;
        bool isNumeric = !key.empty() && std::all_of(key.begin(), key.end(), [](unsigned char c) {
          return std::isdigit(c) != 0;
        });
        if (isNumeric) {
          numericKeys.push_back(key);
        } else if (isSymbolPropertyKey(key)) {
          symbolKeys.push_back(key);
        } else {
          stringKeys.push_back(key);
        }
      }
      std::sort(numericKeys.begin(), numericKeys.end(), [](const std::string& a, const std::string& b) {
        return std::stoull(a) < std::stoull(b);
      });
      std::vector<std::string> orderedCopyKeys;
      orderedCopyKeys.insert(orderedCopyKeys.end(), numericKeys.begin(), numericKeys.end());
      orderedCopyKeys.insert(orderedCopyKeys.end(), stringKeys.begin(), stringKeys.end());
      orderedCopyKeys.insert(orderedCopyKeys.end(), symbolKeys.begin(), symbolKeys.end());

      for (const auto& key : orderedCopyKeys) {
        auto getterIt = obj->properties.find("__get_" + key);
        if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
          restObj->properties[key] = callFunction(getterIt->second, {}, value);
          if (flow_.type == ControlFlow::Type::Throw) LIGHTJS_RETURN(Value(Undefined{}));
          continue;
        }
        auto valueIt = obj->properties.find(key);
        if (valueIt != obj->properties.end()) {
          restObj->properties[key] = valueIt->second;
        }
      }
      { auto t = bindDestructuringPattern(*objPat->rest, Value(restObj), isConst, useSet); LIGHTJS_RUN_TASK_VOID(t); }
    }
  }
  // Other node types are ignored (error case in real JS)

  LIGHTJS_RETURN(Value(Undefined{}));}

// Helper to invoke a JavaScript function synchronously (used by native array methods for callbacks)
Value Interpreter::invokeFunction(GCPtr<Function> func, const std::vector<Value>& args, const Value& thisValue) {
  if (func->isNative) {
    try {
      auto itUsesThis = func->properties.find("__uses_this_arg__");
      if (itUsesThis != func->properties.end() && itUsesThis->second.isBool() && itUsesThis->second.toBool()) {
        std::vector<Value> nativeArgs;
        nativeArgs.reserve(args.size() + 1);
        nativeArgs.push_back(thisValue);
        nativeArgs.insert(nativeArgs.end(), args.begin(), args.end());
        return func->nativeFunc(nativeArgs);
      }
      return func->nativeFunc(args);
    } catch (const JsValueException& e) {
      flow_.type = ControlFlow::Type::Throw;
      flow_.value = e.value();
      return Value(Undefined{});
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
        errorType = ErrorType::Error;
      }
      throwError(errorType, message);
      return Value(Undefined{});
    }
  }

  auto previousPrivateOwnerClass = activePrivateOwnerClass_;
  if (auto ownerClassIt = func->properties.find("__private_owner_class__");
      ownerClassIt != func->properties.end() && ownerClassIt->second.isClass()) {
    activePrivateOwnerClass_ = ownerClassIt->second.getGC<Class>();
  }
  struct PrivateOwnerClassGuard {
    Interpreter* interpreter;
    GCPtr<Class> previousOwnerClass;
    ~PrivateOwnerClassGuard() {
      interpreter->activePrivateOwnerClass_ = previousOwnerClass;
    }
  } privateOwnerClassGuard{this, previousPrivateOwnerClass};

  auto previousActiveFunction = activeFunction_;
  activeFunction_ = func;
  struct ActiveFunctionGuard {
    Interpreter* interpreter;
    GCPtr<Function> previous;
    ~ActiveFunctionGuard() {
      interpreter->activeFunction_ = previous;
    }
  } activeFunctionGuard{this, previousActiveFunction};

  // Save current environment
  auto prevEnv = env_;
  env_ = func->closure;
  env_ = env_->createChild();
  env_->define("__var_scope__", Value(true), true);

  bool isArrowFunction = false;
  auto arrowIt = func->properties.find("__is_arrow_function__");
  if (arrowIt != func->properties.end() && arrowIt->second.isBool() && arrowIt->second.toBool()) {
    isArrowFunction = true;
  }

  Value boundThis = thisValue;
  if (!isArrowFunction) {
    if (!func->isStrict && (boundThis.isUndefined() || boundThis.isNull())) {
      if (auto globalThisValue = env_->get("globalThis")) {
        boundThis = *globalThisValue;
      }
    }
    if (!boundThis.isUndefined()) {
      env_->define("this", boundThis);
    }
  }

  bool superSet = false;
  auto ownerIt = func->properties.find("__private_owner_class__");
  if (ownerIt != func->properties.end() && ownerIt->second.isClass() &&
      boundThis.isClass() &&
      boundThis.getGC<Class>().get() == ownerIt->second.getGC<Class>().get()) {
    auto ownerCls = ownerIt->second.getGC<Class>();
    auto protoIt = ownerCls->properties.find("__proto__");
    if (protoIt != ownerCls->properties.end()) {
      env_->define("__super__", protoIt->second);
      superSet = true;
    }
  }

  auto homeIt = func->properties.find("__home_object__");
  if (!superSet &&
      homeIt != func->properties.end() &&
      (homeIt->second.isObject() || homeIt->second.isClass())) {
    Value superBase(Undefined{});
    if (homeIt->second.isObject()) {
      auto homeObj = homeIt->second.getGC<Object>();
      auto homeProtoIt = homeObj->properties.find("__proto__");
      if (homeProtoIt != homeObj->properties.end()) {
        superBase = homeProtoIt->second;
      }
    } else {
      auto homeCls = homeIt->second.getGC<Class>();
      auto homeProtoIt = homeCls->properties.find("__proto__");
      if (homeProtoIt != homeCls->properties.end()) {
        superBase = homeProtoIt->second;
      }
    }
    if (superBase.isUndefined()) {
      auto objectCtor = env_->get("Object");
      if (objectCtor && objectCtor->isFunction()) {
        auto ctor = objectCtor->getGC<Function>();
        auto protoIt = ctor->properties.find("prototype");
        if (protoIt != ctor->properties.end()) {
          superBase = protoIt->second;
        }
      }
    }
    if (!superBase.isUndefined()) {
      env_->define("__super__", superBase);
    }
  } else if (!superSet) {
    auto superIt = func->properties.find("__super_class__");
    if (superIt != func->properties.end()) {
      env_->define("__super__", superIt->second);
    }
  }

  if (!isArrowFunction) {
    auto argumentsArray = GarbageCollector::makeGC<Array>();
    GarbageCollector::instance().reportAllocation(sizeof(Array));
    argumentsArray->elements = args;
    argumentsArray->properties["__is_arguments_object__"] = Value(true);
    // Set arguments [[Prototype]] to Object.prototype
    if (auto objCtor = env_->get("Object"); objCtor.has_value() && objCtor->isFunction()) {
      auto protoIt = objCtor->getGC<Function>()->properties.find("prototype");
      if (protoIt != objCtor->getGC<Function>()->properties.end()) {
        argumentsArray->properties["__proto__"] = protoIt->second;
      }
    }
    if (func->isStrict) {
      auto throwTypeErrorAccessor = GarbageCollector::makeGC<Function>();
      throwTypeErrorAccessor->isNative = true;
      throwTypeErrorAccessor->nativeFunc = [](const std::vector<Value>&) -> Value {
        throw std::runtime_error(
          "TypeError: 'caller', 'callee', and 'arguments' properties may not be accessed");
      };
      argumentsArray->properties["__get_callee"] = Value(throwTypeErrorAccessor);
      argumentsArray->properties["__set_callee"] = Value(throwTypeErrorAccessor);
    } else {
      argumentsArray->properties["callee"] = Value(func);
    }
    argumentsArray->properties["__non_enum_callee"] = Value(true);
    if (auto objProtoVal = env_->getRoot()->get("__object_prototype__");
        objProtoVal && objProtoVal->isObject()) {
      argumentsArray->properties["__proto__"] = *objProtoVal;
    }
    env_->define("arguments", Value(argumentsArray));
  }
  if (func->restParam.has_value()) {
    env_->defineTDZ(*func->restParam);
  }

  for (size_t i = 0; i < func->params.size(); ++i) {
    Value paramValue = (i < args.size()) ? args[i] : Value(Undefined{});
    if (func->params[i].defaultValue && paramValue.isUndefined()) {
      auto prevParamInitEval = activeParameterInitializerEvaluation_;
      activeParameterInitializerEvaluation_ = true;
      auto defaultExpr = std::static_pointer_cast<Expression>(func->params[i].defaultValue);
      auto defaultTask = evaluate(*defaultExpr);
      LIGHTJS_RUN_TASK_VOID_SYNC(defaultTask);
      activeParameterInitializerEvaluation_ = prevParamInitEval;
      if (flow_.type == ControlFlow::Type::Throw || hasError()) {
        env_ = prevEnv;
        return Value(Undefined{});
      }
      env_->define(func->params[i].name, defaultTask.result());
    } else {
      env_->define(func->params[i].name, paramValue);
    }
  }

  // Handle rest parameter
  if (func->restParam.has_value()) {
    auto restArr = GarbageCollector::makeGC<Array>();
    GarbageCollector::instance().reportAllocation(sizeof(Array));
    setArrayPrototype(restArr, env_.get());
    for (size_t i = func->params.size(); i < args.size(); ++i) {
      restArr->elements.push_back(args[i]);
    }
    env_->define(*func->restParam, Value(restArr));
  }

  if (func->destructurePrologue) {
    auto prologuePtr = std::static_pointer_cast<std::vector<StmtPtr>>(func->destructurePrologue);
    for (const auto& stmt : *prologuePtr) {
      auto stmtTask = evaluate(*stmt);
      Value stmtResult = Value(Undefined{});
      LIGHTJS_RUN_TASK_SYNC(stmtTask, stmtResult);
      if (flow_.type == ControlFlow::Type::Throw || hasError()) {
        env_ = prevEnv;
        return Value(Undefined{});
      }
    }
  }

  bool hasParameterExpressions = false;
  for (const auto& param : func->params) {
    if (param.defaultValue) {
      hasParameterExpressions = true;
      break;
    }
  }
  if (hasParameterExpressions) {
    env_ = env_->createChild();
  }

  // Execute function body
  auto bodyPtr = std::static_pointer_cast<std::vector<StmtPtr>>(func->body);
  bool previousStrictMode = strictMode_;
  strictMode_ = func->isStrict;

  // Initialize TDZ for lexical declarations in function body
  for (const auto& s : *bodyPtr) {
    if (auto* varDecl = std::get_if<VarDeclaration>(&s->node)) {
      if (varDecl->kind == VarDeclaration::Kind::Let ||
          varDecl->kind == VarDeclaration::Kind::Const) {
        for (const auto& declarator : varDecl->declarations) {
          std::vector<std::string> names;
          collectVarHoistNames(*declarator.pattern, names);
          for (const auto& name : names) {
            env_->defineTDZ(name);
          }
        }
      }
    } else if (auto* classDecl = std::get_if<ClassDeclaration>(&s->node)) {
      env_->defineTDZ(classDecl->id.name);
    }
  }

  // Hoist var and function declarations in function body.
  hoistVarDeclarations(*bodyPtr);
  for (const auto& hoistStmt : *bodyPtr) {
    if (std::holds_alternative<FunctionDeclaration>(hoistStmt->node)) {
      auto hoistTask = evaluate(*hoistStmt);
      LIGHTJS_RUN_TASK_VOID_SYNC(hoistTask);
    }
  }

  Value result = Value(Undefined{});
  bool returned = false;

  auto prevFlow = flow_;
  flow_ = ControlFlow{};

  for (const auto& stmt : *bodyPtr) {
    auto stmtTask = evaluate(*stmt);
    Value stmtResult = Value(Undefined{});
    LIGHTJS_RUN_TASK_SYNC(stmtTask, stmtResult);
    if (flow_.type == ControlFlow::Type::Return) {
      result = flow_.value;
      returned = true;
      break;
    }
    if (flow_.type == ControlFlow::Type::Throw) {
      break;
    }
  }

  if (!returned && flow_.type != ControlFlow::Type::Throw) {
    result = Value(Undefined{});
  }
  if (flow_.type != ControlFlow::Type::Throw) {
    flow_ = prevFlow;
  }
  strictMode_ = previousStrictMode;
  env_ = prevEnv;
  return result;
}

Task Interpreter::evaluateVarDecl(const VarDeclaration& decl) {
  for (const auto& declarator : decl.declarations) {
    // Per spec (13.3.2.4): ResolveBinding BEFORE evaluating initializer.
    // For var declarations in with-scope, capture the with-scope object first.
    std::optional<Value> preResolvedWithValue;
    std::string preResolvedName;
    if (decl.kind == VarDeclaration::Kind::Var && declarator.init) {
      if (auto* id = std::get_if<Identifier>(&declarator.pattern->node)) {
        preResolvedWithValue = env_->resolveWithScopeValue(id->name);
        if (preResolvedWithValue) {
          preResolvedName = id->name;
        }
      }
    }

    Value value = Value(Undefined{});
    auto previousPendingAnonymousClassName = pendingAnonymousClassName_;
    struct PendingAnonymousClassNameGuard {
      Interpreter* interpreter;
      std::optional<std::string> previous;
      ~PendingAnonymousClassNameGuard() {
        interpreter->pendingAnonymousClassName_ = previous;
      }
    } pendingAnonymousClassNameGuard{this, previousPendingAnonymousClassName};
    if (declarator.init) {
      if (auto* id = std::get_if<Identifier>(&declarator.pattern->node)) {
        if (auto* classExpr = std::get_if<ClassExpr>(&declarator.init->node);
            classExpr && classExpr->name.empty()) {
          pendingAnonymousClassName_ = id->name;
        }
      }
    }
    if (declarator.init) {
      auto task = evaluate(*declarator.init);
  LIGHTJS_RUN_TASK(task, value);
    } else if (decl.kind == VarDeclaration::Kind::Var) {
      // Runtime evaluation of `var x;` has no effect; bindings are created
      // during declaration instantiation/hoisting.
      continue;
    }

    // Function name inference: set .name on anonymous functions assigned to variables
    // Per spec, only applies when IsAnonymousFunctionDefinition(Initializer) is true:
    // the initializer must directly be a function/class expression (not comma expr etc.)
    if (declarator.init) {
      if (auto* id = std::get_if<Identifier>(&declarator.pattern->node)) {
        bool isAnonFnDef = false;
        if (auto* fnExpr = std::get_if<FunctionExpr>(&declarator.init->node)) {
          // Anonymous function expression (not named like `function foo() {}`)
          isAnonFnDef = fnExpr->name.empty();
        } else if (std::get_if<ClassExpr>(&declarator.init->node)) {
          isAnonFnDef = value.isClass() && value.getGC<Class>()->name.empty();
        }
        if (isAnonFnDef && value.isFunction()) {
          auto fn = value.getGC<Function>();
          auto nameIt = fn->properties.find("name");
          if (nameIt != fn->properties.end() && nameIt->second.isString() && nameIt->second.toString().empty()) {
            fn->properties["name"] = Value(id->name);
            fn->properties["__non_writable_name"] = Value(true);
            fn->properties["__non_enum_name"] = Value(true);
          }
        } else if (isAnonFnDef && value.isClass()) {
          auto cls = value.getGC<Class>();
          auto nameIt = cls->properties.find("name");
          bool shouldSet = nameIt == cls->properties.end() ||
                           (nameIt->second.isString() && nameIt->second.toString().empty());
          if (shouldSet) {
            cls->name = id->name;
            cls->properties["name"] = Value(id->name);
            cls->properties["__non_writable_name"] = Value(true);
            cls->properties["__non_enum_name"] = Value(true);
          }
        }
      }
    }

    // If binding was pre-resolved to a with-scope object, write there directly (PutValue)
    if (preResolvedWithValue && !preResolvedName.empty()) {
      env_->setWithScopeBindingValue(*preResolvedWithValue, preResolvedName, value, false);
      continue;
    }

    // Use the unified destructuring helper
    bool isConst = (decl.kind == VarDeclaration::Kind::Const);
    // For var declarations, use set() to update the hoisted binding in function scope
    // rather than define() which would create a new binding in the current block scope
    bool useSet = (decl.kind == VarDeclaration::Kind::Var);
    { auto t = bindDestructuringPattern(*declarator.pattern, value, isConst, useSet); LIGHTJS_RUN_TASK_VOID(t); }
  }
  LIGHTJS_RETURN(Value(Empty{}));
}

// Recursively collect var declarations from a statement and hoist them
// Helper: collect bound names from a pattern expression for var hoisting
static void collectVarHoistNames(const Expression& expr, std::vector<std::string>& names) {
  if (auto* id = std::get_if<Identifier>(&expr.node)) {
    names.push_back(id->name);
  } else if (auto* assign = std::get_if<AssignmentPattern>(&expr.node)) {
    if (assign->left) collectVarHoistNames(*assign->left, names);
  } else if (auto* arrPat = std::get_if<ArrayPattern>(&expr.node)) {
    for (const auto& elem : arrPat->elements) {
      if (elem) collectVarHoistNames(*elem, names);
    }
    if (arrPat->rest) collectVarHoistNames(*arrPat->rest, names);
  } else if (auto* objPat = std::get_if<ObjectPattern>(&expr.node)) {
    for (const auto& prop : objPat->properties) {
      if (prop.value) collectVarHoistNames(*prop.value, names);
    }
    if (objPat->rest) collectVarHoistNames(*objPat->rest, names);
  }
}

void Interpreter::hoistVarDeclarationsFromStmt(const Statement& stmt) {
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.node)) {
    if (varDecl->kind == VarDeclaration::Kind::Var) {
      for (const auto& declarator : varDecl->declarations) {
        std::vector<std::string> names;
        if (declarator.pattern) {
          collectVarHoistNames(*declarator.pattern, names);
        }
        for (const auto& name : names) {
          if (!env_->hasLocal(name)) {
            // At global scope, check if property already exists on globalThis
            // (e.g., via Object.defineProperty). Per spec CreateGlobalVarBinding:
            // if the property already exists, do NOT change its attributes or value.
            if (!env_->getParent()) {
              auto globalThisOpt = env_->get("globalThis");
              if (globalThisOpt && globalThisOpt->isObject()) {
                auto globalObj = std::get<GCPtr<Object>>(globalThisOpt->data);
                auto existingProp = globalObj->properties.find(name);
                if (existingProp != globalObj->properties.end()) {
                  // Property already exists on globalThis - just create the env binding
                  // pointing to the existing value, don't sync back to globalThis
                  env_->setBindingDirect(name, existingProp->second);
                } else {
                  env_->define(name, Value(Undefined{}));
                  globalObj->properties["__non_configurable_" + name] = Value(true);
                }
              } else {
                env_->define(name, Value(Undefined{}));
              }
            } else {
              env_->define(name, Value(Undefined{}));
            }
          }
        }
      }
    }
  } else if (auto* block = std::get_if<BlockStmt>(&stmt.node)) {
    hoistVarDeclarations(block->body);
  } else if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    if (ifStmt->consequent) hoistVarDeclarationsFromStmt(*ifStmt->consequent);
    if (ifStmt->alternate) hoistVarDeclarationsFromStmt(*ifStmt->alternate);
  } else if (auto* whileStmt = std::get_if<WhileStmt>(&stmt.node)) {
    if (whileStmt->body) hoistVarDeclarationsFromStmt(*whileStmt->body);
  } else if (auto* doWhile = std::get_if<DoWhileStmt>(&stmt.node)) {
    if (doWhile->body) hoistVarDeclarationsFromStmt(*doWhile->body);
  } else if (auto* forStmt = std::get_if<ForStmt>(&stmt.node)) {
    if (forStmt->init) hoistVarDeclarationsFromStmt(*forStmt->init);
    if (forStmt->body) hoistVarDeclarationsFromStmt(*forStmt->body);
  } else if (auto* forIn = std::get_if<ForInStmt>(&stmt.node)) {
    if (forIn->left) hoistVarDeclarationsFromStmt(*forIn->left);
    if (forIn->body) hoistVarDeclarationsFromStmt(*forIn->body);
  } else if (auto* forOf = std::get_if<ForOfStmt>(&stmt.node)) {
    if (forOf->left) hoistVarDeclarationsFromStmt(*forOf->left);
    if (forOf->body) hoistVarDeclarationsFromStmt(*forOf->body);
  } else if (auto* switchStmt = std::get_if<SwitchStmt>(&stmt.node)) {
    for (const auto& caseClause : switchStmt->cases) {
      hoistVarDeclarations(caseClause.consequent);
    }
  } else if (auto* tryStmt = std::get_if<TryStmt>(&stmt.node)) {
    hoistVarDeclarations(tryStmt->block);
    if (tryStmt->hasHandler) hoistVarDeclarations(tryStmt->handler.body);
    if (tryStmt->hasFinalizer) hoistVarDeclarations(tryStmt->finalizer);
  } else if (auto* labelled = std::get_if<LabelledStmt>(&stmt.node)) {
    if (labelled->body) hoistVarDeclarationsFromStmt(*labelled->body);
  } else if (auto* withStmt = std::get_if<WithStmt>(&stmt.node)) {
    if (withStmt->body) hoistVarDeclarationsFromStmt(*withStmt->body);
  } else if (auto* exportNamed = std::get_if<ExportNamedDeclaration>(&stmt.node)) {
    // Handle export var declarations: export var x = ...
    if (exportNamed->declaration) {
      hoistVarDeclarationsFromStmt(*exportNamed->declaration);
    }
  }
}

// Hoist var declarations and function declarations from a list of statements
void Interpreter::hoistVarDeclarations(const std::vector<StmtPtr>& body) {
  for (const auto& stmt : body) {
    hoistVarDeclarationsFromStmt(*stmt);
  }
}

Task Interpreter::evaluateFuncDecl(const FunctionDeclaration& decl) {
  auto func = GarbageCollector::makeGC<Function>();
  func->isNative = false;
  func->isAsync = decl.isAsync;
  func->isGenerator = decl.isGenerator;
  func->isStrict = strictMode_ || hasUseStrictDirective(decl.body);

  for (const auto& param : decl.params) {
    FunctionParam funcParam;
    funcParam.name = param.name.name;
    if (param.defaultValue) {
      funcParam.defaultValue = std::shared_ptr<void>(const_cast<Expression*>(param.defaultValue.get()), [](void*){});
    }
    func->params.push_back(funcParam);
  }

  if (decl.restParam.has_value()) {
    func->restParam = decl.restParam->name;
  }

  func->body = std::shared_ptr<void>(const_cast<std::vector<StmtPtr>*>(&decl.body), [](void*){});
  func->destructurePrologue = std::shared_ptr<void>(const_cast<std::vector<StmtPtr>*>(&decl.destructurePrologue), [](void*){});
  // If evaluating in an eval context, keep the AST alive
  if (sourceKeepAlive_) {
    func->astOwner = sourceKeepAlive_;
  }
  func->closure = env_;
  if (activePrivateOwnerClass_) {
    func->properties["__private_owner_class__"] = Value(activePrivateOwnerClass_);
  }
  // Compute length: number of params before first default parameter
  size_t funcDeclLen = 0;
  for (const auto& param : decl.params) {
    if (param.defaultValue) break;
    funcDeclLen++;
  }
  func->properties["length"] = Value(static_cast<double>(funcDeclLen));
  func->properties["__non_writable_length"] = Value(true);
  func->properties["__non_enum_length"] = Value(true);
  func->properties["name"] = Value(decl.id.name);
  func->properties["__non_writable_name"] = Value(true);
  func->properties["__non_enum_name"] = Value(true);
  func->sourceText = decl.sourceText;
  // Async functions (non-generator) are not constructors (no MakeConstructor call per spec)
  // Async generators are also not constructors but DO have a .prototype object
  func->isConstructor = !func->isGenerator && !func->isAsync;

  // Create default prototype object (only for constructors and generators, also async generators)
  if (!(func->isAsync && !func->isGenerator)) {
    auto proto = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    if (func->isGenerator) {
      if (auto genProto = env_->get("__generator_prototype__"); genProto && genProto->isObject()) {
        proto->properties["__proto__"] = *genProto;
      }
      func->properties["__non_enum_prototype"] = Value(true);
      func->properties["__non_configurable_prototype"] = Value(true);
    } else {
      proto->properties["constructor"] = Value(func);
      proto->properties["__non_enum_constructor"] = Value(true);
      // Ordinary function prototypes inherit from Object.prototype.
      if (auto objectCtorVal = env_->getRoot()->get("Object"); objectCtorVal && objectCtorVal->isFunction()) {
        auto objectCtor = objectCtorVal->getGC<Function>();
        auto objectProtoIt = objectCtor->properties.find("prototype");
        if (objectProtoIt != objectCtor->properties.end()) {
          proto->properties["__proto__"] = objectProtoIt->second;
        }
      }
    }
    func->properties["prototype"] = Value(proto);
    func->properties["__non_enum_prototype"] = Value(true);
    func->properties["__non_configurable_prototype"] = Value(true);
  }

  // Set __proto__ to Function.prototype or GeneratorFunction.prototype
  std::string funcProtoName = func->isGenerator ? "__generator_function_prototype__" : "Function";
  auto funcVal = env_->getRoot()->get(funcProtoName);
  if (funcVal.has_value()) {
    if (func->isGenerator && funcVal->isObject()) {
      func->properties["__proto__"] = *funcVal;
    } else if (funcVal->isFunction()) {
      auto funcCtor = std::get<GCPtr<Function>>(funcVal->data);
      auto protoIt = funcCtor->properties.find("prototype");
      if (protoIt != funcCtor->properties.end()) {
        func->properties["__proto__"] = protoIt->second;
      }
    }
  }

  // At global scope, implement CreateGlobalFunctionBinding (ES spec 8.1.1.4.18)
  if (!env_->getParent()) {
    auto globalThisOpt = env_->get("globalThis");
    if (globalThisOpt && globalThisOpt->isObject()) {
      auto globalObj = std::get<GCPtr<Object>>(globalThisOpt->data);
      auto existingProp = globalObj->properties.find(decl.id.name);
      bool isConfigurable = true;
      if (existingProp != globalObj->properties.end()) {
        // Check if existing property is configurable
        isConfigurable = globalObj->properties.find("__non_configurable_" + decl.id.name) == globalObj->properties.end();
      }
      // Set the value
      globalObj->properties[decl.id.name] = Value(func);
      if (existingProp == globalObj->properties.end() || isConfigurable) {
        // Full descriptor: {value: fn, writable: true, enumerable: true, configurable: false}
        globalObj->properties.erase("__non_writable_" + decl.id.name);
        globalObj->properties.erase("__non_enum_" + decl.id.name);
        globalObj->properties["__non_configurable_" + decl.id.name] = Value(true);
      }
      // else: only value changes, other attributes preserved
    }
    env_->setBindingDirect(decl.id.name, Value(func));
  } else {
    env_->define(decl.id.name, Value(func));
  }

  LIGHTJS_RETURN(Value(Empty{}));
}

Task Interpreter::evaluateReturn(const ReturnStmt& stmt) {
  Value result = Value(Undefined{});
  if (stmt.argument) {
    bool prevTailPosition = inTailPosition_;
    inTailPosition_ = true;
    auto task = evaluate(*stmt.argument);
    LIGHTJS_RUN_TASK(task, result);
    inTailPosition_ = prevTailPosition;

    // If an error was thrown during argument evaluation, preserve it
    if (flow_.type == ControlFlow::Type::Throw) {
      LIGHTJS_RETURN(result);
    }
  }
  flow_.type = ControlFlow::Type::Return;
  flow_.value = result;
  LIGHTJS_RETURN(result);
}

Task Interpreter::evaluateExprStmt(const ExpressionStmt& stmt) {
  auto task = evaluate(*stmt.expression);
  LIGHTJS_RUN_TASK_VOID(task);
  LIGHTJS_RETURN(task.result());
}

Task Interpreter::evaluateBlock(const BlockStmt& stmt) {
  auto prevEnv = env_;
  env_ = env_->createChild();

  // Initialize TDZ for lexical declarations in this block (non-recursive)
  for (const auto& s : stmt.body) {
    if (auto* varDecl = std::get_if<VarDeclaration>(&s->node)) {
      if (varDecl->kind == VarDeclaration::Kind::Let ||
          varDecl->kind == VarDeclaration::Kind::Const) {
        for (const auto& declarator : varDecl->declarations) {
          std::vector<std::string> names;
          collectVarHoistNames(*declarator.pattern, names);
          for (const auto& name : names) {
            env_->defineTDZ(name);
          }
        }
      }
    } else if (auto* classDecl = std::get_if<ClassDeclaration>(&s->node)) {
      env_->defineTDZ(classDecl->id.name);
    }
  }

  Value result = Value(Empty{});
  for (const auto& s : stmt.body) {
    auto task = evaluate(*s);
    LIGHTJS_RUN_TASK_VOID(task);
    if (!task.result().isEmpty()) {
      result = task.result();
    }
    if (flow_.type != ControlFlow::Type::None) {
      break;
    }
  }

  env_ = prevEnv;
  LIGHTJS_RETURN(result);
}

Task Interpreter::evaluateIf(const IfStmt& stmt) {
  auto testTask = evaluate(*stmt.test);
  LIGHTJS_RUN_TASK_VOID(testTask);
  if (flow_.type != ControlFlow::Type::None) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  if (testTask.result().toBool()) {
    auto consTask = evaluate(*stmt.consequent);
    LIGHTJS_RUN_TASK_VOID(consTask);
    // Spec: Return Completion(UpdateEmpty(stmtCompletion, undefined))
    Value result = consTask.result();
    if (result.isEmpty()) result = Value(Undefined{});
    LIGHTJS_RETURN(result);
  } else if (stmt.alternate) {
    auto altTask = evaluate(*stmt.alternate);
    LIGHTJS_RUN_TASK_VOID(altTask);
    // Spec: Return Completion(UpdateEmpty(stmtCompletion, undefined))
    Value result = altTask.result();
    if (result.isEmpty()) result = Value(Undefined{});
    LIGHTJS_RETURN(result);
  }

  // Spec: if no else and condition is false, return NormalCompletion(undefined)
  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::evaluateWhile(const WhileStmt& stmt) {
  Value result = Value(Undefined{});
  std::string myLabel = pendingIterationLabel_;
  pendingIterationLabel_.clear();
  size_t loopIter = 0;

  while (true) {
    if (maxLoopIterations_ > 0 && ++loopIter > maxLoopIterations_) {
      throwError(ErrorType::RangeError, "Maximum loop iterations exceeded");
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    auto testTask = evaluate(*stmt.test);
    LIGHTJS_RUN_TASK_VOID(testTask);

    if (!testTask.result().toBool()) {
      break;
    }

    Value bodyResult;
    auto bodyTask = evaluate(*stmt.body);
  LIGHTJS_RUN_TASK(bodyTask, bodyResult);

    // UpdateEmpty: only update V when body result is not empty
    if (!bodyResult.isEmpty()) {
      result = bodyResult;
    }

    if (flow_.type == ControlFlow::Type::Break) {
      if (flow_.breakCompletionValue.has_value()) {
        result = *flow_.breakCompletionValue;
        flow_.breakCompletionValue = std::nullopt;
      }
      if (flow_.label.empty()) flow_.type = ControlFlow::Type::None;
      break;
    } else if (flow_.type == ControlFlow::Type::Continue) {
      if (flow_.breakCompletionValue.has_value()) {
        result = *flow_.breakCompletionValue;
        flow_.breakCompletionValue = std::nullopt;
      }
      if (flow_.label.empty() || (!myLabel.empty() && flow_.label == myLabel)) {
        flow_.type = ControlFlow::Type::None;
        flow_.label.clear();
        continue;
      }
      break;
    } else if (flow_.type != ControlFlow::Type::None) {
      break;
    }
  }

  LIGHTJS_RETURN(result);
}

Task Interpreter::evaluateWith(const WithStmt& stmt) {
  if (strictMode_) {
    throwError(ErrorType::SyntaxError, "Strict mode code may not include a with statement");
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  Value scopeValue;

  { auto _t = evaluate(*stmt.object); LIGHTJS_RUN_TASK(_t, scopeValue); }
  if (flow_.type != ControlFlow::Type::None) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  // with(null) or with(undefined) throws TypeError per spec
  if (scopeValue.isNull() || scopeValue.isUndefined()) {
    throwError(ErrorType::TypeError, "Cannot convert undefined or null to object");
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  auto prevEnv = env_;
  env_ = env_->createChild();
  if (isObjectLike(scopeValue) || scopeValue.isProxy()) {
    env_->define("__with_scope_object__", scopeValue);
  }
  struct EnvRestoreGuard {
    Interpreter* interpreter;
    GCPtr<Environment> previous;
    ~EnvRestoreGuard() { interpreter->env_ = previous; }
  } restore{this, prevEnv};

  auto isVisibleKey = [](const std::string& key) -> bool {
    if (key.empty()) {
      return false;
    }
    return key.rfind("__", 0) != 0;
  };

  auto defineVisible = [&](const std::string& key, const Value& value) {
    if (isVisibleKey(key)) {
      env_->define(key, value);
    }
  };

  auto bindObjectChain = [&](const GCPtr<Object>& root) {
    std::unordered_set<Object*> visited;
    auto current = root;
    int depth = 0;
    while (current && depth < 32 && visited.insert(current.get()).second) {
      for (const auto& [key, value] : current->properties) {
        defineVisible(key, value);
      }
      auto protoIt = current->properties.find("__proto__");
      if (protoIt == current->properties.end() || !protoIt->second.isObject()) {
        break;
      }
      current = protoIt->second.getGC<Object>();
      depth++;
    }
  };

  auto resolvePromisePrototype = [&](const Value& ctorValue) -> GCPtr<Object> {
    if (!ctorValue.isFunction()) {
      return nullptr;
    }
    auto ctorFn = ctorValue.getGC<Function>();
    auto protoIt = ctorFn->properties.find("prototype");
    if (protoIt == ctorFn->properties.end() || !protoIt->second.isObject()) {
      return nullptr;
    }
    return protoIt->second.getGC<Object>();
  };

  if (isObjectLike(scopeValue) || scopeValue.isProxy()) {
    // Keep with-object property resolution dynamic via __with_scope_object__.
    // Copying properties into lexical bindings breaks unscopables semantics.
  } else if (scopeValue.isPromise()) {
    auto promisePtr = scopeValue.getGC<Promise>();
    for (const auto& [key, value] : promisePtr->properties) {
      defineVisible(key, value);
    }

    Value ctorValue = Value(Undefined{});
    auto ctorIt = promisePtr->properties.find("__constructor__");
    if (ctorIt != promisePtr->properties.end()) {
      ctorValue = ctorIt->second;
    } else if (auto intrinsicPromise = env_->get("__intrinsic_Promise__")) {
      ctorValue = *intrinsicPromise;
    } else if (auto promiseCtor = env_->get("Promise")) {
      ctorValue = *promiseCtor;
    }
    if (!ctorValue.isUndefined()) {
      env_->define("constructor", ctorValue);
    }

    auto promiseProto = resolvePromisePrototype(ctorValue);
    if (!promiseProto) {
      if (auto intrinsicPromise = env_->get("__intrinsic_Promise__")) {
        promiseProto = resolvePromisePrototype(*intrinsicPromise);
      }
    }
    if (promiseProto) {
      auto thenIt = promiseProto->properties.find("then");
      if (thenIt != promiseProto->properties.end()) {
        env_->define("then", thenIt->second);
      }
      auto catchIt = promiseProto->properties.find("catch");
      if (catchIt != promiseProto->properties.end()) {
        env_->define("catch", catchIt->second);
      }
      auto finallyIt = promiseProto->properties.find("finally");
      if (finallyIt != promiseProto->properties.end()) {
        env_->define("finally", finallyIt->second);
      }
    }
  }

  Value result;

  { auto _t = evaluate(*stmt.body); LIGHTJS_RUN_TASK(_t, result); }

  // Per spec: Return Completion(UpdateEmpty(C, undefined))
  if (result.isEmpty()) {
    result = Value(Undefined{});
  }

  // When body completes with break/continue, carry the UpdateEmpty'd value
  if (flow_.type == ControlFlow::Type::Break ||
      flow_.type == ControlFlow::Type::Continue) {
    // Propagate it through breakCompletionValue for the enclosing loop
    flow_.breakCompletionValue = result;
  }

  LIGHTJS_RETURN(result);
}

Task Interpreter::evaluateFor(const ForStmt& stmt) {
  auto prevEnv = env_;
  env_ = env_->createChild();
  std::string myLabel = pendingIterationLabel_;
  pendingIterationLabel_.clear();

  if (stmt.init) {
    auto initTask = evaluate(*stmt.init);
    LIGHTJS_RUN_TASK_VOID(initTask);
    // If init threw (e.g. destructuring null/undefined), propagate the error
    if (flow_.type == ControlFlow::Type::Throw) {
      env_ = prevEnv;
      LIGHTJS_RETURN(Value(Undefined{}));
    }
  }

  // Collect per-iteration let bindings for freshness (ES6 13.7.4.7)
  // Only let bindings are refreshed per iteration; const bindings are not (they can't change)
  std::vector<std::string> perIterationBindings;
  if (stmt.init) {
    if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.init->node)) {
      if (varDecl->kind == VarDeclaration::Kind::Let) {
        for (const auto& declarator : varDecl->declarations) {
          std::vector<std::string> names;
          if (declarator.pattern) {
            // Use simple name extraction for identifiers
            if (auto* id = std::get_if<Identifier>(&declarator.pattern->node)) {
              names.push_back(id->name);
            }
          }
          for (const auto& name : names) {
            perIterationBindings.push_back(name);
          }
        }
      }
    }
  }

  Value result = Value(Undefined{});
  size_t loopIter = 0;

  // CreatePerIterationEnvironment helper (ES6 14.7.4.4)
  // Creates a fresh scope copying let bindings, so closures capture per-iteration values
  auto createPerIterationEnv = [&]() {
    if (perIterationBindings.empty()) return;
    auto outerEnv = env_->getParent();
    auto iterEnv = outerEnv->createChild();
    for (const auto& name : perIterationBindings) {
      auto val = env_->get(name);
      iterEnv->define(name, val.has_value() ? *val : Value(Undefined{}));
    }
    env_ = iterEnv;
  };

  // Initial CreatePerIterationEnvironment (spec step 2)
  createPerIterationEnv();

  while (true) {
    if (maxLoopIterations_ > 0 && ++loopIter > maxLoopIterations_) {
      throwError(ErrorType::RangeError, "Maximum loop iterations exceeded");
      env_ = prevEnv;
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (stmt.test) {
      auto testTask = evaluate(*stmt.test);
      LIGHTJS_RUN_TASK_VOID(testTask);
      if (!testTask.result().toBool()) {
        break;
      }
    }

    Value bodyResult;
    auto bodyTask = evaluate(*stmt.body);
  LIGHTJS_RUN_TASK(bodyTask, bodyResult);

    // UpdateEmpty: only update V when body result is not empty
    if (!bodyResult.isEmpty()) {
      result = bodyResult;
    }

    if (flow_.type == ControlFlow::Type::Break) {
      if (flow_.breakCompletionValue.has_value()) {
        result = *flow_.breakCompletionValue;
        flow_.breakCompletionValue = std::nullopt;
      }
      if (flow_.label.empty()) flow_.type = ControlFlow::Type::None;
      break;
    } else if (flow_.type == ControlFlow::Type::Continue) {
      if (flow_.breakCompletionValue.has_value()) {
        result = *flow_.breakCompletionValue;
        flow_.breakCompletionValue = std::nullopt;
      }
      if (flow_.label.empty() || (!myLabel.empty() && flow_.label == myLabel)) {
        flow_.type = ControlFlow::Type::None;
        flow_.label.clear();
      } else {
        break;
      }
    } else if (flow_.type != ControlFlow::Type::None) {
      break;
    }

    // CreatePerIterationEnvironment BEFORE update (spec step 3e)
    // This ensures closures from body see their iteration's value, not the updated one
    createPerIterationEnv();

    if (stmt.update) {
      auto updateTask = evaluate(*stmt.update);
      LIGHTJS_RUN_TASK_VOID(updateTask);
    }
  }

  env_ = prevEnv;
  LIGHTJS_RETURN(result);
}

Task Interpreter::evaluateDoWhile(const DoWhileStmt& stmt) {
  Value result = Value(Undefined{});
  // Consume pending label if this loop is directly labeled
  std::string myLabel = pendingIterationLabel_;
  pendingIterationLabel_.clear();
  size_t loopIter = 0;

  do {
    if (maxLoopIterations_ > 0 && ++loopIter > maxLoopIterations_) {
      throwError(ErrorType::RangeError, "Maximum loop iterations exceeded");
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    Value bodyResult;
    auto bodyTask = evaluate(*stmt.body);
  LIGHTJS_RUN_TASK(bodyTask, bodyResult);

    // UpdateEmpty: only update V when body result is not empty
    if (!bodyResult.isEmpty()) {
      result = bodyResult;
    }

    if (flow_.type == ControlFlow::Type::Break) {
      // Use try/finally completion value if available (spec UpdateEmpty)
      if (flow_.breakCompletionValue.has_value()) {
        result = *flow_.breakCompletionValue;
        flow_.breakCompletionValue = std::nullopt;
      }
      if (flow_.label.empty()) flow_.type = ControlFlow::Type::None;
      break;
    } else if (flow_.type == ControlFlow::Type::Continue) {
      // Use try/finally completion value if available (spec UpdateEmpty)
      if (flow_.breakCompletionValue.has_value()) {
        result = *flow_.breakCompletionValue;
        flow_.breakCompletionValue = std::nullopt;
      }
      if (flow_.label.empty()) {
        flow_.type = ControlFlow::Type::None;
      } else if (!myLabel.empty() && flow_.label == myLabel) {
        // continue targets this labeled loop - consume and check condition
        flow_.type = ControlFlow::Type::None;
        flow_.label.clear();
      } else {
        break;  // continue targets an outer loop
      }
    } else if (flow_.type != ControlFlow::Type::None) {
      break;
    }

    auto testTask = evaluate(*stmt.test);
    LIGHTJS_RUN_TASK_VOID(testTask);

    if (!testTask.result().toBool()) {
      break;
    }
  } while (true);

  LIGHTJS_RETURN(result);
}

Task Interpreter::evaluateForIn(const ForInStmt& stmt) {
  auto prevEnv = env_;
  env_ = env_->createChild();
  std::string myLabel = pendingIterationLabel_;
  pendingIterationLabel_.clear();

  Value result = Value(Undefined{});

  // Get the variable name from the left side (before evaluating RHS, for TDZ)
  std::string varName;
  bool isVarDecl = false;
  bool isLetOrConst = false;
  bool isConst = false;
  const Expression* memberExpr = nullptr;  // For MemberExpr targets
  const Expression* dstrPattern = nullptr;  // For destructuring patterns
  bool dstrIsDecl = false;  // true if destructuring is from var/let/const declaration

  // Helper to collect bound names from a pattern for TDZ
  std::function<void(const Expression&, std::vector<std::string>&)> collectBoundNames;
  collectBoundNames = [&collectBoundNames](const Expression& expr, std::vector<std::string>& names) {
    if (auto* id = std::get_if<Identifier>(&expr.node)) {
      names.push_back(id->name);
    } else if (auto* arr = std::get_if<ArrayPattern>(&expr.node)) {
      for (const auto& elem : arr->elements) {
        if (elem) collectBoundNames(*elem, names);
      }
      if (arr->rest) collectBoundNames(*arr->rest, names);
    } else if (auto* obj = std::get_if<ObjectPattern>(&expr.node)) {
      for (const auto& prop : obj->properties) {
        if (prop.value) collectBoundNames(*prop.value, names);
      }
      if (obj->rest) collectBoundNames(*obj->rest, names);
    } else if (auto* assign = std::get_if<AssignmentPattern>(&expr.node)) {
      if (assign->left) collectBoundNames(*assign->left, names);
    }
  };

  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.left->node)) {
    isVarDecl = true;
    isLetOrConst = (varDecl->kind == VarDeclaration::Kind::Let || varDecl->kind == VarDeclaration::Kind::Const);
    isConst = (varDecl->kind == VarDeclaration::Kind::Const);
    if (!varDecl->declarations.empty()) {
      if (auto* id = std::get_if<Identifier>(&varDecl->declarations[0].pattern->node)) {
        varName = id->name;
      } else {
        // Destructuring pattern in declaration: for (var [a, b] in ...) or for (let {x} in ...)
        dstrPattern = varDecl->declarations[0].pattern.get();
        dstrIsDecl = true;
      }
      // For var declarations, define in loop scope
      if (!isLetOrConst && !varName.empty()) {
        env_->define(varName, Value(Undefined{}));
      }
    }
  } else if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt.left->node)) {
    if (auto* ident = std::get_if<Identifier>(&exprStmt->expression->node)) {
      varName = ident->name;
    } else if (std::get_if<MemberExpr>(&exprStmt->expression->node)) {
      memberExpr = exprStmt->expression.get();
    } else if (std::get_if<ArrayPattern>(&exprStmt->expression->node) ||
               std::get_if<ObjectPattern>(&exprStmt->expression->node)) {
      // Expression destructuring: for ([a, b] in ...) or for ({x} in ...)
      dstrPattern = exprStmt->expression.get();
      dstrIsDecl = false;
    }
  }

  // Per spec (ForIn/OfHeadEvaluation): create TDZ environment for let/const bound names
  // before evaluating the RHS expression.
  auto envBeforeTDZ = env_;
  if (isLetOrConst) {
    std::vector<std::string> tdzNames;
    if (!varName.empty()) {
      tdzNames.push_back(varName);
    } else if (dstrPattern) {
      collectBoundNames(*dstrPattern, tdzNames);
    }
    if (!tdzNames.empty()) {
      auto tdzEnv = env_->createChild();
      for (const auto& name : tdzNames) {
        tdzEnv->defineTDZ(name);
      }
      env_ = tdzEnv;
    }
  }

  // Evaluate the right-hand side (the object to iterate over)
  auto rightTask = evaluate(*stmt.right);
  Value obj;
  LIGHTJS_RUN_TASK(rightTask, obj);

  // Restore env after RHS evaluation (remove TDZ from execution context)
  env_ = envBeforeTDZ;

  // For-in over null/undefined should not execute the body (spec 13.7.5.11 step 5)
  if (obj.isNull() || obj.isUndefined()) {
    env_ = prevEnv;
    LIGHTJS_RETURN(result);
  }

  auto isInternalProp = [](const std::string& key) -> bool {
    return key.size() >= 4 && key.substr(0, 2) == "__" &&
           key.substr(key.size() - 2) == "__";
  };

  auto isMetaProp = [](const std::string& key) -> bool {
    return key.substr(0, 6) == "__get_" || key.substr(0, 6) == "__set_" ||
           key.substr(0, 11) == "__non_enum_" || key.substr(0, 15) == "__non_writable_" ||
           key.substr(0, 19) == "__non_configurable_" || key.substr(0, 7) == "__enum_";
  };

  // Helper to assign a key to the loop variable
  // For let/const, creates a fresh child environment per iteration
  auto assignKey = [&](const std::string& key) -> void {
    if (dstrPattern) {
      // Destructuring pattern: bind key through destructuring
      if (isLetOrConst) {
        auto iterEnv = env_->createChild();
        env_ = iterEnv;
      }
      { auto t = bindDestructuringPattern(*dstrPattern, Value(key), isConst, !dstrIsDecl); LIGHTJS_RUN_TASK_VOID_SYNC(t); }
      if (this->flow_.type == ControlFlow::Type::Yield) return;
      return;
    }
    if (isLetOrConst && !varName.empty()) {
      // Create a fresh scope for each iteration (per-iteration binding)
      auto iterEnv = env_->createChild();
      env_ = iterEnv;
      env_->define(varName, Value(key), isConst);
      return;
    }
    if (memberExpr) {
      if (auto* member = std::get_if<MemberExpr>(&memberExpr->node)) {
        (void)member;
        auto t = bindDestructuringPattern(*memberExpr, Value(key), false, true);
        LIGHTJS_RUN_TASK_VOID_SYNC(t);
        if (this->flow_.type == ControlFlow::Type::Yield) return;
      }
    } else if (!varName.empty()) {
      env_->set(varName, Value(key));
    }
  };

  // Helper to sort keys per spec: integer indices ascending first, then string keys in insertion order.
  // Uses orderedKeys() from OrderedMap for correct insertion-order enumeration.
  auto sortKeys = [](std::vector<std::string>& keys) {
    // Partition into numeric indices and string keys (preserving relative order)
    std::vector<std::string> numericKeys, stringKeys;
    for (const auto& k : keys) {
      bool isNum = !k.empty() && std::all_of(k.begin(), k.end(), ::isdigit);
      if (isNum) {
        numericKeys.push_back(k);
      } else {
        stringKeys.push_back(k);
      }
    }
    // Sort numeric indices in ascending numeric order
    std::sort(numericKeys.begin(), numericKeys.end(), [](const std::string& a, const std::string& b) {
      return std::stoul(a) < std::stoul(b);
    });
    // Reassemble: numeric first, then string keys in insertion order (already in order)
    keys.clear();
    keys.insert(keys.end(), numericKeys.begin(), numericKeys.end());
    keys.insert(keys.end(), stringKeys.begin(), stringKeys.end());
  };

  // Helper to collect enumerable keys from an object (including prototype chain)
  auto collectObjectKeys = [&](const GCPtr<Object>& objPtr) -> std::vector<std::string> {
    std::vector<std::string> keys;
    std::unordered_set<std::string> seen;
    // Walk prototype chain
    auto current = objPtr;
    int depth = 0;
    while (current && depth < 50) {
      // Collect keys at this level using insertion order from OrderedMap
      std::vector<std::string> levelKeys;
      for (const auto& key : current->properties.orderedKeys()) {
        if (isInternalProp(key)) continue;
        if (isMetaProp(key)) continue;
        if (isSymbolPropertyKey(key)) continue;
        if (seen.count(key)) continue;
        levelKeys.push_back(key);
      }
      sortKeys(levelKeys);
      for (const auto& key : levelKeys) {
        seen.insert(key);  // Mark as seen BEFORE enum check (shadows proto)
        if (current->properties.count("__non_enum_" + key)) continue;
        keys.push_back(key);
      }
      // Walk up prototype chain
      auto protoIt = current->properties.find("__proto__");
      if (protoIt != current->properties.end() && protoIt->second.isObject()) {
        current = protoIt->second.getGC<Object>();
        depth++;
      } else {
        break;
      }
    }
    return keys;
  };

  // Iterate over object properties (including prototype chain)
  if (auto* objPtr = std::get_if<GCPtr<Object>>(&obj.data)) {
    std::vector<std::string> keys = collectObjectKeys(*objPtr);

    for (const auto& key : keys) {
      // Check if property still exists (may have been deleted during iteration)
      // Only check on the own object for deletions
      bool exists = false;
      auto current = *objPtr;
      int depth = 0;
      while (current && depth < 50) {
        if ((current->properties.count(key) || current->properties.count("__get_" + key) || current->properties.count("__set_" + key)) &&
            !current->properties.count("__non_enum_" + key)) {
          exists = true;
          break;
        }
        auto protoIt = current->properties.find("__proto__");
        if (protoIt != current->properties.end() && protoIt->second.isObject()) {
          current = protoIt->second.getGC<Object>();
          depth++;
        } else {
          break;
        }
      }
      if (!exists) continue;

      auto loopEnv = env_;  // Save loop environment
      assignKey(key);

      Value bodyResult;
      auto bodyTask = evaluate(*stmt.body);
  LIGHTJS_RUN_TASK(bodyTask, bodyResult);

      // UpdateEmpty: only update V when body result is not empty
      if (!bodyResult.isEmpty()) {
        result = bodyResult;
      }

      if (isLetOrConst) env_ = loopEnv;  // Restore to loop scope

      if (flow_.type == ControlFlow::Type::Break) {
        if (flow_.label.empty()) flow_.type = ControlFlow::Type::None;
        break;
      } else if (flow_.type == ControlFlow::Type::Continue) {
        if (flow_.label.empty() || (!myLabel.empty() && flow_.label == myLabel)) {
          flow_.type = ControlFlow::Type::None;
          flow_.label.clear();
        } else {
          break;
        }
      } else if (flow_.type != ControlFlow::Type::None) {
        break;
      }
    }
  }

  // Iterate over array indices and properties
  else if (auto* arrPtr = std::get_if<GCPtr<Array>>(&obj.data)) {
    std::vector<std::string> keys;
    // Add numeric indices first (skip holes and deleted)
    for (size_t i = 0; i < (*arrPtr)->elements.size(); ++i) {
      auto iStr = std::to_string(i);
      if ((*arrPtr)->properties.count("__hole_" + iStr + "__")) continue;
      if ((*arrPtr)->properties.count("__deleted_" + iStr + "__")) continue;
      keys.push_back(iStr);
    }
    // Add named properties in insertion order
    {
      std::vector<std::string> namedKeys;
      for (const auto& key : (*arrPtr)->properties.orderedKeys()) {
        if (isInternalProp(key)) continue;
        if (isMetaProp(key)) continue;
        if (isSymbolPropertyKey(key)) continue;
        // Skip numeric indices already added above
        bool isNum = !key.empty() && std::all_of(key.begin(), key.end(), ::isdigit);
        if (isNum) continue;
        namedKeys.push_back(key);
      }
      for (const auto& key : namedKeys) {
        if ((*arrPtr)->properties.count("__non_enum_" + key)) continue;
        keys.push_back(key);
      }
    }

    for (const auto& key : keys) {
      auto loopEnv = env_;
      assignKey(key);
      Value bodyResult;
      auto bodyTask = evaluate(*stmt.body);
      LIGHTJS_RUN_TASK(bodyTask, bodyResult);
      if (!bodyResult.isEmpty()) {
        result = bodyResult;
      }
      if (isLetOrConst) env_ = loopEnv;
      if (flow_.type == ControlFlow::Type::Break) {
        if (flow_.label.empty()) flow_.type = ControlFlow::Type::None;
        break;
      } else if (flow_.type == ControlFlow::Type::Continue) {
        if (flow_.label.empty() || (!myLabel.empty() && flow_.label == myLabel)) {
          flow_.type = ControlFlow::Type::None;
          flow_.label.clear();
        } else {
          break;
        }
      } else if (flow_.type != ControlFlow::Type::None) {
        break;
      }
    }
  }

  // Iterate over function properties
  else if (auto* fnPtr = std::get_if<GCPtr<Function>>(&obj.data)) {
    std::vector<std::string> keys;
    for (const auto& [key, _] : (*fnPtr)->properties) {
      if (isInternalProp(key)) continue;
      if (isMetaProp(key)) continue;
      if (isSymbolPropertyKey(key)) continue;
      // name, length, prototype are non-enumerable on functions
      if (key == "name" || key == "length" || key == "prototype") continue;
      // Built-in function properties are non-enumerable unless explicitly marked
      if (!(*fnPtr)->properties.count("__enum_" + key)) continue;
      keys.push_back(key);
    }

    for (const auto& key : keys) {
      auto loopEnv = env_;
      assignKey(key);
      Value bodyResult;
      auto bodyTask = evaluate(*stmt.body);
      LIGHTJS_RUN_TASK(bodyTask, bodyResult);
      if (!bodyResult.isEmpty()) {
        result = bodyResult;
      }
      if (isLetOrConst) env_ = loopEnv;
      if (flow_.type == ControlFlow::Type::Break) {
        if (flow_.label.empty()) flow_.type = ControlFlow::Type::None;
        break;
      } else if (flow_.type == ControlFlow::Type::Continue) {
        if (flow_.label.empty()) flow_.type = ControlFlow::Type::None;
        else break;
      } else if (flow_.type != ControlFlow::Type::None) {
        break;
      }
    }
  }

  // Iterate over typed array indices and enumerable own properties.
  else if (auto* taPtr = std::get_if<GCPtr<TypedArray>>(&obj.data)) {
    std::vector<std::string> keys;
    for (size_t i = 0; i < (*taPtr)->currentLength(); ++i) {
      keys.push_back(std::to_string(i));
    }
    for (const auto& key : (*taPtr)->properties.orderedKeys()) {
      if (isInternalProp(key)) continue;
      if (isMetaProp(key)) continue;
      if (isSymbolPropertyKey(key)) continue;
      bool isNum = !key.empty() && std::all_of(key.begin(), key.end(), ::isdigit);
      if (isNum) continue;
      if ((*taPtr)->properties.count("__non_enum_" + key)) continue;
      keys.push_back(key);
    }

    for (const auto& key : keys) {
      auto loopEnv = env_;
      assignKey(key);
      Value bodyResult;
      auto bodyTask = evaluate(*stmt.body);
      LIGHTJS_RUN_TASK(bodyTask, bodyResult);
      if (!bodyResult.isEmpty()) {
        result = bodyResult;
      }
      if (isLetOrConst) env_ = loopEnv;
      if (flow_.type == ControlFlow::Type::Break) {
        if (flow_.label.empty()) flow_.type = ControlFlow::Type::None;
        break;
      } else if (flow_.type == ControlFlow::Type::Continue) {
        if (flow_.label.empty() || (!myLabel.empty() && flow_.label == myLabel)) {
          flow_.type = ControlFlow::Type::None;
          flow_.label.clear();
        } else {
          break;
        }
      } else if (flow_.type != ControlFlow::Type::None) {
        break;
      }
    }
  }

  // Iterate over class constructor properties.
  else if (auto* clsPtr = std::get_if<GCPtr<Class>>(&obj.data)) {
    std::vector<std::string> keys;
    for (const auto& key : (*clsPtr)->properties.orderedKeys()) {
      if (isInternalProp(key)) continue;
      if (isMetaProp(key)) continue;
      if (isSymbolPropertyKey(key)) continue;
      if ((*clsPtr)->properties.count("__non_enum_" + key)) continue;
      keys.push_back(key);
    }

    for (const auto& key : keys) {
      auto loopEnv = env_;
      assignKey(key);
      Value bodyResult;
      auto bodyTask = evaluate(*stmt.body);
      LIGHTJS_RUN_TASK(bodyTask, bodyResult);
      if (!bodyResult.isEmpty()) {
        result = bodyResult;
      }
      if (isLetOrConst) env_ = loopEnv;
      if (flow_.type == ControlFlow::Type::Break) {
        if (flow_.label.empty()) flow_.type = ControlFlow::Type::None;
        break;
      } else if (flow_.type == ControlFlow::Type::Continue) {
        if (flow_.label.empty() || (!myLabel.empty() && flow_.label == myLabel)) {
          flow_.type = ControlFlow::Type::None;
          flow_.label.clear();
        } else {
          break;
        }
      } else if (flow_.type != ControlFlow::Type::None) {
        break;
      }
    }
  }

  env_ = prevEnv;
  LIGHTJS_RETURN(result);
}

Task Interpreter::evaluateForOf(const ForOfStmt& stmt) {
  auto prevEnv = env_;
  env_ = env_->createChild();

  Value result = Value(Undefined{});
  std::string myLabel = pendingIterationLabel_;
  pendingIterationLabel_.clear();

  // Determine the LHS binding type (before evaluating RHS, for TDZ)
  enum class ForOfLHS { SimpleVar, DestructuringVar, ExpressionTarget };
  ForOfLHS lhsType = ForOfLHS::SimpleVar;
  std::string varName;
  bool isConst = false;
  bool isLetOrConst = false;
  bool isDeclaration = false;  // true for var/let/const declarations, false for expression targets
  const Expression* lhsPattern = nullptr;
  const Expression* lhsExpr = nullptr;

  // Helper to collect bound names from a pattern for TDZ
  std::function<void(const Expression&, std::vector<std::string>&)> collectBoundNames;
  collectBoundNames = [&collectBoundNames](const Expression& expr, std::vector<std::string>& names) {
    if (auto* id = std::get_if<Identifier>(&expr.node)) {
      names.push_back(id->name);
    } else if (auto* arr = std::get_if<ArrayPattern>(&expr.node)) {
      for (const auto& elem : arr->elements) {
        if (elem) collectBoundNames(*elem, names);
      }
      if (arr->rest) collectBoundNames(*arr->rest, names);
    } else if (auto* obj = std::get_if<ObjectPattern>(&expr.node)) {
      for (const auto& prop : obj->properties) {
        if (prop.value) collectBoundNames(*prop.value, names);
      }
      if (obj->rest) collectBoundNames(*obj->rest, names);
    } else if (auto* assign = std::get_if<AssignmentPattern>(&expr.node)) {
      if (assign->left) collectBoundNames(*assign->left, names);
    }
  };

  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.left->node)) {
    isDeclaration = true;
    isConst = (varDecl->kind == VarDeclaration::Kind::Const);
    isLetOrConst = (varDecl->kind == VarDeclaration::Kind::Let ||
                    varDecl->kind == VarDeclaration::Kind::Const);
    if (!varDecl->declarations.empty()) {
      const auto& pattern = varDecl->declarations[0].pattern;
      if (auto* id = std::get_if<Identifier>(&pattern->node)) {
        varName = id->name;
        lhsType = ForOfLHS::SimpleVar;
        // For var declarations, define in the outer scope
        if (varDecl->kind == VarDeclaration::Kind::Var) {
          env_->define(varName, Value(Undefined{}));
        }
      } else {
        lhsType = ForOfLHS::DestructuringVar;
        lhsPattern = pattern.get();
      }
    }
  } else if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt.left->node)) {
    if (auto* ident = std::get_if<Identifier>(&exprStmt->expression->node)) {
      varName = ident->name;
      lhsType = ForOfLHS::SimpleVar;
    } else {
      lhsType = ForOfLHS::ExpressionTarget;
      lhsExpr = exprStmt->expression.get();
    }
  }

  // Per spec (ForIn/OfHeadEvaluation): create TDZ environment for let/const bound names
  // before evaluating the RHS expression. Closures created during RHS evaluation will
  // capture this TDZ env, so accessing the variable will always throw ReferenceError.
  auto envBeforeTDZ = env_;
  if (isLetOrConst) {
    std::vector<std::string> tdzNames;
    if (!varName.empty()) {
      tdzNames.push_back(varName);
    } else if (lhsPattern) {
      collectBoundNames(*lhsPattern, tdzNames);
    }
    if (!tdzNames.empty()) {
      auto tdzEnv = env_->createChild();
      for (const auto& name : tdzNames) {
        tdzEnv->defineTDZ(name);
      }
      env_ = tdzEnv;
    }
  }

  // Evaluate the right-hand side (the iterable to iterate over)
  auto rightTask = evaluate(*stmt.right);
  Value iterable;
  LIGHTJS_RUN_TASK(rightTask, iterable);
  if (hasError()) {
    env_ = prevEnv;
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  // Restore env after RHS evaluation (spec step 4: set LexicalEnvironment to oldEnv)
  // The TDZ child env persists for any closures that captured it.
  env_ = envBeforeTDZ;

  std::optional<IteratorRecord> iteratorOpt;
  if (stmt.isAwait && iterable.isObject()) {
    const auto& asyncIteratorKey = WellKnownSymbols::asyncIteratorKey();
    auto obj = iterable.getGC<Object>();
    auto asyncIt = obj->properties.find(asyncIteratorKey);
    if (asyncIt != obj->properties.end() && asyncIt->second.isFunction()) {
      Value asyncIterValue = callFunction(asyncIt->second, {}, iterable);
      if (asyncIterValue.isObject()) {
        IteratorRecord record;
        record.kind = IteratorRecord::Kind::IteratorObject;
        record.iteratorValue = asyncIterValue;
        auto [foundNext, nextMethod] = getPropertyForPrimitive(asyncIterValue, "next");
        if (flow_.type == ControlFlow::Type::Throw) {
          env_ = prevEnv;
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        if (foundNext) record.nextMethod = nextMethod;
        iteratorOpt = std::move(record);
      }
    }
  }

  if (!iteratorOpt.has_value()) {
    iteratorOpt = getIterator(iterable);
  }
  if (!iteratorOpt.has_value()) {
    env_ = prevEnv;
    throwError(ErrorType::TypeError, "Value is not iterable");
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  auto iterator = std::move(*iteratorOpt);
  while (true) {
    Value stepResult = iteratorNext(iterator);
    // Per spec: if IteratorNext (calling next()) throws, propagate error WITHOUT calling iteratorClose
    if (flow_.type == ControlFlow::Type::Throw) {
      env_ = prevEnv;
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (stmt.isAwait && stepResult.isPromise()) {
      auto promise = stepResult.getGC<Promise>();
      if (promise->state == PromiseState::Rejected) {
        env_ = prevEnv;
        flow_.type = ControlFlow::Type::Throw;
        flow_.value = promise->result;
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (promise->state == PromiseState::Fulfilled) {
        stepResult = promise->result;
      } else {
        break;
      }
    }
    // Per spec (7.4.2 IteratorNext), result must be an Object type
    // In JS, Object includes plain objects, arrays, functions, regex, etc.
    // Helper to extract properties from any object-like Value (supports getters)
    auto getProperty = [this](const Value& val, const std::string& key) -> std::optional<Value> {
      if (val.isProxy()) {
        // Use Proxy get trap
        auto proxyPtr = val.getGC<Proxy>();
        if (proxyPtr->handler && proxyPtr->handler->isObject()) {
          auto handlerObj = std::get<GCPtr<Object>>(proxyPtr->handler->data);
          auto trapIt = handlerObj->properties.find("get");
          if (trapIt != handlerObj->properties.end() && trapIt->second.isFunction()) {
            return callFunction(
                trapIt->second,
                {proxyPtr->target ? *proxyPtr->target : Value(Undefined{}), toPropertyKeyValue(key), val},
                Value(Undefined{}));
          }
        }
        // Fall through to target if no get trap
        if (proxyPtr->target && proxyPtr->target->isObject()) {
          auto obj = std::get<GCPtr<Object>>(proxyPtr->target->data);
          auto it = obj->properties.find(key);
          if (it != obj->properties.end()) return it->second;
        }
        return std::nullopt;
      }
      if (val.isObject()) {
        auto obj = val.getGC<Object>();
        // Check for getter first
        auto getterIt = obj->properties.find("__get_" + key);
        if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
          return callFunction(getterIt->second, {}, val);
        }
        auto it = obj->properties.find(key);
        if (it != obj->properties.end()) return it->second;
      } else if (val.isArray()) {
        auto arr = val.getGC<Array>();
        auto it = arr->properties.find(key);
        if (it != arr->properties.end()) return it->second;
      } else if (val.isFunction()) {
        auto fn = val.getGC<Function>();
        auto it = fn->properties.find(key);
        if (it != fn->properties.end()) return it->second;
      } else if (val.isRegex()) {
        auto rx = val.getGC<Regex>();
        auto it = rx->properties.find(key);
        if (it != rx->properties.end()) return it->second;
      }
      return std::nullopt;
    };

    if (!isObjectLike(stepResult)) {
      if (iterator.kind == IteratorRecord::Kind::IteratorObject) {
        iteratorClose(iterator);
        throwError(ErrorType::TypeError, "Iterator result " + stepResult.toString() + " is not an object");
        env_ = prevEnv;
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      break;
    }

    bool isDone = false;
    if (auto doneOpt = getProperty(stepResult, "done")) {
      isDone = doneOpt->toBool();
    }
    // Per spec: if IteratorStep throws (e.g., getter on 'done'), propagate without closing
    if (flow_.type == ControlFlow::Type::Throw) {
      env_ = prevEnv;
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (isDone) {
      break;
    }

    Value currentValue = Value(Undefined{});
    if (auto valueOpt = getProperty(stepResult, "value")) {
      currentValue = *valueOpt;
    }
    // Per spec: if IteratorValue throws (e.g., getter on 'value'), propagate without closing
    if (flow_.type == ControlFlow::Type::Throw) {
      env_ = prevEnv;
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (stmt.isAwait && currentValue.isPromise()) {
      auto valuePromise = currentValue.getGC<Promise>();
      if (valuePromise->state == PromiseState::Rejected) {
        env_ = prevEnv;
        flow_.type = ControlFlow::Type::Throw;
        flow_.value = valuePromise->result;
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (valuePromise->state == PromiseState::Fulfilled) {
        currentValue = valuePromise->result;
      } else {
        break;
      }
    }

    // Create per-iteration scope for let/const declarations
    auto iterEnv = env_->createChild();
    auto outerEnv = env_;
    env_ = iterEnv;

    auto handleAbruptBindingCompletion = [&]() -> bool {
      if (flow_.type == ControlFlow::Type::None ||
          flow_.type == ControlFlow::Type::Yield) {
        return false;
      }
      if (flow_.type == ControlFlow::Type::Throw) {
        // Per spec IteratorClose step 5: original throw completion always wins.
        auto savedFlow = flow_;
        flow_.type = ControlFlow::Type::None;
        iteratorClose(iterator);
        flow_ = savedFlow;
      } else {
        iteratorClose(iterator);
      }
      env_ = prevEnv;
      return true;
    };

    // Assign value to LHS
    if (lhsType == ForOfLHS::SimpleVar) {
      if (isConst) {
        // let/const declaration: define in per-iteration scope
        env_->define(varName, currentValue, true);
      } else if (!varName.empty() && env_->isConst(varName)) {
        // Expression target assigning to const variable
        throwError(ErrorType::TypeError, "Assignment to constant variable '" + varName + "'");
        env_ = prevEnv;
        LIGHTJS_RETURN(Value(Undefined{}));
      } else if (isDeclaration) {
        // var/let declaration: define in scope
        env_->define(varName, currentValue);
      } else {
        // Expression target: update existing variable in parent scope
        if (!env_->set(varName, currentValue)) {
          env_->define(varName, currentValue);
        }
      }
    } else if (lhsType == ForOfLHS::DestructuringVar) {
      { auto t = bindDestructuringPattern(*lhsPattern, currentValue, isConst); LIGHTJS_RUN_TASK_VOID(t); }
      if (handleAbruptBindingCompletion()) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
    } else if (lhsType == ForOfLHS::ExpressionTarget && lhsExpr) {
      // Bare destructuring assignment or member expression target
      if (std::get_if<ArrayPattern>(&lhsExpr->node) || std::get_if<ObjectPattern>(&lhsExpr->node)) {
        // Destructuring assignment (for ({a, b} of ...) or for ([a, b] of ...))
        { auto t = bindDestructuringPattern(*lhsExpr, currentValue, false, true); LIGHTJS_RUN_TASK_VOID(t); }
        if (handleAbruptBindingCompletion()) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      } else if (auto* member = std::get_if<MemberExpr>(& lhsExpr->node)) {
        (void)member;
        auto t = bindDestructuringPattern(*lhsExpr, currentValue, false, true);
        LIGHTJS_RUN_TASK_VOID(t);
        if (handleAbruptBindingCompletion()) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      }
    }

    Value bodyResult;
    auto bodyTask = evaluate(*stmt.body);
    LIGHTJS_RUN_TASK(bodyTask, bodyResult);

    // UpdateEmpty: only update V when body result is not empty
    if (!bodyResult.isEmpty()) {
      result = bodyResult;
    }

    // Restore to outer loop env
    env_ = outerEnv;

    if (flow_.type == ControlFlow::Type::Break) {
      if (flow_.label.empty()) flow_.type = ControlFlow::Type::None;
      iteratorClose(iterator);
      break;
    } else if (flow_.type == ControlFlow::Type::Continue) {
      if (flow_.label.empty() || (!myLabel.empty() && flow_.label == myLabel)) {
        flow_.type = ControlFlow::Type::None;
        flow_.label.clear();
        // continue this loop
      } else {
        iteratorClose(iterator);
        break;
      }
    } else if (flow_.type == ControlFlow::Type::Return) {
      iteratorClose(iterator);
      break;
    } else if (flow_.type == ControlFlow::Type::Throw) {
      // For throw completion, still try to close iterator but preserve the throw
      auto savedFlow = flow_;
      flow_.type = ControlFlow::Type::None;
      iteratorClose(iterator);
      // AsyncIteratorClose preserves the original throw completion even if
      // retrieving/calling `return` also throws.
      if (stmt.isAwait || flow_.type == ControlFlow::Type::None) {
        flow_ = savedFlow;
      }
      break;
    } else if (flow_.type != ControlFlow::Type::None) {
      iteratorClose(iterator);
      break;
    }
  }

  env_ = prevEnv;
  LIGHTJS_RETURN(result);
}

Task Interpreter::evaluateSwitch(const SwitchStmt& stmt) {
  // Evaluate the discriminant
  auto discriminantTask = evaluate(*stmt.discriminant);
  Value discriminant;
  LIGHTJS_RUN_TASK(discriminantTask, discriminant);

  // Create a new lexical environment for the entire switch block (ES spec 13.12.11)
  auto prevEnv = env_;
  env_ = env_->createChild();

  Value result = Value(Undefined{});
  bool foundMatch = false;
  bool hasDefault = false;
  size_t defaultIndex = 0;

  // Find default case if any
  for (size_t i = 0; i < stmt.cases.size(); i++) {
    if (!stmt.cases[i].test) {
      hasDefault = true;
      defaultIndex = i;
      break;
    }
  }

  // First pass: find matching case
  for (size_t i = 0; i < stmt.cases.size(); i++) {
    const auto& caseClause = stmt.cases[i];

    if (caseClause.test) {
      auto testTask = evaluate(*caseClause.test);
  Value testValue;
  LIGHTJS_RUN_TASK(testTask, testValue);

      // Perform strict equality check (===)
      bool isEqual = false;
      if (discriminant.isBigInt() && testValue.isBigInt()) {
        isEqual = (discriminant.toBigInt() == testValue.toBigInt());
      } else if (discriminant.isNumber() && testValue.isNumber()) {
        isEqual = (discriminant.toNumber() == testValue.toNumber());
      } else if (discriminant.isString() && testValue.isString()) {
        isEqual = (discriminant.toString() == testValue.toString());
      } else if (discriminant.isBool() && testValue.isBool()) {
        isEqual = (discriminant.toBool() == testValue.toBool());
      } else if (discriminant.isNull() && testValue.isNull()) {
        isEqual = true;
      } else if (discriminant.isUndefined() && testValue.isUndefined()) {
        isEqual = true;
      } else if (discriminant.isFunction() && testValue.isFunction()) {
        isEqual = (discriminant.getGC<Function>().get() ==
                   testValue.getGC<Function>().get());
      } else if (discriminant.isObject() && testValue.isObject()) {
        isEqual = (discriminant.getGC<Object>().get() ==
                   testValue.getGC<Object>().get());
      } else if (discriminant.isArray() && testValue.isArray()) {
        isEqual = (discriminant.getGC<Array>().get() ==
                   testValue.getGC<Array>().get());
      }

      if (isEqual) {
        foundMatch = true;
      }
    }

    // Execute if we found a match or if we're in fall-through mode
    if (foundMatch) {
      for (const auto& consequentStmt : caseClause.consequent) {
        Value stmtResult;
        auto stmtTask = evaluate(*consequentStmt);
  LIGHTJS_RUN_TASK(stmtTask, stmtResult);

        // UpdateEmpty semantics (ES spec 13.12.9):
        // Update V when R.[[value]] is not empty.
        if (!stmtResult.isEmpty()) {
          result = stmtResult;
        }

        if (flow_.type == ControlFlow::Type::Break) {
          if (flow_.label.empty()) flow_.type = ControlFlow::Type::None;
          env_ = prevEnv;
          LIGHTJS_RETURN(result);
        } else if (flow_.type != ControlFlow::Type::None) {
          env_ = prevEnv;
          LIGHTJS_RETURN(result);
        }
      }
    }
  }

  // If no match found, execute default case and fall through to subsequent cases
  if (!foundMatch && hasDefault) {
    for (size_t i = defaultIndex; i < stmt.cases.size(); ++i) {
      const auto& caseClause = stmt.cases[i];
      for (const auto& consequentStmt : caseClause.consequent) {
        Value stmtResult;
        auto stmtTask = evaluate(*consequentStmt);
  LIGHTJS_RUN_TASK(stmtTask, stmtResult);

        // UpdateEmpty semantics (ES spec 13.12.9):
        // Update V when R.[[value]] is not empty.
        if (!stmtResult.isEmpty()) {
          result = stmtResult;
        }

        if (flow_.type == ControlFlow::Type::Break) {
          if (flow_.label.empty()) flow_.type = ControlFlow::Type::None;
          env_ = prevEnv;
          LIGHTJS_RETURN(result);
        } else if (flow_.type != ControlFlow::Type::None) {
          env_ = prevEnv;
          LIGHTJS_RETURN(result);
        }
      }
    }
  }

  env_ = prevEnv;
  LIGHTJS_RETURN(result);
}

Task Interpreter::evaluateTry(const TryStmt& stmt) {
  auto prevFlow = flow_;
  Value result = Value(Undefined{});
  auto envBeforeTry = env_;

  // Create block scope for try block (let/const should be scoped here)
  auto tryBlockEnv = env_->createChild();
  env_ = tryBlockEnv;

  for (const auto& s : stmt.block) {
    Value stmtResult;
    auto task = evaluate(*s);
  LIGHTJS_RUN_TASK(task, stmtResult);
    if (!stmtResult.isEmpty()) {
      result = stmtResult;
    }

    if (flow_.type == ControlFlow::Type::Throw && stmt.hasHandler) {
      // Restore environment to before the try block
      env_ = envBeforeTry;

      // Per spec: create catch parameter environment
      auto catchParamEnv = env_->createChild();
      auto prevEnv = env_;
      env_ = catchParamEnv;

      // Save thrown value and clear flow BEFORE binding - bindDestructuringPattern
      // checks flow_.type == Throw internally and would exit early otherwise
      Value thrownValue = flow_.value;
      flow_.type = ControlFlow::Type::None;

      if (stmt.handler.paramPattern) {
        { auto t = bindDestructuringPattern(*stmt.handler.paramPattern, thrownValue, false); LIGHTJS_RUN_TASK_VOID(t); }
      } else if (!stmt.handler.param.name.empty()) {
        env_->define(stmt.handler.param.name, thrownValue);
      }

      // Per spec: create separate block environment (child of param env)
      // so closures in param initializers vs block body capture different scopes
      auto catchBlockEnv = env_->createChild();
      env_ = catchBlockEnv;

      for (const auto& catchStmt : stmt.handler.body) {
        Value catchResult;
        auto catchTask = evaluate(*catchStmt);
  LIGHTJS_RUN_TASK(catchTask, catchResult);
        if (!catchResult.isEmpty()) {
          result = catchResult;
        }
        if (flow_.type != ControlFlow::Type::None) {
          break;
        }
      }

      env_ = prevEnv;
      break;
    }

    if (flow_.type != ControlFlow::Type::None) {
      break;
    }
  }

  // Restore environment after try block (unwind try block scope)
  if (env_ == tryBlockEnv) {
    env_ = envBeforeTry;
  }

  if (stmt.hasFinalizer) {
    // In generators, yield should NOT trigger finally — only run finally on
    // return/throw/normal completion, not on yield suspension
    if (flow_.type == ControlFlow::Type::Yield) {
      LIGHTJS_RETURN(result);
    }

    // Save current control flow state (from try/catch)
    auto savedFlow = flow_;
    flow_.type = ControlFlow::Type::None;
    flow_.label.clear();

    Value finallyValue = Value(Undefined{});
    auto finallyEnv = env_->createChild();
    auto envBeforeFinally = env_;
    env_ = finallyEnv;
    for (const auto& finalStmt : stmt.finalizer) {
      auto finalTask = evaluate(*finalStmt);
      Value finalResult;
      LIGHTJS_RUN_TASK(finalTask, finalResult);
      // UpdateEmpty semantics: track last non-empty completion value
      if (!finalResult.isEmpty()) {
        finallyValue = finalResult;
      }
      if (flow_.type != ControlFlow::Type::None) {
        break;
      }
    }
    env_ = envBeforeFinally;

    // If finally block produced its own control flow, it overrides try/catch's
    // and its completion value replaces the try/catch result
    if (flow_.type == ControlFlow::Type::None) {
      flow_ = savedFlow;
    } else {
      // Per spec: Return Completion(UpdateEmpty(F, undefined))
      // The finally block's abrupt completion overrides try/catch.
      // Carry the completion value through breakCompletionValue so
      // enclosing loops can use it (avoids the empty-vs-undefined problem).
      if (flow_.type == ControlFlow::Type::Break ||
          flow_.type == ControlFlow::Type::Continue) {
        flow_.breakCompletionValue = finallyValue;
      }
      result = finallyValue;
    }
  }

  LIGHTJS_RETURN(result);
}

Task Interpreter::evaluateImport(const ImportDeclaration& stmt) {
  auto importFnValue = env_->get("import");
  if (!importFnValue || !importFnValue->isFunction()) {
    throwError(ErrorType::ReferenceError, "import is not defined");
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  Value importResult = callFunction(*importFnValue, {Value(stmt.source)}, Value(Undefined{}));
  if (hasError()) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }
  if (!importResult.isPromise()) {
    throwError(ErrorType::TypeError, "import() did not return a Promise");
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  auto promise = importResult.getGC<Promise>();
  if (promise->state == PromiseState::Rejected) {
    flow_.type = ControlFlow::Type::Throw;
    flow_.value = promise->result;
    LIGHTJS_RETURN(Value(Undefined{}));
  }
  if (promise->state != PromiseState::Fulfilled || !promise->result.isObject()) {
    throwError(ErrorType::Error, "Failed to resolve import '" + stmt.source + "'");
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  Value namespaceValue = promise->result;
  auto namespaceObj = namespaceValue.getGC<Object>();

  auto hasExport = [&](const std::string& name) -> bool {
    if (namespaceObj->isModuleNamespace) {
      return std::find(namespaceObj->moduleExportNames.begin(),
                       namespaceObj->moduleExportNames.end(),
                       name) != namespaceObj->moduleExportNames.end();
    }
    return namespaceObj->properties.find(name) != namespaceObj->properties.end();
  };

  auto readExport = [&](const std::string& name) -> Value {
    if (namespaceObj->isModuleNamespace) {
      auto getterIt = namespaceObj->properties.find("__get_" + name);
      if (getterIt != namespaceObj->properties.end() && getterIt->second.isFunction()) {
        Value v = callFunction(getterIt->second, {}, namespaceValue);
        if (hasError()) {
          return Value(Undefined{});
        }
        return v;
      }
    }
    auto it = namespaceObj->properties.find(name);
    if (it != namespaceObj->properties.end()) {
      return it->second;
    }
    return Value(Undefined{});
  };

  if (stmt.defaultImport) {
    if (!hasExport("default")) {
      throwError(ErrorType::SyntaxError, "Module '" + stmt.source + "' does not export 'default'");
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    env_->define(stmt.defaultImport->name, readExport("default"));
  }

  if (stmt.namespaceImport) {
    env_->define(stmt.namespaceImport->name, namespaceValue);
  }

  for (const auto& spec : stmt.specifiers) {
    const std::string& importedName = spec.imported.name;
    if (!hasExport(importedName)) {
      throwError(ErrorType::SyntaxError, "Module '" + stmt.source + "' does not export '" + importedName + "'");
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    env_->define(spec.local.name, readExport(importedName));
  }

  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::evaluateExportNamed(const ExportNamedDeclaration& stmt) {
  // If there's a declaration, evaluate it
  if (stmt.declaration) {
    { auto _t = evaluate(*stmt.declaration); Value _v; LIGHTJS_RUN_TASK(_t, _v); LIGHTJS_RETURN(_v); }
  }

  // Export bindings are handled at the module level
  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::evaluateExportDefault(const ExportDefaultDeclaration& stmt) {
  auto previousPendingAnonymousClassName = pendingAnonymousClassName_;
  struct PendingAnonymousClassNameGuard {
    Interpreter* interpreter;
    std::optional<std::string> previous;
    ~PendingAnonymousClassNameGuard() {
      interpreter->pendingAnonymousClassName_ = previous;
    }
  } pendingAnonymousClassNameGuard{this, previousPendingAnonymousClassName};

  if (auto* classExpr = std::get_if<ClassExpr>(&stmt.declaration->node);
      classExpr && classExpr->name.empty()) {
    pendingAnonymousClassName_ = std::string("default");
  }

  // Evaluate the expression being exported
  auto task = evaluate(*stmt.declaration);
  LIGHTJS_RUN_TASK_VOID(task);

  // The module system will capture this value
  LIGHTJS_RETURN(task.result());
}

Task Interpreter::evaluateExportAll(const ExportAllDeclaration& stmt) {
  // Re-exports are handled at the module level
  LIGHTJS_RETURN(Value(Undefined{}));
}

double toNumberES(const Value& v) {
  // For primitives, just call toNumber directly
  if (!v.isObject() && !v.isArray() && !v.isFunction() && !v.isClass() &&
      !v.isPromise() && !v.isMap() && !v.isSet() && !v.isRegex() &&
      !v.isProxy() && !v.isGenerator() && !v.isWeakMap() && !v.isWeakSet() &&
      !v.isTypedArray() && !v.isArrayBuffer() && !v.isDataView() && !v.isError()) {
    return v.toNumber();
  }
  // For objects, call ToPrimitive with hint "number" via the global interpreter
  auto* interp = getGlobalInterpreter();
  if (!interp) {
    return v.toNumber(); // fallback
  }
  Value prim = interp->toPrimitive(v, false); // preferString=false => hint "number"
  return prim.toNumber();
}

}
