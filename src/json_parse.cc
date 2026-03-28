#include "json_internal.h"
#include <cctype>
#include <stdexcept>

namespace lightjs::detail {

JSONParser::JSONParser(const std::string& str) : str_(str), pos_(0) {}

void JSONParser::skipWhitespace() {
    while (pos_ < str_.size()) {
        char ch = str_[pos_];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            pos_++;
        } else {
            break;
        }
    }
}

Value JSONParser::parseValue(std::string* outSource) {
    skipWhitespace();
    if (pos_ >= str_.size()) {
        throw std::runtime_error("Unexpected end of JSON input");
    }

    size_t startPos = pos_;
    char ch = str_[pos_];
    Value result;
    bool isPrimitive = false;
    switch (ch) {
        case '"':
            result = parseString();
            isPrimitive = true;
            break;
        case 't':
            result = parseTrue();
            isPrimitive = true;
            break;
        case 'f':
            result = parseFalse();
            isPrimitive = true;
            break;
        case 'n':
            result = parseNull();
            isPrimitive = true;
            break;
        case '{':
            result = parseObject();
            break;
        case '[':
            result = parseArray();
            break;
        case '-':
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            result = parseNumber();
            isPrimitive = true;
            break;
        default:
            throw std::runtime_error("Unexpected character in JSON");
    }
    if (outSource && isPrimitive) {
        *outSource = str_.substr(startPos, pos_ - startPos);
    }
    return result;
}

Value JSONParser::parseString() {
    if (str_[pos_] != '"') {
        throw std::runtime_error("Expected '\"' at start of string");
    }
    pos_++;

    std::string result;
    while (pos_ < str_.size() && str_[pos_] != '"') {
        if (str_[pos_] == '\\') {
            pos_++;
            if (pos_ >= str_.size()) {
                throw std::runtime_error("Unexpected end of string");
            }
            switch (str_[pos_]) {
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case '/': result += '/'; break;
                case 'b': result += '\b'; break;
                case 'f': result += '\f'; break;
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                case 'u': {
                    pos_++;
                    if (pos_ + 4 > str_.size()) {
                        throw std::runtime_error("Invalid unicode escape in JSON string");
                    }
                    std::string hex = str_.substr(pos_, 4);
                    pos_ += 3;
                    unsigned int codePoint = 0;
                    for (char c : hex) {
                        codePoint <<= 4;
                        if (c >= '0' && c <= '9') codePoint |= (c - '0');
                        else if (c >= 'a' && c <= 'f') codePoint |= (c - 'a' + 10);
                        else if (c >= 'A' && c <= 'F') codePoint |= (c - 'A' + 10);
                        else throw std::runtime_error("Invalid hex digit in unicode escape");
                    }
                    if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
                        if (pos_ + 2 < str_.size() && str_[pos_ + 1] == '\\' && str_[pos_ + 2] == 'u') {
                            pos_ += 3;
                            if (pos_ + 4 > str_.size()) throw std::runtime_error("Invalid surrogate pair");
                            std::string hex2 = str_.substr(pos_, 4);
                            pos_ += 3;
                            unsigned int low = 0;
                            for (char c : hex2) {
                                low <<= 4;
                                if (c >= '0' && c <= '9') low |= (c - '0');
                                else if (c >= 'a' && c <= 'f') low |= (c - 'a' + 10);
                                else if (c >= 'A' && c <= 'F') low |= (c - 'A' + 10);
                            }
                            if (low >= 0xDC00 && low <= 0xDFFF) {
                                codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                            }
                        }
                    }
                    if (codePoint <= 0x7F) {
                        result += static_cast<char>(codePoint);
                    } else if (codePoint <= 0x7FF) {
                        result += static_cast<char>(0xC0 | (codePoint >> 6));
                        result += static_cast<char>(0x80 | (codePoint & 0x3F));
                    } else if (codePoint <= 0xFFFF) {
                        result += static_cast<char>(0xE0 | (codePoint >> 12));
                        result += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (codePoint & 0x3F));
                    } else if (codePoint <= 0x10FFFF) {
                        result += static_cast<char>(0xF0 | (codePoint >> 18));
                        result += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
                        result += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (codePoint & 0x3F));
                    }
                    break;
                }
                default:
                    throw std::runtime_error("Invalid escape sequence");
            }
        } else {
            unsigned char ch = static_cast<unsigned char>(str_[pos_]);
            if (ch < 0x20) {
                throw std::runtime_error("Unexpected control character in JSON string");
            }
            result += str_[pos_];
        }
        pos_++;
    }

    if (pos_ >= str_.size()) {
        throw std::runtime_error("Unterminated string");
    }
    pos_++;

    return Value(result);
}

Value JSONParser::parseNumber() {
    size_t start = pos_;
    if (str_[pos_] == '-') pos_++;

    if (pos_ >= str_.size() || !std::isdigit(static_cast<unsigned char>(str_[pos_]))) {
        throw std::runtime_error("Invalid number");
    }

    if (str_[pos_] == '0') {
        pos_++;
    } else {
        while (pos_ < str_.size() && std::isdigit(static_cast<unsigned char>(str_[pos_]))) {
            pos_++;
        }
    }

    if (pos_ < str_.size() && str_[pos_] == '.') {
        pos_++;
        if (pos_ >= str_.size() || !std::isdigit(static_cast<unsigned char>(str_[pos_]))) {
            throw std::runtime_error("Invalid number");
        }
        while (pos_ < str_.size() && std::isdigit(static_cast<unsigned char>(str_[pos_]))) {
            pos_++;
        }
    }

    if (pos_ < str_.size() && (str_[pos_] == 'e' || str_[pos_] == 'E')) {
        pos_++;
        if (pos_ < str_.size() && (str_[pos_] == '+' || str_[pos_] == '-')) {
            pos_++;
        }
        if (pos_ >= str_.size() || !std::isdigit(static_cast<unsigned char>(str_[pos_]))) {
            throw std::runtime_error("Invalid number");
        }
        while (pos_ < str_.size() && std::isdigit(static_cast<unsigned char>(str_[pos_]))) {
            pos_++;
        }
    }

    std::string numStr = str_.substr(start, pos_ - start);
    return Value(std::stod(numStr));
}

Value JSONParser::parseTrue() {
    if (str_.substr(pos_, 4) != "true") {
        throw std::runtime_error("Invalid literal");
    }
    pos_ += 4;
    return Value(true);
}

Value JSONParser::parseFalse() {
    if (str_.substr(pos_, 5) != "false") {
        throw std::runtime_error("Invalid literal");
    }
    pos_ += 5;
    return Value(false);
}

Value JSONParser::parseNull() {
    if (str_.substr(pos_, 4) != "null") {
        throw std::runtime_error("Invalid literal");
    }
    pos_ += 4;
    return Value(Null{});
}

Value JSONParser::parseObject() {
    if (str_[pos_] != '{') {
        throw std::runtime_error("Expected '{'");
    }
    pos_++;

    auto obj = makeObjectWithPrototype();
    skipWhitespace();

    if (pos_ < str_.size() && str_[pos_] == '}') {
        pos_++;
        return Value(obj);
    }

    while (true) {
        skipWhitespace();
        if (pos_ >= str_.size()) {
            throw std::runtime_error("Unexpected end of object");
        }

        Value keyValue = parseString();
        if (!keyValue.isString()) {
            throw std::runtime_error("Object key must be string");
        }
        std::string key = std::get<std::string>(keyValue.data);

        skipWhitespace();
        if (pos_ >= str_.size() || str_[pos_] != ':') {
            throw std::runtime_error("Expected ':' after object key");
        }
        pos_++;

        std::string valSource;
        Value value = parseValue(&valSource);
        if (key == "__proto__") {
            obj->properties["__own_prop___proto__"] = value;
            if (!valSource.empty()) obj->properties["__json_source___proto__"] = Value(valSource);
        } else {
            obj->properties[key] = value;
            if (!valSource.empty()) obj->properties["__json_source_" + key] = Value(valSource);
        }

        skipWhitespace();
        if (pos_ >= str_.size()) {
            throw std::runtime_error("Unexpected end of object");
        }

        if (str_[pos_] == '}') {
            pos_++;
            break;
        } else if (str_[pos_] == ',') {
            pos_++;
            continue;
        } else {
            throw std::runtime_error("Expected ',' or '}' in object");
        }
    }

    return Value(obj);
}

Value JSONParser::parseArray() {
    if (str_[pos_] != '[') {
        throw std::runtime_error("Expected '['");
    }
    pos_++;

    auto arr = makeArrayWithPrototype();
    skipWhitespace();

    if (pos_ < str_.size() && str_[pos_] == ']') {
        pos_++;
        return Value(arr);
    }

    while (true) {
        std::string valSource;
        Value value = parseValue(&valSource);
        size_t idx = arr->elements.size();
        arr->elements.push_back(value);
        if (!valSource.empty()) {
            arr->properties["__json_source_" + std::to_string(idx)] = Value(valSource);
        }

        skipWhitespace();
        if (pos_ >= str_.size()) {
            throw std::runtime_error("Unexpected end of array");
        }

        if (str_[pos_] == ']') {
            pos_++;
            break;
        } else if (str_[pos_] == ',') {
            pos_++;
            continue;
        } else {
            throw std::runtime_error("Expected ',' or ']' in array");
        }
    }

    return Value(arr);
}

Value JSONParser::parse(std::string* outSource) {
    Value result = parseValue(outSource);
    skipWhitespace();
    if (pos_ < str_.size()) {
        throw std::runtime_error("Unexpected trailing characters");
    }
    return result;
}

}  // namespace lightjs::detail
