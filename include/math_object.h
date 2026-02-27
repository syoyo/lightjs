#pragma once

#include "value.h"
#include <vector>

namespace lightjs {

// Math object methods
Value Math_abs(const std::vector<Value>& args);
Value Math_ceil(const std::vector<Value>& args);
Value Math_floor(const std::vector<Value>& args);
Value Math_round(const std::vector<Value>& args);
Value Math_trunc(const std::vector<Value>& args);
Value Math_max(const std::vector<Value>& args);
Value Math_min(const std::vector<Value>& args);
Value Math_pow(const std::vector<Value>& args);
Value Math_sqrt(const std::vector<Value>& args);
Value Math_sin(const std::vector<Value>& args);
Value Math_cos(const std::vector<Value>& args);
Value Math_tan(const std::vector<Value>& args);
Value Math_random(const std::vector<Value>& args);
Value Math_sign(const std::vector<Value>& args);
Value Math_log(const std::vector<Value>& args);
Value Math_log10(const std::vector<Value>& args);
Value Math_exp(const std::vector<Value>& args);
Value Math_cbrt(const std::vector<Value>& args);
Value Math_log2(const std::vector<Value>& args);
Value Math_hypot(const std::vector<Value>& args);
Value Math_expm1(const std::vector<Value>& args);
Value Math_log1p(const std::vector<Value>& args);
Value Math_fround(const std::vector<Value>& args);
Value Math_clz32(const std::vector<Value>& args);
Value Math_imul(const std::vector<Value>& args);
Value Math_asin(const std::vector<Value>& args);
Value Math_acos(const std::vector<Value>& args);
Value Math_atan(const std::vector<Value>& args);
Value Math_atan2(const std::vector<Value>& args);
Value Math_sinh(const std::vector<Value>& args);
Value Math_cosh(const std::vector<Value>& args);
Value Math_tanh(const std::vector<Value>& args);
Value Math_asinh(const std::vector<Value>& args);
Value Math_acosh(const std::vector<Value>& args);
Value Math_atanh(const std::vector<Value>& args);

} // namespace lightjs