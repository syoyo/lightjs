# ARM64 Cross-Compilation Guide

## Overview
This guide documents the ARM64 cross-compilation setup for LightJS using `aarch64-linux-gnu-g++-14` and execution with QEMU user static.

## Prerequisites

Install required packages:
```bash
sudo apt-get install -y gcc-14-aarch64-linux-gnu g++-14-aarch64-linux-gnu qemu-user-static
```

## Build Steps

### 1. Clean and Configure
```bash
cd build
rm -rf *
cmake .. \
  -DCROSS_COMPILE_ARM64=ON \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++-14 \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc-14 \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64
```

### 2. Build
```bash
make -j$(nproc)
```

### 3. Verify Binary Architecture
```bash
file lightjs
# Output: ELF 64-bit LSB pie executable, ARM aarch64, ...
```

## Running with QEMU

### Run Tests
```bash
qemu-aarch64-static -L /usr/aarch64-linux-gnu ./lightjs_test
```

### Run JavaScript Files
```bash
qemu-aarch64-static -L /usr/aarch64-linux-gnu ./lightjs your_script.js
```

### Interactive REPL
```bash
qemu-aarch64-static -L /usr/aarch64-linux-gnu ./lightjs
```

## SIMD Optimizations

The ARM64 build automatically enables NEON SIMD optimizations for:
- TypedArray operations (Float32Array, Int32Array, etc.)
- Type conversions (Float64→Int32, Float32→Uint8, etc.)
- Memory operations (bulk copy, fill)
- Float16 conversions

### Compilation Flags
- `-march=armv8-a` - ARMv8-A baseline with NEON support
- SIMD architecture: `arm64`
- SIMD definitions: `USE_SIMD=1`, `SIMD_ARCH_ARM64=1`, `SIMD_NEON=1`

## NEON Implementation Fix

During cross-compilation, we fixed a NEON intrinsic issue in `include/simd.h`:

**Problem:** `vcvt_s32_f64()` doesn't exist in NEON

**Solution:** Use two-step conversion:
```cpp
// Convert float64x2_t → int64x2_t → int32x2_t
float64x2_t doubles = vld1q_f64(src + i);
int64x2_t ints64 = vcvtq_s64_f64(doubles);  // Step 1: f64 → i64
int32x2_t ints = vmovn_s64(ints64);         // Step 2: i64 → i32 (narrow)
vst1_s32(dst + i, ints);
```

## Test Results

All 197 tests passed successfully on ARM64 with QEMU:
- ✅ Basic JavaScript operations
- ✅ TypedArrays with NEON acceleration
- ✅ BigInt arithmetic
- ✅ Async/await and Promises
- ✅ Unicode support (emoji, CJK, Arabic)
- ✅ ArrayBuffer and DataView
- ✅ Cryptographic functions (SHA-256, HMAC)
- ✅ Regex engine
- ✅ Symbol and Iterator protocols

## Alternative: A64FX (Fugaku Supercomputer)

For Fujitsu A64FX with SVE (512-bit vectors):
```bash
cmake .. \
  -DCROSS_COMPILE_A64FX=ON \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++-14
make -j$(nproc)
```

This enables:
- ARMv8.2-A with SVE extensions
- 512-bit vector length
- Optimized for Fugaku supercomputer
- Flags: `-mcpu=a64fx -march=armv8.2-a+sve`

## Library Path

QEMU requires the ARM64 system libraries:
- **Location:** `/usr/aarch64-linux-gnu/`
- **Dynamic linker:** `/usr/aarch64-linux-gnu/lib/ld-linux-aarch64.so.1`
- **Flag:** `-L /usr/aarch64-linux-gnu`

## Troubleshooting

### Error: "Could not open '/lib/ld-linux-aarch64.so.1'"
**Solution:** Add `-L /usr/aarch64-linux-gnu` to QEMU command

### Error: "compiler not found"
**Solution:** Use full compiler name with version suffix:
```bash
-DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++-14
-DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc-14
```

### NEON compilation errors
**Solution:** Ensure correct NEON intrinsics are used (see NEON Implementation Fix above)

## Performance Notes

ARM64 NEON provides significant speedups for:
- **Float32Array operations:** 4x parallelism (128-bit vectors)
- **Int32Array operations:** 4x parallelism
- **Float64 conversions:** 2x parallelism
- **Uint8Clamped (image processing):** 16x parallelism

For production ARM64 deployments (Raspberry Pi, AWS Graviton, etc.), run natively without QEMU for full performance.
