#pragma once

#include <boost/multiprecision/cpp_int.hpp>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <limits>
#include <cmath>
#include <stdexcept>
#include <string>
#include <algorithm>

namespace lightjs::bigint {

using BigIntValue = boost::multiprecision::cpp_int;

inline std::string trimAsciiWhitespace(const std::string& input) {
  size_t start = 0;
  while (start < input.size() &&
         std::isspace(static_cast<unsigned char>(input[start]))) {
    start++;
  }
  size_t end = input.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(input[end - 1]))) {
    end--;
  }
  return input.substr(start, end - start);
}

inline int digitValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
  return -1;
}

inline bool parseWithOptions(const std::string& raw,
                             BigIntValue& out,
                             bool allowSign,
                             bool allowSeparators,
                             bool trimWhitespace,
                             bool allowEmptyAsZero,
                             bool rejectSignWithPrefix) {
  std::string s = trimWhitespace ? trimAsciiWhitespace(raw) : raw;
  if (s.empty()) {
    if (allowEmptyAsZero) {
      out = 0;
      return true;
    }
    return false;
  }

  size_t i = 0;
  bool negative = false;
  bool hadSign = false;
  if (allowSign && (s[i] == '+' || s[i] == '-')) {
    hadSign = true;
    negative = (s[i] == '-');
    i++;
  }
  if (i >= s.size()) return false;

  int base = 10;
  if (i + 1 < s.size() && s[i] == '0') {
    if (s[i + 1] == 'x' || s[i + 1] == 'X') {
      if (hadSign && rejectSignWithPrefix) return false;
      base = 16;
      i += 2;
    } else if (s[i + 1] == 'o' || s[i + 1] == 'O') {
      if (hadSign && rejectSignWithPrefix) return false;
      base = 8;
      i += 2;
    } else if (s[i + 1] == 'b' || s[i + 1] == 'B') {
      if (hadSign && rejectSignWithPrefix) return false;
      base = 2;
      i += 2;
    }
  }
  if (i >= s.size()) return false;

  BigIntValue value = 0;
  bool sawDigit = false;
  bool prevSep = false;
  for (; i < s.size(); i++) {
    char c = s[i];
    if (c == '_') {
      if (!allowSeparators || !sawDigit || prevSep) return false;
      prevSep = true;
      continue;
    }
    int d = digitValue(c);
    if (d < 0 || d >= base) return false;
    value *= base;
    value += d;
    sawDigit = true;
    prevSep = false;
  }
  if (!sawDigit || prevSep) return false;

  out = negative ? -value : value;
  return true;
}

inline bool parseBigIntLiteral(const std::string& raw, BigIntValue& out) {
  return parseWithOptions(raw, out, false, true, false, false, true);
}

inline bool parseBigIntString(const std::string& raw, BigIntValue& out) {
  return parseWithOptions(raw, out, true, false, true, true, true);
}

inline bool fitsInt64(const BigIntValue& v) {
  static const BigIntValue kMin = std::numeric_limits<int64_t>::min();
  static const BigIntValue kMax = std::numeric_limits<int64_t>::max();
  return v >= kMin && v <= kMax;
}

inline bool fitsUint64(const BigIntValue& v) {
  static const BigIntValue kMax = std::numeric_limits<uint64_t>::max();
  return v >= 0 && v <= kMax;
}

inline BigIntValue asUintN(uint64_t bits, const BigIntValue& v) {
  if (bits == 0) return 0;
  BigIntValue modulus = BigIntValue(1);
  modulus <<= bits;
  BigIntValue out = v % modulus;
  if (out < 0) out += modulus;
  return out;
}

inline BigIntValue asIntN(uint64_t bits, const BigIntValue& v) {
  if (bits == 0) return 0;
  BigIntValue modulus = BigIntValue(1);
  modulus <<= bits;
  BigIntValue unsignedValue = asUintN(bits, v);
  BigIntValue signBit = BigIntValue(1) << (bits - 1);
  if (unsignedValue >= signBit) {
    return unsignedValue - modulus;
  }
  return unsignedValue;
}

inline uint64_t toUint64Trunc(const BigIntValue& v) {
  return asUintN(64, v).convert_to<uint64_t>();
}

inline int64_t toInt64Trunc(const BigIntValue& v) {
  return static_cast<int64_t>(toUint64Trunc(v));
}

inline std::string toString(const BigIntValue& value, int radix = 10) {
  if (radix < 2 || radix > 36) {
    throw std::runtime_error("RangeError: radix must be between 2 and 36");
  }
  if (value == 0) {
    return "0";
  }
  if (radix == 10) {
    return value.convert_to<std::string>();
  }

  static const char* kDigits = "0123456789abcdefghijklmnopqrstuvwxyz";
  bool negative = value < 0;
  BigIntValue n = negative ? -value : value;
  BigIntValue base = radix;
  std::string out;
  while (n > 0) {
    BigIntValue rem = n % base;
    n /= base;
    out.push_back(kDigits[rem.convert_to<unsigned int>()]);
  }
  if (negative) out.push_back('-');
  std::reverse(out.begin(), out.end());
  return out;
}

inline bool toSizeT(const BigIntValue& v, size_t& out) {
  if (v < 0) return false;
  static const BigIntValue kMax = std::numeric_limits<size_t>::max();
  if (v > kMax) return false;
  out = v.convert_to<size_t>();
  return true;
}

inline bool fromIntegralDouble(double n, BigIntValue& out) {
  if (!std::isfinite(n) || std::trunc(n) != n) return false;
  if (n == 0.0) {
    out = 0;
    return true;
  }

  uint64_t bits = 0;
  std::memcpy(&bits, &n, sizeof(double));
  bool negative = (bits >> 63) != 0;
  uint64_t expBits = (bits >> 52) & 0x7ffu;
  uint64_t frac = bits & 0x000fffffffffffffull;

  if (expBits == 0x7ffu) return false;  // NaN/Inf
  if (expBits == 0u) {  // subnormal integrals can only be zero
    out = 0;
    return false;
  }

  int exp = static_cast<int>(expBits) - 1023;
  uint64_t significand = (uint64_t{1} << 52) | frac;
  BigIntValue value = significand;
  if (exp >= 52) {
    value <<= static_cast<unsigned int>(exp - 52);
  } else if (exp >= 0) {
    uint64_t mask = (uint64_t{1} << (52 - exp)) - 1;
    if ((significand & mask) != 0) return false;
    value >>= static_cast<unsigned int>(52 - exp);
  } else {
    return false;
  }

  out = negative ? -value : value;
  return true;
}

}  // namespace lightjs::bigint
