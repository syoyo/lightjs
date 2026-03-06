#include "value.h"
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

std::string valueToPropertyKey(const Value& value) {
  if (value.isSymbol()) {
    return symbolToPropertyKey(std::get<Symbol>(value.data));
  }
  if (value.isNumber()) {
    double number = value.toNumber();
    if (std::isnan(number)) return "NaN";
    if (std::isinf(number)) return number < 0 ? "-Infinity" : "Infinity";
    if (number == 0.0) return "0";
    double absNumber = std::fabs(number);
    const bool useExponent = absNumber >= 1e21 || (absNumber > 0.0 && absNumber < 1e-6);

    std::ostringstream oss;
    if (useExponent) {
      oss << std::scientific << std::setprecision(15) << number;
    } else {
      // Fixed with trimming approximates ES Number::toString for the ranges
      // exercised by property key conversion tests.
      oss << std::fixed << std::setprecision(15) << number;
    }
    std::string out = oss.str();

    auto expPos = out.find_first_of("eE");
    if (expPos != std::string::npos) {
      std::string mantissa = out.substr(0, expPos);
      std::string exponent = out.substr(expPos + 1);
      // Trim mantissa trailing zeros/dot.
      auto dot = mantissa.find('.');
      if (dot != std::string::npos) {
        while (!mantissa.empty() && mantissa.back() == '0') mantissa.pop_back();
        if (!mantissa.empty() && mantissa.back() == '.') mantissa.pop_back();
      }

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
      return out;
    }

    // Non-exponent form: trim trailing zeros/dot.
    auto dot = out.find('.');
    if (dot != std::string::npos) {
      while (!out.empty() && out.back() == '0') out.pop_back();
      if (!out.empty() && out.back() == '.') out.pop_back();
    }
    return out;
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
      size_t start = 0;
      while (start < arg.size() &&
             std::isspace(static_cast<unsigned char>(arg[start]))) {
        start++;
      }
      size_t end = arg.size();
      while (end > start &&
             std::isspace(static_cast<unsigned char>(arg[end - 1]))) {
        end--;
      }
      std::string s = arg.substr(start, end - start);
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

      char* parseEnd = nullptr;
      errno = 0;
      double parsed = std::strtod(s.c_str(), &parseEnd);
      if (parseEnd == s.c_str()) {
        return std::nan("");
      }
      while (*parseEnd != '\0' &&
             std::isspace(static_cast<unsigned char>(*parseEnd))) {
        parseEnd++;
      }
      if (*parseEnd != '\0') {
        return std::nan("");
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
      if (std::isnan(arg)) return "NaN";
      if (std::isinf(arg)) return arg > 0 ? "Infinity" : "-Infinity";
      std::ostringstream oss;
      oss << arg;
      return oss.str();
    } else if constexpr (std::is_same_v<T, BigInt>) {
      return bigint::toString(arg.value);
    } else if constexpr (std::is_same_v<T, Symbol>) {
      return "Symbol(" + arg.description + ")";
    } else if constexpr (std::is_same_v<T, ModuleBinding>) {
      return "[ModuleBinding]";
    } else if constexpr (std::is_same_v<T, std::string>) {
      return arg;
    } else if constexpr (std::is_same_v<T, GCPtr<Function>>) {
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
  const uint8_t* ptr = &bytes[byteIndex];

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
  if (index >= currentLength()) return;

  size_t byteIndex = byteOffset + index * elementSize();
  auto& bytes = storage();
  uint8_t* ptr = &bytes[byteIndex];

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

  // Fast path: same type, just memcpy
  if (type == source.type) {
    size_t bytesToCopy = count * elementSize();
    size_t srcByteOffset = source.byteOffset + srcOffset * source.elementSize();
    size_t dstByteOffset = byteOffset + dstOffset * elementSize();
    simd::memcpySIMD(&targetBytes[dstByteOffset], &sourceBytes[srcByteOffset], bytesToCopy);
    return;
  }

  // Type conversion paths with SIMD acceleration
#if USE_SIMD
  // Float32 -> Int32
  if (source.type == TypedArrayType::Float32 && type == TypedArrayType::Int32) {
    const float* src = reinterpret_cast<const float*>(&sourceBytes[source.byteOffset + srcOffset * 4]);
    int32_t* dst = reinterpret_cast<int32_t*>(&targetBytes[byteOffset + dstOffset * 4]);
    simd::convertFloat32ToInt32(src, dst, count);
    return;
  }

  // Int32 -> Float32
  if (source.type == TypedArrayType::Int32 && type == TypedArrayType::Float32) {
    const int32_t* src = reinterpret_cast<const int32_t*>(&sourceBytes[source.byteOffset + srcOffset * 4]);
    float* dst = reinterpret_cast<float*>(&targetBytes[byteOffset + dstOffset * 4]);
    simd::convertInt32ToFloat32(src, dst, count);
    return;
  }

  // Float64 -> Int32
  if (source.type == TypedArrayType::Float64 && type == TypedArrayType::Int32) {
    const double* src = reinterpret_cast<const double*>(&sourceBytes[source.byteOffset + srcOffset * 8]);
    int32_t* dst = reinterpret_cast<int32_t*>(&targetBytes[byteOffset + dstOffset * 4]);
    simd::convertFloat64ToInt32(src, dst, count);
    return;
  }

  // Uint8 -> Float32
  if (source.type == TypedArrayType::Uint8 && type == TypedArrayType::Float32) {
    const uint8_t* src = &sourceBytes[source.byteOffset + srcOffset];
    float* dst = reinterpret_cast<float*>(&targetBytes[byteOffset + dstOffset * 4]);
    simd::convertUint8ToFloat32(src, dst, count);
    return;
  }

  // Float32 -> Uint8
  if (source.type == TypedArrayType::Float32 && type == TypedArrayType::Uint8) {
    const float* src = reinterpret_cast<const float*>(&sourceBytes[source.byteOffset + srcOffset * 4]);
    uint8_t* dst = &targetBytes[byteOffset + dstOffset];
    simd::convertFloat32ToUint8(src, dst, count);
    return;
  }

  // Float32 -> Uint8Clamped
  if (source.type == TypedArrayType::Float32 && type == TypedArrayType::Uint8Clamped) {
    const float* src = reinterpret_cast<const float*>(&sourceBytes[source.byteOffset + srcOffset * 4]);
    uint8_t* dst = &targetBytes[byteOffset + dstOffset];
    simd::clampFloat32ToUint8(src, dst, count);
    return;
  }

  // Int16 -> Float32
  if (source.type == TypedArrayType::Int16 && type == TypedArrayType::Float32) {
    const int16_t* src = reinterpret_cast<const int16_t*>(&sourceBytes[source.byteOffset + srcOffset * 2]);
    float* dst = reinterpret_cast<float*>(&targetBytes[byteOffset + dstOffset * 4]);
    simd::convertInt16ToFloat32(src, dst, count);
    return;
  }

  // Float32 -> Int16
  if (source.type == TypedArrayType::Float32 && type == TypedArrayType::Int16) {
    const float* src = reinterpret_cast<const float*>(&sourceBytes[source.byteOffset + srcOffset * 4]);
    int16_t* dst = reinterpret_cast<int16_t*>(&targetBytes[byteOffset + dstOffset * 2]);
    simd::convertFloat32ToInt16(src, dst, count);
    return;
  }

  // Float32 -> Float16 (batch)
  if (source.type == TypedArrayType::Float32 && type == TypedArrayType::Float16) {
    const float* src = reinterpret_cast<const float*>(&sourceBytes[source.byteOffset + srcOffset * 4]);
    uint16_t* dst = reinterpret_cast<uint16_t*>(&targetBytes[byteOffset + dstOffset * 2]);
    simd::convertFloat32ToFloat16Batch(src, dst, count);
    return;
  }

  // Float16 -> Float32 (batch)
  if (source.type == TypedArrayType::Float16 && type == TypedArrayType::Float32) {
    const uint16_t* src = reinterpret_cast<const uint16_t*>(&sourceBytes[source.byteOffset + srcOffset * 2]);
    float* dst = reinterpret_cast<float*>(&targetBytes[byteOffset + dstOffset * 4]);
    simd::convertFloat16ToFloat32Batch(src, dst, count);
    return;
  }
#endif

  // Generic fallback: element-by-element conversion
  for (size_t i = 0; i < count; ++i) {
    double val = source.getElement(srcOffset + i);
    setElement(dstOffset + i, val);
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
  if (a.isNull() || a.isUndefined()) return true;

  // For objects, compare by reference
  if (a.isObject()) return a.getGC<Object>() == b.getGC<Object>();
  if (a.isArray()) return a.getGC<Array>() == b.getGC<Array>();
  if (a.isFunction()) return a.getGC<Function>() == b.getGC<Function>();
  if (a.isMap()) return a.getGC<Map>() == b.getGC<Map>();
  if (a.isSet()) return a.getGC<Set>() == b.getGC<Set>();

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
  auto result = GarbageCollector::makeGC<Array>();
  if (args.empty()) return Value(result);

  // Handle Object with module namespace
  if (args[0].isObject()) {
    auto obj = args[0].getGC<Object>();
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

  auto* props = getPropertiesMap(args[0]);
  if (!props) return Value(result);

  for (const auto& key : sortOwnPropertyKeys(props->orderedKeys())) {
    if (props->count("__non_enum_" + key)) continue;
    result->elements.push_back(Value(key));
  }

  return Value(result);
}

Value Object_values(const std::vector<Value>& args) {
  auto result = GarbageCollector::makeGC<Array>();
  if (args.empty()) return Value(result);
  auto* props = getPropertiesMap(args[0]);
  if (!props) return Value(result);

  for (const auto& key : sortOwnPropertyKeys(props->orderedKeys())) {
    if (props->count("__non_enum_" + key)) continue;
    auto it = props->find(key);
    result->elements.push_back(it->second);
  }

  return Value(result);
}

Value Object_entries(const std::vector<Value>& args) {
  auto result = GarbageCollector::makeGC<Array>();
  if (args.empty()) return Value(result);
  auto* props = getPropertiesMap(args[0]);
  if (!props) return Value(result);

  for (const auto& key : sortOwnPropertyKeys(props->orderedKeys())) {
    if (props->count("__non_enum_" + key)) continue;
    auto it = props->find(key);
    auto entry = GarbageCollector::makeGC<Array>();
    entry->elements.push_back(Value(key));
    entry->elements.push_back(it->second);
    result->elements.push_back(Value(entry));
  }

  return Value(result);
}

Value Object_assign(const std::vector<Value>& args) {
  if (args.empty()) {
    throw std::runtime_error("Object.assign requires at least 1 argument");
  }

  Value target = args[0];
  if (target.isNull() || target.isUndefined()) {
    throw std::runtime_error("Cannot convert undefined or null to object");
  }

  // Convert target to object if needed
  GCPtr<Object> targetObj;
  if (target.isObject()) {
    targetObj = target.getGC<Object>();
  } else {
    targetObj = GarbageCollector::makeGC<Object>();
  }

  // Copy properties from sources
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i].isNull() || args[i].isUndefined()) {
      continue; // Skip null/undefined sources
    }

    if (args[i].isObject()) {
      auto sourceObj = args[i].getGC<Object>();
      for (const auto& [key, value] : sourceObj->properties) {
        targetObj->properties[key] = value;
      }
    }
  }

  return Value(targetObj);
}

Value Object_hasOwnProperty(const std::vector<Value>& args) {
  if (args.size() < 2) {
    return Value(false);
  }

  std::string key = valueToPropertyKey(args[1]);

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
    // Arrays always have an own `length` data property.
    if (key == "length") return Value(true);
    try {
      size_t idx = std::stoull(key);
      if (idx < arr->elements.size()) return Value(true);
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
  if (isInternalProperty(key)) return Value(false);
  if (obj->properties.find(key) != obj->properties.end()) return Value(true);
  if (obj->properties.find("__get_" + key) != obj->properties.end()) return Value(true);
  if (obj->properties.find("__set_" + key) != obj->properties.end()) return Value(true);
  return Value(false);
}

static bool isInternalProperty(const std::string& key) {
  if (key.size() >= 4 && key.substr(0, 2) == "__" &&
      key.substr(key.size() - 2) == "__") return true;
  if (key.substr(0, 6) == "__get_" || key.substr(0, 6) == "__set_" ||
      key.substr(0, 11) == "__non_enum_" || key.substr(0, 15) == "__non_writable_" ||
      key.substr(0, 19) == "__non_configurable_" || key.substr(0, 7) == "__enum_") return true;
  return false;
}

Value Object_getOwnPropertyNames(const std::vector<Value>& args) {
  auto result = GarbageCollector::makeGC<Array>();

  if (args.empty()) {
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
      if (!exposed.empty() && seen.insert(exposed).second) {
        keys.push_back(exposed);
      }
    }
    appendInOwnPropertyKeyOrder(keys, true);
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
        rawKey.rfind("__enum_", 0) == 0) {
      continue;
    }
    std::string exposed = rawKey;
    if (rawKey.rfind("__get_", 0) == 0 || rawKey.rfind("__set_", 0) == 0) {
      exposed = rawKey.substr(6);
    }
    if (!exposed.empty() && seen.insert(exposed).second) {
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
      auto writableIt = desc->properties.find("writable");
      if (writableIt != desc->properties.end() && !writableIt->second.toBool()) {
        newObj->properties["__non_writable_" + key] = Value(true);
      }
      auto enumIt = desc->properties.find("enumerable");
      if (enumIt != desc->properties.end() && !enumIt->second.toBool()) {
        newObj->properties["__non_enum_" + key] = Value(true);
      }
      auto configIt = desc->properties.find("configurable");
      if (configIt != desc->properties.end() && !configIt->second.toBool()) {
        newObj->properties["__non_configurable_" + key] = Value(true);
      }
    }
  }

  return Value(newObj);
}

Value Object_fromEntries(const std::vector<Value>& args) {
  auto newObj = GarbageCollector::makeGC<Object>();

  if (args.empty() || !args[0].isArray()) {
    return Value(newObj);
  }

  auto arr = args[0].getGC<Array>();
  for (const auto& entry : arr->elements) {
    // Each entry should be an array with [key, value]
    if (entry.isArray()) {
      auto pair = entry.getGC<Array>();
      if (pair->elements.size() >= 2) {
        std::string key = pair->elements[0].toString();
        newObj->properties[key] = pair->elements[1];
      }
    }
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

// DataView get methods
int8_t DataView::getInt8(size_t offset) const {
  if (offset >= byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }
  return static_cast<int8_t>(buffer->data[byteOffset + offset]);
}

uint8_t DataView::getUint8(size_t offset) const {
  if (offset >= byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }
  return buffer->data[byteOffset + offset];
}

int16_t DataView::getInt16(size_t offset, bool littleEndian) const {
  if (offset + sizeof(int16_t) > byteLength) {
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
  if (offset + sizeof(uint16_t) > byteLength) {
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
  if (offset + sizeof(int32_t) > byteLength) {
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
  if (offset + sizeof(uint32_t) > byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  uint32_t value;
  std::memcpy(&value, &buffer->data[byteOffset + offset], sizeof(uint32_t));

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  return value;
}

float DataView::getFloat32(size_t offset, bool littleEndian) const {
  if (offset + sizeof(float) > byteLength) {
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
  if (offset + sizeof(double) > byteLength) {
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
  if (offset + sizeof(int64_t) > byteLength) {
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
  if (offset + sizeof(uint64_t) > byteLength) {
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
  if (offset >= byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }
  buffer->data[byteOffset + offset] = static_cast<uint8_t>(value);
}

void DataView::setUint8(size_t offset, uint8_t value) {
  if (offset >= byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }
  buffer->data[byteOffset + offset] = value;
}

void DataView::setInt16(size_t offset, int16_t value, bool littleEndian) {
  if (offset + sizeof(int16_t) > byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  std::memcpy(&buffer->data[byteOffset + offset], &value, sizeof(int16_t));
}

void DataView::setUint16(size_t offset, uint16_t value, bool littleEndian) {
  if (offset + sizeof(uint16_t) > byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  std::memcpy(&buffer->data[byteOffset + offset], &value, sizeof(uint16_t));
}

void DataView::setInt32(size_t offset, int32_t value, bool littleEndian) {
  if (offset + sizeof(int32_t) > byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  std::memcpy(&buffer->data[byteOffset + offset], &value, sizeof(int32_t));
}

void DataView::setUint32(size_t offset, uint32_t value, bool littleEndian) {
  if (offset + sizeof(uint32_t) > byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  std::memcpy(&buffer->data[byteOffset + offset], &value, sizeof(uint32_t));
}

void DataView::setFloat32(size_t offset, float value, bool littleEndian) {
  if (offset + sizeof(float) > byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  std::memcpy(&buffer->data[byteOffset + offset], &value, sizeof(float));
}

void DataView::setFloat64(size_t offset, double value, bool littleEndian) {
  if (offset + sizeof(double) > byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  std::memcpy(&buffer->data[byteOffset + offset], &value, sizeof(double));
}

void DataView::setBigInt64(size_t offset, int64_t value, bool littleEndian) {
  if (offset + sizeof(int64_t) > byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  std::memcpy(&buffer->data[byteOffset + offset], &value, sizeof(int64_t));
}

void DataView::setBigUint64(size_t offset, uint64_t value, bool littleEndian) {
  if (offset + sizeof(uint64_t) > byteLength) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }

  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  std::memcpy(&buffer->data[byteOffset + offset], &value, sizeof(uint64_t));
}

}
