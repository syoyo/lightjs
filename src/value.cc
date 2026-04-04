#include "value_internal.h"

namespace lightjs {

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
  auto* interp = getGlobalInterpreter();
  bool isObjectLikeForPropertyKey =
    value.isObject() || value.isArray() || value.isFunction() || value.isRegex() ||
    value.isProxy() || value.isPromise() || value.isGenerator() || value.isClass() ||
    value.isMap() || value.isSet() || value.isWeakMap() || value.isWeakSet() ||
    value.isTypedArray() || value.isArrayBuffer() || value.isDataView() || value.isError();
  if (interp && isObjectLikeForPropertyKey) {
    Value primitive = interp->toPrimitive(value, true);
    if (interp->hasError()) {
      Value err = interp->getError();
      interp->clearError();
      throw JsValueException(err);
    }
    if (primitive.isSymbol()) {
      return symbolToPropertyKey(std::get<Symbol>(primitive.data));
    }
    return primitive.toString();
  }

  // Best-effort fallback when there is no active interpreter.
  if (value.isObject() || value.isArray() || value.isFunction()) {
    if (value.isObject()) {
    auto obj = value.getGC<Object>();
    auto primIt = obj->properties.find("__primitive_value__");
    if (primIt != obj->properties.end()) {
      if (primIt->second.isSymbol()) {
        return symbolToPropertyKey(std::get<Symbol>(primIt->second.data));
      }
      return primIt->second.toString();
    }
    } // end if (value.isObject())
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
      return "/" + escapeRegexPatternSource(arg->pattern, arg->flags) + "/" +
             canonicalizeRegexFlags(arg->flags);
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

}  // namespace lightjs
