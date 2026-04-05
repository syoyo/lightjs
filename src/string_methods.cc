#include "string_methods.h"
#include "unicode.h"
#include "interpreter.h"
#include "error_formatter.h"
#include "simple_regex.h"
#include "symbols.h"
#include "environment.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace lightjs {

// Internal helper for string methods
namespace {

constexpr size_t kMaxSplitResultElements = (256 * 1024 * 1024) / sizeof(Value);

void appendSplitResult(const GCPtr<Array>& result,
                       uint32_t limit,
                       const Value& value) {
    if (result->elements.size() >= limit) {
        return;
    }
    if (result->elements.size() >= kMaxSplitResultElements) {
        throw std::runtime_error("RangeError: split result exceeds implementation limit");
    }
    result->elements.push_back(value);
}

bool isObjectLikeForStringBuiltin(const Value& v) {
    return v.isObject() || v.isArray() || v.isFunction() || v.isRegex() || 
           v.isProxy() || v.isPromise() || v.isGenerator() || v.isClass() ||
           v.isMap() || v.isSet() || v.isWeakMap() || v.isWeakSet() ||
           v.isTypedArray() || v.isArrayBuffer() || v.isDataView() || v.isError();
}

Value toPrimitiveForStringBuiltinImpl(const Value& value, bool preferString) {
    if (!isObjectLikeForStringBuiltin(value)) {
        return value;
    }

    auto* interpreter = getGlobalInterpreter();
    auto getPropertyForCoercion = [&](const Value& obj, const std::string& key) -> std::optional<Value> {
        if (!interpreter) return std::nullopt;
        auto [found, val] = interpreter->getPropertyForExternal(obj, key);
        if (found) return val;
        return std::nullopt;
    };

    auto throwInterpreterError = [&](Interpreter* interp) {
        Value err = interp->getError();
        interp->clearError();
        throw JsValueException(err);
    };

    // Step 2: Check Symbol.toPrimitive
    const std::string& toPrimKey = WellKnownSymbols::toPrimitiveKey();
    auto toPrim = getPropertyForCoercion(value, toPrimKey);
    if (toPrim.has_value() && !toPrim->isUndefined() && !toPrim->isNull()) {
        if (!toPrim->isFunction()) {
            throw std::runtime_error("TypeError: @@toPrimitive is not callable");
        }
        std::string hint = preferString ? "string" : "number";
        Value result = interpreter->callForHarness(*toPrim, {Value(hint)}, value);
        if (interpreter->hasError()) {
            throwInterpreterError(interpreter);
        }
        if (!isObjectLikeForStringBuiltin(result)) {
            return result;
        }
        throw std::runtime_error("TypeError: @@toPrimitive must return a primitive value");
    }

    // Special case for primitive wrapper objects (e.g., new String("abc"))
    if (value.isObject()) {
        auto obj = value.getGC<Object>();
        auto primitiveIt = obj->properties.find("__primitive_value__");
        if (primitiveIt != obj->properties.end() && !isObjectLikeForStringBuiltin(primitiveIt->second)) {
            return primitiveIt->second;
        }
    }

    // Step 3: OrdinaryToPrimitive - try toString/valueOf in preferred order
    const char* firstMethod = preferString ? "toString" : "valueOf";
    const char* secondMethod = preferString ? "valueOf" : "toString";
    for (const char* methodName : {firstMethod, secondMethod}) {
        auto method = getPropertyForCoercion(value, methodName);
        if (!method.has_value()) {
            continue;
        }
        if (!method->isFunction()) {
            continue;
        }
        if (!interpreter) {
            break;
        }
        Value primitive = interpreter->callForHarness(*method, {}, value);
        if (interpreter->hasError()) {
            throwInterpreterError(interpreter);
        }
        if (!isObjectLikeForStringBuiltin(primitive)) {
            return primitive;
        }
    }

    throw std::runtime_error("TypeError: Cannot convert object to primitive value");
}

std::string requireStringCoercibleThis(const std::vector<Value>& args, const char* methodName) {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
        throw std::runtime_error(std::string("TypeError: String.prototype.") + methodName +
                                 " called on null or undefined");
    }

    Value primitive = toPrimitiveForStringBuiltinImpl(args[0], true);
    if (primitive.isSymbol()) {
        throw std::runtime_error(std::string("TypeError: Cannot convert Symbol to string"));
    }
    return primitive.toString();
}

std::string utf16CodeUnitStringAt(const std::string& str, size_t targetIndex) {
    uint16_t codeUnit = 0;
    if (!unicode::utf16CodeUnitAt(str, targetIndex, codeUnit)) {
        return "";
    }
    return unicode::encodeUTF8(codeUnit);
}

} // namespace

double toIntegerForStringBuiltinArg(const Value& value) {
    Value primitive = toPrimitiveForStringBuiltinImpl(value, false);
    if (primitive.isSymbol() || primitive.isBigInt()) {
        throw std::runtime_error("TypeError: Cannot convert Symbol to number");
    }
    double num = primitive.toNumber();
    if (std::isnan(num) || num == 0.0) return 0.0;
    if (!std::isfinite(num)) return num;
    return std::trunc(num);
}

double toNumberForStringBuiltinArg(const Value& value) {
    Value primitive = toPrimitiveForStringBuiltinImpl(value, false);
    if (primitive.isSymbol() || primitive.isBigInt()) {
        throw std::runtime_error("TypeError: Cannot convert value to number");
    }
    return primitive.toNumber();
}

std::string toStringForStringBuiltinArg(const Value& value) {
    Value primitive = toPrimitiveForStringBuiltinImpl(value, true);
    if (primitive.isSymbol()) {
        throw std::runtime_error("TypeError: Cannot convert Symbol to string");
    }
    return primitive.toString();
}

Value toPrimitiveForStringBuiltin(const Value& value, bool preferString) {
    return toPrimitiveForStringBuiltinImpl(value, preferString);
}

size_t String_utf16Length(const std::string& str) {
    return unicode::utf16Length(str);
}

std::string String_utf16CodeUnitStringAt(const std::string& str, size_t targetIndex) {
    return utf16CodeUnitStringAt(str, targetIndex);
}

// String.prototype.charAt (UTF-16 code unit based)
Value String_charAt(const std::vector<Value>& args) {
    std::string str = requireStringCoercibleThis(args, "charAt");
    int index = 0;
    if (args.size() > 1 && !args[1].isUndefined()) {
        index = static_cast<int>(toIntegerForStringBuiltinArg(args[1]));
    }

    if (index < 0) {
        return Value(std::string(""));
    }

    std::string codeUnitString = utf16CodeUnitStringAt(str, static_cast<size_t>(index));
    if (codeUnitString.empty()) {
        return Value(std::string(""));
    }
    return Value(codeUnitString);
}

// String.prototype.charCodeAt (UTF-16 code unit based)
Value String_charCodeAt(const std::vector<Value>& args) {
    std::string str = requireStringCoercibleThis(args, "charCodeAt");
    int index = 0;
    if (args.size() > 1 && !args[1].isUndefined()) {
        index = static_cast<int>(toIntegerForStringBuiltinArg(args[1]));
    }

    uint16_t codeUnit = 0;
    if (index < 0 || !unicode::utf16CodeUnitAt(str, static_cast<size_t>(index), codeUnit)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    return Value(static_cast<double>(codeUnit));
}

// String.prototype.codePointAt
Value String_codePointAt(const std::vector<Value>& args) {
    std::string str = requireStringCoercibleThis(args, "codePointAt");
    int index = 0;
    if (args.size() > 1 && !args[1].isUndefined()) {
        index = static_cast<int>(toIntegerForStringBuiltinArg(args[1]));
    }

    if (index < 0 || static_cast<size_t>(index) >= unicode::utf16Length(str)) {
        return Value(Undefined{});
    }

    size_t byteIndex = 0;
    size_t utf16Index = 0;
    while (byteIndex < str.length()) {
        uint32_t cp = unicode::decodeUTF8(str, byteIndex);
        if (utf16Index == static_cast<size_t>(index)) {
            return Value(static_cast<double>(cp));
        }
        
        if (cp > 0xFFFF) {
            if (utf16Index + 1 == static_cast<size_t>(index)) {
                // Return the low surrogate's code unit
                return Value(static_cast<double>(0xDC00 + ((cp - 0x10000) & 0x3FF)));
            }
            utf16Index += 2;
        } else {
            utf16Index += 1;
        }
        if (utf16Index > static_cast<size_t>(index)) break;
    }

    return Value(Undefined{});
}

// String.prototype.at
Value String_at(const std::vector<Value>& args) {
    std::string str = requireStringCoercibleThis(args, "at");
    int len = static_cast<int>(unicode::utf16Length(str));
    int index = 0;
    if (args.size() > 1 && !args[1].isUndefined()) {
        index = static_cast<int>(toIntegerForStringBuiltinArg(args[1]));
    }

    if (index < 0) index = len + index;
    if (index < 0 || index >= len) return Value(Undefined{});

    std::string codeUnitString = utf16CodeUnitStringAt(str, static_cast<size_t>(index));
    if (codeUnitString.empty()) return Value(Undefined{});
    return Value(codeUnitString);
}

// String.prototype.indexOf
Value String_indexOf(const std::vector<Value>& args) {
    std::string str = requireStringCoercibleThis(args, "indexOf");
    std::string searchStr = (args.size() > 1) ? toStringForStringBuiltinArg(args[1]) : "undefined";
    
    int len = static_cast<int>(unicode::utf16Length(str));
    int start = 0;
    if (args.size() > 2 && !args[2].isUndefined()) {
        start = static_cast<int>(toIntegerForStringBuiltinArg(args[2]));
    }
    
    int pos = std::max(0, std::min(start, len));
    
    size_t bytePos = 0;
    size_t currentUtf16 = 0;
    while (bytePos < str.size() && currentUtf16 < static_cast<size_t>(pos)) {
        size_t nextBytePos = bytePos;
        uint32_t cp = unicode::decodeUTF8(str, nextBytePos);
        bytePos = nextBytePos;
        currentUtf16 += (cp > 0xFFFF) ? 2 : 1;
    }
    
    size_t foundBytePos = str.find(searchStr, bytePos);
    if (foundBytePos == std::string::npos) return Value(-1.0);
    
    size_t resultUtf16 = 0;
    size_t i = 0;
    while (i < foundBytePos && i < str.size()) {
        uint32_t cp = unicode::decodeUTF8(str, i);
        resultUtf16 += (cp > 0xFFFF) ? 2 : 1;
    }
    return Value(static_cast<double>(resultUtf16));
}

// String.prototype.lastIndexOf
Value String_lastIndexOf(const std::vector<Value>& args) {
    std::string str = requireStringCoercibleThis(args, "lastIndexOf");
    std::string searchStr = (args.size() > 1) ? toStringForStringBuiltinArg(args[1]) : "undefined";
    
    int len = static_cast<int>(unicode::utf16Length(str));
    int start = len;
    if (args.size() > 2 && !args[2].isUndefined()) {
        double num = toNumberForStringBuiltinArg(args[2]);
        if (std::isnan(num)) start = len;
        else start = static_cast<int>(toIntegerForStringBuiltinArg(args[2]));
    }
    
    int pos = std::max(0, std::min(start, len));
    
    size_t byteLimit = 0;
    size_t currentUtf16 = 0;
    size_t i = 0;
    while (i < str.size() && currentUtf16 <= static_cast<size_t>(pos)) {
        byteLimit = i;
        uint32_t cp = unicode::decodeUTF8(str, i);
        currentUtf16 += (cp > 0xFFFF) ? 2 : 1;
    }
    if (currentUtf16 <= static_cast<size_t>(pos)) byteLimit = str.size();

    size_t foundBytePos = str.rfind(searchStr, byteLimit);
    if (foundBytePos == std::string::npos) return Value(-1.0);
    
    size_t resultUtf16 = 0;
    i = 0;
    while (i < foundBytePos && i < str.size()) {
        uint32_t cp = unicode::decodeUTF8(str, i);
        resultUtf16 += (cp > 0xFFFF) ? 2 : 1;
    }
    return Value(static_cast<double>(resultUtf16));
}

// String.prototype.substring
Value String_substring(const std::vector<Value>& args) {
    std::string str = requireStringCoercibleThis(args, "substring");
    int len = static_cast<int>(unicode::utf16Length(str));
    auto clampSubstringIndex = [len](const Value& value) -> int {
        double number = toNumberForStringBuiltinArg(value);
        if (std::isnan(number) || number <= 0.0) return 0;
        if (!std::isfinite(number) || number >= static_cast<double>(len)) return len;
        return static_cast<int>(std::trunc(number));
    };

    int start = 0;
    if (args.size() > 1 && !args[1].isUndefined()) {
        start = clampSubstringIndex(args[1]);
    }
    int end = len;
    if (args.size() > 2 && !args[2].isUndefined()) {
        end = clampSubstringIndex(args[2]);
    }

    if (start > end) std::swap(start, end);
    
    return Value(unicode::utf16Slice(str, start, end));
}

// String.prototype.substr
Value String_substr(const std::vector<Value>& args) {
    std::string str = requireStringCoercibleThis(args, "substr");
    int len = static_cast<int>(unicode::utf16Length(str));
    int start = 0;
    if (args.size() > 1 && !args[1].isUndefined()) {
        start = static_cast<int>(toIntegerForStringBuiltinArg(args[1]));
    }
    if (start < 0) start = std::max(0, len + start);
    
    int length = len - start;
    if (args.size() > 2 && !args[2].isUndefined()) {
        length = static_cast<int>(toIntegerForStringBuiltinArg(args[2]));
    }
    if (length <= 0) return Value(std::string(""));
    
    return Value(unicode::utf16Slice(str, start, start + length));
}

// String.prototype.slice
Value String_slice(const std::vector<Value>& args) {
    std::string str = requireStringCoercibleThis(args, "slice");
    int len = static_cast<int>(unicode::utf16Length(str));
    auto clampSliceIndex = [len](double index) -> int {
        if (std::isnan(index)) return 0;
        if (!std::isfinite(index)) return index < 0 ? 0 : len;
        int integer = static_cast<int>(std::trunc(index));
        if (integer < 0) return std::max(len + integer, 0);
        return std::min(integer, len);
    };

    int start = 0;
    if (args.size() > 1 && !args[1].isUndefined()) {
        start = clampSliceIndex(toIntegerForStringBuiltinArg(args[1]));
    }
    int end = len;
    if (args.size() > 2 && !args[2].isUndefined()) {
        end = clampSliceIndex(toIntegerForStringBuiltinArg(args[2]));
    }

    if (end < start) {
        end = start;
    }
    return Value(unicode::utf16Slice(str, start, end));
}
// String.prototype.toLowerCase
Value String_toLowerCase(const std::vector<Value>& args) {
    std::string str = requireStringCoercibleThis(args, "toLowerCase");
    return Value(unicode::toLower(str));
}

// String.prototype.toUpperCase
Value String_toUpperCase(const std::vector<Value>& args) {
    std::string str = requireStringCoercibleThis(args, "toUpperCase");
    return Value(unicode::toUpper(str));
}


// String.prototype.trim
Value String_trim(const std::vector<Value>& args) {
    std::string str = requireStringCoercibleThis(args, "trim");
    return Value(stripESWhitespace(str));
}

// String.prototype.split
Value String_split(const std::vector<Value>& args) {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
        throw std::runtime_error("TypeError: String.prototype.split called on null or undefined");
    }

    if (args.size() >= 2 && !args[1].isUndefined() && !args[1].isNull() &&
        isObjectLikeForStringBuiltin(args[1])) {
        auto* interp = getGlobalInterpreter();
        if (interp) {
            const std::string& splitKey = WellKnownSymbols::splitKey();
            auto [found, splitter] = interp->getPropertyForExternal(args[1], splitKey);
            if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
            if (found && !splitter.isUndefined() && !splitter.isNull()) {
                if (!splitter.isFunction()) throw std::runtime_error("TypeError: @@split is not a function");
                Value limitVal = args.size() > 2 ? args[2] : Value(Undefined{});
                Value result = interp->callForHarness(splitter, {args[0], limitVal}, args[1]);
                if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
                return result;
            }
        }
    }

    std::string str = requireStringCoercibleThis(args, "split");
    auto result = makeArrayWithPrototype();
    uint32_t limit = 0xFFFFFFFF;
    if (args.size() > 2 && !args[2].isUndefined()) {
        double lim = toNumberForStringBuiltinArg(args[2]);
        if (std::isnan(lim) || lim == 0.0) limit = 0;
        else if (!std::isfinite(lim)) limit = (lim > 0) ? 0xFFFFFFFF : 0;
        else {
            double integer = std::trunc(lim);
            double mod = std::fmod(integer, 4294967296.0);
            if (mod < 0) mod += 4294967296.0;
            limit = static_cast<uint32_t>(mod);
        }
    }

    if (args.size() < 2 || args[1].isUndefined()) {
        if (limit == 0) return Value(result);
        appendSplitResult(result, limit, Value(str));
        return Value(result);
    }

    std::string separator = toStringForStringBuiltinArg(args[1]);
    if (limit == 0) return Value(result);
    if (separator.empty()) {
        size_t len = unicode::utf16Length(str);
        for (size_t i = 0; i < len && result->elements.size() < limit; ++i) {
            uint16_t unit;
            if (unicode::utf16CodeUnitAt(str, i, unit)) {
                appendSplitResult(result, limit, Value(unicode::encodeUTF8(unit)));
            }
        }
        return Value(result);
    }

    size_t start = 0;
    size_t pos;
    while ((pos = str.find(separator, start)) != std::string::npos) {
        if (result->elements.size() >= limit) break;
        appendSplitResult(result, limit, Value(str.substr(start, pos - start)));
        start = pos + separator.length();
    }
    if (result->elements.size() < limit) {
        appendSplitResult(result, limit, Value(str.substr(start)));
    }
    return Value(result);
}

// String.prototype.replace
Value String_replace(const std::vector<Value>& args) {
    std::string str = requireStringCoercibleThis(args, "replace");
    
    if (args.size() >= 2 && !args[1].isUndefined() && !args[1].isNull() && 
        (args[1].isObject() || args[1].isProxy() || args[1].isRegex() || args[1].isFunction())) {
        auto* interp = getGlobalInterpreter();
        if (interp) {
            const std::string& replaceKey = WellKnownSymbols::replaceKey();
            auto [found, replacer] = interp->getPropertyForExternal(args[1], replaceKey);
            if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
            if (found && !replacer.isUndefined() && !replacer.isNull()) {
                if (!replacer.isFunction()) throw std::runtime_error("TypeError: @@replace is not a function");
                Value replaceValue = args.size() > 2 ? args[2] : Value(Undefined{});
                Value result = interp->callForHarness(replacer, {Value(str), replaceValue}, args[1]);
                if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
                return result;
            }
        }
    }

    if (args.size() < 2) return Value(str);
    std::string searchStr = toStringForStringBuiltinArg(args[1]);
    bool isFnReplacer = args.size() > 2 && args[2].isFunction();
    std::string replaceTemplate = (args.size() > 2 && !isFnReplacer)
        ? toStringForStringBuiltinArg(args[2])
        : "undefined";
    
    size_t pos = str.find(searchStr);
    if (pos == std::string::npos) return Value(str);

    if (isFnReplacer) {
        auto* interp = getGlobalInterpreter();
        if (!interp) {
            throw std::runtime_error("TypeError: Interpreter unavailable for callable replacement");
        }
        std::vector<Value> cbArgs;
        cbArgs.push_back(Value(searchStr));
        cbArgs.push_back(Value(static_cast<double>(pos)));
        cbArgs.push_back(Value(str));
        Value replacement = interp->callForHarness(args[2], cbArgs, Value(Undefined{}));
        if (interp->hasError()) {
            Value err = interp->getError();
            interp->clearError();
            throw JsValueException(err);
        }
        std::string result = str;
        result.replace(pos, searchStr.length(), replacement.toString());
        return Value(result);
    }

    std::string replaceStr;
    for (size_t i = 0; i < replaceTemplate.size(); i++) {
        if (replaceTemplate[i] == '$' && i + 1 < replaceTemplate.size()) {
            char next = replaceTemplate[i + 1];
            if (next == '$') { replaceStr += '$'; i++; }
            else if (next == '&') { replaceStr += searchStr; i++; }
            else if (next == '`') { replaceStr += str.substr(0, pos); i++; }
            else if (next == '\'') { replaceStr += str.substr(pos + searchStr.length()); i++; }
            else { replaceStr += '$'; }
        } else {
            replaceStr += replaceTemplate[i];
        }
    }

    std::string result = str;
    result.replace(pos, searchStr.length(), replaceStr);
    return Value(result);
}

// String.prototype.replaceAll
Value String_replaceAll(const std::vector<Value>& args) {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
        throw std::runtime_error("TypeError: String.prototype.replaceAll called on null or undefined");
    }

    if (args.size() > 1 && !args[1].isUndefined() && !args[1].isNull() &&
        isObjectLikeForStringBuiltin(args[1])) {
        auto* interp = getGlobalInterpreter();
        if (interp) {
            const std::string& matchKey = WellKnownSymbols::matchKey();
            auto [hasMatch, matchProp] = interp->getPropertyForExternal(args[1], matchKey);
            if (interp->hasError()) {
                Value err = interp->getError();
                interp->clearError();
                throw JsValueException(err);
            }
            bool isRegExp = args[1].isRegex() ||
                            (hasMatch && !matchProp.isUndefined() && !matchProp.isNull());
            if (isRegExp) {
                auto [hasFlags, flagsVal] = interp->getPropertyForExternal(args[1], "flags");
                if (interp->hasError()) {
                    Value err = interp->getError();
                    interp->clearError();
                    throw JsValueException(err);
                }
                std::string flags = hasFlags ? toStringForStringBuiltinArg(flagsVal) : "";
                if (flags.find('g') == std::string::npos) {
                    throw std::runtime_error("TypeError: String.prototype.replaceAll called with a non-global RegExp argument");
                }
            }

            const std::string& replaceKey = WellKnownSymbols::replaceKey();
            auto [found, replacer] = interp->getPropertyForExternal(args[1], replaceKey);
            if (interp->hasError()) {
                Value err = interp->getError();
                interp->clearError();
                throw JsValueException(err);
            }
            if (found && !replacer.isUndefined() && !replacer.isNull()) {
                if (!replacer.isFunction()) throw std::runtime_error("TypeError: @@replace is not a function");
                Value replaceValue = args.size() > 2 ? args[2] : Value(Undefined{});
                Value result = interp->callForHarness(replacer, {args[0], replaceValue}, args[1]);
                if (interp->hasError()) {
                    Value err = interp->getError();
                    interp->clearError();
                    throw JsValueException(err);
                }
                return result;
            }
        }
    }

    std::string str = toStringForStringBuiltinArg(args[0]);
    if (args.size() < 2) return Value(str);

    std::string searchStr = toStringForStringBuiltinArg(args[1]);
    bool isFnReplacer = args.size() > 2 && args[2].isFunction();
    std::string replaceTemplate = (!isFnReplacer && args.size() > 2)
        ? toStringForStringBuiltinArg(args[2])
        : "undefined";

    auto utf16IndexForByteOffset = [&str](size_t byteOffset) -> double {
        size_t cursor = 0;
        size_t utf16Units = 0;
        while (cursor < byteOffset && cursor < str.size()) {
            uint32_t cp = unicode::decodeUTF8(str, cursor);
            utf16Units += (cp > 0xFFFF) ? 2 : 1;
        }
        return static_cast<double>(utf16Units);
    };

    auto byteOffsetForUtf16Index = [&str](size_t targetIndex) -> size_t {
        size_t cursor = 0;
        size_t utf16Units = 0;
        while (cursor < str.size() && utf16Units < targetIndex) {
            uint32_t cp = unicode::decodeUTF8(str, cursor);
            utf16Units += (cp > 0xFFFF) ? 2 : 1;
        }
        return cursor;
    };

    struct MatchPosition {
        size_t byteStart;
        size_t byteEnd;
        double utf16Index;
    };

    std::vector<MatchPosition> positions;
    if (searchStr.empty()) {
        size_t len = unicode::utf16Length(str);
        for (size_t i = 0; i <= len; ++i) {
            size_t byteOffset = byteOffsetForUtf16Index(i);
            positions.push_back({byteOffset, byteOffset, static_cast<double>(i)});
        }
    } else {
        size_t pos = 0;
        while (true) {
            size_t found = str.find(searchStr, pos);
            if (found == std::string::npos) break;
            positions.push_back({found, found + searchStr.size(), utf16IndexForByteOffset(found)});
            pos = found + searchStr.size();
        }
    }

    if (positions.empty()) return Value(str);

    auto substitutionForTemplate = [&](const MatchPosition& match) -> std::string {
        std::string replaceStr;
        for (size_t i = 0; i < replaceTemplate.size(); ++i) {
            if (replaceTemplate[i] == '$' && i + 1 < replaceTemplate.size()) {
                char next = replaceTemplate[i + 1];
                if (next == '$') { replaceStr += '$'; i++; }
                else if (next == '&') { replaceStr += searchStr; i++; }
                else if (next == '`') { replaceStr += str.substr(0, match.byteStart); i++; }
                else if (next == '\'') { replaceStr += str.substr(match.byteEnd); i++; }
                else { replaceStr += '$'; }
            } else {
                replaceStr += replaceTemplate[i];
            }
        }
        return replaceStr;
    };

    std::string result;
    size_t lastByteEnd = 0;
    auto* interp = getGlobalInterpreter();
    for (const auto& match : positions) {
        result += str.substr(lastByteEnd, match.byteStart - lastByteEnd);
        if (isFnReplacer) {
            if (!interp) {
                throw std::runtime_error("TypeError: Interpreter unavailable for callable replacement");
            }
            Value replacement = interp->callForHarness(args[2], {
                Value(searchStr),
                Value(match.utf16Index),
                Value(str)
            }, Value(Undefined{}));
            if (interp->hasError()) {
                Value err = interp->getError();
                interp->clearError();
                throw JsValueException(err);
            }
            result += toStringForStringBuiltinArg(replacement);
        } else {
            result += substitutionForTemplate(match);
        }
        lastByteEnd = match.byteEnd;
    }
    result += str.substr(lastByteEnd);
    return Value(result);
}

// String.prototype.search
Value String_search(const std::vector<Value>& args) {
    std::string str = requireStringCoercibleThis(args, "search");

    if (args.size() > 1 && !args[1].isUndefined() && !args[1].isNull() &&
        (args[1].isObject() || args[1].isProxy() || args[1].isRegex() || args[1].isFunction())) {
        auto* interp = getGlobalInterpreter();
        if (interp) {
            const std::string& searchKey = WellKnownSymbols::searchKey();
            auto [found, searcher] = interp->getPropertyForExternal(args[1], searchKey);
            if (interp->hasError()) {
                Value err = interp->getError();
                interp->clearError();
                throw JsValueException(err);
            }
            if (found && !searcher.isUndefined() && !searcher.isNull()) {
                if (!searcher.isFunction()) throw std::runtime_error("TypeError: @@search is not a function");
                Value result = interp->callForHarness(searcher, {Value(str)}, args[1]);
                if (interp->hasError()) {
                    Value err = interp->getError();
                    interp->clearError();
                    throw JsValueException(err);
                }
                return result;
            }
        }
    }

    auto* interp = getGlobalInterpreter();
    if (interp) {
        auto regexpCtor = interp->resolveVariable("RegExp");
        if (regexpCtor.has_value()) {
            std::vector<Value> ctorArgs;
            if (args.size() > 1 && !args[1].isUndefined()) {
                if (args[1].isRegex()) {
                    ctorArgs.push_back(args[1]);
                } else {
                    ctorArgs.push_back(Value(toStringForStringBuiltinArg(args[1])));
                }
            }
            Value rx = interp->constructFromNative(*regexpCtor, ctorArgs);
            if (interp->hasError()) {
                Value err = interp->getError();
                interp->clearError();
                throw JsValueException(err);
            }
            const std::string& searchKey = WellKnownSymbols::searchKey();
            auto [found, searcher] = interp->getPropertyForExternal(rx, searchKey);
            if (interp->hasError()) {
                Value err = interp->getError();
                interp->clearError();
                throw JsValueException(err);
            }
            if (found && searcher.isFunction()) {
                Value result = interp->callForHarness(searcher, {Value(str)}, rx);
                if (interp->hasError()) {
                    Value err = interp->getError();
                    interp->clearError();
                    throw JsValueException(err);
                }
                return result;
            }
        }
    }

    std::string searchStr;
    if (args.size() <= 1 || args[1].isUndefined()) {
        searchStr = "";
    } else {
        searchStr = toStringForStringBuiltinArg(args[1]);
    }
    auto pos = str.find(searchStr);
    return Value(pos != std::string::npos ? static_cast<double>(pos) : -1.0);
}

// String.prototype.match
Value String_match(const std::vector<Value>& args) {
    std::string str = requireStringCoercibleThis(args, "match");
    auto* interp = getGlobalInterpreter();

    auto getMatchMethod = [&](const Value& target) -> std::pair<bool, Value> {
        if (!interp) {
            return {false, Value(Undefined{})};
        }

        const std::string& matchKey = WellKnownSymbols::matchKey();
        auto [found, matcher] = interp->getPropertyForExternal(target, matchKey);
        if (interp->hasError()) {
            Value err = interp->getError();
            interp->clearError();
            throw JsValueException(err);
        }
        if (found) {
            return {true, matcher};
        }

        if (!target.isRegex()) {
            return {false, Value(Undefined{})};
        }

        auto regexpCtor = interp->resolveVariable("RegExp");
        if (!regexpCtor.has_value()) {
            return {false, Value(Undefined{})};
        }
        auto [hasPrototype, prototype] = interp->getPropertyForExternal(*regexpCtor, "prototype");
        if (interp->hasError()) {
            Value err = interp->getError();
            interp->clearError();
            throw JsValueException(err);
        }
        if (!hasPrototype) {
            return {false, Value(Undefined{})};
        }
        auto [hasMatcher, protoMatcher] = interp->getPropertyForExternal(prototype, matchKey);
        if (interp->hasError()) {
            Value err = interp->getError();
            interp->clearError();
            throw JsValueException(err);
        }
        if (!hasMatcher) {
            return {false, Value(Undefined{})};
        }
        return {true, protoMatcher};
    };

    if (args.size() > 1 && !args[1].isUndefined() && !args[1].isNull() &&
        isObjectLikeForStringBuiltin(args[1])) {
        if (interp) {
            auto [found, matcher] = getMatchMethod(args[1]);
            if (found && !matcher.isUndefined() && !matcher.isNull()) {
                if (!matcher.isFunction()) {
                    throw std::runtime_error("TypeError: @@match is not a function");
                }
                Value result = interp->callForHarness(matcher, {Value(str)}, args[1]);
                if (interp->hasError()) {
                    Value err = interp->getError();
                    interp->clearError();
                    throw JsValueException(err);
                }
                return result;
            }
        }
    }

    if (interp) {
        auto regexpCtor = interp->resolveVariable("RegExp");
        if (regexpCtor.has_value()) {
            std::vector<Value> ctorArgs;
            if (args.size() > 1 && !args[1].isUndefined()) {
                ctorArgs.push_back(args[1]);
            }
            Value rx = interp->constructFromNative(*regexpCtor, ctorArgs);
            if (interp->hasError()) {
                Value err = interp->getError();
                interp->clearError();
                throw JsValueException(err);
            }
            auto [found, matcher] = getMatchMethod(rx);
            if (found && !matcher.isUndefined() && !matcher.isNull()) {
                if (!matcher.isFunction()) {
                    throw std::runtime_error("TypeError: @@match is not a function");
                }
                Value result = interp->callForHarness(matcher, {Value(str)}, rx);
                if (interp->hasError()) {
                    Value err = interp->getError();
                    interp->clearError();
                    throw JsValueException(err);
                }
                return result;
            }
        }
    }

    std::string searchStr = (args.size() <= 1 || args[1].isUndefined())
        ? ""
        : toStringForStringBuiltinArg(args[1]);
    auto pos = str.find(searchStr);
    if (pos == std::string::npos) {
        return Value(Null{});
    }
    auto result = makeArrayWithPrototype();
    result->elements.push_back(Value(searchStr));
    result->properties["index"] = Value(static_cast<double>(pos));
    result->properties["input"] = Value(str);
    return Value(result);
}

// String.prototype[@@iterator]
Value String_iterator(const std::vector<Value>& args) {
    std::string str = requireStringCoercibleThis(args, "@@iterator");
    auto charArray = GarbageCollector::makeGC<Array>();
    GarbageCollector::instance().reportAllocation(sizeof(Array));
    size_t byteIndex = 0;
    while (byteIndex < str.size()) {
        size_t start = byteIndex;
        unicode::decodeUTF8(str, byteIndex);
        charArray->elements.push_back(Value(str.substr(start, byteIndex - start)));
    }
    auto iteratorObj = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    auto state = std::make_shared<size_t>(0);
    auto nextFn = GarbageCollector::makeGC<Function>();
    nextFn->isNative = true;
    nextFn->nativeFunc = [charArray, state](const std::vector<Value>&) -> Value {
        if (!charArray || *state >= charArray->elements.size()) {
            return Interpreter::makeIteratorResult(Value(Undefined{}), true);
        }
        Value value = charArray->elements[(*state)++];
        return Interpreter::makeIteratorResult(value, false);
    };
    iteratorObj->properties["next"] = Value(nextFn);
    return Value(iteratorObj);
}

// String.fromCharCode (static method)
Value String_fromCharCode(const std::vector<Value>& args) {
    std::string result;
    for (const auto& arg : args) {
        double num = toNumberForStringBuiltinArg(arg);
        uint32_t code = static_cast<uint32_t>(static_cast<int32_t>(std::fmod(num, 65536.0)));
        result += unicode::encodeUTF8(code & 0xFFFF);
    }
    return Value(unicode::normalizeUTF8(result));
}

// String.fromCodePoint (static method)
Value String_fromCodePoint(const std::vector<Value>& args) {
    std::vector<uint32_t> codePoints;
    for (const auto& arg : args) {
        double num = toNumberForStringBuiltinArg(arg);
        if (num < 0 || num > 0x10FFFF || std::isnan(num) || num != std::trunc(num)) {
            throw std::runtime_error("RangeError: Invalid code point");
        }
        codePoints.push_back(static_cast<uint32_t>(num));
    }
    return Value(unicode::normalizeUTF8(unicode::fromCodePoints(codePoints)));
}

} // namespace lightjs
