#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace lightjs::checked {

template <typename T>
using EnableUnsignedIntegral =
    std::enable_if_t<std::is_integral_v<T> && std::is_unsigned_v<T>, bool>;

template <typename T, EnableUnsignedIntegral<T> = true>
constexpr bool add(T lhs, T rhs, T& out) {
  if (lhs > std::numeric_limits<T>::max() - rhs) {
    return false;
  }
  out = static_cast<T>(lhs + rhs);
  return true;
}

template <typename T, EnableUnsignedIntegral<T> = true>
constexpr bool sub(T lhs, T rhs, T& out) {
  if (rhs > lhs) {
    return false;
  }
  out = static_cast<T>(lhs - rhs);
  return true;
}

template <typename T, EnableUnsignedIntegral<T> = true>
constexpr bool mul(T lhs, T rhs, T& out) {
  if (lhs != 0 && rhs > std::numeric_limits<T>::max() / lhs) {
    return false;
  }
  out = static_cast<T>(lhs * rhs);
  return true;
}

template <typename T, EnableUnsignedIntegral<T> = true>
constexpr bool rangeWithin(T offset, T width, T available) {
  T end = 0;
  return add(offset, width, end) && end <= available;
}

template <typename To, typename From,
          std::enable_if_t<std::is_integral_v<To> && std::is_unsigned_v<To> &&
                               std::is_integral_v<From> && std::is_unsigned_v<From>,
                           bool> = true>
constexpr bool narrow(From value, To& out) {
  if (value > static_cast<From>(std::numeric_limits<To>::max())) {
    return false;
  }
  out = static_cast<To>(value);
  return true;
}

}  // namespace lightjs::checked
