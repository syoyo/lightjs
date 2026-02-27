#include "value.h"
#include <cmath>
#include <random>
#include <stdexcept>
#include <algorithm>
#include <limits>

namespace lightjs {

// Math.abs
Value Math_abs(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    if (!args[0].isNumber()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = std::get<double>(args[0].data);
    return Value(std::abs(x));
}

// Math.ceil
Value Math_ceil(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    if (!args[0].isNumber()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = std::get<double>(args[0].data);
    return Value(std::ceil(x));
}

// Math.floor
Value Math_floor(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    if (!args[0].isNumber()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = std::get<double>(args[0].data);
    return Value(std::floor(x));
}

// Math.round
Value Math_round(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    if (!args[0].isNumber()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = std::get<double>(args[0].data);
    return Value(std::round(x));
}

// Math.trunc
Value Math_trunc(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    if (!args[0].isNumber()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = std::get<double>(args[0].data);
    return Value(std::trunc(x));
}

// Math.max
Value Math_max(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(-std::numeric_limits<double>::infinity());
    }

    double result = -std::numeric_limits<double>::infinity();
    for (const auto& arg : args) {
        if (!arg.isNumber()) {
            return Value(std::numeric_limits<double>::quiet_NaN());
        }
        double val = std::get<double>(arg.data);
        if (std::isnan(val)) {
            return Value(std::numeric_limits<double>::quiet_NaN());
        }
        result = std::max(result, val);
    }
    return Value(result);
}

// Math.min
Value Math_min(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::infinity());
    }

    double result = std::numeric_limits<double>::infinity();
    for (const auto& arg : args) {
        if (!arg.isNumber()) {
            return Value(std::numeric_limits<double>::quiet_NaN());
        }
        double val = std::get<double>(arg.data);
        if (std::isnan(val)) {
            return Value(std::numeric_limits<double>::quiet_NaN());
        }
        result = std::min(result, val);
    }
    return Value(result);
}

// Math.pow
Value Math_pow(const std::vector<Value>& args) {
    if (args.size() < 2) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    if (!args[0].isNumber() || !args[1].isNumber()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double base = std::get<double>(args[0].data);
    double exponent = std::get<double>(args[1].data);
    return Value(std::pow(base, exponent));
}

// Math.sqrt
Value Math_sqrt(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    if (!args[0].isNumber()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = std::get<double>(args[0].data);
    return Value(std::sqrt(x));
}

// Math.sin
Value Math_sin(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    if (!args[0].isNumber()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = std::get<double>(args[0].data);
    return Value(std::sin(x));
}

// Math.cos
Value Math_cos(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    if (!args[0].isNumber()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = std::get<double>(args[0].data);
    return Value(std::cos(x));
}

// Math.tan
Value Math_tan(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    if (!args[0].isNumber()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = std::get<double>(args[0].data);
    return Value(std::tan(x));
}

// Math.random
Value Math_random(const std::vector<Value>& args) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_real_distribution<double> dis(0.0, 1.0);

    return Value(dis(gen));
}

// Math.sign
Value Math_sign(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    if (!args[0].isNumber()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = std::get<double>(args[0].data);
    if (std::isnan(x)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    if (x > 0) return Value(1.0);
    if (x < 0) return Value(-1.0);
    return Value(x); // Preserves +0 and -0
}

// Math.log
Value Math_log(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    if (!args[0].isNumber()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = std::get<double>(args[0].data);
    return Value(std::log(x));
}

// Math.log10
Value Math_log10(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    if (!args[0].isNumber()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = std::get<double>(args[0].data);
    return Value(std::log10(x));
}

// Math.exp
Value Math_exp(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    if (!args[0].isNumber()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = std::get<double>(args[0].data);
    return Value(std::exp(x));
}

// Math.cbrt - cube root
Value Math_cbrt(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    if (!args[0].isNumber()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = std::get<double>(args[0].data);
    return Value(std::cbrt(x));
}

// Math.log2 - base 2 logarithm
Value Math_log2(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    if (!args[0].isNumber()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = std::get<double>(args[0].data);
    return Value(std::log2(x));
}

// Math.hypot - hypotenuse
Value Math_hypot(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(0.0);
    }

    double result = 0.0;
    for (const auto& arg : args) {
        if (!arg.isNumber()) {
            return Value(std::numeric_limits<double>::quiet_NaN());
        }
        double val = std::get<double>(arg.data);
        if (std::isnan(val)) {
            return Value(std::numeric_limits<double>::quiet_NaN());
        }
        if (std::isinf(val)) {
            return Value(std::numeric_limits<double>::infinity());
        }
        result += val * val;
    }
    return Value(std::sqrt(result));
}

// Math.expm1 - e^x - 1 (more accurate for small x)
Value Math_expm1(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    if (!args[0].isNumber()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = std::get<double>(args[0].data);
    return Value(std::expm1(x));
}

// Math.log1p - ln(1 + x) (more accurate for small x)
Value Math_log1p(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    if (!args[0].isNumber()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = std::get<double>(args[0].data);
    return Value(std::log1p(x));
}

// Math.fround - round to nearest 32-bit float
Value Math_fround(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    if (!args[0].isNumber()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = std::get<double>(args[0].data);
    return Value(static_cast<double>(static_cast<float>(x)));
}

// Math.clz32 - count leading zeros in 32-bit integer
Value Math_clz32(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(32.0);
    }

    if (!args[0].isNumber()) {
        return Value(32.0);
    }

    double x = std::get<double>(args[0].data);
    uint32_t n = static_cast<uint32_t>(static_cast<int32_t>(x));
    if (n == 0) {
        return Value(32.0);
    }
    int count = 0;
    while ((n & 0x80000000) == 0) {
        count++;
        n <<= 1;
    }
    return Value(static_cast<double>(count));
}

// Math.imul - 32-bit integer multiplication
Value Math_imul(const std::vector<Value>& args) {
    if (args.size() < 2) {
        return Value(0.0);
    }

    int32_t a = static_cast<int32_t>(args[0].toNumber());
    int32_t b = static_cast<int32_t>(args[1].toNumber());
    return Value(static_cast<double>(a * b));
}

Value Math_asin(const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(std::asin(args[0].toNumber()));
}

Value Math_acos(const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(std::acos(args[0].toNumber()));
}

Value Math_atan(const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(std::atan(args[0].toNumber()));
}

Value Math_atan2(const std::vector<Value>& args) {
    if (args.size() < 2) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(std::atan2(args[0].toNumber(), args[1].toNumber()));
}

Value Math_sinh(const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(std::sinh(args[0].toNumber()));
}

Value Math_cosh(const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(std::cosh(args[0].toNumber()));
}

Value Math_tanh(const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(std::tanh(args[0].toNumber()));
}

Value Math_asinh(const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(std::asinh(args[0].toNumber()));
}

Value Math_acosh(const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(std::acosh(args[0].toNumber()));
}

Value Math_atanh(const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(std::atanh(args[0].toNumber()));
}

} // namespace lightjs