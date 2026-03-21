#include "value.h"
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
#include <limits>
#include <cstdlib>
#include <cctype>

namespace lightjs {

namespace {
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

void writeTypedArrayNumberUnchecked(TypedArrayType type, uint8_t* ptr, double value) {
  switch (type) {
    case TypedArrayType::Int8:
      *reinterpret_cast<int8_t*>(ptr) = static_cast<int8_t>(toUintN<uint8_t>(value));
      break;
    case TypedArrayType::Uint8:
      *reinterpret_cast<uint8_t*>(ptr) = toUintN<uint8_t>(value);
      break;
    case TypedArrayType::Uint8Clamped:
      *reinterpret_cast<uint8_t*>(ptr) = toUint8Clamp(value);
      break;
    case TypedArrayType::Int16:
      *reinterpret_cast<int16_t*>(ptr) = static_cast<int16_t>(toUintN<uint16_t>(value));
      break;
    case TypedArrayType::Uint16:
      *reinterpret_cast<uint16_t*>(ptr) = toUintN<uint16_t>(value);
      break;
    case TypedArrayType::Float16:
      *reinterpret_cast<uint16_t*>(ptr) = float64_to_float16(value);
      break;
    case TypedArrayType::Int32:
      *reinterpret_cast<int32_t*>(ptr) = toInt32Element(value);
      break;
    case TypedArrayType::Uint32:
      *reinterpret_cast<uint32_t*>(ptr) = toUintN<uint32_t>(value);
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

// Initialize static member for Symbol IDs
size_t Symbol::nextId = 0;

namespace {
constexpr const char* kSymbolPropertyKeyPrefix = "@@sym:";
}  // namespace

std::string symbolToPropertyKey(const Symbol& symbol) {
  return std::string(kSymbolPropertyKeyPrefix) + std::to_string(symbol.id) + ":" + symbol.description;
}

bool isSymbolPropertyKey(const std::string& key) {
  if (key.rfind(kSymbolPropertyKeyPrefix, 0) != 0) {
    return false;
  }
  const size_t idStart = std::char_traits<char>::length(kSymbolPropertyKeyPrefix);
  const size_t colonPos = key.find(':', idStart);
  if (colonPos == std::string::npos || colonPos == idStart) {
    return false;
  }
  for (size_t i = idStart; i < colonPos; ++i) {
    if (key[i] < '0' || key[i] > '9') {
      return false;
    }
  }
  return true;
}

bool propertyKeyToSymbol(const std::string& key, Symbol& outSymbol) {
  if (!isSymbolPropertyKey(key)) {
    return false;
  }
  const size_t idStart = std::char_traits<char>::length(kSymbolPropertyKeyPrefix);
  const size_t colonPos = key.find(':', idStart);
  size_t symbolId = 0;
  try {
    symbolId = std::stoull(key.substr(idStart, colonPos - idStart));
  } catch (...) {
    return false;
  }
  const std::string description = key.substr(colonPos + 1);
  outSymbol = Symbol(symbolId, description);
  return true;
}

// ES spec Unicode whitespace check at byte position in UTF-8 string.
// Returns true if the byte(s) at pos form a whitespace code point, sets advance to byte count.
bool isESWhitespace(const std::string& str, size_t pos, size_t& advance) {
  if (pos >= str.size()) { advance = 0; return false; }
  unsigned char c = static_cast<unsigned char>(str[pos]);
  // ASCII whitespace: TAB, LF, VT, FF, CR, SP
  if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
    advance = 1; return true;
  }
  // 2-byte: U+00A0 NBSP (C2 A0)
  if (c == 0xC2 && pos + 1 < str.size() && static_cast<unsigned char>(str[pos+1]) == 0xA0) {
    advance = 2; return true;
  }
  // 3-byte sequences
  if (pos + 2 < str.size()) {
    unsigned char c2 = static_cast<unsigned char>(str[pos+1]);
    unsigned char c3 = static_cast<unsigned char>(str[pos+2]);
    if (c == 0xE1 && c2 == 0x9A && c3 == 0x80) { advance = 3; return true; } // U+1680
    if (c == 0xE2 && c2 == 0x80 && c3 >= 0x80 && c3 <= 0x8A) { advance = 3; return true; } // U+2000-U+200A
    if (c == 0xE2 && c2 == 0x80 && (c3 == 0xA8 || c3 == 0xA9)) { advance = 3; return true; } // U+2028/2029
    if (c == 0xE2 && c2 == 0x80 && c3 == 0xAF) { advance = 3; return true; } // U+202F
    if (c == 0xE2 && c2 == 0x81 && c3 == 0x9F) { advance = 3; return true; } // U+205F
    if (c == 0xE3 && c2 == 0x80 && c3 == 0x80) { advance = 3; return true; } // U+3000
    if (c == 0xEF && c2 == 0xBB && c3 == 0xBF) { advance = 3; return true; } // U+FEFF
  }
  advance = 1;
  return false;
}

std::string stripLeadingESWhitespace(const std::string& str) {
  size_t pos = 0;
  while (pos < str.size()) {
    size_t adv = 1;
    if (!isESWhitespace(str, pos, adv)) break;
    pos += adv;
  }
  return str.substr(pos);
}

std::string stripTrailingESWhitespace(const std::string& str) {
  size_t end = str.size();
  while (end > 0) {
    bool found = false;
    if (end >= 3) {
      size_t adv = 1;
      if (isESWhitespace(str, end - 3, adv) && adv == 3) {
        end -= 3; found = true; continue;
      }
    }
    if (!found && end >= 2) {
      size_t adv = 1;
      if (isESWhitespace(str, end - 2, adv) && adv == 2) {
        end -= 2; found = true; continue;
      }
    }
    if (!found) {
      unsigned char c = static_cast<unsigned char>(str[end - 1]);
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
        end -= 1; continue;
      }
      break;
    }
  }
  return str.substr(0, end);
}

std::string stripESWhitespace(const std::string& str) {
  return stripTrailingESWhitespace(stripLeadingESWhitespace(str));
}

std::string ecmaNumberToString(double number) {
  if (std::isnan(number)) return "NaN";
  if (std::isinf(number)) return number < 0 ? "-Infinity" : "Infinity";
  if (number == 0.0) return "0";

  // Find shortest decimal representation that round-trips
  char buf[64];
  int prec;
  for (prec = 1; prec <= 21; prec++) {
    snprintf(buf, sizeof(buf), "%.*g", prec, number);
    char* end;
    double parsed = strtod(buf, &end);
    if (parsed == number) break;
  }
  // buf now has the shortest representation in %g format
  std::string s(buf);

  // Parse the %g output and reformat per ES spec
  double absNumber = std::fabs(number);
  std::string sign;
  std::string digits;
  int exponent = 0;

  auto epos = s.find_first_of("eE");
  if (epos != std::string::npos) {
    // %g chose scientific notation
    std::string mPart = s.substr(0, epos);
    exponent = std::stoi(s.substr(epos + 1));
    // Remove sign from mantissa
    size_t mStart = 0;
    if (!mPart.empty() && (mPart[0] == '-' || mPart[0] == '+')) mStart = 1;
    for (size_t i = mStart; i < mPart.size(); i++) {
      if (mPart[i] >= '0' && mPart[i] <= '9') digits += mPart[i];
    }
  } else {
    // %g chose fixed notation
    size_t mStart = 0;
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) mStart = 1;
    auto dotPos = s.find('.');
    if (dotPos != std::string::npos) {
      std::string intPart = s.substr(mStart, dotPos - mStart);
      std::string fracPart = s.substr(dotPos + 1);
      if (intPart == "0" || intPart.empty()) {
        // e.g., "0.001" → digits from frac, count leading zeros for exponent
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
        digits = (firstNonZero != std::string::npos) ? digits.substr(firstNonZero) : "0";
        exponent = -(leadingZeros + 1);
      } else {
        digits = intPart + fracPart;
        exponent = static_cast<int>(intPart.size()) - 1;
      }
    } else {
      // Pure integer from %g (e.g., "100")
      digits = s.substr(mStart);
      // Remove trailing zeros to get significant digits, but track original length for exponent
      exponent = static_cast<int>(digits.size()) - 1;
      while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    }
  }

  int n = static_cast<int>(digits.size()); // number of significant digits
  int k = n;
  int e = exponent; // exponent such that value = digits * 10^(e-k+1)

  // ES spec formatting rules (7.1.12.1):
  // Let s be sign, k = number of digits, n = exponent + 1
  int nn = e + 1; // ES spec's n
  if (number < 0) sign = "-";

  if (k <= nn && nn <= 21) {
    // Case: integer-like, no exponent needed
    // m = digits + (nn-k) zeros
    return sign + digits + std::string(nn - k, '0');
  }
  if (0 < nn && nn <= 21) {
    // Case: insert decimal point
    return sign + digits.substr(0, nn) + "." + digits.substr(nn);
  }
  if (-6 < nn && nn <= 0) {
    // Case: 0.000...digits
    return sign + "0." + std::string(-nn, '0') + digits;
  }
  // Case: exponential notation
  std::string result = sign;
  if (k == 1) {
    result += digits;
  } else {
    result += digits.substr(0, 1) + "." + digits.substr(1);
  }
  result += "e";
  result += (e >= 0) ? "+" : "-";
  result += std::to_string(std::abs(e));
  return result;
}

std::string valueToPropertyKey(const Value& value) {
  if (value.isSymbol()) {
    return symbolToPropertyKey(std::get<Symbol>(value.data));
  }
  if (value.isNumber()) {
    return ecmaNumberToString(value.toNumber());
  }
  // ToPrimitive for objects: call toString/valueOf
  if (value.isObject()) {
    auto obj = value.getGC<Object>();
    auto primIt = obj->properties.find("__primitive_value__");
    if (primIt != obj->properties.end()) {
      if (primIt->second.isSymbol()) {
        return symbolToPropertyKey(std::get<Symbol>(primIt->second.data));
      }
      return primIt->second.toString();
    }
    // Try calling toString() then valueOf() via the interpreter
    auto* interp = getGlobalInterpreter();
    if (interp) {
      // Check for own toString method first, then prototype chain
      auto findMethod = [&](const std::string& methodName) -> Value {
        auto it = obj->properties.find(methodName);
        if (it != obj->properties.end() && it->second.isFunction()) {
          return it->second;
        }
        // Walk prototype chain
        auto protoIt = obj->properties.find("__proto__");
        if (protoIt != obj->properties.end() && protoIt->second.isObject()) {
          auto proto = protoIt->second.getGC<Object>();
          int depth = 0;
          while (proto && depth < 20) {
            auto methodIt = proto->properties.find(methodName);
            if (methodIt != proto->properties.end() && methodIt->second.isFunction()) {
              return methodIt->second;
            }
            auto nextIt = proto->properties.find("__proto__");
            if (nextIt == proto->properties.end() || !nextIt->second.isObject()) break;
            proto = nextIt->second.getGC<Object>();
            depth++;
          }
        }
        return Value(Undefined{});
      };
      // ES spec: ToPrimitive with hint "string" tries toString first, then valueOf
      Value toStringMethod = findMethod("toString");
      if (toStringMethod.isFunction()) {
        Value result = interp->callForHarness(toStringMethod, {}, value);
        if (!result.isObject() && !result.isArray() && !result.isFunction()) {
          if (result.isSymbol()) {
            return symbolToPropertyKey(std::get<Symbol>(result.data));
          }
          return result.toString();
        }
      }
      Value valueOfMethod = findMethod("valueOf");
      if (valueOfMethod.isFunction()) {
        Value result = interp->callForHarness(valueOfMethod, {}, value);
        if (!result.isObject() && !result.isArray() && !result.isFunction()) {
          if (result.isSymbol()) {
            return symbolToPropertyKey(std::get<Symbol>(result.data));
          }
          return result.toString();
        }
      }
      // Both returned objects: throw TypeError
      throw std::runtime_error("TypeError: Cannot convert object to primitive value");
    }
  }
  if (value.isArray()) {
    // Arrays: try toString via join
    auto arr = value.getGC<Array>();
    std::string result;
    for (size_t i = 0; i < arr->elements.size(); i++) {
      if (i > 0) result += ",";
      if (!arr->elements[i].isUndefined() && !arr->elements[i].isNull()) {
        result += arr->elements[i].toString();
      }
    }
    return result;
  }
  return value.toString();
}

bool Value::toBool() const {
  return std::visit([](auto&& arg) -> bool {
    using T = std::decay_t<decltype(arg)>;
    if constexpr (std::is_same_v<T, Undefined> || std::is_same_v<T, Empty>) {
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
    if constexpr (std::is_same_v<T, Undefined> || std::is_same_v<T, Empty>) {
      return std::nan("");
    } else if constexpr (std::is_same_v<T, Null>) {
      return 0.0;
    } else if constexpr (std::is_same_v<T, bool>) {
      return arg ? 1.0 : 0.0;
    } else if constexpr (std::is_same_v<T, double>) {
      return arg;
    } else if constexpr (std::is_same_v<T, BigInt>) {
      return arg.value.template convert_to<double>();
    } else if constexpr (std::is_same_v<T, std::string>) {
      std::string s = stripESWhitespace(arg);
      if (s.empty()) {
        return 0.0;
      }

      // ECMAScript ToNumber(String) recognizes "Infinity" (case-sensitive),
      // but should not accept "inf"/"infinity" in other casings even though
      // libc strtod() does.
      if (s == "Infinity" || s == "+Infinity") {
        return std::numeric_limits<double>::infinity();
      }
      if (s == "-Infinity") {
        return -std::numeric_limits<double>::infinity();
      }
      auto lowerAscii = [](std::string in) {
        for (char& ch : in) {
          if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        }
        return in;
      };
      std::string lower = lowerAscii(s);
      if (lower == "inf" || lower == "+inf" || lower == "-inf" ||
          lower == "infinity" || lower == "+infinity" || lower == "-infinity") {
        return std::nan("");
      }

      // Handle binary literals: 0b/0B (no sign allowed)
      if (s.size() >= 3 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
        double result = 0.0;
        for (size_t i = 2; i < s.size(); ++i) {
          if (s[i] != '0' && s[i] != '1') return std::nan("");
          result = result * 2 + (s[i] - '0');
        }
        return result;
      }

      // Handle octal literals: 0o/0O (no sign allowed)
      if (s.size() >= 3 && s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) {
        double result = 0.0;
        for (size_t i = 2; i < s.size(); ++i) {
          if (s[i] < '0' || s[i] > '7') return std::nan("");
          result = result * 8 + (s[i] - '0');
        }
        return result;
      }

      // Handle hex literals: 0x/0X (no sign allowed per ES spec)
      if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        if (s.size() == 2) return std::nan("");  // "0x" alone
        double result = 0.0;
        for (size_t i = 2; i < s.size(); ++i) {
          char c = s[i];
          int digit;
          if (c >= '0' && c <= '9') digit = c - '0';
          else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
          else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
          else return std::nan("");  // invalid char, decimal point, etc.
          result = result * 16 + digit;
        }
        return result;
      }

      // Reject signed hex/binary/octal: +0x..., -0x..., etc.
      if (s.size() >= 3 && (s[0] == '+' || s[0] == '-') &&
          s[1] == '0' && (s[2] == 'x' || s[2] == 'X' || s[2] == 'b' || s[2] == 'B' || s[2] == 'o' || s[2] == 'O')) {
        return std::nan("");
      }

      char* parseEnd = nullptr;
      errno = 0;
      double parsed = std::strtod(s.c_str(), &parseEnd);
      if (parseEnd == s.c_str()) {
        return std::nan("");
      }
      if (*parseEnd != '\0') {
        return std::nan("");
      }
      // Reject non-spec infinity from strtod
      if (std::isinf(parsed) && s != "Infinity" && s != "+Infinity" && s != "-Infinity") {
        // Only if it wasn't a genuine overflow
        if (errno != ERANGE) return std::nan("");
      }
      return parsed;
    } else {
      return std::nan("");
    }
  }, data);
}

bigint::BigIntValue Value::toBigInt() const {
  return std::visit([](auto&& arg) -> bigint::BigIntValue {
    using T = std::decay_t<decltype(arg)>;
    if constexpr (std::is_same_v<T, Undefined> || std::is_same_v<T, Empty>) {
      return 0;
    } else if constexpr (std::is_same_v<T, Null>) {
      return 0;
    } else if constexpr (std::is_same_v<T, bool>) {
      return arg ? 1 : 0;
    } else if constexpr (std::is_same_v<T, double>) {
      bigint::BigIntValue out = 0;
      if (bigint::fromIntegralDouble(arg, out)) return out;
      if (!std::isfinite(arg)) return 0;
      return static_cast<int64_t>(arg);
    } else if constexpr (std::is_same_v<T, BigInt>) {
      return arg.value;
    } else if constexpr (std::is_same_v<T, std::string>) {
      bigint::BigIntValue parsed = 0;
      if (bigint::parseBigIntString(arg, parsed)) return parsed;
      return 0;
    } else {
      return 0;
    }
  }, data);
}

std::string Value::toString() const {
  return std::visit([](auto&& arg) -> std::string {
    using T = std::decay_t<decltype(arg)>;
    if constexpr (std::is_same_v<T, Undefined> || std::is_same_v<T, Empty>) {
      return "undefined";
    } else if constexpr (std::is_same_v<T, Null>) {
      return "null";
    } else if constexpr (std::is_same_v<T, bool>) {
      return arg ? "true" : "false";
    } else if constexpr (std::is_same_v<T, double>) {
      return ecmaNumberToString(arg);
    } else if constexpr (std::is_same_v<T, BigInt>) {
      return bigint::toString(arg.value);
    } else if constexpr (std::is_same_v<T, Symbol>) {
      if (arg.hasDescription) {
        return "Symbol(" + arg.description + ")";
      }
      return "Symbol()";
    } else if constexpr (std::is_same_v<T, ModuleBinding>) {
      return "[ModuleBinding]";
    } else if constexpr (std::is_same_v<T, std::string>) {
      return arg;
    } else if constexpr (std::is_same_v<T, GCPtr<Function>>) {
      return "[Function]";
    } else if constexpr (std::is_same_v<T, GCPtr<Class>>) {
      return "[Function]";
    } else if constexpr (std::is_same_v<T, GCPtr<Array>>) {
      return "[Array]";
    } else if constexpr (std::is_same_v<T, GCPtr<Object>>) {
      return "[Object]";
    } else if constexpr (std::is_same_v<T, GCPtr<TypedArray>>) {
      return "[TypedArray]";
    } else if constexpr (std::is_same_v<T, GCPtr<Promise>>) {
      return "[Promise]";
    } else if constexpr (std::is_same_v<T, GCPtr<Regex>>) {
      return "/" + arg->pattern + "/" + arg->flags;
    } else if constexpr (std::is_same_v<T, GCPtr<Error>>) {
      return arg->toString();
    } else if constexpr (std::is_same_v<T, GCPtr<Generator>>) {
      return "[Generator]";
    } else if constexpr (std::is_same_v<T, GCPtr<Proxy>>) {
      return "[Proxy]";
    } else if constexpr (std::is_same_v<T, GCPtr<WeakMap>>) {
      return "[WeakMap]";
    } else if constexpr (std::is_same_v<T, GCPtr<WeakSet>>) {
      return "[WeakSet]";
    } else if constexpr (std::is_same_v<T, GCPtr<ArrayBuffer>>) {
      return "[ArrayBuffer]";
    } else if constexpr (std::is_same_v<T, GCPtr<DataView>>) {
      return "[DataView]";
    } else if constexpr (std::is_same_v<T, GCPtr<ReadableStream>>) {
      return "[ReadableStream]";
    } else if constexpr (std::is_same_v<T, GCPtr<WritableStream>>) {
      return "[WritableStream]";
    } else if constexpr (std::is_same_v<T, GCPtr<TransformStream>>) {
      return "[TransformStream]";
    } else {
      return "";
    }
  }, data);
}

std::string Value::toDisplayString() const {
  if (isBigInt()) {
    return bigint::toString(std::get<BigInt>(data).value) + "n";
  }
  return toString();
}

double TypedArray::getElement(size_t index) const {
  if (index >= currentLength()) return 0.0;

  size_t byteIndex = byteOffset + index * elementSize();
  const auto& bytes = storage();
  return readTypedArrayNumberUnchecked(type, &bytes[byteIndex]);
}

void TypedArray::setElement(size_t index, double value) {
  if (index >= currentLength()) return;

  size_t byteIndex = byteOffset + index * elementSize();
  auto& bytes = storage();
  writeTypedArrayNumberUnchecked(type, &bytes[byteIndex], value);
}

int64_t TypedArray::getBigIntElement(size_t index) const {
  if (index >= currentLength()) return 0;

  size_t byteIndex = byteOffset + index * elementSize();
  const auto& bytes = storage();
  const uint8_t* ptr = &bytes[byteIndex];

  switch (type) {
    case TypedArrayType::BigInt64:
      return *reinterpret_cast<const int64_t*>(ptr);
    case TypedArrayType::BigUint64:
      return static_cast<int64_t>(*reinterpret_cast<const uint64_t*>(ptr));
    default:
      return static_cast<int64_t>(getElement(index));
  }
}

uint64_t TypedArray::getBigUintElement(size_t index) const {
  if (index >= currentLength()) return 0;

  size_t byteIndex = byteOffset + index * elementSize();
  const auto& bytes = storage();
  const uint8_t* ptr = &bytes[byteIndex];

  switch (type) {
    case TypedArrayType::BigUint64:
      return *reinterpret_cast<const uint64_t*>(ptr);
    case TypedArrayType::BigInt64:
      return static_cast<uint64_t>(*reinterpret_cast<const int64_t*>(ptr));
    default:
      return static_cast<uint64_t>(getElement(index));
  }
}

void TypedArray::setBigIntElement(size_t index, int64_t value) {
  if (index >= currentLength()) return;

  size_t byteIndex = byteOffset + index * elementSize();
  auto& bytes = storage();
  uint8_t* ptr = &bytes[byteIndex];

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

void TypedArray::setBigUintElement(size_t index, uint64_t value) {
  if (index >= currentLength()) return;

  size_t byteIndex = byteOffset + index * elementSize();
  auto& bytes = storage();
  uint8_t* ptr = &bytes[byteIndex];

  switch (type) {
    case TypedArrayType::BigUint64:
      *reinterpret_cast<uint64_t*>(ptr) = value;
      break;
    case TypedArrayType::BigInt64:
      *reinterpret_cast<int64_t*>(ptr) = static_cast<int64_t>(value);
      break;
    default:
      setElement(index, static_cast<double>(value));
      break;
  }
}

bool TypedArray::isOutOfBounds() const {
  if (!viewedBuffer) {
    return false;
  }
  if (byteOffset > viewedBuffer->byteLength) {
    return true;
  }
  if (lengthTracking) {
    return false;
  }
  const size_t size = elementSize();
  if (length > (std::numeric_limits<size_t>::max() - byteOffset) / size) {
    return true;
  }
  return byteOffset + length * size > viewedBuffer->byteLength;
}

size_t TypedArray::currentLength() const {
  if (!viewedBuffer) {
    return length;
  }
  if (isOutOfBounds()) {
    return 0;
  }
  if (!lengthTracking) {
    return length;
  }
  if (byteOffset >= viewedBuffer->byteLength) {
    return 0;
  }
  return (viewedBuffer->byteLength - byteOffset) / elementSize();
}

size_t TypedArray::currentByteLength() const {
  return currentLength() * elementSize();
}

std::vector<uint8_t>& TypedArray::storage() {
  return viewedBuffer ? viewedBuffer->data : buffer;
}

const std::vector<uint8_t>& TypedArray::storage() const {
  return viewedBuffer ? viewedBuffer->data : buffer;
}

// =============================================================================
// TypedArray bulk operations (SIMD-accelerated)
// =============================================================================

void TypedArray::copyFrom(const TypedArray& source, size_t srcOffset, size_t dstOffset, size_t count) {
  // Validate bounds
  size_t sourceLength = source.currentLength();
  size_t targetLength = currentLength();
  if (srcOffset >= sourceLength || dstOffset >= targetLength) return;
  count = std::min(count, std::min(sourceLength - srcOffset, targetLength - dstOffset));
  if (count == 0) return;

  const auto& sourceBytes = source.storage();
  auto& targetBytes = storage();
  const size_t sourceElementSize = source.elementSize();
  const size_t targetElementSize = elementSize();
  const size_t srcByteOffset = source.byteOffset + srcOffset * sourceElementSize;
  const size_t dstByteOffset = byteOffset + dstOffset * targetElementSize;

  // Fast path: same representation, just move the bytes.
  if (type == source.type ||
      ((type == TypedArrayType::BigInt64 || type == TypedArrayType::BigUint64) &&
       (source.type == TypedArrayType::BigInt64 || source.type == TypedArrayType::BigUint64))) {
    size_t bytesToCopy = count * targetElementSize;
    std::memmove(&targetBytes[dstByteOffset], &sourceBytes[srcByteOffset], bytesToCopy);
    return;
  }

  // Type conversion paths with SIMD acceleration
#if USE_SIMD
  // Float32 -> Int32
  if (source.type == TypedArrayType::Float32 && type == TypedArrayType::Int32) {
    const float* src = reinterpret_cast<const float*>(&sourceBytes[srcByteOffset]);
    int32_t* dst = reinterpret_cast<int32_t*>(&targetBytes[dstByteOffset]);
    simd::convertFloat32ToInt32(src, dst, count);
    return;
  }

  // Int32 -> Float32
  if (source.type == TypedArrayType::Int32 && type == TypedArrayType::Float32) {
    const int32_t* src = reinterpret_cast<const int32_t*>(&sourceBytes[srcByteOffset]);
    float* dst = reinterpret_cast<float*>(&targetBytes[dstByteOffset]);
    simd::convertInt32ToFloat32(src, dst, count);
    return;
  }

  // Float64 -> Int32
  if (source.type == TypedArrayType::Float64 && type == TypedArrayType::Int32) {
    const double* src = reinterpret_cast<const double*>(&sourceBytes[srcByteOffset]);
    int32_t* dst = reinterpret_cast<int32_t*>(&targetBytes[dstByteOffset]);
    simd::convertFloat64ToInt32(src, dst, count);
    return;
  }

  // Uint8 -> Float32
  if (source.type == TypedArrayType::Uint8 && type == TypedArrayType::Float32) {
    const uint8_t* src = &sourceBytes[srcByteOffset];
    float* dst = reinterpret_cast<float*>(&targetBytes[dstByteOffset]);
    simd::convertUint8ToFloat32(src, dst, count);
    return;
  }

  // Float32 -> Uint8
  if (source.type == TypedArrayType::Float32 && type == TypedArrayType::Uint8) {
    const float* src = reinterpret_cast<const float*>(&sourceBytes[srcByteOffset]);
    uint8_t* dst = &targetBytes[dstByteOffset];
    simd::convertFloat32ToUint8(src, dst, count);
    return;
  }

  // Float32 -> Uint8Clamped
  if (source.type == TypedArrayType::Float32 && type == TypedArrayType::Uint8Clamped) {
    const float* src = reinterpret_cast<const float*>(&sourceBytes[srcByteOffset]);
    uint8_t* dst = &targetBytes[dstByteOffset];
    simd::clampFloat32ToUint8(src, dst, count);
    return;
  }

  // Int16 -> Float32
  if (source.type == TypedArrayType::Int16 && type == TypedArrayType::Float32) {
    const int16_t* src = reinterpret_cast<const int16_t*>(&sourceBytes[srcByteOffset]);
    float* dst = reinterpret_cast<float*>(&targetBytes[dstByteOffset]);
    simd::convertInt16ToFloat32(src, dst, count);
    return;
  }

  // Float32 -> Int16
  if (source.type == TypedArrayType::Float32 && type == TypedArrayType::Int16) {
    const float* src = reinterpret_cast<const float*>(&sourceBytes[srcByteOffset]);
    int16_t* dst = reinterpret_cast<int16_t*>(&targetBytes[dstByteOffset]);
    simd::convertFloat32ToInt16(src, dst, count);
    return;
  }

  // Float32 -> Float16 (batch)
  if (source.type == TypedArrayType::Float32 && type == TypedArrayType::Float16) {
    const float* src = reinterpret_cast<const float*>(&sourceBytes[srcByteOffset]);
    uint16_t* dst = reinterpret_cast<uint16_t*>(&targetBytes[dstByteOffset]);
    simd::convertFloat32ToFloat16Batch(src, dst, count);
    return;
  }

  // Float16 -> Float32 (batch)
  if (source.type == TypedArrayType::Float16 && type == TypedArrayType::Float32) {
    const uint16_t* src = reinterpret_cast<const uint16_t*>(&sourceBytes[srcByteOffset]);
    float* dst = reinterpret_cast<float*>(&targetBytes[dstByteOffset]);
    simd::convertFloat16ToFloat32Batch(src, dst, count);
    return;
  }
#endif

  // Generic fallback: convert raw elements without re-checking bounds on every step.
  const uint8_t* src = &sourceBytes[srcByteOffset];
  uint8_t* dst = &targetBytes[dstByteOffset];
  for (size_t i = 0; i < count; ++i) {
    double val = readTypedArrayNumberUnchecked(source.type, src + i * sourceElementSize);
    writeTypedArrayNumberUnchecked(type, dst + i * targetElementSize, val);
  }
}

void TypedArray::fill(double value) {
  fill(value, 0, currentLength());
}

void TypedArray::fill(double value, size_t start, size_t end) {
  size_t targetLength = currentLength();
  if (start >= targetLength) return;
  end = std::min(end, targetLength);
  if (start >= end) return;

  size_t count = end - start;
  auto& bytes = storage();

#if USE_SIMD
  // SIMD-accelerated fill for common types
  switch (type) {
    case TypedArrayType::Float32: {
      float* ptr = reinterpret_cast<float*>(&bytes[byteOffset + start * 4]);
      simd::fillFloat32(ptr, static_cast<float>(value), count);
      return;
    }
    case TypedArrayType::Int32: {
      int32_t* ptr = reinterpret_cast<int32_t*>(&bytes[byteOffset + start * 4]);
      simd::fillInt32(ptr, static_cast<int32_t>(value), count);
      return;
    }
    case TypedArrayType::Uint32: {
      int32_t* ptr = reinterpret_cast<int32_t*>(&bytes[byteOffset + start * 4]);
      simd::fillInt32(ptr, static_cast<int32_t>(static_cast<uint32_t>(value)), count);
      return;
    }
    default:
      break;
  }
#endif

  // Scalar fallback
  for (size_t i = start; i < end; ++i) {
    setElement(i, value);
  }
}

void TypedArray::setElements(const double* values, size_t offset, size_t count) {
  size_t targetLength = currentLength();
  if (offset >= targetLength) return;
  count = std::min(count, targetLength - offset);
  if (count == 0) return;
  auto& bytes = storage();

#if USE_SIMD
  // SIMD-accelerated bulk set for Float32
  if (type == TypedArrayType::Float32) {
    // Convert doubles to floats first
    float* dst = reinterpret_cast<float*>(&bytes[byteOffset + offset * 4]);
    for (size_t i = 0; i < count; ++i) {
      dst[i] = static_cast<float>(values[i]);
    }
    return;
  }

  // SIMD-accelerated bulk set for Int32
  if (type == TypedArrayType::Int32) {
    // Convert doubles to int32 using SIMD where possible
    // For now, use scalar since input is double and we'd need temp buffer
    int32_t* dst = reinterpret_cast<int32_t*>(&bytes[byteOffset + offset * 4]);
    for (size_t i = 0; i < count; ++i) {
      dst[i] = static_cast<int32_t>(values[i]);
    }
    return;
  }

  // Float64 can be directly copied
  if (type == TypedArrayType::Float64) {
    double* dst = reinterpret_cast<double*>(&bytes[byteOffset + offset * 8]);
    simd::memcpySIMD(dst, values, count * sizeof(double));
    return;
  }
#endif

  // Scalar fallback
  for (size_t i = 0; i < count; ++i) {
    setElement(offset + i, values[i]);
  }
}

void TypedArray::getElements(double* values, size_t offset, size_t count) const {
  size_t sourceLength = currentLength();
  if (offset >= sourceLength) return;
  count = std::min(count, sourceLength - offset);
  if (count == 0) return;
  const auto& bytes = storage();

#if USE_SIMD
  // Float64 can be directly copied
  if (type == TypedArrayType::Float64) {
    const double* src = reinterpret_cast<const double*>(&bytes[byteOffset + offset * 8]);
    simd::memcpySIMD(values, src, count * sizeof(double));
    return;
  }

  // Float32 -> double conversion
  if (type == TypedArrayType::Float32) {
    const float* src = reinterpret_cast<const float*>(&bytes[byteOffset + offset * 4]);
    for (size_t i = 0; i < count; ++i) {
      values[i] = static_cast<double>(src[i]);
    }
    return;
  }

  // Int32 -> double conversion
  if (type == TypedArrayType::Int32) {
    const int32_t* src = reinterpret_cast<const int32_t*>(&bytes[byteOffset + offset * 4]);
    for (size_t i = 0; i < count; ++i) {
      values[i] = static_cast<double>(src[i]);
    }
    return;
  }
#endif

  // Scalar fallback
  for (size_t i = 0; i < count; ++i) {
    values[i] = getElement(offset + i);
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
  if (a.isSymbol()) return std::get<Symbol>(a.data).id == std::get<Symbol>(b.data).id;
  if (a.isNull() || a.isUndefined()) return true;

  // For objects, compare by reference
  if (a.isObject()) return a.getGC<Object>() == b.getGC<Object>();
  if (a.isArray()) return a.getGC<Array>() == b.getGC<Array>();
  if (a.isFunction()) return a.getGC<Function>() == b.getGC<Function>();
  if (a.isMap()) return a.getGC<Map>() == b.getGC<Map>();
  if (a.isSet()) return a.getGC<Set>() == b.getGC<Set>();
  if (a.isWeakMap()) return a.getGC<WeakMap>() == b.getGC<WeakMap>();
  if (a.isWeakSet()) return a.getGC<WeakSet>() == b.getGC<WeakSet>();
  if (a.isPromise()) return a.getGC<Promise>() == b.getGC<Promise>();
  if (a.isRegex()) return a.getGC<Regex>() == b.getGC<Regex>();
  if (a.isError()) return a.getGC<Error>() == b.getGC<Error>();
  if (a.isTypedArray()) return a.getGC<TypedArray>() == b.getGC<TypedArray>();
  if (a.isArrayBuffer()) return a.getGC<ArrayBuffer>() == b.getGC<ArrayBuffer>();
  if (a.isDataView()) return a.getGC<DataView>() == b.getGC<DataView>();
  if (a.isGenerator()) return a.getGC<Generator>() == b.getGC<Generator>();
  if (a.isClass()) return a.getGC<Class>() == b.getGC<Class>();

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

  auto callbacks = fulfilledCallbacks;
  auto chained = chainedPromises;
  fulfilledCallbacks.clear();
  rejectedCallbacks.clear();
  chainedPromises.clear();

  // Promise reactions must run as microtasks.
  for (size_t i = 0; i < callbacks.size(); ++i) {
    auto callback = callbacks[i];
    GCPtr<Promise> chainedPromise = i < chained.size() ? chained[i] : GCPtr<Promise>{};
    queuePromiseCallback([callback, chainedPromise, val]() {
      if (!chainedPromise) {
        return;
      }
      try {
        Value callbackResult = callback ? callback(val) : val;
        resolveChainedPromise(chainedPromise, callbackResult);
      } catch (const std::exception& e) {
        chainedPromise->reject(Value(std::string(e.what())));
      } catch (...) {
        chainedPromise->reject(Value("Callback error"));
      }
    });
  }
}

void Promise::reject(Value val) {
  if (state != PromiseState::Pending) return;

  state = PromiseState::Rejected;
  result = val;

  auto callbacks = rejectedCallbacks;
  auto chained = chainedPromises;
  fulfilledCallbacks.clear();
  rejectedCallbacks.clear();
  chainedPromises.clear();

  // Promise reactions must run as microtasks.
  for (size_t i = 0; i < callbacks.size(); ++i) {
    auto callback = callbacks[i];
    GCPtr<Promise> chainedPromise = i < chained.size() ? chained[i] : GCPtr<Promise>{};
    queuePromiseCallback([callback, chainedPromise, val]() {
      if (!chainedPromise) {
        return;
      }
      if (callback) {
        try {
          Value callbackResult = callback(val);
          resolveChainedPromise(chainedPromise, callbackResult);
        } catch (const std::exception& e) {
          chainedPromise->reject(Value(std::string(e.what())));
        } catch (...) {
          chainedPromise->reject(Value("Callback error"));
        }
      } else {
        chainedPromise->reject(val);
      }
    });
  }
}

GCPtr<Promise> Promise::then(
    std::function<Value(Value)> onFulfilled,
    std::function<Value(Value)> onRejected) {
  auto chainedPromise = GarbageCollector::makeGC<Promise>();

  if (state == PromiseState::Pending) {
    fulfilledCallbacks.push_back(onFulfilled ? onFulfilled : [](Value v) { return v; });
    rejectedCallbacks.push_back(onRejected);
    chainedPromises.push_back(chainedPromise);
  } else if (state == PromiseState::Fulfilled) {
    Value settled = result;
    queuePromiseCallback([onFulfilled, chainedPromise, settled]() {
      if (onFulfilled) {
        try {
          Value callbackResult = onFulfilled(settled);
          resolveChainedPromise(chainedPromise, callbackResult);
        } catch (const std::exception& e) {
          chainedPromise->reject(Value(std::string(e.what())));
        } catch (...) {
          chainedPromise->reject(Value("Callback error"));
        }
      } else {
        chainedPromise->resolve(settled);
      }
    });
  } else if (state == PromiseState::Rejected) {
    Value settled = result;
    queuePromiseCallback([onRejected, chainedPromise, settled]() {
      if (onRejected) {
        try {
          Value callbackResult = onRejected(settled);
          resolveChainedPromise(chainedPromise, callbackResult);
        } catch (const std::exception& e) {
          chainedPromise->reject(Value(std::string(e.what())));
        } catch (...) {
          chainedPromise->reject(Value("Callback error"));
        }
      } else {
        chainedPromise->reject(settled);
      }
    });
  }

  return chainedPromise;
}

GCPtr<Promise> Promise::catch_(std::function<Value(Value)> onRejected) {
  return then(nullptr, onRejected);
}

GCPtr<Promise> Promise::finally(std::function<Value()> onFinally) {
  if (!onFinally) {
    return then(nullptr, nullptr);
  }

  auto chainedPromise = GarbageCollector::makeGC<Promise>();

  auto settleWithOriginal = [chainedPromise](const Value& original,
                                             bool rejectOriginal,
                                             const Value& finallyResult) {
    auto settleOriginal = [chainedPromise, original, rejectOriginal]() {
      if (rejectOriginal) {
        chainedPromise->reject(original);
      } else {
        chainedPromise->resolve(original);
      }
    };

    if (finallyResult.isPromise()) {
      auto finPromise = finallyResult.getGC<Promise>();
      if (finPromise->state == PromiseState::Fulfilled) {
        settleOriginal();
        return;
      }
      if (finPromise->state == PromiseState::Rejected) {
        chainedPromise->reject(finPromise->result);
        return;
      }
      finPromise->then(
        [settleOriginal](Value v) -> Value {
          settleOriginal();
          return v;
        },
        [chainedPromise](Value reason) -> Value {
          chainedPromise->reject(reason);
          return reason;
        });
      return;
    }

    settleOriginal();
  };

  then(
    [onFinally, settleWithOriginal, chainedPromise](Value fulfilled) -> Value {
      try {
        Value finallyResult = onFinally();
        settleWithOriginal(fulfilled, false, finallyResult);
      } catch (const std::exception& e) {
        chainedPromise->reject(Value(std::string(e.what())));
      } catch (...) {
        chainedPromise->reject(Value("Callback error"));
      }
      return Value(Undefined{});
    },
    [onFinally, settleWithOriginal, chainedPromise](Value rejected) -> Value {
      try {
        Value finallyResult = onFinally();
        settleWithOriginal(rejected, true, finallyResult);
      } catch (const std::exception& e) {
        chainedPromise->reject(Value(std::string(e.what())));
      } catch (...) {
        chainedPromise->reject(Value("Callback error"));
      }
      return Value(Undefined{});
    });

  return chainedPromise;
}

GCPtr<Promise> Promise::all(const std::vector<GCPtr<Promise>>& promises) {
  auto resultPromise = GarbageCollector::makeGC<Promise>();

  if (promises.empty()) {
    auto emptyArray = GarbageCollector::makeGC<Array>();
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
          auto arrayResult = GarbageCollector::makeGC<Array>();
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

GCPtr<Promise> Promise::race(const std::vector<GCPtr<Promise>>& promises) {
  auto resultPromise = GarbageCollector::makeGC<Promise>();

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

GCPtr<Promise> Promise::resolved(const Value& value) {
  auto promise = GarbageCollector::makeGC<Promise>();
  promise->resolve(value);
  return promise;
}

GCPtr<Promise> Promise::rejected(const Value& reason) {
  auto promise = GarbageCollector::makeGC<Promise>();
  promise->reject(reason);
  return promise;
}

static bool isInternalProperty(const std::string& key);

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

Value Object_keys(const std::vector<Value>& args) {
  auto result = makeArrayWithPrototype();
  if (args.empty()) {
    throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
  }

  const auto& arg = args[0];
  // Throw TypeError for null/undefined
  if (arg.isNull() || arg.isUndefined()) {
    throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
  }

  // Handle string primitives: Object.keys("abc") → ["0","1","2"]
  if (arg.isString()) {
    const std::string& str = std::get<std::string>(arg.data);
    // Count code points for proper Unicode support
    size_t cpIdx = 0;
    size_t bytePos = 0;
    while (bytePos < str.size()) {
      result->elements.push_back(Value(std::to_string(cpIdx)));
      // Skip UTF-8 bytes
      unsigned char c = str[bytePos];
      if (c < 0x80) bytePos += 1;
      else if ((c & 0xE0) == 0xC0) bytePos += 2;
      else if ((c & 0xF0) == 0xE0) bytePos += 3;
      else bytePos += 4;
      cpIdx++;
    }
    return Value(result);
  }

  // Handle Object with module namespace
  if (arg.isObject()) {
    auto obj = arg.getGC<Object>();
    if (obj->isModuleNamespace) {
      for (const auto& key : obj->moduleExportNames) {
        auto getterIt = obj->properties.find("__get_" + key);
        if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
          auto getter = getterIt->second.getGC<Function>();
          if (getter && getter->isNative) {
            getter->nativeFunc({});
          }
        }
        result->elements.push_back(Value(key));
      }
      return Value(result);
    }
  }

  // Handle number/boolean primitives - no enumerable own properties
  if (arg.isNumber() || arg.isBool()) {
    return Value(result);
  }

  // For Arrays, include numeric indices first
  if (arg.isArray()) {
    auto arr = arg.getGC<Array>();
    for (size_t i = 0; i < arr->elements.size(); ++i) {
      std::string key = std::to_string(i);
      // Skip non-enumerable
      if (arr->properties.count("__non_enum_" + key)) continue;
      // Skip holes (deleted or never-set elements)
      if (arr->properties.count("__deleted_" + key + "__")) continue;
      if (arr->properties.count("__hole_" + key + "__")) continue;
      result->elements.push_back(Value(key));
    }
    // Then add non-index enumerable properties (including large indices stored as properties)
    for (const auto& key : arr->properties.orderedKeys()) {
      if (arr->properties.count("__non_enum_" + key)) continue;
      if (key.find("__") == 0) continue; // skip internal markers
      // Skip numeric indices already added from elements array
      uint32_t idx = 0;
      if (isArrayIndex(key, idx) && idx < arr->elements.size()) continue;
      if (key == "length") continue; // length is not enumerable
      result->elements.push_back(Value(key));
    }
    return Value(result);
  }

  auto* props = getPropertiesMap(arg);
  if (!props) return Value(result);

  for (const auto& key : sortOwnPropertyKeys(props->orderedKeys())) {
    if (props->count("__non_enum_" + key)) continue;
    result->elements.push_back(Value(key));
  }

  return Value(result);
}

Value Object_values(const std::vector<Value>& args) {
  auto result = makeArrayWithPrototype();
  if (args.empty()) {
    throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
  }

  const auto& arg = args[0];
  if (arg.isNull() || arg.isUndefined()) {
    throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
  }

  // Handle string primitives: Object.values("abc") → ["a","b","c"]
  if (arg.isString()) {
    const std::string& str = std::get<std::string>(arg.data);
    size_t bytePos = 0;
    while (bytePos < str.size()) {
      unsigned char c = str[bytePos];
      size_t len = 1;
      if (c >= 0x80) {
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else len = 4;
      }
      result->elements.push_back(Value(str.substr(bytePos, len)));
      bytePos += len;
    }
    return Value(result);
  }

  if (arg.isNumber() || arg.isBool()) {
    return Value(result);
  }

  // For Arrays, include element values
  if (arg.isArray()) {
    auto arr = arg.getGC<Array>();
    Interpreter* interp = getGlobalInterpreter();
    for (size_t i = 0; i < arr->elements.size(); ++i) {
      std::string key = std::to_string(i);
      if (!arr->properties.count("__non_enum_" + key)) {
        if (interp) {
          auto [found, val] = interp->getPropertyForExternal(arg, key);
          if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
          if (found) result->elements.push_back(val);
        } else {
          result->elements.push_back(arr->elements[i]);
        }
      }
    }
    return Value(result);
  }

  auto* props = getPropertiesMap(arg);
  if (!props) return Value(result);

  Interpreter* interp = getGlobalInterpreter();
  for (const auto& key : sortOwnPropertyKeys(props->orderedKeys())) {
    if (props->count("__non_enum_" + key)) continue;
    if (interp) {
      auto [found, val] = interp->getPropertyForExternal(arg, key);
      if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
      if (found) result->elements.push_back(val);
    } else {
      auto it = props->find(key);
      result->elements.push_back(it->second);
    }
  }

  return Value(result);
}

Value Object_entries(const std::vector<Value>& args) {
  auto result = makeArrayWithPrototype();
  if (args.empty()) {
    throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
  }

  const auto& arg = args[0];
  if (arg.isNull() || arg.isUndefined()) {
    throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
  }

  // Handle string primitives: Object.entries("abc") → [["0","a"],["1","b"],["2","c"]]
  if (arg.isString()) {
    const std::string& str = std::get<std::string>(arg.data);
    size_t cpIdx = 0;
    size_t bytePos = 0;
    while (bytePos < str.size()) {
      unsigned char c = str[bytePos];
      size_t len = 1;
      if (c >= 0x80) {
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else len = 4;
      }
      auto entry = makeArrayWithPrototype();
      entry->elements.push_back(Value(std::to_string(cpIdx)));
      entry->elements.push_back(Value(str.substr(bytePos, len)));
      result->elements.push_back(Value(entry));
      bytePos += len;
      cpIdx++;
    }
    return Value(result);
  }

  if (arg.isNumber() || arg.isBool()) {
    return Value(result);
  }

  // For Arrays, include element entries
  if (arg.isArray()) {
    auto arr = arg.getGC<Array>();
    Interpreter* interp = getGlobalInterpreter();
    for (size_t i = 0; i < arr->elements.size(); ++i) {
      std::string key = std::to_string(i);
      if (!arr->properties.count("__non_enum_" + key)) {
        auto entry = makeArrayWithPrototype();
        entry->elements.push_back(Value(key));
        if (interp) {
          auto [found, val] = interp->getPropertyForExternal(arg, key);
          if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
          entry->elements.push_back(found ? val : Value(Undefined{}));
        } else {
          entry->elements.push_back(arr->elements[i]);
        }
        result->elements.push_back(Value(entry));
      }
    }
    return Value(result);
  }

  auto* props = getPropertiesMap(arg);
  if (!props) return Value(result);

  Interpreter* interp = getGlobalInterpreter();
  for (const auto& key : sortOwnPropertyKeys(props->orderedKeys())) {
    if (props->count("__non_enum_" + key)) continue;
    auto entry = makeArrayWithPrototype();
    entry->elements.push_back(Value(key));
    if (interp) {
      auto [found, val] = interp->getPropertyForExternal(arg, key);
      if (interp->hasError()) {
        Value err = interp->getError();
        interp->clearError();
        throw JsValueException(err);
      }
      entry->elements.push_back(found ? val : Value(Undefined{}));
    } else {
      auto it = props->find(key);
      entry->elements.push_back(it->second);
    }
    result->elements.push_back(Value(entry));
  }

  return Value(result);
}

// Helper: get enumerable own string-keyed properties from a source value
// Returns pairs of (key, value) in ES property order
static std::vector<std::pair<std::string, Value>> getEnumerableOwnProperties(const Value& source) {
  std::vector<std::pair<std::string, Value>> result;

  // String sources: each character is an enumerable indexed property
  if (source.isString()) {
    const std::string& str = std::get<std::string>(source.data);
    // Iterate by UTF-16 code units for ES compat
    size_t i = 0;
    size_t idx = 0;
    while (i < str.size()) {
      unsigned char c = str[i];
      size_t charLen = 1;
      if ((c & 0x80) == 0) charLen = 1;
      else if ((c & 0xE0) == 0xC0) charLen = 2;
      else if ((c & 0xF0) == 0xE0) charLen = 3;
      else if ((c & 0xF8) == 0xF0) charLen = 4;
      std::string ch = str.substr(i, charLen);
      result.push_back({std::to_string(idx), Value(ch)});
      i += charLen;
      idx++;
    }
    return result;
  }

  // Number, Boolean, Symbol, BigInt: no own enumerable string-keyed properties
  if (source.isNumber() || source.isBool() || source.isSymbol() || source.isBigInt()) {
    return result;
  }

  // Array sources: indexed elements + own properties
  if (source.isArray()) {
    auto arr = source.getGC<Array>();
    // Add array elements (indexed, enumerable by default)
    for (size_t i = 0; i < arr->elements.size(); i++) {
      std::string key = std::to_string(i);
      // Skip deleted elements and holes
      if (arr->properties.find("__deleted_" + key + "__") != arr->properties.end()) continue;
      if (arr->properties.find("__hole_" + key + "__") != arr->properties.end()) continue;
      if (arr->properties.find("__non_enum_" + key) != arr->properties.end()) continue;
      result.push_back({key, arr->elements[i]});
    }
    // Add non-indexed own properties
    for (const auto& key : arr->properties.orderedKeys()) {
      if (isInternalProperty(key)) continue;
      if (isSymbolPropertyKey(key)) continue;
      if (arr->properties.count("__non_enum_" + key)) continue;
      // Skip numeric indices already handled
      uint32_t idx2;
      if (isArrayIndex(key, idx2) && idx2 < arr->elements.size()) continue;
      if (key == "length") {
        result.push_back({key, Value(static_cast<double>(arr->elements.size()))});
        continue;
      }
      auto it = arr->properties.find(key);
      if (it != arr->properties.end()) {
        result.push_back({key, it->second});
      }
    }
    return result;
  }

  // Generic object-like types: Object, Function, Class, etc.
  OrderedMap<std::string, Value>* props = nullptr;
  if (source.isObject()) props = &source.getGC<Object>()->properties;
  else if (source.isFunction()) props = &source.getGC<Function>()->properties;
  else if (source.isClass()) props = &source.getGC<Class>()->properties;
  else if (source.isPromise()) props = &source.getGC<Promise>()->properties;
  else if (source.isRegex()) props = &source.getGC<Regex>()->properties;
  else if (source.isError()) props = &source.getGC<Error>()->properties;
  else if (source.isMap()) props = &source.getGC<Map>()->properties;
  else if (source.isSet()) props = &source.getGC<Set>()->properties;

  if (!props) return result;

  // String keys first (in property order: integer indices, then strings)
  for (const auto& key : sortOwnPropertyKeys(props->orderedKeys())) {
    if (isInternalProperty(key)) continue;
    if (isSymbolPropertyKey(key)) continue;
    if (props->count("__non_enum_" + key)) continue;
    auto it = props->find(key);
    if (it != props->end()) {
      result.push_back({key, it->second});
    }
  }
  // Symbol keys last (in insertion order)
  for (const auto& key : props->orderedKeys()) {
    if (!isSymbolPropertyKey(key)) continue;
    if (isInternalProperty(key)) continue;
    if (props->count("__non_enum_" + key)) continue;
    auto it = props->find(key);
    if (it != props->end()) {
      result.push_back({key, it->second});
    }
  }
  return result;
}

// Helper: assign a property value to a target, respecting non-writable constraints
static void assignToTarget(Value& target, const std::string& key, const Value& value) {
  if (target.isArray()) {
    auto arr = target.getGC<Array>();
    // Check non-writable
    if (arr->properties.count("__non_writable_" + key)) {
      throw std::runtime_error("TypeError: Cannot assign to read only property '" + key + "'");
    }
    // Handle "length" specially for arrays
    if (key == "length" && value.isNumber()) {
      double newLen = value.toNumber();
      uint32_t len = static_cast<uint32_t>(newLen);
      if (len < arr->elements.size()) {
        arr->elements.resize(len);
      } else if (len > arr->elements.size()) {
        arr->elements.resize(len, Value(Undefined{}));
      }
      return;
    }
    // Handle numeric index
    uint32_t idx;
    if (isArrayIndex(key, idx)) {
      if (idx >= arr->elements.size()) {
        arr->elements.resize(idx + 1, Value(Undefined{}));
      }
      arr->elements[idx] = value;
      return;
    }
    arr->properties[key] = value;
    return;
  }

  OrderedMap<std::string, Value>* props = nullptr;
  if (target.isObject()) props = &target.getGC<Object>()->properties;
  else if (target.isFunction()) props = &target.getGC<Function>()->properties;
  else if (target.isClass()) props = &target.getGC<Class>()->properties;
  else if (target.isPromise()) props = &target.getGC<Promise>()->properties;
  else if (target.isError()) props = &target.getGC<Error>()->properties;

  if (!props) return;

  // Check if target has a setter for this property (accessor property)
  // Setter invocation happens even on sealed/frozen objects per spec
  auto setterIt = props->find("__set_" + key);
  if (setterIt != props->end() && setterIt->second.isFunction()) {
    auto* interp = getGlobalInterpreter();
    if (interp) {
      interp->callForHarness(setterIt->second, {value}, target);
      if (interp->hasError()) {
        Value err = interp->getError();
        interp->clearError();
        throw JsValueException(err);
      }
    }
    return;
  }

  // Data property: check non-writable
  if (props->count("__non_writable_" + key)) {
    throw std::runtime_error("TypeError: Cannot assign to read only property '" + key + "'");
  }

  // Check non-extensible: reject new properties
  if (props->find(key) == props->end() &&
      props->find("__get_" + key) == props->end() &&
      props->find("__set_" + key) == props->end()) {
    if (props->count("__non_extensible__") ||
        (target.isObject() && (target.getGC<Object>()->sealed || target.getGC<Object>()->nonExtensible || target.getGC<Object>()->frozen))) {
      throw std::runtime_error("TypeError: Cannot add property " + key + ", object is not extensible");
    }
  }

  (*props)[key] = value;
}

Value Object_assign(const std::vector<Value>& args) {
  if (args.empty()) {
    throw std::runtime_error("TypeError: Object.assign requires at least 1 argument");
  }

  Value target = args[0];
  if (target.isNull() || target.isUndefined()) {
    throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
  }

  // ToObject: if target is already an object-like type, keep it; otherwise wrap
  Value targetVal = target;
  if (target.isString() || target.isNumber() || target.isBool() ||
      target.isBigInt() || target.isSymbol()) {
    // Wrap primitive to object
    auto wrapped = GarbageCollector::makeGC<Object>();
    wrapped->properties["__primitive_value__"] = target;

    // For string targets, add indexed characters as non-writable, non-configurable, enumerable
    if (target.isString()) {
      const std::string& str = std::get<std::string>(target.data);
      size_t i = 0, idx = 0;
      while (i < str.size()) {
        unsigned char c = str[i];
        size_t charLen = 1;
        if ((c & 0x80) == 0) charLen = 1;
        else if ((c & 0xE0) == 0xC0) charLen = 2;
        else if ((c & 0xF0) == 0xE0) charLen = 3;
        else if ((c & 0xF8) == 0xF0) charLen = 4;
        std::string ch = str.substr(i, charLen);
        std::string key = std::to_string(idx);
        wrapped->properties[key] = Value(ch);
        wrapped->properties["__non_writable_" + key] = Value(true);
        wrapped->properties["__non_configurable_" + key] = Value(true);
        i += charLen;
        idx++;
      }
      wrapped->properties["length"] = Value(static_cast<double>(idx));
      wrapped->properties["__non_writable_length"] = Value(true);
      wrapped->properties["__non_enum_length"] = Value(true);
      wrapped->properties["__non_configurable_length"] = Value(true);
    }

    // Set __proto__ to the appropriate prototype for valueOf/toString inheritance
    auto* interp = getGlobalInterpreter();
    if (interp) {
      auto env = interp->getEnvironment();
      // Walk to global env
      while (env && env->getParent()) env = env->getParentPtr();
      if (env) {
        std::string ctorName;
        if (target.isNumber()) ctorName = "Number";
        else if (target.isBool()) ctorName = "Boolean";
        else if (target.isString()) ctorName = "String";
        else if (target.isSymbol()) ctorName = "Symbol";
        else if (target.isBigInt()) ctorName = "BigInt";
        if (!ctorName.empty()) {
          if (auto ctor = env->get(ctorName)) {
            OrderedMap<std::string, Value>* ctorProps = nullptr;
            if (ctor->isFunction()) {
              ctorProps = &ctor->getGC<Function>()->properties;
            } else if (ctor->isObject()) {
              ctorProps = &ctor->getGC<Object>()->properties;
            } else if (ctor->isClass()) {
              ctorProps = &ctor->getGC<Class>()->properties;
            }
            if (ctorProps) {
              auto protoIt = ctorProps->find("prototype");
              if (protoIt != ctorProps->end()) {
                wrapped->properties["__proto__"] = protoIt->second;
              }
            }
          }
        }
      }
    }

    targetVal = Value(wrapped);
  }

  // Copy properties from each source
  for (size_t i = 1; i < args.size(); ++i) {
    const Value& source = args[i];
    if (source.isNull() || source.isUndefined()) {
      continue; // Skip null/undefined sources
    }

    auto ownProps = getEnumerableOwnProperties(source);
    for (const auto& [key, value] : ownProps) {
      assignToTarget(targetVal, key, value);
    }
  }

  return targetVal;
}

Value Object_hasOwnProperty(const std::vector<Value>& args) {
  if (args.size() < 2) {
    // Still need to check this value
    if (!args.empty() && (args[0].isUndefined() || args[0].isNull())) {
      throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
    }
    return Value(false);
  }

  // Step 1: ToPropertyKey(V) - must happen before ToObject(this)
  std::string key = valueToPropertyKey(args[1]);

  // Step 2: ToObject(this) - throw TypeError for undefined/null
  if (args[0].isUndefined() || args[0].isNull()) {
    throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
  }

  // Handle Function objects
  if (args[0].isFunction()) {
    auto fn = args[0].getGC<Function>();
    // Internal properties are not own properties
    if (isInternalProperty(key)) return Value(false);
    if (fn->properties.find(key) != fn->properties.end()) return Value(true);
    if (fn->properties.find("__get_" + key) != fn->properties.end()) return Value(true);
    if (fn->properties.find("__set_" + key) != fn->properties.end()) return Value(true);
    return Value(false);
  }

  // Handle Class objects
  if (args[0].isClass()) {
    auto cls = args[0].getGC<Class>();
    if (isInternalProperty(key)) return Value(false);
    if (cls->properties.find(key) != cls->properties.end()) return Value(true);
    if (cls->properties.find("__get_" + key) != cls->properties.end()) return Value(true);
    if (cls->properties.find("__set_" + key) != cls->properties.end()) return Value(true);
    return Value(false);
  }

  // Handle Array objects
  if (args[0].isArray()) {
    auto arr = args[0].getGC<Array>();
    if (isInternalProperty(key)) return Value(false);
    // Arrays always have an own `length` data property (unless deleted from arguments).
    if (key == "length") {
      if (arr->properties.find("__deleted_length__") != arr->properties.end()) {
        return Value(false);
      }
      return Value(true);
    }
    try {
      size_t idx = std::stoull(key);
      if (idx < arr->elements.size()) {
        // Check if this index was deleted or is a hole
        if (arr->properties.find("__deleted_" + key + "__") != arr->properties.end()) {
          return Value(false);
        }
        if (arr->properties.find("__hole_" + key + "__") != arr->properties.end()) {
          return Value(false);
        }
        return Value(true);
      }
    } catch (...) {}
    if (arr->properties.find(key) != arr->properties.end()) return Value(true);
    if (arr->properties.find("__get_" + key) != arr->properties.end()) return Value(true);
    if (arr->properties.find("__set_" + key) != arr->properties.end()) return Value(true);
    return Value(false);
  }

  if (args[0].isRegex()) {
    auto rx = args[0].getGC<Regex>();
    if (isInternalProperty(key)) return Value(false);
    if (key == "source" || key == "flags") return Value(true);
    if (rx->properties.find(key) != rx->properties.end()) return Value(true);
    if (rx->properties.find("__get_" + key) != rx->properties.end()) return Value(true);
    if (rx->properties.find("__set_" + key) != rx->properties.end()) return Value(true);
    return Value(false);
  }

  if (args[0].isPromise()) {
    auto p = args[0].getGC<Promise>();
    if (isInternalProperty(key)) return Value(false);
    if (p->properties.find(key) != p->properties.end()) return Value(true);
    if (p->properties.find("__get_" + key) != p->properties.end()) return Value(true);
    if (p->properties.find("__set_" + key) != p->properties.end()) return Value(true);
    return Value(false);
  }

  if (args[0].isError()) {
    auto e = args[0].getGC<Error>();
    if (isInternalProperty(key)) return Value(false);
    if (e->properties.find(key) != e->properties.end()) return Value(true);
    if (e->properties.find("__get_" + key) != e->properties.end()) return Value(true);
    if (e->properties.find("__set_" + key) != e->properties.end()) return Value(true);
    return Value(false);
  }

  if (args[0].isMap()) {
    auto m = args[0].getGC<Map>();
    if (isInternalProperty(key)) return Value(false);
    if (m->properties.find(key) != m->properties.end()) return Value(true);
    if (m->properties.find("__get_" + key) != m->properties.end()) return Value(true);
    if (m->properties.find("__set_" + key) != m->properties.end()) return Value(true);
    return Value(false);
  }

  if (args[0].isSet()) {
    auto s = args[0].getGC<Set>();
    if (isInternalProperty(key)) return Value(false);
    if (s->properties.find(key) != s->properties.end()) return Value(true);
    if (s->properties.find("__get_" + key) != s->properties.end()) return Value(true);
    if (s->properties.find("__set_" + key) != s->properties.end()) return Value(true);
    return Value(false);
  }

  if (args[0].isWeakMap()) {
    auto wm = args[0].getGC<WeakMap>();
    if (isInternalProperty(key)) return Value(false);
    if (wm->properties.find(key) != wm->properties.end()) return Value(true);
    if (wm->properties.find("__get_" + key) != wm->properties.end()) return Value(true);
    if (wm->properties.find("__set_" + key) != wm->properties.end()) return Value(true);
    return Value(false);
  }

  if (args[0].isWeakSet()) {
    auto ws = args[0].getGC<WeakSet>();
    if (isInternalProperty(key)) return Value(false);
    if (ws->properties.find(key) != ws->properties.end()) return Value(true);
    if (ws->properties.find("__get_" + key) != ws->properties.end()) return Value(true);
    if (ws->properties.find("__set_" + key) != ws->properties.end()) return Value(true);
    return Value(false);
  }

  if (args[0].isTypedArray()) {
    auto ta = args[0].getGC<TypedArray>();
    if (isInternalProperty(key)) return Value(false);
    if (key == "length" || key == "byteLength" || key == "buffer" || key == "byteOffset") return Value(true);
    if (!key.empty() && std::all_of(key.begin(), key.end(), ::isdigit)) {
      try {
        size_t idx = std::stoull(key);
        if (idx < ta->currentLength()) return Value(true);
      } catch (...) {
      }
    }
    if (ta->properties.find(key) != ta->properties.end()) return Value(true);
    if (ta->properties.find("__get_" + key) != ta->properties.end()) return Value(true);
    if (ta->properties.find("__set_" + key) != ta->properties.end()) return Value(true);
    return Value(false);
  }

  if (args[0].isArrayBuffer()) {
    auto b = args[0].getGC<ArrayBuffer>();
    if (isInternalProperty(key)) return Value(false);
    if (key == "byteLength") return Value(true);
    if (b->properties.find(key) != b->properties.end()) return Value(true);
    if (b->properties.find("__get_" + key) != b->properties.end()) return Value(true);
    if (b->properties.find("__set_" + key) != b->properties.end()) return Value(true);
    return Value(false);
  }

  if (args[0].isDataView()) {
    auto v = args[0].getGC<DataView>();
    if (isInternalProperty(key)) return Value(false);
    if (v->properties.find(key) != v->properties.end()) return Value(true);
    if (v->properties.find("__get_" + key) != v->properties.end()) return Value(true);
    if (v->properties.find("__set_" + key) != v->properties.end()) return Value(true);
    return Value(false);
  }

  if (args[0].isGenerator()) {
    auto g = args[0].getGC<Generator>();
    if (isInternalProperty(key)) return Value(false);
    if (g->properties.find(key) != g->properties.end()) return Value(true);
    if (g->properties.find("__get_" + key) != g->properties.end()) return Value(true);
    if (g->properties.find("__set_" + key) != g->properties.end()) return Value(true);
    return Value(false);
  }

  if (!args[0].isObject()) {
    return Value(false);
  }

  auto obj = args[0].getGC<Object>();

  if (obj->isModuleNamespace) {
    if (key == WellKnownSymbols::toStringTagKey()) {
      return Value(true);
    }
    bool isExport = std::find(obj->moduleExportNames.begin(), obj->moduleExportNames.end(), key) !=
                    obj->moduleExportNames.end();
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

  // Internal properties are not own properties
  if (key == "__proto__" && obj->properties.find("__own_prop___proto__") != obj->properties.end()) {
    return Value(true);
  }
  // Check for accessor properties before isInternalProperty check
  // (e.g., Object.prototype has __get___proto__ / __set___proto__)
  if (obj->properties.find("__get_" + key) != obj->properties.end()) return Value(true);
  if (obj->properties.find("__set_" + key) != obj->properties.end()) return Value(true);
  if (isInternalProperty(key)) return Value(false);
  if (obj->properties.find(key) != obj->properties.end()) return Value(true);
  return Value(false);
}

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

Value Object_getOwnPropertyNames(const std::vector<Value>& args) {
  auto result = makeArrayWithPrototype();

  if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
    throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
  }

  // Primitives: ToObject then get own property names
  if (args[0].isNumber() || args[0].isBool() || args[0].isBigInt() || args[0].isSymbol()) {
    return Value(result); // No own string-keyed properties
  }
  if (args[0].isString()) {
    const auto& str = std::get<std::string>(args[0].data);
    // String objects have indices 0..n-1 and "length"
    size_t cpIdx = 0, bytePos = 0;
    while (bytePos < str.size()) {
      result->elements.push_back(Value(std::to_string(cpIdx)));
      unsigned char c = str[bytePos];
      if (c < 0x80) bytePos += 1;
      else if ((c & 0xE0) == 0xC0) bytePos += 2;
      else if ((c & 0xF0) == 0xE0) bytePos += 3;
      else bytePos += 4;
      cpIdx++;
    }
    result->elements.push_back(Value(std::string("length")));
    return Value(result);
  }

  auto parseArrayIndexKey = [](const std::string& key, uint32_t& out) -> bool {
    if (key.empty()) return false;
    if (key.size() > 1 && key[0] == '0') return false;
    for (char c : key) {
      if (c < '0' || c > '9') return false;
    }
    try {
      unsigned long long parsed = std::stoull(key);
      if (parsed >= static_cast<unsigned long long>(std::numeric_limits<uint32_t>::max())) {
        return false;
      }
      out = static_cast<uint32_t>(parsed);
      return true;
    } catch (...) {
      return false;
    }
  };

  auto appendInOwnPropertyKeyOrder = [&](const std::vector<std::string>& keys,
                                         bool prioritizeLengthName) {
    std::vector<std::pair<uint32_t, std::string>> indexKeys;
    std::vector<std::string> stringKeys;
    for (const auto& k : keys) {
      uint32_t idx = 0;
      if (parseArrayIndexKey(k, idx)) {
        indexKeys.emplace_back(idx, k);
      } else {
        stringKeys.push_back(k);
      }
    }
    std::sort(indexKeys.begin(), indexKeys.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [_, k] : indexKeys) {
      result->elements.push_back(Value(k));
    }
    if (prioritizeLengthName) {
      auto emitFirst = [&](const char* key) {
        for (size_t i = 0; i < stringKeys.size(); i++) {
          if (stringKeys[i] == key) {
            result->elements.push_back(Value(stringKeys[i]));
            stringKeys.erase(stringKeys.begin() + static_cast<long>(i));
            return;
          }
        }
      };
      emitFirst("length");
      emitFirst("name");
    }
    for (const auto& k : stringKeys) {
      result->elements.push_back(Value(k));
    }
  };

  if (args[0].isFunction()) {
    auto fn = args[0].getGC<Function>();
    std::vector<std::string> keys;
    keys.reserve(fn->properties.size());
    for (const auto& rawKey : fn->properties.orderedKeys()) {
      if (isInternalProperty(rawKey)) continue;
      if (isSymbolPropertyKey(rawKey)) continue;
      keys.push_back(rawKey);
    }
    appendInOwnPropertyKeyOrder(keys, true);
    return Value(result);
  }

  if (args[0].isClass()) {
    auto cls = args[0].getGC<Class>();
    std::unordered_set<std::string> seen;
    std::vector<std::string> keys;
    keys.reserve(cls->properties.size());
    for (const auto& rawKey : cls->properties.orderedKeys()) {
      if (rawKey.size() >= 4 && rawKey.substr(0, 2) == "__" &&
          rawKey.substr(rawKey.size() - 2) == "__") {
        continue;
      }
      if (isSymbolPropertyKey(rawKey)) continue;
      if (rawKey.rfind("__non_enum_", 0) == 0 ||
          rawKey.rfind("__non_writable_", 0) == 0 ||
          rawKey.rfind("__non_configurable_", 0) == 0 ||
          rawKey.rfind("__enum_", 0) == 0) {
        continue;
      }
      std::string exposed = rawKey;
      if (rawKey.rfind("__get_", 0) == 0 || rawKey.rfind("__set_", 0) == 0) {
        exposed = rawKey.substr(6);
      }
      if (seen.insert(exposed).second) {
        keys.push_back(exposed);
      }
    }
    appendInOwnPropertyKeyOrder(keys, true);
    return Value(result);
  }

  if (args[0].isArray()) {
    auto arr = args[0].getGC<Array>();
    // Add numeric index keys for non-hole, non-deleted elements
    std::vector<std::string> keys;
    for (size_t i = 0; i < arr->elements.size(); i++) {
      std::string key = std::to_string(i);
      if (arr->properties.find("__deleted_" + key + "__") != arr->properties.end()) continue;
      if (arr->properties.find("__hole_" + key + "__") != arr->properties.end()) continue;
      keys.push_back(key);
    }
    // Add "length"
    if (arr->properties.find("__deleted_length__") == arr->properties.end()) {
      keys.push_back("length");
    }
    // Add non-index, non-internal own properties
    for (const auto& rawKey : arr->properties.orderedKeys()) {
      if (isInternalProperty(rawKey)) continue;
      if (isSymbolPropertyKey(rawKey)) continue;
      // Skip already-added numeric indices and length
      if (rawKey == "length") continue;
      uint32_t idx2;
      if (parseArrayIndexKey(rawKey, idx2)) continue;
      keys.push_back(rawKey);
    }
    for (const auto& k : keys) {
      result->elements.push_back(Value(k));
    }
    return Value(result);
  }

  if (args[0].isError()) {
    auto err = args[0].getGC<Error>();
    std::vector<std::string> keys;
    for (const auto& rawKey : err->properties.orderedKeys()) {
      if (isInternalProperty(rawKey)) continue;
      if (isSymbolPropertyKey(rawKey)) continue;
      keys.push_back(rawKey);
    }
    appendInOwnPropertyKeyOrder(keys, false);
    return Value(result);
  }

  if (args[0].isPromise()) {
    auto p = args[0].getGC<Promise>();
    std::vector<std::string> keys;
    for (const auto& rawKey : p->properties.orderedKeys()) {
      if (isInternalProperty(rawKey)) continue;
      if (isSymbolPropertyKey(rawKey)) continue;
      keys.push_back(rawKey);
    }
    appendInOwnPropertyKeyOrder(keys, false);
    return Value(result);
  }

  if (args[0].isRegex()) {
    auto rx = args[0].getGC<Regex>();
    std::vector<std::string> keys;
    // Add standard regex properties
    keys.push_back("source");
    keys.push_back("flags");
    for (const auto& rawKey : rx->properties.orderedKeys()) {
      if (isInternalProperty(rawKey)) continue;
      if (isSymbolPropertyKey(rawKey)) continue;
      if (rawKey == "source" || rawKey == "flags") continue;
      keys.push_back(rawKey);
    }
    appendInOwnPropertyKeyOrder(keys, false);
    return Value(result);
  }

  if (!args[0].isObject()) {
    return Value(result);
  }

  auto obj = args[0].getGC<Object>();

  if (obj->isModuleNamespace) {
    for (const auto& key : obj->moduleExportNames) {
      result->elements.push_back(Value(key));
    }
    return Value(result);
  }

  std::unordered_set<std::string> seen;
  std::vector<std::string> keys;
  keys.reserve(obj->properties.size());
  for (const auto& rawKey : obj->properties.orderedKeys()) {
    if (rawKey.size() >= 4 && rawKey.substr(0, 2) == "__" &&
        rawKey.substr(rawKey.size() - 2) == "__") {
      continue;
    }
    if (isSymbolPropertyKey(rawKey)) continue;
    if (rawKey.rfind("__non_enum_", 0) == 0 ||
        rawKey.rfind("__non_writable_", 0) == 0 ||
        rawKey.rfind("__non_configurable_", 0) == 0 ||
        rawKey.rfind("__enum_", 0) == 0 ||
        rawKey.rfind("__json_source_", 0) == 0) {
      continue;
    }
    std::string exposed = rawKey;
    if (rawKey.rfind("__get_", 0) == 0 || rawKey.rfind("__set_", 0) == 0) {
      exposed = rawKey.substr(6);
    }
    if (seen.insert(exposed).second) {
      keys.push_back(exposed);
    }
  }
  appendInOwnPropertyKeyOrder(keys, false);

  return Value(result);
}

Value Object_create(const std::vector<Value>& args) {
  auto newObj = GarbageCollector::makeGC<Object>();
  auto isObjectLikePrototype = [](const Value& value) {
    return value.isObject() || value.isArray() || value.isFunction() || value.isRegex() ||
           value.isProxy() || value.isPromise() || value.isGenerator() || value.isClass() ||
           value.isMap() || value.isSet() || value.isWeakMap() || value.isWeakSet() ||
           value.isTypedArray() || value.isArrayBuffer() || value.isDataView() || value.isError();
  };

  // Object.create(proto[, propertiesObject])
  if (!args.empty()) {
    const Value& proto = args[0];
    if (isObjectLikePrototype(proto)) {
      newObj->properties["__proto__"] = proto;
    } else if (proto.isNull()) {
      newObj->properties["__proto__"] = Value(Null{});
    } else if (!proto.isUndefined()) {
      // Spec: TypeError if proto is neither Object nor null.
      throw std::runtime_error("TypeError: Object prototype may only be an Object or null");
    }
  }

  if (args.size() > 1 && args[1].isObject()) {
    // Add properties from the properties descriptor object
    // Use orderedKeys() for spec-compliant property creation order
    auto props = args[1].getGC<Object>();
    for (const auto& key : props->properties.orderedKeys()) {
      // Skip internal property markers
      if (key.size() >= 4 && key.substr(0, 2) == "__" && key.substr(key.size() - 2) == "__") continue;
      if (key.rfind("__non_writable_", 0) == 0 || key.rfind("__non_enum_", 0) == 0 ||
          key.rfind("__non_configurable_", 0) == 0 || key.rfind("__enum_", 0) == 0 ||
          key.rfind("__get_", 0) == 0 || key.rfind("__set_", 0) == 0) continue;
      auto descIt = props->properties.find(key);
      if (descIt == props->properties.end() || !descIt->second.isObject()) continue;
      auto desc = std::get<GCPtr<Object>>(descIt->second.data);
      auto valueIt = desc->properties.find("value");
      auto getIt = desc->properties.find("get");
      auto setIt = desc->properties.find("set");
      if (valueIt != desc->properties.end()) {
        newObj->properties[key] = valueIt->second;
      } else if (getIt != desc->properties.end() || setIt != desc->properties.end()) {
        newObj->properties[key] = Value(Undefined{});
      }
      if (getIt != desc->properties.end() && getIt->second.isFunction()) {
        newObj->properties["__get_" + key] = getIt->second;
      }
      if (setIt != desc->properties.end() && setIt->second.isFunction()) {
        newObj->properties["__set_" + key] = setIt->second;
      }
      // Ensure property key exists even if no value/get/set
      if (newObj->properties.find(key) == newObj->properties.end()) {
        newObj->properties[key] = Value(Undefined{});
      }
      // Validate descriptor
      bool hasGetField = getIt != desc->properties.end();
      bool hasSetField = setIt != desc->properties.end();
      bool hasValueField = valueIt != desc->properties.end();
      auto writableIt = desc->properties.find("writable");
      bool hasWritableField = writableIt != desc->properties.end();
      // Validate: cannot mix data and accessor
      if ((hasGetField || hasSetField) && (hasValueField || hasWritableField)) {
        throw std::runtime_error("TypeError: Invalid property descriptor. Cannot both specify accessors and a value or writable attribute");
      }
      // Validate: getter/setter must be callable or undefined
      if (hasGetField && !getIt->second.isUndefined() && !getIt->second.isFunction()) {
        throw std::runtime_error("TypeError: Getter must be a function: " + getIt->second.toString());
      }
      if (hasSetField && !setIt->second.isUndefined() && !setIt->second.isFunction()) {
        throw std::runtime_error("TypeError: Setter must be a function: " + setIt->second.toString());
      }
      // Attributes default to false for defineProperty/Object.create
      bool isAccessor = hasGetField || hasSetField;
      if (!isAccessor) {
        if (hasWritableField && writableIt->second.toBool()) {
          newObj->properties.erase("__non_writable_" + key);
        } else {
          newObj->properties["__non_writable_" + key] = Value(true);
        }
      }
      auto enumIt = desc->properties.find("enumerable");
      if (enumIt != desc->properties.end() && enumIt->second.toBool()) {
        newObj->properties.erase("__non_enum_" + key);
      } else {
        newObj->properties["__non_enum_" + key] = Value(true);
      }
      auto configIt = desc->properties.find("configurable");
      if (configIt != desc->properties.end() && configIt->second.toBool()) {
        newObj->properties.erase("__non_configurable_" + key);
      } else {
        newObj->properties["__non_configurable_" + key] = Value(true);
      }
    }
  }

  return Value(newObj);
}

Value Object_fromEntries(const std::vector<Value>& args) {
  auto newObj = makeObjectWithPrototype();

  if (args.empty()) {
    throw std::runtime_error("TypeError: Object.fromEntries requires an iterable argument");
  }
  if (args[0].isNull() || args[0].isUndefined()) {
    throw std::runtime_error("TypeError: Object.fromEntries requires an iterable argument");
  }

  auto* interp = getGlobalInterpreter();
  if (!interp) {
    throw std::runtime_error("TypeError: Object.fromEntries requires an interpreter");
  }

  const Value& iterable = args[0];

  // Get @@iterator method
  const auto& iterKey = WellKnownSymbols::iteratorKey();
  auto [hasIter, iterMethod] = interp->getPropertyForExternal(iterable, iterKey);
  if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
  if (!hasIter || !iterMethod.isFunction()) {
    throw std::runtime_error("TypeError: object is not iterable");
  }

  // Call @@iterator to get iterator object
  Value iteratorObj = interp->callForHarness(iterMethod, {}, iterable);
  if (interp->hasError()) {
    Value err = interp->getError(); interp->clearError();
    throw JsValueException(err);
  }

  // Get next method
  Value nextFn;
  bool isGen = iteratorObj.isGenerator();
  if (!isGen) {
    auto [nf, fn] = interp->getPropertyForExternal(iteratorObj, "next");
    if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
    if (!nf || !fn.isFunction()) {
      throw std::runtime_error("TypeError: iterator result is not an object");
    }
    nextFn = fn;
  }

  // Cache return method for iterator closing
  Value returnFn;
  if (!isGen) {
    auto [hr, rf] = interp->getPropertyForExternal(iteratorObj, "return");
    if (interp->hasError()) interp->clearError();
    if (hr && rf.isFunction()) returnFn = rf;
  }
  auto closeIter = [&]() {
    if (returnFn.isFunction()) {
      interp->callForHarness(returnFn, {}, iteratorObj);
      if (interp->hasError()) interp->clearError();
    }
  };

  // Iterate
  for (int i = 0; i < 100000; ++i) {
    Value step;
    if (isGen) {
      step = interp->generatorNext(iteratorObj, Value(Undefined{}));
    } else {
      step = interp->callForHarness(nextFn, {}, iteratorObj);
    }
    if (interp->hasError()) {
      Value err = interp->getError(); interp->clearError();
      throw JsValueException(err);
    }

    // Check done
    auto [hasDone, doneVal] = interp->getPropertyForExternal(step, "done");
    if (interp->hasError()) {
      Value err = interp->getError(); interp->clearError();
      throw JsValueException(err);
    }
    if (hasDone && doneVal.toBool()) break;

    // Get value
    auto [hasVal, entry] = interp->getPropertyForExternal(step, "value");
    if (interp->hasError()) {
      Value err = interp->getError(); interp->clearError();
      throw JsValueException(err);
    }

    // Entry must be an object (array-like with [0] and [1])
    if (entry.isNull() || entry.isUndefined() || entry.isBool() ||
        entry.isNumber() || entry.isBigInt() || entry.isSymbol()) {
      closeIter();
      throw std::runtime_error("TypeError: Iterator value " + entry.toString() + " is not an entry object");
    }

    // String entries: "ab" → key=0, value=1 (char at index 0 and 1)
    if (entry.isString()) {
      const std::string& str = std::get<std::string>(entry.data);
      // Strings are array-like; access [0] and [1] via getPropertyForExternal
      auto [k0, key] = interp->getPropertyForExternal(entry, "0");
      auto [k1, val] = interp->getPropertyForExternal(entry, "1");
      if (interp->hasError()) {
        Value err = interp->getError(); interp->clearError();
        closeIter();
        throw JsValueException(err);
      }
      if (!k0 || !k1) {
        closeIter();
        throw std::runtime_error("TypeError: Iterator value " + str + " is not an entry object");
      }
      newObj->properties[valueToPropertyKey(key)] = val;
      continue;
    }

    // Get key (entry[0]) and value (entry[1]) via property access
    auto [k0, keyVal] = interp->getPropertyForExternal(entry, "0");
    if (interp->hasError()) {
      Value err = interp->getError(); interp->clearError();
      closeIter();
      throw JsValueException(err);
    }
    auto [k1, valVal] = interp->getPropertyForExternal(entry, "1");
    if (interp->hasError()) {
      Value err = interp->getError(); interp->clearError();
      closeIter();
      throw JsValueException(err);
    }

    std::string key = valueToPropertyKey(keyVal);
    newObj->properties[key] = valVal;
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

static size_t dataViewAccessibleLength(const DataView& view, size_t elementSize) {
  if (!view.buffer || view.buffer->detached) {
    throw std::runtime_error("TypeError: DataView has a detached buffer");
  }
  size_t visibleLength = view.currentByteLength();
  if (view.byteOffset > view.buffer->byteLength || elementSize > visibleLength) {
    return visibleLength;
  }
  return visibleLength;
}

// DataView get methods
int8_t DataView::getInt8(size_t offset) const {
  size_t visibleLength = dataViewAccessibleLength(*this, sizeof(int8_t));
  if (offset >= visibleLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }
  return static_cast<int8_t>(buffer->data[byteOffset + offset]);
}

uint8_t DataView::getUint8(size_t offset) const {
  size_t visibleLength = dataViewAccessibleLength(*this, sizeof(uint8_t));
  if (offset >= visibleLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }
  return buffer->data[byteOffset + offset];
}

int16_t DataView::getInt16(size_t offset, bool littleEndian) const {
  size_t visibleLength = dataViewAccessibleLength(*this, sizeof(int16_t));
  if (offset + sizeof(int16_t) > visibleLength) {
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
  size_t visibleLength = dataViewAccessibleLength(*this, sizeof(uint16_t));
  if (offset + sizeof(uint16_t) > visibleLength) {
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
  size_t visibleLength = dataViewAccessibleLength(*this, sizeof(int32_t));
  if (offset + sizeof(int32_t) > visibleLength) {
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
  size_t visibleLength = dataViewAccessibleLength(*this, sizeof(uint32_t));
  if (offset + sizeof(uint32_t) > visibleLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  uint32_t value;
  std::memcpy(&value, &buffer->data[byteOffset + offset], sizeof(uint32_t));

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  return value;
}

float DataView::getFloat16(size_t offset, bool littleEndian) const {
  size_t visibleLength = dataViewAccessibleLength(*this, sizeof(uint16_t));
  if (offset + sizeof(uint16_t) > visibleLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  uint16_t value;
  std::memcpy(&value, &buffer->data[byteOffset + offset], sizeof(uint16_t));

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  return float16_to_float32(value);
}

float DataView::getFloat32(size_t offset, bool littleEndian) const {
  size_t visibleLength = dataViewAccessibleLength(*this, sizeof(float));
  if (offset + sizeof(float) > visibleLength) {
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
  size_t visibleLength = dataViewAccessibleLength(*this, sizeof(double));
  if (offset + sizeof(double) > visibleLength) {
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
  size_t visibleLength = dataViewAccessibleLength(*this, sizeof(int64_t));
  if (offset + sizeof(int64_t) > visibleLength) {
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
  size_t visibleLength = dataViewAccessibleLength(*this, sizeof(uint64_t));
  if (offset + sizeof(uint64_t) > visibleLength) {
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
  size_t visibleLength = dataViewAccessibleLength(*this, sizeof(int8_t));
  if (offset >= visibleLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }
  buffer->data[byteOffset + offset] = static_cast<uint8_t>(value);
}

void DataView::setUint8(size_t offset, uint8_t value) {
  size_t visibleLength = dataViewAccessibleLength(*this, sizeof(uint8_t));
  if (offset >= visibleLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }
  buffer->data[byteOffset + offset] = value;
}

void DataView::setInt16(size_t offset, int16_t value, bool littleEndian) {
  size_t visibleLength = dataViewAccessibleLength(*this, sizeof(int16_t));
  if (offset + sizeof(int16_t) > visibleLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  std::memcpy(&buffer->data[byteOffset + offset], &value, sizeof(int16_t));
}

void DataView::setUint16(size_t offset, uint16_t value, bool littleEndian) {
  size_t visibleLength = dataViewAccessibleLength(*this, sizeof(uint16_t));
  if (offset + sizeof(uint16_t) > visibleLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  std::memcpy(&buffer->data[byteOffset + offset], &value, sizeof(uint16_t));
}

void DataView::setInt32(size_t offset, int32_t value, bool littleEndian) {
  size_t visibleLength = dataViewAccessibleLength(*this, sizeof(int32_t));
  if (offset + sizeof(int32_t) > visibleLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  std::memcpy(&buffer->data[byteOffset + offset], &value, sizeof(int32_t));
}

void DataView::setUint32(size_t offset, uint32_t value, bool littleEndian) {
  size_t visibleLength = dataViewAccessibleLength(*this, sizeof(uint32_t));
  if (offset + sizeof(uint32_t) > visibleLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  std::memcpy(&buffer->data[byteOffset + offset], &value, sizeof(uint32_t));
}

void DataView::setFloat16(size_t offset, double value, bool littleEndian) {
  size_t visibleLength = dataViewAccessibleLength(*this, sizeof(uint16_t));
  if (offset + sizeof(uint16_t) > visibleLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  uint16_t bits = float64_to_float16(value);
  if (littleEndian != isLittleEndian()) {
    bits = swapEndian(bits);
  }
  std::memcpy(&buffer->data[byteOffset + offset], &bits, sizeof(uint16_t));
}

void DataView::setFloat32(size_t offset, float value, bool littleEndian) {
  size_t visibleLength = dataViewAccessibleLength(*this, sizeof(float));
  if (offset + sizeof(float) > visibleLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  std::memcpy(&buffer->data[byteOffset + offset], &value, sizeof(float));
}

void DataView::setFloat64(size_t offset, double value, bool littleEndian) {
  size_t visibleLength = dataViewAccessibleLength(*this, sizeof(double));
  if (offset + sizeof(double) > visibleLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  std::memcpy(&buffer->data[byteOffset + offset], &value, sizeof(double));
}

void DataView::setBigInt64(size_t offset, int64_t value, bool littleEndian) {
  size_t visibleLength = dataViewAccessibleLength(*this, sizeof(int64_t));
  if (offset + sizeof(int64_t) > visibleLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  std::memcpy(&buffer->data[byteOffset + offset], &value, sizeof(int64_t));
}

void DataView::setBigUint64(size_t offset, uint64_t value, bool littleEndian) {
  size_t visibleLength = dataViewAccessibleLength(*this, sizeof(uint64_t));
  if (offset + sizeof(uint64_t) > visibleLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  std::memcpy(&buffer->data[byteOffset + offset], &value, sizeof(uint64_t));
}

}
