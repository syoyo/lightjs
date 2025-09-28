#include "value.h"
#include <sstream>
#include <cmath>

namespace tinyjs {

bool Value::toBool() const {
  return std::visit([](auto&& arg) -> bool {
    using T = std::decay_t<decltype(arg)>;
    if constexpr (std::is_same_v<T, Undefined>) {
      return false;
    } else if constexpr (std::is_same_v<T, Null>) {
      return false;
    } else if constexpr (std::is_same_v<T, bool>) {
      return arg;
    } else if constexpr (std::is_same_v<T, double>) {
      return arg != 0.0 && !std::isnan(arg);
    } else if constexpr (std::is_same_v<T, BigInt>) {
      return arg.value != 0;
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
    if constexpr (std::is_same_v<T, Undefined>) {
      return std::nan("");
    } else if constexpr (std::is_same_v<T, Null>) {
      return 0.0;
    } else if constexpr (std::is_same_v<T, bool>) {
      return arg ? 1.0 : 0.0;
    } else if constexpr (std::is_same_v<T, double>) {
      return arg;
    } else if constexpr (std::is_same_v<T, BigInt>) {
      return static_cast<double>(arg.value);
    } else if constexpr (std::is_same_v<T, std::string>) {
      try {
        return std::stod(arg);
      } catch (...) {
        return std::nan("");
      }
    } else {
      return std::nan("");
    }
  }, data);
}

int64_t Value::toBigInt() const {
  return std::visit([](auto&& arg) -> int64_t {
    using T = std::decay_t<decltype(arg)>;
    if constexpr (std::is_same_v<T, Undefined>) {
      return 0;
    } else if constexpr (std::is_same_v<T, Null>) {
      return 0;
    } else if constexpr (std::is_same_v<T, bool>) {
      return arg ? 1 : 0;
    } else if constexpr (std::is_same_v<T, double>) {
      return static_cast<int64_t>(arg);
    } else if constexpr (std::is_same_v<T, BigInt>) {
      return arg.value;
    } else if constexpr (std::is_same_v<T, std::string>) {
      try {
        return std::stoll(arg);
      } catch (...) {
        return 0;
      }
    } else {
      return 0;
    }
  }, data);
}

std::string Value::toString() const {
  return std::visit([](auto&& arg) -> std::string {
    using T = std::decay_t<decltype(arg)>;
    if constexpr (std::is_same_v<T, Undefined>) {
      return "undefined";
    } else if constexpr (std::is_same_v<T, Null>) {
      return "null";
    } else if constexpr (std::is_same_v<T, bool>) {
      return arg ? "true" : "false";
    } else if constexpr (std::is_same_v<T, double>) {
      std::ostringstream oss;
      oss << arg;
      return oss.str();
    } else if constexpr (std::is_same_v<T, BigInt>) {
      return std::to_string(arg.value) + "n";
    } else if constexpr (std::is_same_v<T, std::string>) {
      return arg;
    } else if constexpr (std::is_same_v<T, std::shared_ptr<Function>>) {
      return "[Function]";
    } else if constexpr (std::is_same_v<T, std::shared_ptr<Array>>) {
      return "[Array]";
    } else if constexpr (std::is_same_v<T, std::shared_ptr<Object>>) {
      return "[Object]";
    } else if constexpr (std::is_same_v<T, std::shared_ptr<TypedArray>>) {
      return "[TypedArray]";
    } else if constexpr (std::is_same_v<T, std::shared_ptr<Promise>>) {
      return "[Promise]";
    } else if constexpr (std::is_same_v<T, std::shared_ptr<Regex>>) {
      return "/" + arg->pattern + "/" + arg->flags;
    } else {
      return "";
    }
  }, data);
}

double TypedArray::getElement(size_t index) const {
  if (index >= length) return 0.0;

  size_t byteIndex = byteOffset + index * elementSize();
  const uint8_t* ptr = &buffer[byteIndex];

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

void TypedArray::setElement(size_t index, double value) {
  if (index >= length) return;

  size_t byteIndex = byteOffset + index * elementSize();
  uint8_t* ptr = &buffer[byteIndex];

  switch (type) {
    case TypedArrayType::Int8:
      *reinterpret_cast<int8_t*>(ptr) = static_cast<int8_t>(value);
      break;
    case TypedArrayType::Uint8:
      *reinterpret_cast<uint8_t*>(ptr) = static_cast<uint8_t>(value);
      break;
    case TypedArrayType::Uint8Clamped: {
      int32_t clamped = static_cast<int32_t>(value);
      if (clamped < 0) clamped = 0;
      if (clamped > 255) clamped = 255;
      *reinterpret_cast<uint8_t*>(ptr) = static_cast<uint8_t>(clamped);
      break;
    }
    case TypedArrayType::Int16:
      *reinterpret_cast<int16_t*>(ptr) = static_cast<int16_t>(value);
      break;
    case TypedArrayType::Uint16:
      *reinterpret_cast<uint16_t*>(ptr) = static_cast<uint16_t>(value);
      break;
    case TypedArrayType::Float16:
      *reinterpret_cast<uint16_t*>(ptr) = float32_to_float16(static_cast<float>(value));
      break;
    case TypedArrayType::Int32:
      *reinterpret_cast<int32_t*>(ptr) = static_cast<int32_t>(value);
      break;
    case TypedArrayType::Uint32:
      *reinterpret_cast<uint32_t*>(ptr) = static_cast<uint32_t>(value);
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

int64_t TypedArray::getBigIntElement(size_t index) const {
  if (index >= length) return 0;

  size_t byteIndex = byteOffset + index * elementSize();
  const uint8_t* ptr = &buffer[byteIndex];

  switch (type) {
    case TypedArrayType::BigInt64:
      return *reinterpret_cast<const int64_t*>(ptr);
    case TypedArrayType::BigUint64:
      return static_cast<int64_t>(*reinterpret_cast<const uint64_t*>(ptr));
    default:
      return static_cast<int64_t>(getElement(index));
  }
}

void TypedArray::setBigIntElement(size_t index, int64_t value) {
  if (index >= length) return;

  size_t byteIndex = byteOffset + index * elementSize();
  uint8_t* ptr = &buffer[byteIndex];

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

}