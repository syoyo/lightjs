#include "value.h"
#include "symbols.h"
#include "environment.h"
#include "interpreter.h"
#include "streams.h"
#include "wasm_js.h"
#include <cmath>
#include <cstring>
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

    double x = toNumberES(args[0]);
    return Value(std::abs(x));
}

// Math.ceil
Value Math_ceil(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = toNumberES(args[0]);
    return Value(std::ceil(x));
}

// Math.floor
Value Math_floor(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = toNumberES(args[0]);
    return Value(std::floor(x));
}

// Math.round - ES spec: round toward +Infinity for .5
Value Math_round(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = toNumberES(args[0]);
    if (std::isnan(x) || std::isinf(x) || x == 0.0) return Value(x);
    // ES spec: If -0.5 <= x < 0, return -0
    if (x >= -0.5 && x < 0.0) return Value(-0.0);
    // ES spec: Math.round ties toward +∞.
    // std::floor(x + 0.5) has FP precision issues (e.g. 0.49999999999999994 + 0.5 = 1.0).
    double n = std::floor(x);
    double frac = x - n;
    if (frac > 0.5 || frac == 0.5) return Value(n + 1.0);
    return Value(n);
}

// Math.trunc
Value Math_trunc(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = toNumberES(args[0]);
    return Value(std::trunc(x));
}

// Math.max - ES spec: coerce args to number, handle -0/+0
Value Math_max(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(-std::numeric_limits<double>::infinity());
    }

    // ES spec: coerce ALL arguments first, then compute
    std::vector<double> coerced;
    coerced.reserve(args.size());
    for (const auto& arg : args) {
        coerced.push_back(toNumberES(arg));
    }

    double result = -std::numeric_limits<double>::infinity();
    for (double val : coerced) {
        if (std::isnan(val)) return Value(std::numeric_limits<double>::quiet_NaN());
        if (val > result || (val == result && val == 0.0 && std::signbit(result) && !std::signbit(val))) {
            result = val;
        }
    }
    return Value(result);
}

// Math.min - ES spec: coerce args to number, handle -0/+0
Value Math_min(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::infinity());
    }

    // ES spec: coerce ALL arguments first, then compute
    std::vector<double> coerced;
    coerced.reserve(args.size());
    for (const auto& arg : args) {
        coerced.push_back(toNumberES(arg));
    }

    double result = std::numeric_limits<double>::infinity();
    for (double val : coerced) {
        if (std::isnan(val)) return Value(std::numeric_limits<double>::quiet_NaN());
        if (val < result || (val == result && val == 0.0 && !std::signbit(result) && std::signbit(val))) {
            result = val;
        }
    }
    return Value(result);
}

// Math.pow - ES spec Number::exponentiate
Value Math_pow(const std::vector<Value>& args) {
    if (args.size() < 2) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double base = toNumberES(args[0]);
    double exponent = toNumberES(args[1]);

    // ES spec special cases
    if (std::isnan(exponent)) return Value(std::numeric_limits<double>::quiet_NaN());
    if (exponent == 0.0) return Value(1.0); // Even for NaN base
    if (std::isnan(base)) return Value(std::numeric_limits<double>::quiet_NaN());
    if (std::abs(base) == 1.0 && std::isinf(exponent)) return Value(std::numeric_limits<double>::quiet_NaN());

    return Value(std::pow(base, exponent));
}

// Math.sqrt
Value Math_sqrt(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = toNumberES(args[0]);
    return Value(std::sqrt(x));
}

// Math.sin
Value Math_sin(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = toNumberES(args[0]);
    return Value(std::sin(x));
}

// Math.cos
Value Math_cos(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = toNumberES(args[0]);
    return Value(std::cos(x));
}

// Math.tan
Value Math_tan(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = toNumberES(args[0]);
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

    double x = toNumberES(args[0]);
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

    double x = toNumberES(args[0]);
    return Value(std::log(x));
}

// Math.log10
Value Math_log10(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = toNumberES(args[0]);
    return Value(std::log10(x));
}

// Math.exp
Value Math_exp(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = toNumberES(args[0]);
    return Value(std::exp(x));
}

// Math.cbrt - cube root
Value Math_cbrt(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = toNumberES(args[0]);
    return Value(std::cbrt(x));
}

// Math.log2 - base 2 logarithm
Value Math_log2(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = toNumberES(args[0]);
    return Value(std::log2(x));
}

// Math.hypot - ES spec: Infinity wins over NaN, coerce args
Value Math_hypot(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(0.0);
    }

    // First pass: coerce all to number, check for Infinity (wins over NaN)
    std::vector<double> values;
    values.reserve(args.size());
    bool hasInf = false;
    bool hasNaN = false;
    for (const auto& arg : args) {
        double val = toNumberES(arg);
        if (std::isinf(val)) hasInf = true;
        if (std::isnan(val)) hasNaN = true;
        values.push_back(val);
    }
    // Per spec: If any is +/-Infinity, return +Infinity (even if NaN present)
    if (hasInf) return Value(std::numeric_limits<double>::infinity());
    if (hasNaN) return Value(std::numeric_limits<double>::quiet_NaN());

    double result = 0.0;
    for (double val : values) {
        result += val * val;
    }
    return Value(std::sqrt(result));
}

// Math.expm1 - e^x - 1 (more accurate for small x)
Value Math_expm1(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = toNumberES(args[0]);
    return Value(std::expm1(x));
}

// Math.log1p - ln(1 + x) (more accurate for small x)
Value Math_log1p(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = toNumberES(args[0]);
    return Value(std::log1p(x));
}

// Math.fround - round to nearest 32-bit float
Value Math_fround(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double x = toNumberES(args[0]);
    return Value(static_cast<double>(static_cast<float>(x)));
}

// ToUint32 helper per ES spec
static uint32_t toUint32(double x) {
    if (std::isnan(x) || std::isinf(x) || x == 0.0) return 0;
    double d = std::trunc(x);
    double two32 = 4294967296.0;
    d = std::fmod(d, two32);
    if (d < 0) d += two32;
    return static_cast<uint32_t>(d);
}

// ToInt32 helper per ES spec
static int32_t toInt32(double x) {
    uint32_t u = toUint32(x);
    if (u >= 2147483648u) return static_cast<int32_t>(u - 4294967296.0);
    return static_cast<int32_t>(u);
}

// Math.clz32 - count leading zeros in 32-bit integer
Value Math_clz32(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(32.0);
    }

    double x = toNumberES(args[0]);
    uint32_t n = toUint32(x);
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

    int32_t a = toInt32(toNumberES(args[0]));
    int32_t b = toInt32(toNumberES(args[1]));
    return Value(static_cast<double>(a * b));
}

Value Math_asin(const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(std::asin(toNumberES(args[0])));
}

Value Math_acos(const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(std::acos(toNumberES(args[0])));
}

Value Math_atan(const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(std::atan(toNumberES(args[0])));
}

Value Math_atan2(const std::vector<Value>& args) {
    if (args.size() < 2) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(std::atan2(toNumberES(args[0]), toNumberES(args[1])));
}

Value Math_sinh(const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(std::sinh(toNumberES(args[0])));
}

Value Math_cosh(const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(std::cosh(toNumberES(args[0])));
}

Value Math_tanh(const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(std::tanh(toNumberES(args[0])));
}

Value Math_asinh(const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(std::asinh(toNumberES(args[0])));
}

Value Math_acosh(const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(std::acosh(toNumberES(args[0])));
}

Value Math_atanh(const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(std::atanh(toNumberES(args[0])));
}

// Math.f16round - round to IEEE 754 half-precision float
Value Math_f16round(const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    double x = toNumberES(args[0]);
    if (std::isnan(x)) return Value(std::numeric_limits<double>::quiet_NaN());
    if (std::isinf(x) || x == 0.0) return Value(x);

    // Convert double to float16 and back
    // IEEE 754 half precision: 1 sign, 5 exponent, 10 mantissa bits
    float f = static_cast<float>(x);
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));

    uint32_t sign = (bits >> 31) & 1;
    int32_t exp = ((bits >> 23) & 0xFF) - 127;
    uint32_t mant = bits & 0x7FFFFF;

    uint16_t h;
    if (exp > 15) {
        h = (sign << 15) | (0x1F << 10); // Infinity
    } else if (exp < -14) {
        // Subnormal or zero
        int shift = -14 - exp;
        uint32_t fullMant = (1 << 23) | mant;
        if (shift < 24) {
            uint32_t roundBit = (fullMant >> (shift + 12)) & 1;
            uint32_t stickyBit = (fullMant & ((1 << (shift + 12)) - 1)) ? 1 : 0;
            uint32_t halfMant = fullMant >> (shift + 13);
            // Round to nearest, ties to even
            if (roundBit && (stickyBit || (halfMant & 1))) {
                halfMant++;
            }
            h = (sign << 15) | (halfMant & 0x3FF);
        } else {
            h = sign << 15; // Zero
        }
    } else {
        uint32_t roundBit = (mant >> 12) & 1;
        uint32_t stickyBit = (mant & 0xFFF) ? 1 : 0;
        uint32_t halfMant = mant >> 13;
        // Round to nearest, ties to even
        if (roundBit && (stickyBit || (halfMant & 1))) {
            halfMant++;
            if (halfMant >= 0x400) {
                halfMant = 0;
                exp++;
                if (exp > 15) {
                    h = (sign << 15) | (0x1F << 10);
                    goto done;
                }
            }
        }
        h = (sign << 15) | ((exp + 15) << 10) | (halfMant & 0x3FF);
    }
done:
    // Convert back to double
    uint32_t hSign = (h >> 15) & 1;
    uint32_t hExp = (h >> 10) & 0x1F;
    uint32_t hMant = h & 0x3FF;

    double result;
    if (hExp == 0) {
        if (hMant == 0) {
            result = hSign ? -0.0 : 0.0;
        } else {
            result = std::ldexp(static_cast<double>(hMant), -24);
            if (hSign) result = -result;
        }
    } else if (hExp == 0x1F) {
        result = hMant ? std::numeric_limits<double>::quiet_NaN()
                       : (hSign ? -std::numeric_limits<double>::infinity()
                                : std::numeric_limits<double>::infinity());
    } else {
        result = std::ldexp(1.0 + static_cast<double>(hMant) / 1024.0, hExp - 15);
        if (hSign) result = -result;
    }
    return Value(result);
}

// Two-sum: exact computation of a + b = hi + lo where hi = fl(a+b)
static inline void twoSum(double a, double b, double& hi, double& lo) {
    hi = a + b;
    double v = hi - a;
    lo = (a - (hi - v)) + (b - v);
}

// Maximally precise summation using Shewchuk algorithm with overflow prevention.
static Value shewchukSum(const std::vector<double>& numbers) {
    bool hasInf = false, hasNegInf = false, hasNaN = false;
    bool allNegZeroOrZero = true;
    bool hasPositiveZero = false;

    for (double val : numbers) {
        if (std::isnan(val)) { hasNaN = true; allNegZeroOrZero = false; }
        else if (val == std::numeric_limits<double>::infinity()) { hasInf = true; allNegZeroOrZero = false; }
        else if (val == -std::numeric_limits<double>::infinity()) { hasNegInf = true; allNegZeroOrZero = false; }
        else if (val != 0.0) { allNegZeroOrZero = false; }
        else if (!std::signbit(val)) { hasPositiveZero = true; }
    }
    if (hasInf && hasNegInf) return Value(std::numeric_limits<double>::quiet_NaN());
    if (hasNaN) return Value(std::numeric_limits<double>::quiet_NaN());
    if (hasInf) return Value(std::numeric_limits<double>::infinity());
    if (hasNegInf) return Value(-std::numeric_limits<double>::infinity());

    // Sort values by magnitude descending, then interleave pos/neg
    // to minimize intermediate overflow in Shewchuk algorithm
    std::vector<double> pos, neg;
    for (double val : numbers) {
        if (!std::isfinite(val) || val == 0.0) continue;
        if (val > 0) pos.push_back(val);
        else neg.push_back(-val);
    }
    std::sort(pos.begin(), pos.end(), std::greater<double>());
    std::sort(neg.begin(), neg.end(), std::greater<double>());

    // Interleave: alternate pos/neg, picking the larger magnitude first
    // This ensures consecutive values tend to cancel rather than overflow
    std::vector<double> ordered;
    ordered.reserve(pos.size() + neg.size());
    size_t pi = 0, ni = 0;
    while (pi < pos.size() || ni < neg.size()) {
        // Always alternate: add one pos, then one neg
        if (pi < pos.size()) ordered.push_back(pos[pi++]);
        if (ni < neg.size()) ordered.push_back(-neg[ni++]);
    }

    std::vector<double> partials;
    for (double x : ordered) {
        size_t i = 0;
        for (size_t j = 0; j < partials.size(); j++) {
            double y = partials[j];
            double hi, lo;
            if (std::abs(x) < std::abs(y)) std::swap(x, y);
            twoSum(x, y, hi, lo);
            if (lo != 0.0) partials[i++] = lo;
            x = hi;
        }
        partials.resize(i);
        if (std::isfinite(x)) {
            partials.push_back(x);
        } else {
            // Overflow in intermediate sum
            return Value(x);
        }
    }
    if (partials.empty()) {
        if (numbers.empty() || (allNegZeroOrZero && !hasPositiveZero)) {
            return Value(-0.0);
        }
        return Value(0.0);
    }

    // Sum the partials with correct rounding (CPython fsum algorithm)
    size_t n = partials.size();
    double hi = partials[n - 1];
    double lo = 0.0;
    int roundingIdx = -1;

    if (n >= 2) {
        for (int i = static_cast<int>(n) - 2; i >= 0; i--) {
            double x = hi;
            double y = partials[i];
            hi = x + y;
            lo = y - (hi - x);
            if (lo != 0.0) {
                roundingIdx = i;
                break;
            }
        }
    }

    double sum;
    if (lo != 0.0 && roundingIdx > 0) {
        // Check if we need rounding correction:
        // If lo and the next lower partial have the same sign,
        // and hi + lo rounds to hi, we need to adjust
        double nextPartial = partials[roundingIdx - 1];
        // Test if lo is at the midpoint between two consecutive doubles
        // and the tiebreaker should go in the direction of nextPartial
        double y = lo * 2.0;
        double x = hi + y;
        if (x == hi + lo + lo) {
            // x rounded the same way - check if we're at a tie
            double yr = x - hi;
            if (yr == y && // no rounding occurred
                ((lo < 0.0 && nextPartial < 0.0) || (lo > 0.0 && nextPartial > 0.0))) {
                // Correction needed
                // hi rounds to the wrong direction
                sum = x;
            } else {
                sum = hi + lo;
            }
        } else {
            sum = hi + lo;
        }
    } else if (lo != 0.0) {
        sum = hi + lo;
    } else {
        sum = hi;
    }

    if (sum == 0.0) {
        if (numbers.empty() || (allNegZeroOrZero && !hasPositiveZero)) {
            return Value(-0.0);
        }
        return Value(0.0);
    }

    if (!std::isfinite(sum)) return Value(sum);
    return Value(sum);
}

// Helper: get iterator from a value, returns {iteratorObj, nextFn}
// Throws TypeError if not iterable
static std::pair<Value, Value> getIterator(Interpreter* interp, const Value& val) {
    if (!interp) throw std::runtime_error("TypeError: Math.sumPrecise requires an iterable argument");

    // Generators are their own iterator: gen[Symbol.iterator]() returns gen
    // Since both Symbol.iterator and next are dynamically created in evaluateMember,
    // we handle generators directly
    if (val.isGenerator()) {
        // Generator is its own iterator - next will be called via generatorNext()
        return {val, Value(Undefined{})};
    }

    // Use interpreter to get Symbol.iterator via property lookup (handles prototype chain)
    const auto& iterKey = WellKnownSymbols::iteratorKey();
    auto [found, iterMethod] = interp->getPropertyForExternal(val, iterKey);

    if (!found || !iterMethod.isFunction()) {
        throw std::runtime_error("TypeError: Math.sumPrecise requires an iterable argument");
    }

    Value iteratorObj = interp->callForHarness(iterMethod, {}, val);
    if (interp->hasError()) {
        Value err = interp->getError();
        interp->clearError();
        throw std::runtime_error(err.toString());
    }

    // If the iterator is a generator, handle its dynamic next()
    if (iteratorObj.isGenerator()) {
        return {iteratorObj, Value(Undefined{})};
    }

    // Get next method
    Value nextFn;
    auto [nextFound, fn] = interp->getPropertyForExternal(iteratorObj, "next");
    if (nextFound && fn.isFunction()) {
        nextFn = fn;
    } else {
        throw std::runtime_error("TypeError: iterator does not have a next method");
    }

    return {iteratorObj, nextFn};
}

// Helper: close iterator (call return() if present)
static void closeIterator(Interpreter* interp, const Value& iteratorObj) {
    if (!interp) return;
    Value returnFn;
    if (iteratorObj.isObject()) {
        auto obj = iteratorObj.getGC<Object>();
        auto it = obj->properties.find("return");
        if (it != obj->properties.end() && it->second.isFunction()) returnFn = it->second;
    }
    if (returnFn.isFunction()) {
        interp->callForHarness(returnFn, {}, iteratorObj);
        if (interp->hasError()) interp->clearError();
    }
}

// Math.sumPrecise - precise sum of iterable of numbers
Value Math_sumPrecise(const std::vector<Value>& args) {
    if (args.empty()) {
        throw std::runtime_error("TypeError: Math.sumPrecise requires 1 argument");
    }

    // Non-object/non-iterable values are TypeError
    if (args[0].isNumber() || args[0].isString() || args[0].isBool() ||
        args[0].isNull() || args[0].isUndefined() || args[0].isBigInt()) {
        throw std::runtime_error("TypeError: Math.sumPrecise requires an iterable argument");
    }

    Interpreter* interpreter = getGlobalInterpreter();
    auto [iteratorObj, nextFn] = getIterator(interpreter, args[0]);

    std::vector<double> numbers;
    while (true) {
        Value step;
        if (nextFn.isFunction()) {
            step = interpreter->callForHarness(nextFn, {}, iteratorObj);
        } else if (iteratorObj.isGenerator()) {
            // Generator's next is dynamically created - use generatorNext
            step = interpreter->generatorNext(iteratorObj);
        } else {
            break;
        }
        if (interpreter->hasError()) {
            Value err = interpreter->getError();
            interpreter->clearError();
            throw std::runtime_error(err.toString());
        }
        if (!step.isObject()) break;
        auto stepObj = step.getGC<Object>();
        auto doneIt = stepObj->properties.find("done");
        if (doneIt != stepObj->properties.end() && doneIt->second.toBool()) break;
        auto valueIt = stepObj->properties.find("value");
        Value val = (valueIt != stepObj->properties.end()) ? valueIt->second : Value(Undefined{});
        if (!val.isNumber()) {
            closeIterator(interpreter, iteratorObj);
            throw std::runtime_error("TypeError: Math.sumPrecise requires an iterable of numbers");
        }
        numbers.push_back(std::get<double>(val.data));
    }

    return shewchukSum(numbers);
}

} // namespace lightjs