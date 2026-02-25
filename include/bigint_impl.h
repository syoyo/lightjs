#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace lightjs::bigint {

// Arbitrary-precision integer using sign-magnitude representation.
// Digits are stored little-endian in base 2^32.
class BigIntValue {
 public:
  // --- Constructors ---
  BigIntValue() : negative_(false) {}

  BigIntValue(int v) {
    if (v < 0) {
      negative_ = true;
      // Handle INT_MIN safely
      uint32_t mag = static_cast<uint32_t>(-(static_cast<int64_t>(v)));
      digits_.push_back(mag);
    } else if (v > 0) {
      negative_ = false;
      digits_.push_back(static_cast<uint32_t>(v));
    } else {
      negative_ = false;
    }
  }

  BigIntValue(int64_t v) {
    if (v < 0) {
      negative_ = true;
      uint64_t mag = static_cast<uint64_t>(-(v + 1)) + 1u;
      digits_.push_back(static_cast<uint32_t>(mag));
      uint32_t hi = static_cast<uint32_t>(mag >> 32);
      if (hi) digits_.push_back(hi);
    } else if (v > 0) {
      negative_ = false;
      digits_.push_back(static_cast<uint32_t>(v));
      uint32_t hi = static_cast<uint32_t>(static_cast<uint64_t>(v) >> 32);
      if (hi) digits_.push_back(hi);
    } else {
      negative_ = false;
    }
  }

  BigIntValue(uint64_t v) : negative_(false) {
    if (v > 0) {
      digits_.push_back(static_cast<uint32_t>(v));
      uint32_t hi = static_cast<uint32_t>(v >> 32);
      if (hi) digits_.push_back(hi);
    }
  }

  BigIntValue(const BigIntValue&) = default;
  BigIntValue(BigIntValue&&) = default;
  BigIntValue& operator=(const BigIntValue&) = default;
  BigIntValue& operator=(BigIntValue&&) = default;
  BigIntValue& operator=(int v) { *this = BigIntValue(v); return *this; }
  BigIntValue& operator=(int64_t v) { *this = BigIntValue(v); return *this; }

  // --- Comparison ---
  bool isZero() const { return digits_.empty(); }
  bool isNegative() const { return negative_ && !isZero(); }

  friend int compare(const BigIntValue& a, const BigIntValue& b);
  friend bool operator==(const BigIntValue& a, const BigIntValue& b);
  friend bool operator!=(const BigIntValue& a, const BigIntValue& b);
  friend bool operator<(const BigIntValue& a, const BigIntValue& b);
  friend bool operator>(const BigIntValue& a, const BigIntValue& b);
  friend bool operator<=(const BigIntValue& a, const BigIntValue& b);
  friend bool operator>=(const BigIntValue& a, const BigIntValue& b);

  // Comparison with int
  friend bool operator==(const BigIntValue& a, int b);
  friend bool operator!=(const BigIntValue& a, int b);
  friend bool operator<(const BigIntValue& a, int b);
  friend bool operator>(const BigIntValue& a, int b);
  friend bool operator<=(const BigIntValue& a, int b);
  friend bool operator>=(const BigIntValue& a, int b);

  // --- Arithmetic ---
  friend BigIntValue operator+(const BigIntValue& a, const BigIntValue& b);
  friend BigIntValue operator-(const BigIntValue& a, const BigIntValue& b);
  friend BigIntValue operator*(const BigIntValue& a, const BigIntValue& b);
  friend BigIntValue operator/(const BigIntValue& a, const BigIntValue& b);
  friend BigIntValue operator%(const BigIntValue& a, const BigIntValue& b);
  BigIntValue operator-() const;

  BigIntValue& operator+=(const BigIntValue& rhs);
  BigIntValue& operator-=(const BigIntValue& rhs);
  BigIntValue& operator*=(const BigIntValue& rhs);
  BigIntValue& operator/=(const BigIntValue& rhs);
  BigIntValue& operator*=(int rhs);
  BigIntValue& operator+=(int rhs);

  // --- Bitwise ---
  friend BigIntValue operator&(const BigIntValue& a, const BigIntValue& b);
  friend BigIntValue operator|(const BigIntValue& a, const BigIntValue& b);
  friend BigIntValue operator^(const BigIntValue& a, const BigIntValue& b);
  BigIntValue operator~() const;

  // Bitwise with int (for & 1 pattern)
  friend BigIntValue operator&(const BigIntValue& a, int b);

  // --- Shift ---
  friend BigIntValue operator<<(const BigIntValue& a, size_t count);
  friend BigIntValue operator>>(const BigIntValue& a, size_t count);
  BigIntValue& operator<<=(size_t count);
  BigIntValue& operator>>=(size_t count);

  // Overloads to avoid ambiguity with int/unsigned int literals
  BigIntValue& operator<<=(unsigned int count) {
    return operator<<=(static_cast<size_t>(count));
  }
  BigIntValue& operator>>=(unsigned int count) {
    return operator>>=(static_cast<size_t>(count));
  }
  BigIntValue& operator<<=(int count) {
    return operator<<=(static_cast<size_t>(count));
  }
  BigIntValue& operator>>=(int count) {
    return operator>>=(static_cast<size_t>(count));
  }

  // --- Conversion ---
  template <typename T>
  T convert_to() const;

  // --- Internal access for implementation ---
  const std::vector<uint32_t>& digits() const { return digits_; }
  bool negative() const { return negative_; }

 private:
  bool negative_ = false;
  std::vector<uint32_t> digits_;  // little-endian base-2^32

  void normalize();

  static int compareMagnitude(const std::vector<uint32_t>& a,
                              const std::vector<uint32_t>& b);

  static std::vector<uint32_t> addMagnitude(const std::vector<uint32_t>& a,
                                            const std::vector<uint32_t>& b);

  static std::vector<uint32_t> subMagnitude(const std::vector<uint32_t>& a,
                                            const std::vector<uint32_t>& b);

  static std::vector<uint32_t> mulMagnitude(const std::vector<uint32_t>& a,
                                            const std::vector<uint32_t>& b);

  static void divModMagnitude(const std::vector<uint32_t>& u,
                              const std::vector<uint32_t>& v,
                              std::vector<uint32_t>& q,
                              std::vector<uint32_t>& r);

  static uint32_t divModSingleLimb(std::vector<uint32_t>& u, uint32_t v);

  static std::vector<uint32_t> toTwosComplement(const BigIntValue& val,
                                                size_t width);

  static BigIntValue fromTwosComplement(const std::vector<uint32_t>& bits,
                                        size_t width);
};

// Template specializations declared (defined in bigint_impl.cc)
template <> double BigIntValue::convert_to<double>() const;
template <> std::string BigIntValue::convert_to<std::string>() const;
template <> uint64_t BigIntValue::convert_to<uint64_t>() const;
template <> unsigned int BigIntValue::convert_to<unsigned int>() const;

// size_t may be the same type as uint64_t on 64-bit platforms
#if !defined(__LP64__) && !defined(_LP64) && !defined(__x86_64__) && \
    !defined(__aarch64__) && !defined(_M_X64)
template <> size_t BigIntValue::convert_to<size_t>() const;
#endif

// --- Inline comparison operators ---

inline int compare(const BigIntValue& a, const BigIntValue& b) {
  bool aNeg = a.isNegative();
  bool bNeg = b.isNegative();
  if (a.isZero() && b.isZero()) return 0;
  if (aNeg && !bNeg) return -1;
  if (!aNeg && bNeg) return 1;
  int mc = BigIntValue::compareMagnitude(a.digits_, b.digits_);
  return aNeg ? -mc : mc;
}

inline bool operator==(const BigIntValue& a, const BigIntValue& b) {
  return compare(a, b) == 0;
}
inline bool operator!=(const BigIntValue& a, const BigIntValue& b) {
  return compare(a, b) != 0;
}
inline bool operator<(const BigIntValue& a, const BigIntValue& b) {
  return compare(a, b) < 0;
}
inline bool operator>(const BigIntValue& a, const BigIntValue& b) {
  return compare(a, b) > 0;
}
inline bool operator<=(const BigIntValue& a, const BigIntValue& b) {
  return compare(a, b) <= 0;
}
inline bool operator>=(const BigIntValue& a, const BigIntValue& b) {
  return compare(a, b) >= 0;
}

inline bool operator==(const BigIntValue& a, int b) {
  return a == BigIntValue(b);
}
inline bool operator!=(const BigIntValue& a, int b) {
  return a != BigIntValue(b);
}
inline bool operator<(const BigIntValue& a, int b) {
  return a < BigIntValue(b);
}
inline bool operator>(const BigIntValue& a, int b) {
  return a > BigIntValue(b);
}
inline bool operator<=(const BigIntValue& a, int b) {
  return a <= BigIntValue(b);
}
inline bool operator>=(const BigIntValue& a, int b) {
  return a >= BigIntValue(b);
}

}  // namespace lightjs::bigint
