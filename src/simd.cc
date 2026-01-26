#include "simd.h"
#include <cstring>

namespace lightjs {
namespace simd {

// =============================================================================
// Float16 conversion helpers (software implementation)
// These match the implementations in value.h but are optimized for batch use
// =============================================================================

namespace {

// Convert single float32 to float16 (software)
inline uint16_t f32_to_f16(float value) {
  uint32_t f32;
  std::memcpy(&f32, &value, sizeof(float));

  uint32_t sign = (f32 >> 16) & 0x8000;
  int32_t exponent = ((f32 >> 23) & 0xFF) - 127 + 15;
  uint32_t mantissa = f32 & 0x7FFFFF;

  if (exponent <= 0) {
    if (exponent < -10) {
      return static_cast<uint16_t>(sign);
    }
    mantissa = (mantissa | 0x800000) >> (1 - exponent);
    return static_cast<uint16_t>(sign | (mantissa >> 13));
  } else if (exponent >= 0x1F) {
    return static_cast<uint16_t>(sign | 0x7C00);
  }

  return static_cast<uint16_t>(sign | (exponent << 10) | (mantissa >> 13));
}

// Convert single float16 to float32 (software)
inline float f16_to_f32(uint16_t value) {
  uint32_t sign = (value & 0x8000) << 16;
  uint32_t exponent = (value >> 10) & 0x1F;
  uint32_t mantissa = value & 0x3FF;

  uint32_t f32;
  if (exponent == 0) {
    if (mantissa == 0) {
      f32 = sign;
    } else {
      exponent = 1;
      while (!(mantissa & 0x400)) {
        mantissa <<= 1;
        exponent--;
      }
      mantissa &= 0x3FF;
      f32 = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
    }
  } else if (exponent == 0x1F) {
    f32 = sign | 0x7F800000 | (mantissa << 13);
  } else {
    f32 = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
  }

  float result;
  std::memcpy(&result, &f32, sizeof(float));
  return result;
}

}  // anonymous namespace

// =============================================================================
// Float16 batch conversion implementations
// =============================================================================

void convertFloat32ToFloat16Batch(const float* src, uint16_t* dst, size_t count) {
#if USE_SIMD && SIMD_AVX2
  // AVX2 doesn't have native F16 support (that's F16C/AVX-512)
  // We'll process 8 floats at a time using scalar conversion
  // but with potential cache-friendly access patterns
  size_t i = 0;

  // Prefetch for better cache performance
  for (; i + 8 <= count; i += 8) {
    // Process 8 elements - could potentially use F16C if available
    // but we avoid it for Zen2 compatibility (F16C is AVX feature, not guaranteed)
    dst[i + 0] = f32_to_f16(src[i + 0]);
    dst[i + 1] = f32_to_f16(src[i + 1]);
    dst[i + 2] = f32_to_f16(src[i + 2]);
    dst[i + 3] = f32_to_f16(src[i + 3]);
    dst[i + 4] = f32_to_f16(src[i + 4]);
    dst[i + 5] = f32_to_f16(src[i + 5]);
    dst[i + 6] = f32_to_f16(src[i + 6]);
    dst[i + 7] = f32_to_f16(src[i + 7]);
  }
  for (; i < count; ++i) {
    dst[i] = f32_to_f16(src[i]);
  }

#elif USE_SIMD && SIMD_NEON
  // ARM64 with NEON - check for FP16 support
  // Note: ARMv8.2-A FP16 is optional, so we use software conversion
  size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    dst[i + 0] = f32_to_f16(src[i + 0]);
    dst[i + 1] = f32_to_f16(src[i + 1]);
    dst[i + 2] = f32_to_f16(src[i + 2]);
    dst[i + 3] = f32_to_f16(src[i + 3]);
  }
  for (; i < count; ++i) {
    dst[i] = f32_to_f16(src[i]);
  }

#else
  // Scalar fallback
  for (size_t i = 0; i < count; ++i) {
    dst[i] = f32_to_f16(src[i]);
  }
#endif
}

void convertFloat16ToFloat32Batch(const uint16_t* src, float* dst, size_t count) {
#if USE_SIMD && SIMD_AVX2
  // Process 8 elements at a time for cache efficiency
  size_t i = 0;
  for (; i + 8 <= count; i += 8) {
    dst[i + 0] = f16_to_f32(src[i + 0]);
    dst[i + 1] = f16_to_f32(src[i + 1]);
    dst[i + 2] = f16_to_f32(src[i + 2]);
    dst[i + 3] = f16_to_f32(src[i + 3]);
    dst[i + 4] = f16_to_f32(src[i + 4]);
    dst[i + 5] = f16_to_f32(src[i + 5]);
    dst[i + 6] = f16_to_f32(src[i + 6]);
    dst[i + 7] = f16_to_f32(src[i + 7]);
  }
  for (; i < count; ++i) {
    dst[i] = f16_to_f32(src[i]);
  }

#elif USE_SIMD && SIMD_NEON
  size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    dst[i + 0] = f16_to_f32(src[i + 0]);
    dst[i + 1] = f16_to_f32(src[i + 1]);
    dst[i + 2] = f16_to_f32(src[i + 2]);
    dst[i + 3] = f16_to_f32(src[i + 3]);
  }
  for (; i < count; ++i) {
    dst[i] = f16_to_f32(src[i]);
  }

#else
  // Scalar fallback
  for (size_t i = 0; i < count; ++i) {
    dst[i] = f16_to_f32(src[i]);
  }
#endif
}

}  // namespace simd
}  // namespace lightjs
