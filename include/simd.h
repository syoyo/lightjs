#pragma once

/**
 * SIMD utilities for LightJS TypedArray operations
 *
 * Supported platforms:
 * - x86/x64: SSE2, SSE4.2, AVX2 (up to AMD Zen2)
 * - ARM64: NEON, SVE
 *
 * Build with -DUSE_SIMD=ON to enable SIMD optimizations.
 */

#include <cstdint>
#include <cstddef>
#include <cstring>

// Default: SIMD disabled
#ifndef USE_SIMD
#define USE_SIMD 0
#endif

// Architecture detection
#ifndef SIMD_ARCH_X86
#define SIMD_ARCH_X86 0
#endif

#ifndef SIMD_ARCH_ARM64
#define SIMD_ARCH_ARM64 0
#endif

// Feature detection
#ifndef SIMD_SSE2
#define SIMD_SSE2 0
#endif

#ifndef SIMD_SSE42
#define SIMD_SSE42 0
#endif

#ifndef SIMD_AVX2
#define SIMD_AVX2 0
#endif

#ifndef SIMD_NEON
#define SIMD_NEON 0
#endif

#ifndef SIMD_SVE
#define SIMD_SVE 0
#endif

// Include platform-specific headers
#if USE_SIMD

#if SIMD_ARCH_X86
  #if SIMD_AVX2
    #include <immintrin.h>  // AVX2, AVX, FMA
  #elif SIMD_SSE42
    #include <nmmintrin.h>  // SSE4.2
  #elif SIMD_SSE2
    #include <emmintrin.h>  // SSE2
  #endif
#endif

#if SIMD_ARCH_ARM64
  #include <arm_neon.h>
  #if SIMD_SVE
    #include <arm_sve.h>
  #endif
#endif

#endif  // USE_SIMD

namespace lightjs {
namespace simd {

// =============================================================================
// SIMD capability query
// =============================================================================

/**
 * Check if SIMD is enabled at compile time
 */
constexpr bool isEnabled() {
#if USE_SIMD
  return true;
#else
  return false;
#endif
}

/**
 * Get the SIMD instruction set name
 */
inline const char* getInstructionSet() {
#if SIMD_AVX2
  return "AVX2+FMA";
#elif SIMD_SSE42
  return "SSE4.2";
#elif SIMD_SSE2
  return "SSE2";
#elif SIMD_SVE
  return "SVE";
#elif SIMD_NEON
  return "NEON";
#else
  return "None";
#endif
}

/**
 * Get vector width in bytes for the current SIMD implementation
 */
constexpr size_t vectorWidth() {
#if SIMD_AVX2
  return 32;  // 256-bit
#elif SIMD_SSE2 || SIMD_SSE42
  return 16;  // 128-bit
#elif SIMD_NEON
  return 16;  // 128-bit
#elif SIMD_SVE
  return 32;  // 256-bit minimum (can be larger on some implementations)
#else
  return 0;
#endif
}

// =============================================================================
// Float32 to Int32 conversion (vectorized)
// =============================================================================

/**
 * Convert float array to int32 array with truncation
 * @param src Source float array
 * @param dst Destination int32 array
 * @param count Number of elements
 */
inline void convertFloat32ToInt32(const float* src, int32_t* dst, size_t count) {
#if USE_SIMD && SIMD_AVX2
  // AVX2: Process 8 floats at a time
  size_t i = 0;
  for (; i + 8 <= count; i += 8) {
    __m256 floats = _mm256_loadu_ps(src + i);
    __m256i ints = _mm256_cvttps_epi32(floats);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), ints);
  }
  // Handle remaining elements
  for (; i < count; ++i) {
    dst[i] = static_cast<int32_t>(src[i]);
  }

#elif USE_SIMD && (SIMD_SSE2 || SIMD_SSE42)
  // SSE2/SSE4.2: Process 4 floats at a time
  size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    __m128 floats = _mm_loadu_ps(src + i);
    __m128i ints = _mm_cvttps_epi32(floats);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), ints);
  }
  // Handle remaining elements
  for (; i < count; ++i) {
    dst[i] = static_cast<int32_t>(src[i]);
  }

#elif USE_SIMD && SIMD_NEON
  // NEON: Process 4 floats at a time
  size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    float32x4_t floats = vld1q_f32(src + i);
    int32x4_t ints = vcvtq_s32_f32(floats);
    vst1q_s32(dst + i, ints);
  }
  // Handle remaining elements
  for (; i < count; ++i) {
    dst[i] = static_cast<int32_t>(src[i]);
  }

#elif USE_SIMD && SIMD_SVE
  // SVE: Process vector-length floats at a time
  size_t i = 0;
  svbool_t pg = svptrue_b32();
  size_t vl = svcntw();  // Vector length in 32-bit words
  for (; i + vl <= count; i += vl) {
    svfloat32_t floats = svld1_f32(pg, src + i);
    svint32_t ints = svcvt_s32_f32_z(pg, floats);
    svst1_s32(pg, dst + i, ints);
  }
  // Handle remaining elements with predicate
  if (i < count) {
    svbool_t pg_tail = svwhilelt_b32(i, count);
    svfloat32_t floats = svld1_f32(pg_tail, src + i);
    svint32_t ints = svcvt_s32_f32_z(pg_tail, floats);
    svst1_s32(pg_tail, dst + i, ints);
  }

#else
  // Scalar fallback
  for (size_t i = 0; i < count; ++i) {
    dst[i] = static_cast<int32_t>(src[i]);
  }
#endif
}

// =============================================================================
// Int32 to Float32 conversion (vectorized)
// =============================================================================

/**
 * Convert int32 array to float array
 * @param src Source int32 array
 * @param dst Destination float array
 * @param count Number of elements
 */
inline void convertInt32ToFloat32(const int32_t* src, float* dst, size_t count) {
#if USE_SIMD && SIMD_AVX2
  size_t i = 0;
  for (; i + 8 <= count; i += 8) {
    __m256i ints = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
    __m256 floats = _mm256_cvtepi32_ps(ints);
    _mm256_storeu_ps(dst + i, floats);
  }
  for (; i < count; ++i) {
    dst[i] = static_cast<float>(src[i]);
  }

#elif USE_SIMD && (SIMD_SSE2 || SIMD_SSE42)
  size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    __m128i ints = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
    __m128 floats = _mm_cvtepi32_ps(ints);
    _mm_storeu_ps(dst + i, floats);
  }
  for (; i < count; ++i) {
    dst[i] = static_cast<float>(src[i]);
  }

#elif USE_SIMD && SIMD_NEON
  size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    int32x4_t ints = vld1q_s32(src + i);
    float32x4_t floats = vcvtq_f32_s32(ints);
    vst1q_f32(dst + i, floats);
  }
  for (; i < count; ++i) {
    dst[i] = static_cast<float>(src[i]);
  }

#elif USE_SIMD && SIMD_SVE
  size_t i = 0;
  svbool_t pg = svptrue_b32();
  size_t vl = svcntw();
  for (; i + vl <= count; i += vl) {
    svint32_t ints = svld1_s32(pg, src + i);
    svfloat32_t floats = svcvt_f32_s32_z(pg, ints);
    svst1_f32(pg, dst + i, floats);
  }
  if (i < count) {
    svbool_t pg_tail = svwhilelt_b32(i, count);
    svint32_t ints = svld1_s32(pg_tail, src + i);
    svfloat32_t floats = svcvt_f32_s32_z(pg_tail, ints);
    svst1_f32(pg_tail, dst + i, floats);
  }

#else
  for (size_t i = 0; i < count; ++i) {
    dst[i] = static_cast<float>(src[i]);
  }
#endif
}

// =============================================================================
// Float64 to Int32 conversion (vectorized)
// =============================================================================

/**
 * Convert double array to int32 array with truncation
 * @param src Source double array
 * @param dst Destination int32 array
 * @param count Number of elements
 */
inline void convertFloat64ToInt32(const double* src, int32_t* dst, size_t count) {
#if USE_SIMD && SIMD_AVX2
  size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    __m256d doubles = _mm256_loadu_pd(src + i);
    __m128i ints = _mm256_cvttpd_epi32(doubles);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), ints);
  }
  for (; i < count; ++i) {
    dst[i] = static_cast<int32_t>(src[i]);
  }

#elif USE_SIMD && (SIMD_SSE2 || SIMD_SSE42)
  size_t i = 0;
  for (; i + 2 <= count; i += 2) {
    __m128d doubles = _mm_loadu_pd(src + i);
    __m128i ints = _mm_cvttpd_epi32(doubles);
    // Only store lower 64 bits (2 int32s)
    dst[i] = _mm_cvtsi128_si32(ints);
    dst[i + 1] = _mm_cvtsi128_si32(_mm_srli_si128(ints, 4));
  }
  for (; i < count; ++i) {
    dst[i] = static_cast<int32_t>(src[i]);
  }

#elif USE_SIMD && SIMD_NEON
  size_t i = 0;
  for (; i + 2 <= count; i += 2) {
    float64x2_t doubles = vld1q_f64(src + i);
    int64x2_t ints64 = vcvtq_s64_f64(doubles);
    int32x2_t ints = vmovn_s64(ints64);
    vst1_s32(dst + i, ints);
  }
  for (; i < count; ++i) {
    dst[i] = static_cast<int32_t>(src[i]);
  }

#elif USE_SIMD && SIMD_SVE
  size_t i = 0;
  svbool_t pg64 = svptrue_b64();
  size_t vl = svcntd();  // Vector length in 64-bit elements
  for (; i + vl <= count; i += vl) {
    svfloat64_t doubles = svld1_f64(pg64, src + i);
    svint64_t ints64 = svcvt_s64_f64_z(pg64, doubles);
    // Narrow to int32
    svint32_t ints32 = svuzp1_s32(svreinterpret_s32_s64(ints64), svreinterpret_s32_s64(ints64));
    svbool_t pg32 = svptrue_b32();
    svst1_s32(svwhilelt_b32(static_cast<uint64_t>(0), vl), dst + i, ints32);
  }
  for (; i < count; ++i) {
    dst[i] = static_cast<int32_t>(src[i]);
  }

#else
  for (size_t i = 0; i < count; ++i) {
    dst[i] = static_cast<int32_t>(src[i]);
  }
#endif
}

// =============================================================================
// Uint8 clamped operations (vectorized)
// =============================================================================

/**
 * Clamp float array to uint8 (0-255) and store
 * @param src Source float array
 * @param dst Destination uint8 array
 * @param count Number of elements
 */
inline void clampFloat32ToUint8(const float* src, uint8_t* dst, size_t count) {
#if USE_SIMD && SIMD_AVX2
  size_t i = 0;
  __m256 zero = _mm256_setzero_ps();
  __m256 max_val = _mm256_set1_ps(255.0f);

  for (; i + 8 <= count; i += 8) {
    __m256 floats = _mm256_loadu_ps(src + i);
    // Clamp to [0, 255]
    floats = _mm256_max_ps(floats, zero);
    floats = _mm256_min_ps(floats, max_val);
    // Convert to int32
    __m256i ints = _mm256_cvtps_epi32(floats);
    // Pack to int16 (with saturation)
    __m128i lo = _mm256_castsi256_si128(ints);
    __m128i hi = _mm256_extracti128_si256(ints, 1);
    __m128i packed16 = _mm_packs_epi32(lo, hi);
    // Pack to uint8 (with saturation)
    __m128i packed8 = _mm_packus_epi16(packed16, packed16);
    // Store 8 bytes
    _mm_storel_epi64(reinterpret_cast<__m128i*>(dst + i), packed8);
  }
  for (; i < count; ++i) {
    float val = src[i];
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    dst[i] = static_cast<uint8_t>(val + 0.5f);
  }

#elif USE_SIMD && (SIMD_SSE2 || SIMD_SSE42)
  size_t i = 0;
  __m128 zero = _mm_setzero_ps();
  __m128 max_val = _mm_set1_ps(255.0f);

  for (; i + 4 <= count; i += 4) {
    __m128 floats = _mm_loadu_ps(src + i);
    floats = _mm_max_ps(floats, zero);
    floats = _mm_min_ps(floats, max_val);
    __m128i ints = _mm_cvtps_epi32(floats);
    __m128i packed16 = _mm_packs_epi32(ints, ints);
    __m128i packed8 = _mm_packus_epi16(packed16, packed16);
    // Store 4 bytes
    *reinterpret_cast<uint32_t*>(dst + i) = static_cast<uint32_t>(_mm_cvtsi128_si32(packed8));
  }
  for (; i < count; ++i) {
    float val = src[i];
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    dst[i] = static_cast<uint8_t>(val + 0.5f);
  }

#elif USE_SIMD && SIMD_NEON
  size_t i = 0;
  for (; i + 8 <= count; i += 8) {
    float32x4_t floats_lo = vld1q_f32(src + i);
    float32x4_t floats_hi = vld1q_f32(src + i + 4);
    // Clamp and convert
    int32x4_t ints_lo = vcvtq_s32_f32(vmaxq_f32(vminq_f32(floats_lo, vdupq_n_f32(255.0f)), vdupq_n_f32(0.0f)));
    int32x4_t ints_hi = vcvtq_s32_f32(vmaxq_f32(vminq_f32(floats_hi, vdupq_n_f32(255.0f)), vdupq_n_f32(0.0f)));
    // Narrow to uint16
    uint16x4_t u16_lo = vqmovun_s32(ints_lo);
    uint16x4_t u16_hi = vqmovun_s32(ints_hi);
    uint16x8_t u16 = vcombine_u16(u16_lo, u16_hi);
    // Narrow to uint8
    uint8x8_t u8 = vqmovn_u16(u16);
    vst1_u8(dst + i, u8);
  }
  for (; i < count; ++i) {
    float val = src[i];
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    dst[i] = static_cast<uint8_t>(val + 0.5f);
  }

#elif USE_SIMD && SIMD_SVE
  size_t i = 0;
  svbool_t pg = svptrue_b32();
  size_t vl = svcntw();
  svfloat32_t zero = svdup_f32(0.0f);
  svfloat32_t max_val = svdup_f32(255.0f);

  for (; i + vl <= count; i += vl) {
    svfloat32_t floats = svld1_f32(pg, src + i);
    floats = svmax_f32_z(pg, floats, zero);
    floats = svmin_f32_z(pg, floats, max_val);
    svint32_t ints = svcvt_s32_f32_z(pg, floats);
    // SVE narrowing - simplified for now
    for (size_t j = 0; j < vl && i + j < count; ++j) {
      int32_t val = svlasta_s32(svwhilele_b32(static_cast<uint32_t>(j), static_cast<uint32_t>(j)), ints);
      dst[i + j] = static_cast<uint8_t>(val);
    }
  }
  for (; i < count; ++i) {
    float val = src[i];
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    dst[i] = static_cast<uint8_t>(val + 0.5f);
  }

#else
  for (size_t i = 0; i < count; ++i) {
    float val = src[i];
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    dst[i] = static_cast<uint8_t>(val + 0.5f);
  }
#endif
}

// =============================================================================
// Float32 array operations
// =============================================================================

/**
 * Fill float32 array with a single value
 * @param dst Destination array
 * @param value Value to fill
 * @param count Number of elements
 */
inline void fillFloat32(float* dst, float value, size_t count) {
#if USE_SIMD && SIMD_AVX2
  size_t i = 0;
  __m256 val = _mm256_set1_ps(value);
  for (; i + 8 <= count; i += 8) {
    _mm256_storeu_ps(dst + i, val);
  }
  for (; i < count; ++i) {
    dst[i] = value;
  }

#elif USE_SIMD && (SIMD_SSE2 || SIMD_SSE42)
  size_t i = 0;
  __m128 val = _mm_set1_ps(value);
  for (; i + 4 <= count; i += 4) {
    _mm_storeu_ps(dst + i, val);
  }
  for (; i < count; ++i) {
    dst[i] = value;
  }

#elif USE_SIMD && SIMD_NEON
  size_t i = 0;
  float32x4_t val = vdupq_n_f32(value);
  for (; i + 4 <= count; i += 4) {
    vst1q_f32(dst + i, val);
  }
  for (; i < count; ++i) {
    dst[i] = value;
  }

#elif USE_SIMD && SIMD_SVE
  size_t i = 0;
  svbool_t pg = svptrue_b32();
  size_t vl = svcntw();
  svfloat32_t val = svdup_f32(value);
  for (; i + vl <= count; i += vl) {
    svst1_f32(pg, dst + i, val);
  }
  if (i < count) {
    svbool_t pg_tail = svwhilelt_b32(i, count);
    svst1_f32(pg_tail, dst + i, val);
  }

#else
  for (size_t i = 0; i < count; ++i) {
    dst[i] = value;
  }
#endif
}

// =============================================================================
// Int32 array operations
// =============================================================================

/**
 * Fill int32 array with a single value
 * @param dst Destination array
 * @param value Value to fill
 * @param count Number of elements
 */
inline void fillInt32(int32_t* dst, int32_t value, size_t count) {
#if USE_SIMD && SIMD_AVX2
  size_t i = 0;
  __m256i val = _mm256_set1_epi32(value);
  for (; i + 8 <= count; i += 8) {
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), val);
  }
  for (; i < count; ++i) {
    dst[i] = value;
  }

#elif USE_SIMD && (SIMD_SSE2 || SIMD_SSE42)
  size_t i = 0;
  __m128i val = _mm_set1_epi32(value);
  for (; i + 4 <= count; i += 4) {
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), val);
  }
  for (; i < count; ++i) {
    dst[i] = value;
  }

#elif USE_SIMD && SIMD_NEON
  size_t i = 0;
  int32x4_t val = vdupq_n_s32(value);
  for (; i + 4 <= count; i += 4) {
    vst1q_s32(dst + i, val);
  }
  for (; i < count; ++i) {
    dst[i] = value;
  }

#elif USE_SIMD && SIMD_SVE
  size_t i = 0;
  svbool_t pg = svptrue_b32();
  size_t vl = svcntw();
  svint32_t val = svdup_s32(value);
  for (; i + vl <= count; i += vl) {
    svst1_s32(pg, dst + i, val);
  }
  if (i < count) {
    svbool_t pg_tail = svwhilelt_b32(i, count);
    svst1_s32(pg_tail, dst + i, val);
  }

#else
  for (size_t i = 0; i < count; ++i) {
    dst[i] = value;
  }
#endif
}

// =============================================================================
// Memory copy operations (SIMD-accelerated)
// =============================================================================

/**
 * Copy memory with SIMD alignment optimization
 * Falls back to std::memcpy for small sizes
 * @param dst Destination
 * @param src Source
 * @param bytes Number of bytes to copy
 */
inline void memcpySIMD(void* dst, const void* src, size_t bytes) {
  // For small copies, use standard memcpy
  if (bytes < 64) {
    std::memcpy(dst, src, bytes);
    return;
  }

#if USE_SIMD && SIMD_AVX2
  uint8_t* d = static_cast<uint8_t*>(dst);
  const uint8_t* s = static_cast<const uint8_t*>(src);
  size_t i = 0;

  for (; i + 32 <= bytes; i += 32) {
    __m256i data = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + i));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + i), data);
  }
  // Handle remaining bytes
  if (i < bytes) {
    std::memcpy(d + i, s + i, bytes - i);
  }

#elif USE_SIMD && (SIMD_SSE2 || SIMD_SSE42)
  uint8_t* d = static_cast<uint8_t*>(dst);
  const uint8_t* s = static_cast<const uint8_t*>(src);
  size_t i = 0;

  for (; i + 16 <= bytes; i += 16) {
    __m128i data = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i), data);
  }
  if (i < bytes) {
    std::memcpy(d + i, s + i, bytes - i);
  }

#elif USE_SIMD && SIMD_NEON
  uint8_t* d = static_cast<uint8_t*>(dst);
  const uint8_t* s = static_cast<const uint8_t*>(src);
  size_t i = 0;

  for (; i + 16 <= bytes; i += 16) {
    uint8x16_t data = vld1q_u8(s + i);
    vst1q_u8(d + i, data);
  }
  if (i < bytes) {
    std::memcpy(d + i, s + i, bytes - i);
  }

#else
  std::memcpy(dst, src, bytes);
#endif
}

// =============================================================================
// Float16 batch conversion (software implementation)
// Hardware F16C/AVX-512 not used for Zen2 compatibility
// =============================================================================

/**
 * Convert float32 array to float16 array (software implementation)
 * @param src Source float32 array
 * @param dst Destination uint16 array (as float16 bits)
 * @param count Number of elements
 */
void convertFloat32ToFloat16Batch(const float* src, uint16_t* dst, size_t count);

/**
 * Convert float16 array to float32 array (software implementation)
 * @param src Source uint16 array (as float16 bits)
 * @param dst Destination float32 array
 * @param count Number of elements
 */
void convertFloat16ToFloat32Batch(const uint16_t* src, float* dst, size_t count);

// =============================================================================
// Uint8 to Float32 conversion (vectorized)
// =============================================================================

/**
 * Convert uint8 array to float32 array
 * @param src Source uint8 array
 * @param dst Destination float32 array
 * @param count Number of elements
 */
inline void convertUint8ToFloat32(const uint8_t* src, float* dst, size_t count) {
#if USE_SIMD && SIMD_AVX2
  size_t i = 0;
  for (; i + 8 <= count; i += 8) {
    // Load 8 bytes
    __m128i u8_vals = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(src + i));
    // Zero-extend to 32-bit
    __m256i u32_vals = _mm256_cvtepu8_epi32(u8_vals);
    // Convert to float
    __m256 floats = _mm256_cvtepi32_ps(u32_vals);
    _mm256_storeu_ps(dst + i, floats);
  }
  for (; i < count; ++i) {
    dst[i] = static_cast<float>(src[i]);
  }

#elif USE_SIMD && (SIMD_SSE2 || SIMD_SSE42)
  size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    // Load 4 bytes and zero-extend manually
    __m128i u8_vals = _mm_cvtsi32_si128(*reinterpret_cast<const int32_t*>(src + i));
    // Unpack bytes to words (zero-extend)
    __m128i u16_vals = _mm_unpacklo_epi8(u8_vals, _mm_setzero_si128());
    // Unpack words to dwords (zero-extend)
    __m128i u32_vals = _mm_unpacklo_epi16(u16_vals, _mm_setzero_si128());
    // Convert to float
    __m128 floats = _mm_cvtepi32_ps(u32_vals);
    _mm_storeu_ps(dst + i, floats);
  }
  for (; i < count; ++i) {
    dst[i] = static_cast<float>(src[i]);
  }

#elif USE_SIMD && SIMD_NEON
  size_t i = 0;
  for (; i + 8 <= count; i += 8) {
    uint8x8_t u8_vals = vld1_u8(src + i);
    uint16x8_t u16_vals = vmovl_u8(u8_vals);
    uint32x4_t u32_lo = vmovl_u16(vget_low_u16(u16_vals));
    uint32x4_t u32_hi = vmovl_u16(vget_high_u16(u16_vals));
    float32x4_t f32_lo = vcvtq_f32_u32(u32_lo);
    float32x4_t f32_hi = vcvtq_f32_u32(u32_hi);
    vst1q_f32(dst + i, f32_lo);
    vst1q_f32(dst + i + 4, f32_hi);
  }
  for (; i < count; ++i) {
    dst[i] = static_cast<float>(src[i]);
  }

#else
  for (size_t i = 0; i < count; ++i) {
    dst[i] = static_cast<float>(src[i]);
  }
#endif
}

// =============================================================================
// Float32 to Uint8 conversion (vectorized, with truncation)
// =============================================================================

/**
 * Convert float32 array to uint8 array (truncation, no clamping)
 * @param src Source float32 array
 * @param dst Destination uint8 array
 * @param count Number of elements
 */
inline void convertFloat32ToUint8(const float* src, uint8_t* dst, size_t count) {
#if USE_SIMD && SIMD_AVX2
  size_t i = 0;
  for (; i + 8 <= count; i += 8) {
    __m256 floats = _mm256_loadu_ps(src + i);
    __m256i ints = _mm256_cvttps_epi32(floats);
    // Pack to int16
    __m128i lo = _mm256_castsi256_si128(ints);
    __m128i hi = _mm256_extracti128_si256(ints, 1);
    __m128i packed16 = _mm_packs_epi32(lo, hi);
    // Pack to uint8
    __m128i packed8 = _mm_packus_epi16(packed16, packed16);
    _mm_storel_epi64(reinterpret_cast<__m128i*>(dst + i), packed8);
  }
  for (; i < count; ++i) {
    dst[i] = static_cast<uint8_t>(static_cast<int32_t>(src[i]));
  }

#elif USE_SIMD && (SIMD_SSE2 || SIMD_SSE42)
  size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    __m128 floats = _mm_loadu_ps(src + i);
    __m128i ints = _mm_cvttps_epi32(floats);
    __m128i packed16 = _mm_packs_epi32(ints, ints);
    __m128i packed8 = _mm_packus_epi16(packed16, packed16);
    *reinterpret_cast<uint32_t*>(dst + i) = static_cast<uint32_t>(_mm_cvtsi128_si32(packed8));
  }
  for (; i < count; ++i) {
    dst[i] = static_cast<uint8_t>(static_cast<int32_t>(src[i]));
  }

#elif USE_SIMD && SIMD_NEON
  size_t i = 0;
  for (; i + 8 <= count; i += 8) {
    float32x4_t floats_lo = vld1q_f32(src + i);
    float32x4_t floats_hi = vld1q_f32(src + i + 4);
    int32x4_t ints_lo = vcvtq_s32_f32(floats_lo);
    int32x4_t ints_hi = vcvtq_s32_f32(floats_hi);
    uint16x4_t u16_lo = vqmovun_s32(ints_lo);
    uint16x4_t u16_hi = vqmovun_s32(ints_hi);
    uint16x8_t u16 = vcombine_u16(u16_lo, u16_hi);
    uint8x8_t u8 = vqmovn_u16(u16);
    vst1_u8(dst + i, u8);
  }
  for (; i < count; ++i) {
    dst[i] = static_cast<uint8_t>(static_cast<int32_t>(src[i]));
  }

#else
  for (size_t i = 0; i < count; ++i) {
    dst[i] = static_cast<uint8_t>(static_cast<int32_t>(src[i]));
  }
#endif
}

// =============================================================================
// Int16 conversions (vectorized)
// =============================================================================

/**
 * Convert int16 array to float32 array
 */
inline void convertInt16ToFloat32(const int16_t* src, float* dst, size_t count) {
#if USE_SIMD && SIMD_AVX2
  size_t i = 0;
  for (; i + 8 <= count; i += 8) {
    __m128i i16_vals = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
    __m256i i32_vals = _mm256_cvtepi16_epi32(i16_vals);
    __m256 floats = _mm256_cvtepi32_ps(i32_vals);
    _mm256_storeu_ps(dst + i, floats);
  }
  for (; i < count; ++i) {
    dst[i] = static_cast<float>(src[i]);
  }

#elif USE_SIMD && (SIMD_SSE2 || SIMD_SSE42)
  size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    __m128i i16_vals = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(src + i));
    // Sign-extend to 32-bit
    __m128i i32_vals = _mm_cvtepi16_epi32(i16_vals);
    __m128 floats = _mm_cvtepi32_ps(i32_vals);
    _mm_storeu_ps(dst + i, floats);
  }
  for (; i < count; ++i) {
    dst[i] = static_cast<float>(src[i]);
  }

#elif USE_SIMD && SIMD_NEON
  size_t i = 0;
  for (; i + 8 <= count; i += 8) {
    int16x8_t i16_vals = vld1q_s16(src + i);
    int32x4_t i32_lo = vmovl_s16(vget_low_s16(i16_vals));
    int32x4_t i32_hi = vmovl_s16(vget_high_s16(i16_vals));
    float32x4_t f32_lo = vcvtq_f32_s32(i32_lo);
    float32x4_t f32_hi = vcvtq_f32_s32(i32_hi);
    vst1q_f32(dst + i, f32_lo);
    vst1q_f32(dst + i + 4, f32_hi);
  }
  for (; i < count; ++i) {
    dst[i] = static_cast<float>(src[i]);
  }

#else
  for (size_t i = 0; i < count; ++i) {
    dst[i] = static_cast<float>(src[i]);
  }
#endif
}

/**
 * Convert float32 array to int16 array (with truncation)
 */
inline void convertFloat32ToInt16(const float* src, int16_t* dst, size_t count) {
#if USE_SIMD && SIMD_AVX2
  size_t i = 0;
  for (; i + 8 <= count; i += 8) {
    __m256 floats = _mm256_loadu_ps(src + i);
    __m256i ints = _mm256_cvttps_epi32(floats);
    // Pack to int16 with saturation
    __m128i lo = _mm256_castsi256_si128(ints);
    __m128i hi = _mm256_extracti128_si256(ints, 1);
    __m128i packed = _mm_packs_epi32(lo, hi);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), packed);
  }
  for (; i < count; ++i) {
    dst[i] = static_cast<int16_t>(src[i]);
  }

#elif USE_SIMD && (SIMD_SSE2 || SIMD_SSE42)
  size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    __m128 floats = _mm_loadu_ps(src + i);
    __m128i ints = _mm_cvttps_epi32(floats);
    __m128i packed = _mm_packs_epi32(ints, ints);
    _mm_storel_epi64(reinterpret_cast<__m128i*>(dst + i), packed);
  }
  for (; i < count; ++i) {
    dst[i] = static_cast<int16_t>(src[i]);
  }

#elif USE_SIMD && SIMD_NEON
  size_t i = 0;
  for (; i + 8 <= count; i += 8) {
    float32x4_t f32_lo = vld1q_f32(src + i);
    float32x4_t f32_hi = vld1q_f32(src + i + 4);
    int32x4_t i32_lo = vcvtq_s32_f32(f32_lo);
    int32x4_t i32_hi = vcvtq_s32_f32(f32_hi);
    int16x4_t i16_lo = vqmovn_s32(i32_lo);
    int16x4_t i16_hi = vqmovn_s32(i32_hi);
    int16x8_t i16 = vcombine_s16(i16_lo, i16_hi);
    vst1q_s16(dst + i, i16);
  }
  for (; i < count; ++i) {
    dst[i] = static_cast<int16_t>(src[i]);
  }

#else
  for (size_t i = 0; i < count; ++i) {
    dst[i] = static_cast<int16_t>(src[i]);
  }
#endif
}

}  // namespace simd
}  // namespace lightjs
