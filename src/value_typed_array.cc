#include "value_internal.h"

namespace lightjs {

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

}  // namespace lightjs
