#include "value.h"
#include <cmath>
#include <random>
#include <stdexcept>
#include <algorithm>
#include <limits>

namespace tinyjs {

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

} // namespace tinyjs