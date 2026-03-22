#include "value.h"
#include "streams.h"
#include "wasm_js.h"
#include "gc.h"
#include "unicode.h"
#include "environment.h"
#include "interpreter.h"
#include "symbols.h"
#include <stdexcept>
#include <algorithm>
#include <sstream>
#include <regex>
#include <limits>
#include <cmath>

namespace lightjs {

namespace {

[[noreturn]] void throwInterpreterError(Interpreter* interpreter) {
    Value error = interpreter->getError();
    interpreter->clearError();
    throw JsValueException(error);
}

bool isObjectLikeForStringBuiltin(const Value& value) {
    return value.isObject() || value.isArray() || value.isFunction() || value.isRegex() ||
           value.isProxy() || value.isPromise() || value.isGenerator() || value.isClass() ||
           value.isMap() || value.isSet() || value.isWeakMap() || value.isWeakSet() ||
           value.isTypedArray() || value.isArrayBuffer() || value.isDataView() || value.isError();
}

std::optional<Value> getPropertyForCoercion(const Value& receiver, const std::string& key) {
    auto getFromObject = [&](const auto& objectLike) -> std::optional<Value> {
        std::string getterName = "__get_" + key;
        auto getterIt = objectLike->properties.find(getterName);
        if (getterIt != objectLike->properties.end()) {
            if (!getterIt->second.isFunction()) {
                return Value(Undefined{});
            }
            Interpreter* interpreter = getGlobalInterpreter();
            if (!interpreter) {
                return Value(Undefined{});
            }
            Value out = interpreter->callForHarness(getterIt->second, {}, receiver);
            if (interpreter->hasError()) {
                throwInterpreterError(interpreter);
            }
            return out;
        }

        auto it = objectLike->properties.find(key);
        if (it != objectLike->properties.end()) {
            return it->second;
        }

        auto protoIt = objectLike->properties.find("__proto__");
        if (protoIt != objectLike->properties.end() && isObjectLikeForStringBuiltin(protoIt->second)) {
            return getPropertyForCoercion(protoIt->second, key);
        }
        return std::nullopt;
    };

    if (receiver.isObject()) return getFromObject(receiver.getGC<Object>());
    if (receiver.isArray()) return getFromObject(receiver.getGC<Array>());
    if (receiver.isFunction()) return getFromObject(receiver.getGC<Function>());
    if (receiver.isRegex()) return getFromObject(receiver.getGC<Regex>());
    return std::nullopt;
}

Value toPrimitiveForStringBuiltin(const Value& value, bool preferString) {
    if (!isObjectLikeForStringBuiltin(value)) {
        return value;
    }

    Interpreter* interpreter = getGlobalInterpreter();

    // Step 1: Check for @@toPrimitive (Symbol.toPrimitive)
    const std::string& toPrimitiveKey = WellKnownSymbols::toPrimitiveKey();
    auto exoticPrim = getPropertyForCoercion(value, toPrimitiveKey);
    if (exoticPrim.has_value() && !exoticPrim->isUndefined() && !exoticPrim->isNull()) {
        if (!exoticPrim->isFunction()) {
            throw std::runtime_error("TypeError: @@toPrimitive is not callable");
        }
        if (!interpreter) {
            throw std::runtime_error("TypeError: Cannot convert object to primitive value");
        }
        std::string hint = preferString ? "string" : "number";
        Value result = interpreter->callForHarness(*exoticPrim, {Value(hint)}, value);
        if (interpreter->hasError()) {
            throwInterpreterError(interpreter);
        }
        if (isObjectLikeForStringBuiltin(result)) {
            throw std::runtime_error("TypeError: @@toPrimitive must return a primitive value");
        }
        return result;
    }

    // Step 2: Check for __primitive_value__ (boxed primitives like Object(0n))
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

    Value primitive = toPrimitiveForStringBuiltin(args[0], true);
    if (primitive.isSymbol()) {
        throw std::runtime_error(std::string("TypeError: Cannot convert Symbol to string"));
    }
    return primitive.toString();
}

} // end anonymous namespace

double toIntegerForStringBuiltinArg(const Value& value) {
    Value primitive = toPrimitiveForStringBuiltin(value, false);
    if (primitive.isSymbol()) {
        throw std::runtime_error("TypeError: Cannot convert a Symbol value to a number");
    }
    if (primitive.isBigInt()) {
        throw std::runtime_error("TypeError: Cannot convert a BigInt value to a number");
    }

    double number = primitive.toNumber();
    if (std::isnan(number) || number == 0.0) {
        return 0.0;
    }
    if (!std::isfinite(number)) {
        return number;
    }
    return std::trunc(number);
}

double toNumberForStringBuiltinArg(const Value& value) {
    Value primitive = toPrimitiveForStringBuiltin(value, false);
    if (primitive.isSymbol()) {
        throw std::runtime_error("TypeError: Cannot convert a Symbol value to a number");
    }
    if (primitive.isBigInt()) {
        throw std::runtime_error("TypeError: Cannot convert a BigInt value to a number");
    }
    return primitive.toNumber();
}

std::string toStringForStringBuiltinArg(const Value& value) {
    Value primitive = toPrimitiveForStringBuiltin(value, true);
    if (primitive.isSymbol()) {
        throw std::runtime_error("TypeError: Cannot convert a Symbol value to a string");
    }
    return primitive.toString();
}

namespace {

size_t utf16Length(const std::string& str) {
    size_t units = 0;
    size_t byteIndex = 0;
    while (byteIndex < str.length()) {
        uint32_t codePoint = unicode::decodeUTF8(str, byteIndex);
        units += (codePoint > 0xFFFF) ? 2 : 1;
    }
    return units;
}

bool utf16CodeUnitAt(const std::string& str, size_t targetIndex, uint16_t& outUnit) {
    size_t utf16Index = 0;
    size_t byteIndex = 0;
    while (byteIndex < str.length()) {
        uint32_t codePoint = unicode::decodeUTF8(str, byteIndex);
        if (codePoint <= 0xFFFF) {
            if (utf16Index == targetIndex) {
                outUnit = static_cast<uint16_t>(codePoint);
                return true;
            }
            utf16Index++;
            continue;
        }

        uint32_t v = codePoint - 0x10000;
        uint16_t high = static_cast<uint16_t>(0xD800 + ((v >> 10) & 0x3FF));
        uint16_t low = static_cast<uint16_t>(0xDC00 + (v & 0x3FF));
        if (utf16Index == targetIndex) {
            outUnit = high;
            return true;
        }
        utf16Index++;
        if (utf16Index == targetIndex) {
            outUnit = low;
            return true;
        }
        utf16Index++;
    }
    return false;
}

std::string utf16CodeUnitStringAt(const std::string& str, size_t targetIndex) {
    uint16_t codeUnit = 0;
    if (!utf16CodeUnitAt(str, targetIndex, codeUnit)) {
        return "";
    }
    return unicode::encodeUTF8(codeUnit);
}

} // namespace

size_t String_utf16Length(const std::string& str) {
    return utf16Length(str);
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

    if (index < 0) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    uint16_t codeUnit = 0;
    if (!utf16CodeUnitAt(str, static_cast<size_t>(index), codeUnit)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    return Value(static_cast<double>(codeUnit));
}

// String.prototype.codePointAt (full Unicode code point using UTF-16 indexing)
Value String_codePointAt(const std::vector<Value>& args) {
    std::string str = requireStringCoercibleThis(args, "codePointAt");
    int index = 0;
    if (args.size() > 1 && !args[1].isUndefined()) {
        index = static_cast<int>(toIntegerForStringBuiltinArg(args[1]));
    }

    if (index < 0) {
        return Value(Undefined{});
    }

    uint16_t first = 0;
    if (!utf16CodeUnitAt(str, static_cast<size_t>(index), first)) {
        return Value(Undefined{});
    }

    uint32_t codePoint = first;
    if (first >= 0xD800 && first <= 0xDBFF) {
        uint16_t second = 0;
        if (utf16CodeUnitAt(str, static_cast<size_t>(index + 1), second) &&
            second >= 0xDC00 && second <= 0xDFFF) {
            codePoint = 0x10000 + (((static_cast<uint32_t>(first) - 0xD800) << 10) |
                                   (static_cast<uint32_t>(second) - 0xDC00));
        }
    }
    return Value(static_cast<double>(codePoint));
}

// String.prototype.at (UTF-16 code unit based, relative indexing)
Value String_at(const std::vector<Value>& args) {
    std::string str = requireStringCoercibleThis(args, "at");
    double relativeIndex = 0.0;
    if (args.size() > 1 && !args[1].isUndefined()) {
        relativeIndex = toIntegerForStringBuiltinArg(args[1]);
    }

    int len = static_cast<int>(utf16Length(str));
    int index = static_cast<int>(relativeIndex);
    if (index < 0) {
        index = len + index;
    }
    if (index < 0 || index >= len) {
        return Value(Undefined{});
    }

    std::string codeUnitString = utf16CodeUnitStringAt(str, static_cast<size_t>(index));
    if (codeUnitString.empty()) {
        return Value(Undefined{});
    }
    return Value(codeUnitString);
}

Value String_iterator(const std::vector<Value>& args) {
    std::string str = requireStringCoercibleThis(args, "[Symbol.iterator]");

    auto iteratorObj = GarbageCollector::makeGC<Object>();
    auto byteIndex = std::make_shared<size_t>(0);
    auto nextFn = GarbageCollector::makeGC<Function>();
    nextFn->isNative = true;
    nextFn->isConstructor = false;
    nextFn->properties["__throw_on_new__"] = Value(true);
    nextFn->nativeFunc = [str, byteIndex](const std::vector<Value>&) -> Value {
        auto result = GarbageCollector::makeGC<Object>();
        if (*byteIndex >= str.size()) {
            result->properties["value"] = Value(Undefined{});
            result->properties["done"] = Value(true);
            return Value(result);
        }

        size_t start = *byteIndex;
        unicode::decodeUTF8(str, *byteIndex);
        result->properties["value"] = Value(str.substr(start, *byteIndex - start));
        result->properties["done"] = Value(false);
        return Value(result);
    };
    iteratorObj->properties["next"] = Value(nextFn);
    iteratorObj->properties[WellKnownSymbols::toStringTagKey()] = Value(std::string("String Iterator"));
    iteratorObj->properties["__non_enum_" + WellKnownSymbols::toStringTagKey()] = Value(true);
    return Value(iteratorObj);
}

// String.prototype.indexOf
Value String_indexOf(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isString()) {
        throw std::runtime_error("String.indexOf called on non-string");
    }

    std::string str = std::get<std::string>(args[0].data);

    std::string searchStr;
    if (args.size() < 2 || args[1].isUndefined()) {
        searchStr = "undefined";
    } else {
        searchStr = toStringForStringBuiltinArg(args[1]);
    }

    double fromIndex = 0;
    if (args.size() > 2 && !args[2].isUndefined()) {
        fromIndex = toIntegerForStringBuiltinArg(args[2]);
        if (fromIndex < 0) fromIndex = 0;
    }

    if (fromIndex >= static_cast<double>(str.length())) {
        if (searchStr.empty()) return Value(static_cast<double>(str.length()));
        return Value(-1.0);
    }

    size_t pos = str.find(searchStr, static_cast<size_t>(fromIndex));
    if (pos == std::string::npos) {
        return Value(-1.0);
    }

    return Value(static_cast<double>(pos));
}

// String.prototype.lastIndexOf
Value String_lastIndexOf(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isString()) {
        throw std::runtime_error("String.lastIndexOf called on non-string");
    }

    std::string str = std::get<std::string>(args[0].data);

    std::string searchStr;
    if (args.size() < 2 || args[1].isUndefined()) {
        searchStr = "undefined";
    } else {
        searchStr = toStringForStringBuiltinArg(args[1]);
    }

    double numPos = std::numeric_limits<double>::quiet_NaN();
    if (args.size() > 2 && !args[2].isUndefined()) {
        numPos = toNumberForStringBuiltinArg(args[2]);
    }

    size_t fromIndex;
    if (std::isnan(numPos)) {
        fromIndex = str.length();
    } else {
        double pos = std::trunc(numPos);
        if (pos < 0) pos = 0;
        fromIndex = static_cast<size_t>(std::min(pos, static_cast<double>(str.length())));
    }

    size_t pos = str.rfind(searchStr, fromIndex);
    if (pos == std::string::npos) {
        return Value(-1.0);
    }

    return Value(static_cast<double>(pos));
}

// String.prototype.substring
Value String_substring(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isString()) {
        throw std::runtime_error("String.substring called on non-string");
    }

    std::string str = std::get<std::string>(args[0].data);
    int len = str.length();

    double intStart = 0;
    if (args.size() > 1 && !args[1].isUndefined()) {
        intStart = toIntegerForStringBuiltinArg(args[1]);
    }

    double intEnd = len;
    if (args.size() > 2 && !args[2].isUndefined()) {
        intEnd = toIntegerForStringBuiltinArg(args[2]);
    }

    int start = static_cast<int>(std::max(0.0, std::min(intStart, static_cast<double>(len))));
    int end = static_cast<int>(std::max(0.0, std::min(intEnd, static_cast<double>(len))));

    if (start > end) {
        std::swap(start, end);
    }

    return Value(str.substr(start, end - start));
}

// String.prototype.substr
Value String_substr(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isString()) {
        throw std::runtime_error("String.substr called on non-string");
    }

    std::string str = std::get<std::string>(args[0].data);
    int len = str.length();

    int start = 0;
    if (args.size() > 1 && !args[1].isUndefined()) {
        start = static_cast<int>(toIntegerForStringBuiltinArg(args[1]));
        if (start < 0) start = std::max(0, len + start);
        if (start >= len) return Value(std::string(""));
    }

    int length = len;
    if (args.size() > 2 && !args[2].isUndefined()) {
        length = static_cast<int>(toIntegerForStringBuiltinArg(args[2]));
        length = std::max(0, length);
    }

    length = std::min(length, len - start);
    if (length <= 0) return Value(std::string(""));
    return Value(str.substr(start, length));
}

// String.prototype.slice
Value String_slice(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isString()) {
        throw std::runtime_error("String.slice called on non-string");
    }

    std::string str = std::get<std::string>(args[0].data);
    int len = str.length();

    double intStart = 0;
    if (args.size() > 1 && !args[1].isUndefined()) {
        intStart = toIntegerForStringBuiltinArg(args[1]);
    }

    double intEnd = len;
    if (args.size() > 2 && !args[2].isUndefined()) {
        intEnd = toIntegerForStringBuiltinArg(args[2]);
    }

    int start, end;
    if (intStart < 0) start = static_cast<int>(std::max(static_cast<double>(0), static_cast<double>(len) + intStart));
    else start = static_cast<int>(std::min(intStart, static_cast<double>(len)));

    if (intEnd < 0) end = static_cast<int>(std::max(static_cast<double>(0), static_cast<double>(len) + intEnd));
    else end = static_cast<int>(std::min(intEnd, static_cast<double>(len)));

    if (start >= end) {
        return Value(std::string(""));
    }

    return Value(str.substr(start, end - start));
}

// String.prototype.split
Value String_split(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isString()) {
        throw std::runtime_error("String.split called on non-string");
    }

    std::string str = std::get<std::string>(args[0].data);

    // ES2020: Check for @@split on the separator
    if (args.size() >= 2 && !args[1].isUndefined() && !args[1].isNull() && !args[1].isString() && !args[1].isBool() && !args[1].isNumber() && !args[1].isBigInt() && !args[1].isSymbol()) {
        auto* interp = getGlobalInterpreter();
        if (interp) {
            const std::string& splitKey = WellKnownSymbols::splitKey();
            auto [found, splitter] = interp->getPropertyForExternal(args[1], splitKey);
            if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
            if (found && !splitter.isUndefined() && !splitter.isNull()) {
                if (!splitter.isFunction()) throw std::runtime_error("TypeError: @@split is not a function");
                Value limitVal = args.size() > 2 ? args[2] : Value(Undefined{});
                Value result = interp->callForHarness(splitter, {Value(str), limitVal}, args[1]);
                if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
                return result;
            }
        }
    }

    auto result = makeArrayWithPrototype();

    // Handle limit parameter (ToUint32 per spec) - args[2] since args[0] is this
    uint32_t limit = 0xFFFFFFFF; // max uint32 = no limit
    if (args.size() > 2 && !args[2].isUndefined()) {
        double lim = toNumberForStringBuiltinArg(args[2]);
        if (std::isnan(lim) || lim == 0.0) {
            limit = 0;
        } else if (!std::isfinite(lim)) {
            limit = (lim > 0) ? 0xFFFFFFFF : 0;
        } else {
            double integer = std::trunc(lim);
            double mod = std::fmod(integer, 4294967296.0);
            if (mod < 0) mod += 4294967296.0;
            limit = static_cast<uint32_t>(mod);
        }
    }

    // If limit is 0, return empty array
    if (limit == 0) {
        return Value(result);
    }

    // If separator is undefined, return [str]
    if (args.size() < 2 || args[1].isUndefined()) {
        result->elements.push_back(Value(str));
        return Value(result);
    }

    std::string separator = toStringForStringBuiltinArg(args[1]);

    if (separator.empty()) {
        // Split into individual characters (UTF-8 aware)
        size_t i = 0;
        while (i < str.size() && result->elements.size() < limit) {
            unsigned char c = str[i];
            size_t charLen = 1;
            if ((c & 0x80) == 0) charLen = 1;
            else if ((c & 0xE0) == 0xC0) charLen = 2;
            else if ((c & 0xF0) == 0xE0) charLen = 3;
            else if ((c & 0xF8) == 0xF0) charLen = 4;
            result->elements.push_back(Value(str.substr(i, charLen)));
            i += charLen;
        }
        return Value(result);
    }

    size_t pos = 0;
    size_t found = 0;

    while ((found = str.find(separator, pos)) != std::string::npos &&
           result->elements.size() < limit) {
        result->elements.push_back(Value(str.substr(pos, found - pos)));
        pos = found + separator.length();
    }

    if (result->elements.size() < limit) {
        result->elements.push_back(Value(str.substr(pos)));
    }

    return Value(result);
}

// String.prototype.replace
Value String_replace(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isString()) {
        throw std::runtime_error("String.replace called on non-string");
    }

    std::string str = std::get<std::string>(args[0].data);

    // ES2020: Check for @@replace on the searchValue
    if (args.size() >= 2 && !args[1].isUndefined() && !args[1].isNull() && !args[1].isString() && !args[1].isBool() && !args[1].isNumber() && !args[1].isBigInt() && !args[1].isSymbol()) {
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

    if (args.size() < 3) {
        return Value(str);
    }

    std::string searchValue = toStringForStringBuiltinArg(args[1]);
    bool isFunctionalReplace = args[2].isFunction();

    // Step 6: If not callable, ToString(replaceValue) BEFORE searching
    std::string replaceTemplate;
    if (!isFunctionalReplace) {
        replaceTemplate = toStringForStringBuiltinArg(args[2]);
    }

    if (isFunctionalReplace) {
        size_t pos = str.find(searchValue);
        if (pos != std::string::npos) {
            auto* interp = getGlobalInterpreter();
            if (interp) {
                std::vector<Value> cbArgs;
                cbArgs.push_back(Value(searchValue));
                cbArgs.push_back(Value(static_cast<double>(pos)));
                cbArgs.push_back(Value(str));
                Value replacement = interp->callForHarness(args[2], cbArgs, Value(Undefined{}));
                if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
                str.replace(pos, searchValue.length(), replacement.toString());
            }
        }
        return Value(str);
    }

    size_t pos = str.find(searchValue);
    if (pos != std::string::npos) {
        // GetSubstitution: process $-patterns
        std::string replacement;
        for (size_t i = 0; i < replaceTemplate.size(); i++) {
            if (replaceTemplate[i] == '$' && i + 1 < replaceTemplate.size()) {
                char next = replaceTemplate[i + 1];
                if (next == '$') { replacement += '$'; i++; }
                else if (next == '&') { replacement += searchValue; i++; }
                else if (next == '`') { replacement += str.substr(0, pos); i++; }
                else if (next == '\'') { replacement += str.substr(pos + searchValue.size()); i++; }
                else { replacement += '$'; }
            } else {
                replacement += replaceTemplate[i];
            }
        }
        str.replace(pos, searchValue.length(), replacement);
    }

    return Value(str);
}

// Helper: decode UTF-8 code points from a string
static std::vector<uint32_t> decodeCodePoints(const std::string& str) {
    std::vector<uint32_t> cps;
    size_t i = 0;
    while (i < str.size()) {
        unsigned char c = str[i];
        uint32_t cp = 0;
        size_t len = 1;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        for (size_t j = 1; j < len && i + j < str.size(); j++) {
            cp = (cp << 6) | (str[i + j] & 0x3F);
        }
        cps.push_back(cp);
        i += len;
    }
    return cps;
}

// Helper: encode code point to UTF-8
static std::string encodeCP(uint32_t cp) {
    std::string result;
    if (cp < 0x80) { result += static_cast<char>(cp); }
    else if (cp < 0x800) {
        result += static_cast<char>(0xC0 | (cp >> 6));
        result += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        result += static_cast<char>(0xE0 | (cp >> 12));
        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        result += static_cast<char>(0xF0 | (cp >> 18));
        result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return result;
}

// Helper: is code point a cased letter (for final sigma)
static bool isCased(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return true;
    if (cp >= 'a' && cp <= 'z') return true;
    if (cp >= 0xC0 && cp <= 0x024F) return true; // Latin Extended
    if (cp >= 0x0370 && cp <= 0x03FF) return true; // Greek
    if (cp >= 0x0400 && cp <= 0x04FF) return true; // Cyrillic
    if (cp >= 0x0500 && cp <= 0x052F) return true; // Cyrillic Supplement
    if (cp >= 0x0531 && cp <= 0x0587) return true; // Armenian
    if (cp >= 0x10400 && cp <= 0x1044F) return true; // Deseret
    if (cp >= 0x1D400 && cp <= 0x1D7FF) return true; // Mathematical Alphanumeric Symbols
    if (cp >= 0x1F100 && cp <= 0x1F1FF) return true; // Enclosed Alphanumeric Supplement
    return false;
}

// String.prototype.toLowerCase - with Unicode SpecialCasing
Value String_toLowerCase(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isString()) {
        throw std::runtime_error("String.toLowerCase called on non-string");
    }
    auto cps = decodeCodePoints(std::get<std::string>(args[0].data));
    std::string result;
    for (size_t i = 0; i < cps.size(); i++) {
        uint32_t cp = cps[i];
        // SpecialCasing: İ (U+0130) → i + combining dot above
        if (cp == 0x0130) {
            result += encodeCP(0x0069); // i
            result += encodeCP(0x0307); // combining dot above
            continue;
        }
        // SpecialCasing: Σ (U+03A3) → ς (U+03C2) at word-final position, σ (U+03C3) otherwise
        if (cp == 0x03A3) {
            // Final sigma: preceded by cased, not followed by cased
            bool precededByCased = false;
            for (int j = static_cast<int>(i) - 1; j >= 0; j--) {
                if (isCased(cps[j])) { precededByCased = true; break; }
                uint32_t c = cps[j];
                // Case-ignorable: soft hyphen, zero-width chars, combining marks, format chars
                bool caseIgnorable = (c == 0x00AD || c == 0x200B || c == 0x200C || c == 0x200D ||
                    c == 0x180E || c == 0x00B7 || c == 0x0387 || c == 0x05F4 ||
                    (c >= 0x0300 && c <= 0x036F) || // combining diacritical marks
                    (c >= 0x0483 && c <= 0x0489) || // cyrillic combining
                    c == 0x2019 || c == 0xFE00 || c == 0xFE01);
                if (!caseIgnorable) break;
            }
            bool followedByCased = false;
            for (size_t j = i + 1; j < cps.size(); j++) {
                if (isCased(cps[j])) { followedByCased = true; break; }
                uint32_t c = cps[j];
                bool caseIgnorable = (c == 0x00AD || c == 0x200B || c == 0x200C || c == 0x200D ||
                    c == 0x180E || c == 0x00B7 || c == 0x0387 || c == 0x05F4 ||
                    (c >= 0x0300 && c <= 0x036F) || (c >= 0x0483 && c <= 0x0489) ||
                    c == 0x2019 || c == 0xFE00 || c == 0xFE01);
                if (!caseIgnorable) break;
            }
            if (precededByCased && !followedByCased) {
                result += encodeCP(0x03C2); // final sigma ς
            } else {
                result += encodeCP(0x03C3); // sigma σ
            }
            continue;
        }
        // Supplementary plane: Deseret U+10400-U+10427 → U+10428-U+1044F
        if (cp >= 0x10400 && cp <= 0x10427) {
            result += encodeCP(cp + 0x28);
            continue;
        }
        // Basic Latin/Latin-1
        if (cp < 0x80) { result += static_cast<char>(::tolower(cp)); continue; }
        if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) { result += encodeCP(cp + 0x20); continue; }
        // Greek capitals U+0391-U+03A9 (excluding U+03A2)
        if (cp >= 0x0391 && cp <= 0x03A1) { result += encodeCP(cp + 0x20); continue; }
        if (cp >= 0x03A3 && cp <= 0x03A9) { result += encodeCP(cp + 0x20); continue; }
        // Cyrillic U+0410-U+042F → U+0430-U+044F
        if (cp >= 0x0410 && cp <= 0x042F) { result += encodeCP(cp + 0x20); continue; }
        // Fallback: keep as-is
        result += encodeCP(cp);
    }
    return Value(result);
}

// String.prototype.toUpperCase - with Unicode SpecialCasing
Value String_toUpperCase(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isString()) {
        throw std::runtime_error("String.toUpperCase called on non-string");
    }
    auto cps = decodeCodePoints(std::get<std::string>(args[0].data));
    std::string result;
    for (size_t i = 0; i < cps.size(); i++) {
        uint32_t cp = cps[i];
        // SpecialCasing: ß (U+00DF) → SS
        if (cp == 0x00DF) { result += "SS"; continue; }
        // Latin ligatures
        if (cp == 0xFB00) { result += "FF"; continue; }
        if (cp == 0xFB01) { result += "FI"; continue; }
        if (cp == 0xFB02) { result += "FL"; continue; }
        if (cp == 0xFB03) { result += "FFI"; continue; }
        if (cp == 0xFB04) { result += "FFL"; continue; }
        if (cp == 0xFB05) { result += "ST"; continue; }
        if (cp == 0xFB06) { result += "ST"; continue; }
        // Armenian ligatures
        if (cp == 0x0587) { result += encodeCP(0x0535); result += encodeCP(0x0552); continue; }
        if (cp == 0xFB13) { result += encodeCP(0x0544); result += encodeCP(0x0546); continue; }
        if (cp == 0xFB14) { result += encodeCP(0x0544); result += encodeCP(0x0535); continue; }
        if (cp == 0xFB15) { result += encodeCP(0x0544); result += encodeCP(0x053B); continue; }
        if (cp == 0xFB16) { result += encodeCP(0x054E); result += encodeCP(0x0546); continue; }
        if (cp == 0xFB17) { result += encodeCP(0x0544); result += encodeCP(0x053D); continue; }
        // İ stays as İ in uppercase
        if (cp == 0x0130) { result += encodeCP(0x0130); continue; }
        // ʼn (U+0149) → ʼN
        if (cp == 0x0149) { result += encodeCP(0x02BC); result += encodeCP(0x004E); continue; }
        // ǰ (U+01F0) → J + combining caron
        if (cp == 0x01F0) { result += encodeCP(0x004A); result += encodeCP(0x030C); continue; }
        // Greek with iota subscript → uppercase + IOTA
        if (cp == 0x1F80) { result += encodeCP(0x1F08); result += encodeCP(0x0399); continue; }
        if (cp == 0x1F81) { result += encodeCP(0x1F09); result += encodeCP(0x0399); continue; }
        if (cp == 0x1F82) { result += encodeCP(0x1F0A); result += encodeCP(0x0399); continue; }
        if (cp == 0x1F83) { result += encodeCP(0x1F0B); result += encodeCP(0x0399); continue; }
        if (cp == 0x1F84) { result += encodeCP(0x1F0C); result += encodeCP(0x0399); continue; }
        if (cp == 0x1F85) { result += encodeCP(0x1F0D); result += encodeCP(0x0399); continue; }
        if (cp == 0x1F86) { result += encodeCP(0x1F0E); result += encodeCP(0x0399); continue; }
        if (cp == 0x1F87) { result += encodeCP(0x1F0F); result += encodeCP(0x0399); continue; }
        if (cp == 0x1F88) { result += encodeCP(0x1F08); result += encodeCP(0x0399); continue; }
        if (cp == 0x1F89) { result += encodeCP(0x1F09); result += encodeCP(0x0399); continue; }
        if (cp == 0x1F8A) { result += encodeCP(0x1F0A); result += encodeCP(0x0399); continue; }
        if (cp == 0x1F8B) { result += encodeCP(0x1F0B); result += encodeCP(0x0399); continue; }
        if (cp == 0x1F8C) { result += encodeCP(0x1F0C); result += encodeCP(0x0399); continue; }
        if (cp == 0x1F8D) { result += encodeCP(0x1F0D); result += encodeCP(0x0399); continue; }
        if (cp == 0x1F8E) { result += encodeCP(0x1F0E); result += encodeCP(0x0399); continue; }
        if (cp == 0x1F8F) { result += encodeCP(0x1F0F); result += encodeCP(0x0399); continue; }
        // ᾳ ῃ ῳ → Α+Ι, Η+Ι, Ω+Ι
        if (cp == 0x1FB3) { result += encodeCP(0x0391); result += encodeCP(0x0399); continue; }
        if (cp == 0x1FC3) { result += encodeCP(0x0397); result += encodeCP(0x0399); continue; }
        if (cp == 0x1FF3) { result += encodeCP(0x03A9); result += encodeCP(0x0399); continue; }
        if (cp == 0x1FBC) { result += encodeCP(0x0391); result += encodeCP(0x0399); continue; }
        if (cp == 0x1FCC) { result += encodeCP(0x0397); result += encodeCP(0x0399); continue; }
        if (cp == 0x1FFC) { result += encodeCP(0x03A9); result += encodeCP(0x0399); continue; }
        // Supplementary plane: Deseret U+10428-U+1044F → U+10400-U+10427
        if (cp >= 0x10428 && cp <= 0x1044F) {
            result += encodeCP(cp - 0x28);
            continue;
        }
        // Basic Latin
        if (cp < 0x80) { result += static_cast<char>(::toupper(cp)); continue; }
        // Latin-1 lowercase U+E0-U+FE (except U+F7) → U+C0-U+DE
        if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) { result += encodeCP(cp - 0x20); continue; }
        // Greek smalls U+03B1-U+03C1 → U+0391-U+03A1
        if (cp >= 0x03B1 && cp <= 0x03C1) { result += encodeCP(cp - 0x20); continue; }
        if (cp >= 0x03C3 && cp <= 0x03C9) { result += encodeCP(cp - 0x20); continue; }
        // ς (U+03C2 final sigma) → Σ (U+03A3)
        if (cp == 0x03C2) { result += encodeCP(0x03A3); continue; }
        // Cyrillic U+0430-U+044F → U+0410-U+042F
        if (cp >= 0x0430 && cp <= 0x044F) { result += encodeCP(cp - 0x20); continue; }
        // Fallback: keep as-is
        result += encodeCP(cp);
    }
    return Value(result);
}

// String.prototype.trim
Value String_trim(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isString()) {
        throw std::runtime_error("String.trim called on non-string");
    }

    return Value(stripESWhitespace(std::get<std::string>(args[0].data)));
}

// String.fromCharCode (static method)
Value String_fromCharCode(const std::vector<Value>& args) {
    std::string result;
    for (const auto& arg : args) {
        double num = toNumberForStringBuiltinArg(arg);
        uint32_t code = static_cast<uint32_t>(static_cast<int32_t>(std::fmod(num, 65536.0)));
        // Treat as UTF-16 code unit, but convert to UTF-8
        result += unicode::encodeUTF8(code & 0xFFFF);
    }
    return Value(result);
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
    return Value(unicode::fromCodePoints(codePoints));
}

} // namespace lightjs
