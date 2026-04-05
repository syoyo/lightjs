#include "value_internal.h"

namespace lightjs {

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

static size_t dataViewAccessibleLength(const DataView& view) {
  if (!view.buffer || view.buffer->detached) {
    throw std::runtime_error("TypeError: DataView has a detached buffer");
  }
  return view.currentByteLength();
}

static size_t resolveDataViewOffset(const DataView& view, size_t offset, size_t width) {
  size_t visibleLength = dataViewAccessibleLength(view);
  size_t absoluteOffset = 0;
  if (!checked::rangeWithin(offset, width, visibleLength) ||
      !checked::add(view.byteOffset, offset, absoluteOffset) ||
      !checked::rangeWithin(absoluteOffset, width, view.buffer->data.size())) {
    throw std::runtime_error("RangeError: Offset is outside the bounds of the DataView");
  }
  return absoluteOffset;
}

template <typename T>
T readDataViewValue(const DataView& view, size_t offset, bool littleEndian) {
  size_t absoluteOffset = resolveDataViewOffset(view, offset, sizeof(T));
  T value{};
  std::memcpy(&value, &view.buffer->data[absoluteOffset], sizeof(T));
  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  return value;
}

template <typename T>
void writeDataViewValue(const DataView& view, size_t offset, T value, bool littleEndian) {
  size_t absoluteOffset = resolveDataViewOffset(view, offset, sizeof(T));
  if (littleEndian != isLittleEndian()) {
    value = swapEndian(value);
  }
  std::memcpy(&view.buffer->data[absoluteOffset], &value, sizeof(T));
}

// DataView get methods
int8_t DataView::getInt8(size_t offset) const {
  size_t absoluteOffset = resolveDataViewOffset(*this, offset, sizeof(int8_t));
  return static_cast<int8_t>(buffer->data[absoluteOffset]);
}

uint8_t DataView::getUint8(size_t offset) const {
  size_t absoluteOffset = resolveDataViewOffset(*this, offset, sizeof(uint8_t));
  return buffer->data[absoluteOffset];
}

int16_t DataView::getInt16(size_t offset, bool littleEndian) const {
  return readDataViewValue<int16_t>(*this, offset, littleEndian);
}

uint16_t DataView::getUint16(size_t offset, bool littleEndian) const {
  return readDataViewValue<uint16_t>(*this, offset, littleEndian);
}

int32_t DataView::getInt32(size_t offset, bool littleEndian) const {
  return readDataViewValue<int32_t>(*this, offset, littleEndian);
}

uint32_t DataView::getUint32(size_t offset, bool littleEndian) const {
  return readDataViewValue<uint32_t>(*this, offset, littleEndian);
}

float DataView::getFloat16(size_t offset, bool littleEndian) const {
  return float16_to_float32(readDataViewValue<uint16_t>(*this, offset, littleEndian));
}

float DataView::getFloat32(size_t offset, bool littleEndian) const {
  return readDataViewValue<float>(*this, offset, littleEndian);
}

double DataView::getFloat64(size_t offset, bool littleEndian) const {
  return readDataViewValue<double>(*this, offset, littleEndian);
}

int64_t DataView::getBigInt64(size_t offset, bool littleEndian) const {
  return readDataViewValue<int64_t>(*this, offset, littleEndian);
}

uint64_t DataView::getBigUint64(size_t offset, bool littleEndian) const {
  return readDataViewValue<uint64_t>(*this, offset, littleEndian);
}

// DataView set methods
void DataView::setInt8(size_t offset, int8_t value) {
  size_t absoluteOffset = resolveDataViewOffset(*this, offset, sizeof(int8_t));
  buffer->data[absoluteOffset] = static_cast<uint8_t>(value);
}

void DataView::setUint8(size_t offset, uint8_t value) {
  size_t absoluteOffset = resolveDataViewOffset(*this, offset, sizeof(uint8_t));
  buffer->data[absoluteOffset] = value;
}

void DataView::setInt16(size_t offset, int16_t value, bool littleEndian) {
  writeDataViewValue(*this, offset, value, littleEndian);
}

void DataView::setUint16(size_t offset, uint16_t value, bool littleEndian) {
  writeDataViewValue(*this, offset, value, littleEndian);
}

void DataView::setInt32(size_t offset, int32_t value, bool littleEndian) {
  writeDataViewValue(*this, offset, value, littleEndian);
}

void DataView::setUint32(size_t offset, uint32_t value, bool littleEndian) {
  writeDataViewValue(*this, offset, value, littleEndian);
}

void DataView::setFloat16(size_t offset, double value, bool littleEndian) {
  uint16_t bits = float64_to_float16(value);
  writeDataViewValue(*this, offset, bits, littleEndian);
}

void DataView::setFloat32(size_t offset, float value, bool littleEndian) {
  writeDataViewValue(*this, offset, value, littleEndian);
}

void DataView::setFloat64(size_t offset, double value, bool littleEndian) {
  writeDataViewValue(*this, offset, value, littleEndian);
}

void DataView::setBigInt64(size_t offset, int64_t value, bool littleEndian) {
  writeDataViewValue(*this, offset, value, littleEndian);
}

void DataView::setBigUint64(size_t offset, uint64_t value, bool littleEndian) {
  writeDataViewValue(*this, offset, value, littleEndian);
}

}  // namespace lightjs
