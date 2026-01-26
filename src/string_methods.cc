#include "value.h"
#include "gc.h"
#include "unicode.h"
#include <stdexcept>
#include <algorithm>
#include <sstream>
#include <regex>
#include <limits>

namespace lightjs {

// String.prototype.charAt (Unicode-aware)
Value String_charAt(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isString()) {
        throw std::runtime_error("String.charAt called on non-string");
    }

    std::string str = std::get<std::string>(args[0].data);
    int index = 0;

    if (args.size() > 1 && args[1].isNumber()) {
        index = static_cast<int>(std::get<double>(args[1].data));
    }

    if (index < 0 || index >= static_cast<int>(unicode::utf8Length(str))) {
        return Value(std::string(""));
    }

    return Value(unicode::charAt(str, index));
}

// String.prototype.charCodeAt (Unicode-aware, returns UTF-16 code unit)
Value String_charCodeAt(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isString()) {
        throw std::runtime_error("String.charCodeAt called on non-string");
    }

    std::string str = std::get<std::string>(args[0].data);
    int index = 0;

    if (args.size() > 1 && args[1].isNumber()) {
        index = static_cast<int>(std::get<double>(args[1].data));
    }

    if (index < 0 || index >= static_cast<int>(unicode::utf8Length(str))) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    uint32_t codePoint = unicode::codePointAt(str, index);
    // For BMP characters, return the code point directly
    // For non-BMP, this returns the full code point (JavaScript would use surrogate pairs)
    return Value(static_cast<double>(codePoint));
}

// String.prototype.codePointAt (full Unicode code point)
Value String_codePointAt(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isString()) {
        throw std::runtime_error("String.codePointAt called on non-string");
    }

    std::string str = std::get<std::string>(args[0].data);
    int index = 0;

    if (args.size() > 1 && args[1].isNumber()) {
        index = static_cast<int>(std::get<double>(args[1].data));
    }

    if (index < 0 || index >= static_cast<int>(unicode::utf8Length(str))) {
        return Value(Undefined{});
    }

    uint32_t codePoint = unicode::codePointAt(str, index);
    return Value(static_cast<double>(codePoint));
}

// String.prototype.indexOf
Value String_indexOf(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isString()) {
        throw std::runtime_error("String.indexOf called on non-string");
    }

    std::string str = std::get<std::string>(args[0].data);

    if (args.size() < 2) {
        return Value(-1.0);
    }

    std::string searchStr = args[1].toString();
    int fromIndex = 0;

    if (args.size() > 2 && args[2].isNumber()) {
        fromIndex = static_cast<int>(std::get<double>(args[2].data));
        fromIndex = std::max(0, fromIndex);
    }

    if (fromIndex >= static_cast<int>(str.length())) {
        return Value(-1.0);
    }

    size_t pos = str.find(searchStr, fromIndex);
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

    if (args.size() < 2) {
        return Value(-1.0);
    }

    std::string searchStr = args[1].toString();
    int fromIndex = str.length();

    if (args.size() > 2 && args[2].isNumber()) {
        fromIndex = static_cast<int>(std::get<double>(args[2].data));
        fromIndex = std::min(fromIndex, static_cast<int>(str.length()));
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
    int start = 0;
    int end = len;

    if (args.size() > 1 && args[1].isNumber()) {
        start = static_cast<int>(std::get<double>(args[1].data));
        start = std::max(0, std::min(start, len));
    }

    if (args.size() > 2 && args[2].isNumber()) {
        end = static_cast<int>(std::get<double>(args[2].data));
        end = std::max(0, std::min(end, len));
    }

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
    int length = len;

    if (args.size() > 1 && args[1].isNumber()) {
        start = static_cast<int>(std::get<double>(args[1].data));
        if (start < 0) start = std::max(0, len + start);
        if (start >= len) return Value(std::string(""));
    }

    if (args.size() > 2 && args[2].isNumber()) {
        length = static_cast<int>(std::get<double>(args[2].data));
        length = std::max(0, length);
    }

    length = std::min(length, len - start);
    return Value(str.substr(start, length));
}

// String.prototype.slice
Value String_slice(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isString()) {
        throw std::runtime_error("String.slice called on non-string");
    }

    std::string str = std::get<std::string>(args[0].data);
    int len = str.length();
    int start = 0;
    int end = len;

    if (args.size() > 1 && args[1].isNumber()) {
        start = static_cast<int>(std::get<double>(args[1].data));
        if (start < 0) start = std::max(0, len + start);
        if (start > len) start = len;
    }

    if (args.size() > 2 && args[2].isNumber()) {
        end = static_cast<int>(std::get<double>(args[2].data));
        if (end < 0) end = std::max(0, len + end);
        if (end > len) end = len;
    }

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
    auto result = std::make_shared<Array>();
    GarbageCollector::instance().reportAllocation(sizeof(Array));

    if (args.size() < 2) {
        // No separator - return array with original string
        result->elements.push_back(Value(str));
        return Value(result);
    }

    std::string separator = args[1].toString();
    int limit = -1;

    if (args.size() > 2 && args[2].isNumber()) {
        limit = static_cast<int>(std::get<double>(args[2].data));
    }

    if (separator.empty()) {
        // Split into individual characters
        for (size_t i = 0; i < str.length() && (limit < 0 || result->elements.size() < static_cast<size_t>(limit)); ++i) {
            result->elements.push_back(Value(std::string(1, str[i])));
        }
        return Value(result);
    }

    size_t pos = 0;
    size_t found = 0;

    while ((found = str.find(separator, pos)) != std::string::npos &&
           (limit < 0 || result->elements.size() < static_cast<size_t>(limit))) {
        result->elements.push_back(Value(str.substr(pos, found - pos)));
        pos = found + separator.length();
    }

    if (limit < 0 || result->elements.size() < static_cast<size_t>(limit)) {
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

    if (args.size() < 3) {
        return Value(str);
    }

    std::string searchValue = args[1].toString();
    std::string replaceValue = args[2].toString();

    size_t pos = str.find(searchValue);
    if (pos != std::string::npos) {
        str.replace(pos, searchValue.length(), replaceValue);
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

    std::string str = std::get<std::string>(args[0].data);

    // Trim from start
    str.erase(str.begin(), std::find_if(str.begin(), str.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));

    // Trim from end
    str.erase(std::find_if(str.rbegin(), str.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), str.end());

    return Value(str);
}

// String.fromCharCode (static method)
Value String_fromCharCode(const std::vector<Value>& args) {
    std::string result;
    for (const auto& arg : args) {
        uint32_t code = static_cast<uint32_t>(arg.toNumber());
        // Treat as UTF-16 code unit, but convert to UTF-8
        result += unicode::encodeUTF8(code & 0xFFFF);
    }
    return Value(result);
}

// String.fromCodePoint (static method)
Value String_fromCodePoint(const std::vector<Value>& args) {
    std::vector<uint32_t> codePoints;
    for (const auto& arg : args) {
        double num = arg.toNumber();
        if (num < 0 || num > 0x10FFFF || num != static_cast<uint32_t>(num)) {
            throw std::runtime_error("RangeError: Invalid code point");
        }
        codePoints.push_back(static_cast<uint32_t>(num));
    }
    return Value(unicode::fromCodePoints(codePoints));
}

} // namespace lightjs