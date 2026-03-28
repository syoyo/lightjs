#include "json_internal.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <stdexcept>

namespace lightjs::detail {

void JSONStringifier::setGap(const std::string& g) {
    gap_ = g;
}

void JSONStringifier::setReplacer(const Value& fn) {
    replacerFn_ = fn;
}

void JSONStringifier::setPropertyList(const std::vector<std::string>& list) {
    propertyList_ = list;
    hasPropertyList_ = true;
}

const void* JSONStringifier::getPointer(const Value& v) const {
    if (v.isObject()) return v.getGC<Object>().get();
    if (v.isArray()) return v.getGC<Array>().get();
    if (v.isError()) return v.getGC<Error>().get();
    if (v.isFunction()) return v.getGC<Function>().get();
    if (v.isMap()) return v.getGC<Map>().get();
    if (v.isSet()) return v.getGC<Set>().get();
    if (v.isProxy()) return v.getGC<Proxy>().get();
    return nullptr;
}

void JSONStringifier::checkCircular(const Value& v) {
    auto ptr = getPointer(v);
    if (!ptr) return;
    for (auto p : stack_) {
        if (p == ptr) {
            throw std::runtime_error("TypeError: Converting circular structure to JSON");
        }
    }
}

Value JSONStringifier::callToJSON(const Value& value, const std::string& key) {
    auto* interp = getGlobalInterpreter();
    if (!interp) return value;
    Value toJSON = value.isBigInt() ? jsonGetMethodForBigInt(interp, value, "toJSON")
                                    : jsonGet(interp, value, "toJSON");
    if (toJSON.isFunction()) {
        Value result = interp->callForHarness(toJSON, {Value(key)}, value);
        if (interp->hasError()) {
            throwInterpreterError(interp);
        }
        return result;
    }
    return value;
}

Value JSONStringifier::applyReplacer(const Value& holder, const std::string& key, Value value) {
    if (replacerFn_.isFunction()) {
        auto* interp = getGlobalInterpreter();
        if (interp) {
            value = interp->callForHarness(replacerFn_, {Value(key), value}, holder);
            if (interp->hasError()) {
                throwInterpreterError(interp);
            }
        }
    }
    return value;
}

Value JSONStringifier::unwrapPrimitive(const Value& value) {
    if (value.isObject()) {
        auto obj = value.getGC<Object>();
        auto primIt = obj->properties.find("__primitive_value__");
        if (primIt != obj->properties.end()) {
            const auto& prim = primIt->second;
            if (prim.isNumber()) {
                auto valueOfIt = obj->properties.find("valueOf");
                if (valueOfIt != obj->properties.end() && valueOfIt->second.isFunction()) {
                    auto* interp = getGlobalInterpreter();
                    if (interp) return interp->callForHarness(valueOfIt->second, {}, value);
                }
                return prim;
            } else if (prim.isString()) {
                auto toStringIt = obj->properties.find("toString");
                if (toStringIt != obj->properties.end() && toStringIt->second.isFunction()) {
                    auto* interp = getGlobalInterpreter();
                    if (interp) return interp->callForHarness(toStringIt->second, {}, value);
                }
                return prim;
            } else if (prim.isBool()) {
                return prim;
            }
            return prim;
        }
    }
    return value;
}

bool JSONStringifier::serializeValue(const Value& holder, const std::string& key, Value value) {
    value = callToJSON(value, key);
    value = applyReplacer(holder, key, value);
    value = unwrapPrimitive(value);

    if (value.isUndefined() || value.isFunction() || value.isClass() || value.isSymbol()) {
        return false;
    }
    if (value.isNull()) {
        out_ << "null";
    } else if (value.isBool()) {
        out_ << (std::get<bool>(value.data) ? "true" : "false");
    } else if (value.isNumber()) {
        double num = std::get<double>(value.data);
        if (std::isfinite(num)) {
            out_ << ecmaNumberToString(num);
        } else {
            out_ << "null";
        }
    } else if (value.isBigInt()) {
        throw std::runtime_error("TypeError: Do not know how to serialize a BigInt");
    } else if (value.isString()) {
        stringifyString(std::get<std::string>(value.data));
    } else if (isJSONObjectLike(value) && jsonIsArrayValue(getGlobalInterpreter(), value)) {
        checkCircular(value);
        serializeArray(value);
    } else if (value.isObject() || value.isError() || value.isProxy()) {
        if (value.isObject()) {
            auto obj = value.getGC<Object>();
            auto rawIt = obj->properties.find("__is_raw_json__");
            if (rawIt != obj->properties.end() && rawIt->second.isBool() && rawIt->second.toBool()) {
                auto valIt = obj->properties.find("rawJSON");
                if (valIt != obj->properties.end() && valIt->second.isString()) {
                    out_ << std::get<std::string>(valIt->second.data);
                    return true;
                }
            }
        }
        checkCircular(value);
        serializeObject(value);
    } else if (value.isRegex() || value.isMap() || value.isSet() ||
               value.isWeakMap() || value.isWeakSet()) {
        out_ << "{}";
    } else {
        out_ << "null";
    }
    return true;
}

int32_t JSONStringifier::decodeUTF8(const std::string& str, size_t& pos) {
    unsigned char ch = static_cast<unsigned char>(str[pos]);
    if (ch < 0x80) {
        pos++;
        return ch;
    }
    int32_t cp = 0;
    int extra = 0;
    if ((ch & 0xE0) == 0xC0) {
        cp = ch & 0x1F;
        extra = 1;
    } else if ((ch & 0xF0) == 0xE0) {
        cp = ch & 0x0F;
        extra = 2;
    } else if ((ch & 0xF8) == 0xF0) {
        cp = ch & 0x07;
        extra = 3;
    } else {
        pos++;
        return -1;
    }
    if (pos + extra >= str.size()) {
        pos++;
        return -1;
    }
    for (int i = 0; i < extra; i++) {
        pos++;
        unsigned char cont = static_cast<unsigned char>(str[pos]);
        if ((cont & 0xC0) != 0x80) return -1;
        cp = (cp << 6) | (cont & 0x3F);
    }
    pos++;
    return cp;
}

void JSONStringifier::stringifyString(const std::string& str) {
    out_ << '"';
    size_t i = 0;
    while (i < str.size()) {
        unsigned char ch = static_cast<unsigned char>(str[i]);
        switch (ch) {
            case '"': out_ << "\\\""; i++; break;
            case '\\': out_ << "\\\\"; i++; break;
            case '\b': out_ << "\\b"; i++; break;
            case '\f': out_ << "\\f"; i++; break;
            case '\n': out_ << "\\n"; i++; break;
            case '\r': out_ << "\\r"; i++; break;
            case '\t': out_ << "\\t"; i++; break;
            default:
                if (ch < 0x80) {
                    if (ch < 32) {
                        out_ << "\\u" << std::hex << std::setfill('0') << std::setw(4) << static_cast<int>(ch);
                        out_ << std::dec;
                    } else {
                        out_ << static_cast<char>(ch);
                    }
                    i++;
                } else {
                    size_t start = i;
                    int32_t cp = decodeUTF8(str, i);
                    if (cp < 0) {
                        out_ << str[start];
                    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                        out_ << "\\u" << std::hex << std::setfill('0') << std::setw(4) << cp;
                        out_ << std::dec;
                    } else {
                        for (size_t j = start; j < i; j++) {
                            out_ << str[j];
                        }
                    }
                }
                break;
        }
    }
    out_ << '"';
}

void JSONStringifier::serializeArray(const Value& arrayVal) {
    auto ptr = getPointer(arrayVal);
    stack_.push_back(ptr);
    std::string prevIndent = indent_;
    indent_ += gap_;
    out_ << '[';
    bool empty = true;
    auto* interp = getGlobalInterpreter();
    size_t length = static_cast<size_t>(jsonLengthOfArrayLike(interp, arrayVal));
    for (size_t i = 0; i < length; ++i) {
        if (i > 0) out_ << ',';
        if (!gap_.empty()) {
            out_ << '\n' << indent_;
        }
        empty = false;
        Value element = jsonGet(interp, arrayVal, std::to_string(i));
        if (!serializeValue(arrayVal, std::to_string(i), element)) {
            out_ << "null";
        }
    }
    indent_ = prevIndent;
    if (!empty && !gap_.empty()) {
        out_ << '\n' << indent_;
    }
    out_ << ']';
    stack_.pop_back();
}

void JSONStringifier::serializeObject(const Value& objVal) {
    auto ptr = getPointer(objVal);
    stack_.push_back(ptr);
    std::string prevIndent = indent_;
    indent_ += gap_;
    out_ << '{';
    bool first = true;

    std::vector<std::string> keys;
    if (hasPropertyList_) {
        keys = propertyList_;
    } else {
        keys = jsonEnumerableOwnKeys(getGlobalInterpreter(), objVal);
    }

    for (const auto& key : keys) {
        Value val = jsonGet(getGlobalInterpreter(), objVal, key);

        if (!gap_.empty()) {
            std::ostringstream entry;
            entry << '\n' << indent_;
            stringifyStringTo(entry, key);
            entry << ": ";
            std::string prevOut = out_.str();
            out_.str("");
            out_.clear();
            bool written = serializeValue(objVal, key, val);
            std::string valueStr = out_.str();
            out_.str(prevOut);
            out_.clear();
            out_.seekp(0, std::ios_base::end);
            if (written) {
                if (!first) out_ << ',';
                first = false;
                out_ << entry.str() << valueStr;
            }
        } else {
            std::string prevOut = out_.str();
            out_.str("");
            out_.clear();
            bool written = serializeValue(objVal, key, val);
            std::string valueStr = out_.str();
            out_.str(prevOut);
            out_.clear();
            out_.seekp(0, std::ios_base::end);
            if (written) {
                if (!first) out_ << ',';
                first = false;
                stringifyString(key);
                out_ << ':' << valueStr;
            }
        }
    }
    indent_ = prevIndent;
    if (!first && !gap_.empty()) {
        out_ << '\n' << indent_;
    }
    out_ << '}';
    stack_.pop_back();
}

void JSONStringifier::stringifyStringTo(std::ostringstream& os, const std::string& str) {
    os << '"';
    size_t i = 0;
    while (i < str.size()) {
        unsigned char ch = static_cast<unsigned char>(str[i]);
        switch (ch) {
            case '"': os << "\\\""; i++; break;
            case '\\': os << "\\\\"; i++; break;
            case '\b': os << "\\b"; i++; break;
            case '\f': os << "\\f"; i++; break;
            case '\n': os << "\\n"; i++; break;
            case '\r': os << "\\r"; i++; break;
            case '\t': os << "\\t"; i++; break;
            default:
                if (ch < 0x80) {
                    if (ch < 32) {
                        os << "\\u" << std::hex << std::setfill('0') << std::setw(4) << static_cast<int>(ch);
                        os << std::dec;
                    } else {
                        os << static_cast<char>(ch);
                    }
                    i++;
                } else {
                    size_t start = i;
                    int32_t cp = decodeUTF8(str, i);
                    if (cp < 0) {
                        os << str[start];
                    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                        os << "\\u" << std::hex << std::setfill('0') << std::setw(4) << cp;
                        os << std::dec;
                    } else {
                        for (size_t j = start; j < i; j++) {
                            os << str[j];
                        }
                    }
                }
                break;
        }
    }
    os << '"';
}

std::string JSONStringifier::stringify(const Value& value) {
    out_.str("");
    out_.clear();
    indent_ = "";
    stack_.clear();

    auto holder = makeObjectWithPrototype();
    holder->properties[""] = value;
    Value holderVal(holder);

    if (!serializeValue(holderVal, "", value)) {
        return "";
    }
    return out_.str();
}

}  // namespace lightjs::detail
