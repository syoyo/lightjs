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

// String.prototype.toLowerCase
Value String_toLowerCase(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isString()) {
        throw std::runtime_error("String.toLowerCase called on non-string");
    }

    std::string str = std::get<std::string>(args[0].data);
    std::transform(str.begin(), str.end(), str.begin(), ::tolower);
    return Value(str);
}

// String.prototype.toUpperCase
Value String_toUpperCase(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isString()) {
        throw std::runtime_error("String.toUpperCase called on non-string");
    }

    std::string str = std::get<std::string>(args[0].data);
    std::transform(str.begin(), str.end(), str.begin(), ::toupper);
    return Value(str);
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
