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
    uint16_t halfBits = float64_to_float16(x);
    return Value(static_cast<double>(float16_to_float32(halfBits)));
}

struct ExactAccumulator {
    uint64_t buckets[45]; // ~2880 bits, enough for double range + carries

    ExactAccumulator() {
        memset(buckets, 0, sizeof(buckets));
    }

    void add(double d) {
        if (d == 0.0) return;
        uint64_t u;
        memcpy(&u, &d, 8);
        int e = (u >> 52) & 0x7FF;
        uint64_t m = u & 0xFFFFFFFFFFFFFULL;
        int exp;
        if (e == 0) {
            // Subnormal
            exp = -1022 - 52;
        } else {
            m |= (1ULL << 52);
            exp = e - 1023 - 52;
        }
        
        // Bit 0 is 2^-1074
        int bitPos = exp + 1074;
        int bucketIdx = bitPos / 64;
        int offset = bitPos % 64;
        
        __uint128_t val = (__uint128_t)m << offset;
        int i = bucketIdx;
        while (val > 0 && i < 45) {
            __uint128_t sum = (__uint128_t)buckets[i] + (uint64_t)val;
            buckets[i] = (uint64_t)sum;
            val = (val >> 64) + (sum >> 64);
            i++;
        }
    }

    bool isZero() const {
        for (int i = 0; i < 45; i++) if (buckets[i] != 0) return false;
        return true;
    }

    // Returns true if this >= other, and performs this -= other.
    // Otherwise returns false and leaves this unchanged.
    bool subtract(const ExactAccumulator& other) {
        // Compare first
        bool ge = true;
        for (int i = 44; i >= 0; i--) {
            if (buckets[i] > other.buckets[i]) { ge = true; break; }
            if (buckets[i] < other.buckets[i]) { ge = false; break; }
        }
        if (!ge) return false;

        uint64_t borrow = 0;
        for (int i = 0; i < 45; i++) {
            uint64_t a = buckets[i];
            uint64_t b = other.buckets[i];
            uint64_t next_borrow = 0;
            if (a < b || (a == b && borrow)) {
                if (a < b + borrow || (b == 0xFFFFFFFFFFFFFFFFULL && borrow)) {
                    next_borrow = 1;
                }
            }
            buckets[i] = a - b - borrow;
            borrow = next_borrow;
        }
        return true;
    }

    double roundToDouble(bool negative) const {
        int highestBucket = -1;
        for (int i = 44; i >= 0; i--) {
            if (buckets[i] != 0) { highestBucket = i; break; }
        }
        if (highestBucket == -1) return negative ? -0.0 : 0.0;

        int highestBitInBucket = 63;
        while (!(buckets[highestBucket] & (1ULL << highestBitInBucket))) highestBitInBucket--;
        
        int H = highestBucket * 64 + highestBitInBucket;
        // Mathematical sum is in [2^(H-1074), 2^(H-1074+1))
        int exp = H - 1074;

        if (exp >= 1024) return negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();

        // We need bits [H-52, H] for the mantissa (53 bits total)
        // Bit H is the implicit 1 (for normal numbers)
        // If exp < -1022, it's a subnormal.
        
        if (exp < -1022) {
            // Subnormal rounding
            // Bits are relative to 2^-1074.
            // Result will be M * 2^-1074 where M is in [1, 2^52)
            // We want to round the exact value to nearest M.
            // Our buckets already have bits starting from 2^-1074.
            // So we just need to round the buckets[0..1] to nearest.
            uint64_t m = 0;
            bool half = false;
            bool sticky = false;

            // Bit i corresponds to 2^(i-1074).
            // We want to round to bits [0, 51].
            // Wait, subnormal mantissa is 52 bits, but the implicit bit is 0.
            // So it's bits [0, 51] that are stored in the double.
            // Rounding bit is -1? No, we can't have bits below 2^-1074.
            // So it's exactly represented if it's subnormal!
            // Wait, if exp < -1022, H < -1022 + 1074 = 52.
            // So highest bit is below 52. All bits are exactly representable!
            for (int i = 0; i < 45; i++) {
                if (i == 0) m = buckets[0];
                else if (buckets[i] != 0) {
                    // This should not happen if H < 52
                }
            }
            double res = std::scalbn((double)m, -1074);
            return negative ? -res : res;
        }

        // Normal number rounding
        // Mantissa bits [H-52, H].
        // Get these bits.
        uint64_t m = 0;
        for (int i = 0; i < 53; i++) {
            int bit = H - i;
            if (bit >= 0 && (buckets[bit / 64] & (1ULL << (bit % 64)))) {
                m |= (1ULL << (52 - i));
            }
        }

        // Rounding bit: H-53
        bool roundBit = false;
        if (H - 53 >= 0 && (buckets[(H - 53) / 64] & (1ULL << ((H - 53) % 64)))) roundBit = true;

        // Sticky bit: any bit below H-53
        bool sticky = false;
        for (int bit = H - 54; bit >= 0; bit--) {
            if (buckets[bit / 64] & (1ULL << (bit % 64))) {
                sticky = true;
                break;
            }
        }

        if (roundBit) {
            if (sticky || (m & 1)) {
                m++;
                if (m == (1ULL << 53)) {
                    m = (1ULL << 52);
                    exp++;
                }
            }
        }

        if (exp >= 1024) return negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();

        double res;
        if (exp < -1022) {
            // Became subnormal after rounding? (Should not happen if it was normal before)
            res = std::scalbn((double)m, exp - 52);
        } else {
            // m has the implicit 1 at bit 52.
            // Double format: exp biased by 1023, mantissa 52 bits.
            uint64_t u = (uint64_t)(exp + 1023) << 52;
            u |= (m & 0xFFFFFFFFFFFFFULL);
            memcpy(&res, &u, 8);
        }
        return negative ? -res : res;
    }
};

// Maximally precise summation using exact accumulator.
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

    ExactAccumulator pos, neg;
    for (double val : numbers) {
        if (val > 0) pos.add(val);
        else if (val < 0) neg.add(-val);
    }

    if (pos.isZero() && neg.isZero()) {
        if (numbers.empty() || (allNegZeroOrZero && !hasPositiveZero)) {
            return Value(-0.0);
        }
        return Value(0.0);
    }

    if (pos.subtract(neg)) {
        return Value(pos.roundToDouble(false));
    } else {
        neg.subtract(pos);
        return Value(neg.roundToDouble(true));
    }
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
