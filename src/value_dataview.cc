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

}  // namespace lightjs
