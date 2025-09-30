#include "value.h"
#include <sstream>
#include <stdexcept>
#include <cctype>
#include <iomanip>
#include <cmath>

namespace tinyjs {

class JSONParser {
private:
    const std::string& str_;
    size_t pos_;

    void skipWhitespace() {
        while (pos_ < str_.size() && std::isspace(str_[pos_])) {
            pos_++;
        }
    }

    Value parseValue() {
        skipWhitespace();
        if (pos_ >= str_.size()) {
            throw std::runtime_error("Unexpected end of JSON input");
        }

        char ch = str_[pos_];
        switch (ch) {
            case '"':
                return parseString();
            case 't':
                return parseTrue();
            case 'f':
                return parseFalse();
            case 'n':
                return parseNull();
            case '{':
                return parseObject();
            case '[':
                return parseArray();
            case '-':
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                return parseNumber();
            default:
                throw std::runtime_error("Unexpected character in JSON");
        }
    }

    Value parseString() {
        if (str_[pos_] != '"') {
            throw std::runtime_error("Expected '\"' at start of string");
        }
        pos_++; // Skip opening quote

        std::string result;
        while (pos_ < str_.size() && str_[pos_] != '"') {
            if (str_[pos_] == '\\') {
                pos_++; // Skip backslash
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
                    case 'u':
                        // Unicode escape - simplified implementation
                        pos_ += 4; // Skip the 4 hex digits
                        result += '?'; // Placeholder
                        break;
                    default:
                        throw std::runtime_error("Invalid escape sequence");
                }
            } else {
                result += str_[pos_];
            }
            pos_++;
        }

        if (pos_ >= str_.size()) {
            throw std::runtime_error("Unterminated string");
        }
        pos_++; // Skip closing quote

        return Value(result);
    }

    Value parseNumber() {
        size_t start = pos_;
        if (str_[pos_] == '-') pos_++;

        if (pos_ >= str_.size() || !std::isdigit(str_[pos_])) {
            throw std::runtime_error("Invalid number");
        }

        if (str_[pos_] == '0') {
            pos_++;
        } else {
            while (pos_ < str_.size() && std::isdigit(str_[pos_])) {
                pos_++;
            }
        }

        if (pos_ < str_.size() && str_[pos_] == '.') {
            pos_++;
            if (pos_ >= str_.size() || !std::isdigit(str_[pos_])) {
                throw std::runtime_error("Invalid number");
            }
            while (pos_ < str_.size() && std::isdigit(str_[pos_])) {
                pos_++;
            }
        }

        if (pos_ < str_.size() && (str_[pos_] == 'e' || str_[pos_] == 'E')) {
            pos_++;
            if (pos_ < str_.size() && (str_[pos_] == '+' || str_[pos_] == '-')) {
                pos_++;
            }
            if (pos_ >= str_.size() || !std::isdigit(str_[pos_])) {
                throw std::runtime_error("Invalid number");
            }
            while (pos_ < str_.size() && std::isdigit(str_[pos_])) {
                pos_++;
            }
        }

        std::string numStr = str_.substr(start, pos_ - start);
        return Value(std::stod(numStr));
    }

    Value parseTrue() {
        if (str_.substr(pos_, 4) != "true") {
            throw std::runtime_error("Invalid literal");
        }
        pos_ += 4;
        return Value(true);
    }

    Value parseFalse() {
        if (str_.substr(pos_, 5) != "false") {
            throw std::runtime_error("Invalid literal");
        }
        pos_ += 5;
        return Value(false);
    }

    Value parseNull() {
        if (str_.substr(pos_, 4) != "null") {
            throw std::runtime_error("Invalid literal");
        }
        pos_ += 4;
        return Value(Null{});
    }

    Value parseObject() {
        if (str_[pos_] != '{') {
            throw std::runtime_error("Expected '{'");
        }
        pos_++; // Skip opening brace

        auto obj = std::make_shared<Object>();
        skipWhitespace();

        if (pos_ < str_.size() && str_[pos_] == '}') {
            pos_++; // Skip closing brace
            return Value(obj);
        }

        while (true) {
            skipWhitespace();
            if (pos_ >= str_.size()) {
                throw std::runtime_error("Unexpected end of object");
            }

            // Parse key
            Value keyValue = parseString();
            if (!keyValue.isString()) {
                throw std::runtime_error("Object key must be string");
            }
            std::string key = std::get<std::string>(keyValue.data);

            skipWhitespace();
            if (pos_ >= str_.size() || str_[pos_] != ':') {
                throw std::runtime_error("Expected ':' after object key");
            }
            pos_++; // Skip colon

            // Parse value
            Value value = parseValue();
            obj->properties[key] = value;

            skipWhitespace();
            if (pos_ >= str_.size()) {
                throw std::runtime_error("Unexpected end of object");
            }

            if (str_[pos_] == '}') {
                pos_++; // Skip closing brace
                break;
            } else if (str_[pos_] == ',') {
                pos_++; // Skip comma
                continue;
            } else {
                throw std::runtime_error("Expected ',' or '}' in object");
            }
        }

        return Value(obj);
    }

    Value parseArray() {
        if (str_[pos_] != '[') {
            throw std::runtime_error("Expected '['");
        }
        pos_++; // Skip opening bracket

        auto arr = std::make_shared<Array>();
        skipWhitespace();

        if (pos_ < str_.size() && str_[pos_] == ']') {
            pos_++; // Skip closing bracket
            return Value(arr);
        }

        while (true) {
            Value value = parseValue();
            arr->elements.push_back(value);

            skipWhitespace();
            if (pos_ >= str_.size()) {
                throw std::runtime_error("Unexpected end of array");
            }

            if (str_[pos_] == ']') {
                pos_++; // Skip closing bracket
                break;
            } else if (str_[pos_] == ',') {
                pos_++; // Skip comma
                continue;
            } else {
                throw std::runtime_error("Expected ',' or ']' in array");
            }
        }

        return Value(arr);
    }

public:
    JSONParser(const std::string& str) : str_(str), pos_(0) {}

    Value parse() {
        Value result = parseValue();
        skipWhitespace();
        if (pos_ < str_.size()) {
            throw std::runtime_error("Unexpected trailing characters");
        }
        return result;
    }
};

class JSONStringifier {
private:
    std::ostringstream out_;

    void stringifyValue(const Value& value) {
        if (value.isUndefined()) {
            // undefined becomes null in JSON
            out_ << "null";
        } else if (value.isNull()) {
            out_ << "null";
        } else if (value.isBool()) {
            out_ << (std::get<bool>(value.data) ? "true" : "false");
        } else if (value.isNumber()) {
            double num = std::get<double>(value.data);
            if (std::isfinite(num)) {
                out_ << num;
            } else {
                out_ << "null"; // Infinity and NaN become null
            }
        } else if (value.isBigInt()) {
            // BigInt cannot be JSON serialized
            throw std::runtime_error("Cannot stringify BigInt");
        } else if (value.isString()) {
            stringifyString(std::get<std::string>(value.data));
        } else if (value.isArray()) {
            stringifyArray(*std::get<std::shared_ptr<Array>>(value.data));
        } else if (value.isObject()) {
            stringifyObject(*std::get<std::shared_ptr<Object>>(value.data));
        } else {
            // Functions, symbols, etc. become null
            out_ << "null";
        }
    }

    void stringifyString(const std::string& str) {
        out_ << '"';
        for (char ch : str) {
            switch (ch) {
                case '"': out_ << "\\\""; break;
                case '\\': out_ << "\\\\"; break;
                case '\b': out_ << "\\b"; break;
                case '\f': out_ << "\\f"; break;
                case '\n': out_ << "\\n"; break;
                case '\r': out_ << "\\r"; break;
                case '\t': out_ << "\\t"; break;
                default:
                    if (ch < 32) {
                        out_ << "\\u" << std::hex << std::setfill('0') << std::setw(4) << (int)ch;
                    } else {
                        out_ << ch;
                    }
                    break;
            }
        }
        out_ << '"';
    }

    void stringifyArray(const Array& arr) {
        out_ << '[';
        for (size_t i = 0; i < arr.elements.size(); ++i) {
            if (i > 0) out_ << ',';
            stringifyValue(arr.elements[i]);
        }
        out_ << ']';
    }

    void stringifyObject(const Object& obj) {
        out_ << '{';
        bool first = true;
        for (const auto& [key, value] : obj.properties) {
            if (!first) out_ << ',';
            first = false;
            stringifyString(key);
            out_ << ':';
            stringifyValue(value);
        }
        out_ << '}';
    }

public:
    std::string stringify(const Value& value) {
        out_.str("");
        out_.clear();
        stringifyValue(value);
        return out_.str();
    }
};

// JSON object implementation
Value JSON_parse(const std::vector<Value>& args) {
    if (args.empty()) {
        throw std::runtime_error("JSON.parse requires at least 1 argument");
    }

    if (!args[0].isString()) {
        throw std::runtime_error("JSON.parse argument must be a string");
    }

    std::string jsonStr = std::get<std::string>(args[0].data);
    JSONParser parser(jsonStr);
    return parser.parse();
}

Value JSON_stringify(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value("undefined");
    }

    JSONStringifier stringifier;
    try {
        std::string result = stringifier.stringify(args[0]);
        return Value(result);
    } catch (const std::exception&) {
        return Value(Undefined{});
    }
}

} // namespace tinyjs