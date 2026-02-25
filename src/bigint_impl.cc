#include "bigint_impl.h"

#include <cassert>
#include <cmath>
#include <stdexcept>

namespace lightjs::bigint {

// ===========================================================================
// Internal helpers
// ===========================================================================

void BigIntValue::normalize() {
  while (!digits_.empty() && digits_.back() == 0) {
    digits_.pop_back();
  }
  if (digits_.empty()) negative_ = false;
}

int BigIntValue::compareMagnitude(const std::vector<uint32_t>& a,
                                  const std::vector<uint32_t>& b) {
  if (a.size() != b.size())
    return a.size() < b.size() ? -1 : 1;
  for (size_t i = a.size(); i-- > 0;) {
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  }
  return 0;
}

std::vector<uint32_t> BigIntValue::addMagnitude(
    const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
  size_t n = std::max(a.size(), b.size());
  std::vector<uint32_t> result(n);
  uint64_t carry = 0;
  for (size_t i = 0; i < n; i++) {
    uint64_t sum = carry;
    if (i < a.size()) sum += a[i];
    if (i < b.size()) sum += b[i];
    result[i] = static_cast<uint32_t>(sum);
    carry = sum >> 32;
  }
  if (carry) result.push_back(static_cast<uint32_t>(carry));
  return result;
}

// Precondition: a >= b in magnitude
std::vector<uint32_t> BigIntValue::subMagnitude(
    const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
  std::vector<uint32_t> result(a.size());
  int64_t borrow = 0;
  for (size_t i = 0; i < a.size(); i++) {
    int64_t diff = static_cast<int64_t>(a[i]) - borrow;
    if (i < b.size()) diff -= b[i];
    if (diff < 0) {
      diff += (int64_t{1} << 32);
      borrow = 1;
    } else {
      borrow = 0;
    }
    result[i] = static_cast<uint32_t>(diff);
  }
  return result;
}

// Schoolbook O(n*m) multiplication
std::vector<uint32_t> BigIntValue::mulMagnitude(
    const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
  if (a.empty() || b.empty()) return {};
  std::vector<uint32_t> result(a.size() + b.size(), 0);
  for (size_t i = 0; i < a.size(); i++) {
    uint64_t carry = 0;
    for (size_t j = 0; j < b.size(); j++) {
      uint64_t prod =
          static_cast<uint64_t>(a[i]) * b[j] + result[i + j] + carry;
      result[i + j] = static_cast<uint32_t>(prod);
      carry = prod >> 32;
    }
    result[i + b.size()] += static_cast<uint32_t>(carry);
  }
  return result;
}

// Divide magnitude u by single limb v, returning remainder.
// Modifies u in-place to hold the quotient.
uint32_t BigIntValue::divModSingleLimb(std::vector<uint32_t>& u, uint32_t v) {
  uint64_t rem = 0;
  for (size_t i = u.size(); i-- > 0;) {
    uint64_t cur = (rem << 32) | u[i];
    u[i] = static_cast<uint32_t>(cur / v);
    rem = cur % v;
  }
  return static_cast<uint32_t>(rem);
}

// Knuth Algorithm D for multi-limb division.
// u / v -> quotient q, remainder r.
// Precondition: v has at least 2 limbs and v's leading limb is nonzero.
void BigIntValue::divModMagnitude(const std::vector<uint32_t>& u,
                                  const std::vector<uint32_t>& v,
                                  std::vector<uint32_t>& q,
                                  std::vector<uint32_t>& r) {
  size_t n = v.size();
  size_t m = u.size() - n;

  // Handle case where u < v
  if (compareMagnitude(u, v) < 0) {
    q.clear();
    r = u;
    return;
  }

  if (n == 1) {
    // Single-limb divisor
    std::vector<uint32_t> uCopy = u;
    uint32_t rem = divModSingleLimb(uCopy, v[0]);
    q = std::move(uCopy);
    // Trim leading zeros
    while (!q.empty() && q.back() == 0) q.pop_back();
    r.clear();
    if (rem) r.push_back(rem);
    return;
  }

  // D1: Normalize - shift so that v's leading digit >= 2^31
  int shift = 0;
  {
    uint32_t vn = v[n - 1];
    while ((vn & 0x80000000u) == 0) {
      vn <<= 1;
      shift++;
    }
  }

  // Shift u and v left by 'shift' bits
  std::vector<uint32_t> un(u.size() + 1, 0);
  std::vector<uint32_t> vn(n);

  if (shift > 0) {
    uint32_t carry = 0;
    for (size_t i = 0; i < u.size(); i++) {
      uint64_t tmp = (static_cast<uint64_t>(u[i]) << shift) | carry;
      un[i] = static_cast<uint32_t>(tmp);
      carry = static_cast<uint32_t>(tmp >> 32);
    }
    un[u.size()] = carry;

    carry = 0;
    for (size_t i = 0; i < n; i++) {
      uint64_t tmp = (static_cast<uint64_t>(v[i]) << shift) | carry;
      vn[i] = static_cast<uint32_t>(tmp);
      carry = static_cast<uint32_t>(tmp >> 32);
    }
  } else {
    for (size_t i = 0; i < u.size(); i++) un[i] = u[i];
    for (size_t i = 0; i < n; i++) vn[i] = v[i];
  }

  q.resize(m + 1, 0);

  // D2-D7: Main loop
  for (size_t j = m + 1; j-- > 0;) {
    // D3: Calculate trial quotient
    uint64_t qhat;
    uint64_t rhat;
    uint64_t un_j_n = un[j + n];
    uint64_t un_j_n1 = un[j + n - 1];

    if (un_j_n == vn[n - 1]) {
      qhat = 0xFFFFFFFFULL;
      rhat = un_j_n1 + vn[n - 1];
    } else {
      uint64_t num = (un_j_n << 32) | un_j_n1;
      qhat = num / vn[n - 1];
      rhat = num % vn[n - 1];
    }

    // Refine qhat
    while (qhat > 0xFFFFFFFFULL ||
           qhat * vn[n - 2] > ((rhat << 32) | un[j + n - 2])) {
      qhat--;
      rhat += vn[n - 1];
      if (rhat > 0xFFFFFFFFULL) break;
    }

    // D4: Multiply and subtract
    int64_t borrow = 0;
    for (size_t i = 0; i < n; i++) {
      uint64_t prod = qhat * vn[i];
      int64_t diff =
          static_cast<int64_t>(un[j + i]) - static_cast<int64_t>(prod & 0xFFFFFFFFULL) + borrow;
      un[j + i] = static_cast<uint32_t>(diff);
      borrow = (diff >> 32) - static_cast<int64_t>(prod >> 32);
    }
    int64_t diff2 = static_cast<int64_t>(un[j + n]) + borrow;
    un[j + n] = static_cast<uint32_t>(diff2);

    q[j] = static_cast<uint32_t>(qhat);

    // D5-D6: If we subtracted too much, add back
    if (diff2 < 0) {
      q[j]--;
      uint64_t carry = 0;
      for (size_t i = 0; i < n; i++) {
        uint64_t sum = static_cast<uint64_t>(un[j + i]) + vn[i] + carry;
        un[j + i] = static_cast<uint32_t>(sum);
        carry = sum >> 32;
      }
      un[j + n] += static_cast<uint32_t>(carry);
    }
  }

  // Trim leading zeros from quotient
  while (!q.empty() && q.back() == 0) q.pop_back();

  // D8: Unnormalize remainder
  r.resize(n);
  if (shift > 0) {
    uint32_t carry = 0;
    for (size_t i = n; i-- > 0;) {
      uint64_t tmp = (static_cast<uint64_t>(carry) << 32) | un[i];
      r[i] = static_cast<uint32_t>(tmp >> shift);
      carry = static_cast<uint32_t>(un[i] & ((1u << shift) - 1));
    }
  } else {
    for (size_t i = 0; i < n; i++) r[i] = un[i];
  }
  while (!r.empty() && r.back() == 0) r.pop_back();
}

// ===========================================================================
// Two's complement conversion for bitwise ops on negative values
// ===========================================================================

// Convert BigIntValue to two's complement bit vector of given width (in limbs).
std::vector<uint32_t> BigIntValue::toTwosComplement(const BigIntValue& val,
                                                    size_t width) {
  std::vector<uint32_t> result(width, 0);
  for (size_t i = 0; i < std::min(val.digits_.size(), width); i++) {
    result[i] = val.digits_[i];
  }
  if (val.isNegative()) {
    // Negate: invert all bits and add 1
    uint64_t carry = 1;
    for (size_t i = 0; i < width; i++) {
      uint64_t tmp = static_cast<uint64_t>(~result[i]) + carry;
      result[i] = static_cast<uint32_t>(tmp);
      carry = tmp >> 32;
    }
  }
  return result;
}

// Convert two's complement bit vector back to BigIntValue.
BigIntValue BigIntValue::fromTwosComplement(const std::vector<uint32_t>& bits,
                                            size_t width) {
  if (width == 0) return BigIntValue(0);
  // Check sign bit
  bool neg = (bits[width - 1] & 0x80000000u) != 0;
  BigIntValue result;
  if (neg) {
    // Negate: invert and add 1
    result.digits_.resize(width);
    uint64_t carry = 1;
    for (size_t i = 0; i < width; i++) {
      uint64_t tmp = static_cast<uint64_t>(~bits[i]) + carry;
      result.digits_[i] = static_cast<uint32_t>(tmp);
      carry = tmp >> 32;
    }
    result.negative_ = true;
  } else {
    result.digits_ = bits;
  }
  result.normalize();
  return result;
}

// ===========================================================================
// Arithmetic operators
// ===========================================================================

BigIntValue operator+(const BigIntValue& a, const BigIntValue& b) {
  if (a.isZero()) return b;
  if (b.isZero()) return a;

  BigIntValue result;
  if (a.negative_ == b.negative_) {
    result.digits_ = BigIntValue::addMagnitude(a.digits_, b.digits_);
    result.negative_ = a.negative_;
  } else {
    int cmp = BigIntValue::compareMagnitude(a.digits_, b.digits_);
    if (cmp == 0) return BigIntValue(0);
    if (cmp > 0) {
      result.digits_ = BigIntValue::subMagnitude(a.digits_, b.digits_);
      result.negative_ = a.negative_;
    } else {
      result.digits_ = BigIntValue::subMagnitude(b.digits_, a.digits_);
      result.negative_ = b.negative_;
    }
  }
  result.normalize();
  return result;
}

BigIntValue operator-(const BigIntValue& a, const BigIntValue& b) {
  BigIntValue negB = -b;
  return a + negB;
}

BigIntValue operator*(const BigIntValue& a, const BigIntValue& b) {
  if (a.isZero() || b.isZero()) return BigIntValue(0);
  BigIntValue result;
  result.digits_ = BigIntValue::mulMagnitude(a.digits_, b.digits_);
  result.negative_ = a.negative_ != b.negative_;
  result.normalize();
  return result;
}

BigIntValue operator/(const BigIntValue& a, const BigIntValue& b) {
  if (b.isZero()) {
    throw std::runtime_error("RangeError: Division by zero");
  }
  if (a.isZero()) return BigIntValue(0);

  int cmp = BigIntValue::compareMagnitude(a.digits_, b.digits_);
  if (cmp < 0) return BigIntValue(0);
  if (cmp == 0) return BigIntValue(a.negative_ != b.negative_ ? -1 : 1);

  BigIntValue result;
  if (b.digits_.size() == 1) {
    std::vector<uint32_t> uCopy = a.digits_;
    BigIntValue::divModSingleLimb(uCopy, b.digits_[0]);
    result.digits_ = std::move(uCopy);
    result.normalize();
  } else {
    std::vector<uint32_t> q, r;
    BigIntValue::divModMagnitude(a.digits_, b.digits_, q, r);
    result.digits_ = std::move(q);
  }
  result.negative_ = a.negative_ != b.negative_;
  result.normalize();
  return result;
}

BigIntValue operator%(const BigIntValue& a, const BigIntValue& b) {
  if (b.isZero()) {
    throw std::runtime_error("RangeError: Division by zero");
  }
  if (a.isZero()) return BigIntValue(0);

  int cmp = BigIntValue::compareMagnitude(a.digits_, b.digits_);
  if (cmp < 0) {
    // |a| < |b|, remainder is a
    return a;
  }
  if (cmp == 0) return BigIntValue(0);

  BigIntValue result;
  if (b.digits_.size() == 1) {
    std::vector<uint32_t> uCopy = a.digits_;
    uint32_t rem = BigIntValue::divModSingleLimb(uCopy, b.digits_[0]);
    if (rem) {
      result.digits_.push_back(rem);
    }
  } else {
    std::vector<uint32_t> q, r;
    BigIntValue::divModMagnitude(a.digits_, b.digits_, q, r);
    result.digits_ = std::move(r);
  }
  result.negative_ = a.negative_;  // remainder takes sign of dividend
  result.normalize();
  return result;
}

BigIntValue BigIntValue::operator-() const {
  if (isZero()) return BigIntValue(0);
  BigIntValue result = *this;
  result.negative_ = !negative_;
  return result;
}

BigIntValue& BigIntValue::operator+=(const BigIntValue& rhs) {
  *this = *this + rhs;
  return *this;
}

BigIntValue& BigIntValue::operator-=(const BigIntValue& rhs) {
  *this = *this - rhs;
  return *this;
}

BigIntValue& BigIntValue::operator*=(const BigIntValue& rhs) {
  *this = *this * rhs;
  return *this;
}

BigIntValue& BigIntValue::operator/=(const BigIntValue& rhs) {
  *this = *this / rhs;
  return *this;
}

BigIntValue& BigIntValue::operator*=(int rhs) {
  *this = *this * BigIntValue(rhs);
  return *this;
}

BigIntValue& BigIntValue::operator+=(int rhs) {
  *this = *this + BigIntValue(rhs);
  return *this;
}

// ===========================================================================
// Bitwise operators
// ===========================================================================

BigIntValue operator&(const BigIntValue& a, const BigIntValue& b) {
  if (a.isZero() || b.isZero()) {
    // If both non-negative, result is 0 when either is 0
    if (!a.isNegative() && !b.isNegative()) return BigIntValue(0);
    // -n & 0 = 0, 0 & -n = 0
    if (a.isZero() || b.isZero()) return BigIntValue(0);
  }

  if (!a.isNegative() && !b.isNegative()) {
    // Both non-negative: simple bitwise AND on magnitude
    size_t n = std::min(a.digits_.size(), b.digits_.size());
    BigIntValue result;
    result.digits_.resize(n);
    for (size_t i = 0; i < n; i++) {
      result.digits_[i] = a.digits_[i] & b.digits_[i];
    }
    result.normalize();
    return result;
  }

  // At least one negative: use two's complement
  size_t width = std::max(a.digits_.size(), b.digits_.size()) + 1;
  auto ta = BigIntValue::toTwosComplement(a, width);
  auto tb = BigIntValue::toTwosComplement(b, width);
  std::vector<uint32_t> result(width);
  for (size_t i = 0; i < width; i++) {
    result[i] = ta[i] & tb[i];
  }
  return BigIntValue::fromTwosComplement(result, width);
}

BigIntValue operator|(const BigIntValue& a, const BigIntValue& b) {
  if (!a.isNegative() && !b.isNegative()) {
    size_t n = std::max(a.digits_.size(), b.digits_.size());
    BigIntValue result;
    result.digits_.resize(n, 0);
    for (size_t i = 0; i < a.digits_.size(); i++)
      result.digits_[i] = a.digits_[i];
    for (size_t i = 0; i < b.digits_.size(); i++)
      result.digits_[i] |= b.digits_[i];
    result.normalize();
    return result;
  }

  size_t width = std::max(a.digits_.size(), b.digits_.size()) + 1;
  auto ta = BigIntValue::toTwosComplement(a, width);
  auto tb = BigIntValue::toTwosComplement(b, width);
  std::vector<uint32_t> result(width);
  for (size_t i = 0; i < width; i++) {
    result[i] = ta[i] | tb[i];
  }
  return BigIntValue::fromTwosComplement(result, width);
}

BigIntValue operator^(const BigIntValue& a, const BigIntValue& b) {
  if (!a.isNegative() && !b.isNegative()) {
    size_t n = std::max(a.digits_.size(), b.digits_.size());
    BigIntValue result;
    result.digits_.resize(n, 0);
    for (size_t i = 0; i < a.digits_.size(); i++)
      result.digits_[i] = a.digits_[i];
    for (size_t i = 0; i < b.digits_.size(); i++)
      result.digits_[i] ^= b.digits_[i];
    result.normalize();
    return result;
  }

  size_t width = std::max(a.digits_.size(), b.digits_.size()) + 1;
  auto ta = BigIntValue::toTwosComplement(a, width);
  auto tb = BigIntValue::toTwosComplement(b, width);
  std::vector<uint32_t> result(width);
  for (size_t i = 0; i < width; i++) {
    result[i] = ta[i] ^ tb[i];
  }
  return BigIntValue::fromTwosComplement(result, width);
}

// ~n = -(n+1) for BigInt
BigIntValue BigIntValue::operator~() const {
  return -(*this + BigIntValue(1));
}

BigIntValue operator&(const BigIntValue& a, int b) {
  return a & BigIntValue(b);
}

// ===========================================================================
// Shift operators
// ===========================================================================

BigIntValue operator<<(const BigIntValue& a, size_t count) {
  if (a.isZero() || count == 0) return a;

  size_t limbShift = count / 32;
  size_t bitShift = count % 32;

  BigIntValue result;
  result.negative_ = a.negative_;
  result.digits_.resize(a.digits_.size() + limbShift + 1, 0);

  if (bitShift == 0) {
    for (size_t i = 0; i < a.digits_.size(); i++) {
      result.digits_[i + limbShift] = a.digits_[i];
    }
  } else {
    uint32_t carry = 0;
    for (size_t i = 0; i < a.digits_.size(); i++) {
      uint64_t tmp = (static_cast<uint64_t>(a.digits_[i]) << bitShift) | carry;
      result.digits_[i + limbShift] = static_cast<uint32_t>(tmp);
      carry = static_cast<uint32_t>(tmp >> 32);
    }
    if (carry) {
      result.digits_[a.digits_.size() + limbShift] = carry;
    }
  }

  result.normalize();
  return result;
}

BigIntValue operator>>(const BigIntValue& a, size_t count) {
  if (a.isZero() || count == 0) return a;

  size_t limbShift = count / 32;
  size_t bitShift = count % 32;

  if (limbShift >= a.digits_.size()) {
    // All bits shifted out
    if (a.isNegative()) {
      // Arithmetic shift: floor toward -inf => -1
      return BigIntValue(-1);
    }
    return BigIntValue(0);
  }

  BigIntValue result;
  result.negative_ = a.negative_;
  size_t newSize = a.digits_.size() - limbShift;
  result.digits_.resize(newSize);

  if (bitShift == 0) {
    for (size_t i = 0; i < newSize; i++) {
      result.digits_[i] = a.digits_[i + limbShift];
    }
  } else {
    for (size_t i = 0; i < newSize; i++) {
      result.digits_[i] = a.digits_[i + limbShift] >> bitShift;
      if (i + 1 < newSize) {
        result.digits_[i] |=
            a.digits_[i + limbShift + 1] << (32 - bitShift);
      }
    }
  }

  // For negative numbers: arithmetic shift rounds toward -infinity.
  // If any discarded bits were set, we need to subtract 1 (round away from 0).
  if (a.isNegative()) {
    bool discardedBitsSet = false;
    // Check bits in limbs before limbShift
    for (size_t i = 0; i < limbShift && i < a.digits_.size(); i++) {
      if (a.digits_[i] != 0) {
        discardedBitsSet = true;
        break;
      }
    }
    // Check discarded bits within the transition limb
    if (!discardedBitsSet && bitShift > 0 && limbShift < a.digits_.size()) {
      uint32_t mask = (1u << bitShift) - 1;
      if (a.digits_[limbShift] & mask) {
        discardedBitsSet = true;
      }
    }
    if (discardedBitsSet) {
      // Add 1 to magnitude (since it's negative, this rounds toward -inf)
      uint64_t carry = 1;
      for (size_t i = 0; i < result.digits_.size() && carry; i++) {
        uint64_t sum = static_cast<uint64_t>(result.digits_[i]) + carry;
        result.digits_[i] = static_cast<uint32_t>(sum);
        carry = sum >> 32;
      }
      if (carry) result.digits_.push_back(static_cast<uint32_t>(carry));
    }
  }

  result.normalize();
  return result;
}

BigIntValue& BigIntValue::operator<<=(size_t count) {
  *this = *this << count;
  return *this;
}

BigIntValue& BigIntValue::operator>>=(size_t count) {
  *this = *this >> count;
  return *this;
}

// ===========================================================================
// Conversion specializations
// ===========================================================================

template <>
double BigIntValue::convert_to<double>() const {
  if (isZero()) return 0.0;

  // Use the magnitude to build the double
  double result = 0.0;
  double base = 1.0;
  for (size_t i = 0; i < digits_.size(); i++) {
    result += static_cast<double>(digits_[i]) * base;
    base *= 4294967296.0;  // 2^32
  }
  return negative_ ? -result : result;
}

template <>
std::string BigIntValue::convert_to<std::string>() const {
  if (isZero()) return "0";

  // Convert to decimal using repeated division by 10^9
  const uint32_t kChunkDivisor = 1000000000u;  // 10^9
  std::vector<uint32_t> mag = digits_;
  std::string chunks;

  while (true) {
    // Check if magnitude is zero
    bool allZero = true;
    for (auto d : mag) {
      if (d != 0) {
        allZero = false;
        break;
      }
    }
    if (allZero) break;

    uint32_t rem = divModSingleLimb(mag, kChunkDivisor);
    // Trim leading zeros from mag
    while (!mag.empty() && mag.back() == 0) mag.pop_back();

    // Convert rem to up to 9 digits
    char buf[10];
    int len = 0;
    if (rem == 0 && !mag.empty()) {
      // Mid-string chunk: must be exactly 9 digits
      for (int i = 0; i < 9; i++) buf[len++] = '0';
    } else {
      // Convert rem to string
      if (rem == 0) {
        buf[len++] = '0';
      } else {
        char tmp[10];
        int tmpLen = 0;
        while (rem > 0) {
          tmp[tmpLen++] = '0' + static_cast<char>(rem % 10);
          rem /= 10;
        }
        // If this is not the last chunk, pad to 9 digits
        if (!mag.empty()) {
          while (tmpLen < 9) tmp[tmpLen++] = '0';
        }
        // Reverse
        for (int i = tmpLen - 1; i >= 0; i--) {
          buf[len++] = tmp[i];
        }
      }
    }
    // Prepend this chunk
    chunks = std::string(buf, len) + chunks;
  }

  if (chunks.empty()) return "0";
  if (negative_) chunks = "-" + chunks;
  return chunks;
}

template <>
uint64_t BigIntValue::convert_to<uint64_t>() const {
  uint64_t result = 0;
  if (digits_.size() > 0) result = digits_[0];
  if (digits_.size() > 1) result |= static_cast<uint64_t>(digits_[1]) << 32;
  return result;
}

template <>
unsigned int BigIntValue::convert_to<unsigned int>() const {
  if (digits_.empty()) return 0;
  return digits_[0];
}

// size_t may be the same type as uint64_t on 64-bit platforms
#if !defined(__LP64__) && !defined(_LP64) && !defined(__x86_64__) && \
    !defined(__aarch64__) && !defined(_M_X64)
template <>
size_t BigIntValue::convert_to<size_t>() const {
  if constexpr (sizeof(size_t) == 8) {
    return convert_to<uint64_t>();
  } else {
    return convert_to<unsigned int>();
  }
}
#endif

}  // namespace lightjs::bigint
