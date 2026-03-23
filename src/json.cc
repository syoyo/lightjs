#include "value.h"
#include "environment.h"
#include "interpreter.h"
#include "streams.h"
#include "wasm_js.h"
#include <sstream>
#include <stdexcept>
#include <cctype>
#include <iomanip>
#include <cmath>
#include <algorithm>

namespace lightjs {

namespace {

bool isJSONObjectLike(const Value& value) {
  return value.isObject() || value.isArray() || value.isFunction() || value.isClass() ||
         value.isError() || value.isPromise() || value.isMap() || value.isSet() ||
         value.isRegex() || value.isProxy() || value.isTypedArray() || value.isArrayBuffer() ||
         value.isDataView() || value.isWeakMap() || value.isWeakSet() || value.isGenerator();
}

bool parseJSONArrayIndex(const std::string& key, size_t& index) {
  if (key.empty()) return false;
  if (key.size() > 1 && key[0] == '0') return false;
  size_t value = 0;
  for (unsigned char ch : key) {
    if (!std::isdigit(ch)) return false;
    size_t digit = static_cast<size_t>(ch - '0');
    if (value > (std::numeric_limits<size_t>::max() - digit) / 10) return false;
    value = value * 10 + digit;
  }
  index = value;
  return true;
}

[[noreturn]] void throwInterpreterError(Interpreter* interp) {
  Value err = interp->getError();
  interp->clearError();
  throw std::runtime_error(err.toString());
}

Value jsonGet(Interpreter* interp, const Value& receiver, const std::string& key) {
  if (receiver.isProxy()) {
    auto proxy = receiver.getGC<Proxy>();
    if (proxy && proxy->revoked) {
      throw std::runtime_error("TypeError: Cannot perform 'get' on a revoked Proxy");
    }
    if (proxy && proxy->handler && proxy->handler->isObject()) {
      auto handlerObj = proxy->handler->getGC<Object>();
      auto trapIt = handlerObj->properties.find("get");
      if (trapIt != handlerObj->properties.end() && trapIt->second.isFunction() && proxy->target) {
        Value result = interp->callForHarness(
          trapIt->second,
          {*proxy->target, Value(key), receiver},
          Value(handlerObj));
        if (interp->hasError()) throwInterpreterError(interp);
        return result;
      }
    }
    if (proxy && proxy->target) {
      return jsonGet(interp, *proxy->target, key);
    }
    return Value(Undefined{});
  }

  auto [found, value] = interp->getPropertyForExternal(receiver, key);
  if (interp->hasError()) throwInterpreterError(interp);
  return found ? value : Value(Undefined{});
}

Value jsonNextPrototype(const Value& value) {
  auto findProto = [](const auto& ptr) -> Value {
    auto it = ptr->properties.find("__proto__");
    return it != ptr->properties.end() ? it->second : Value(Undefined{});
  };
  if (value.isObject()) return findProto(value.getGC<Object>());
  if (value.isFunction()) return findProto(value.getGC<Function>());
  if (value.isClass()) return findProto(value.getGC<Class>());
  if (value.isError()) return findProto(value.getGC<Error>());
  if (value.isArray()) return findProto(value.getGC<Array>());
  return Value(Undefined{});
}

Value jsonGetFromPrototypeChain(Interpreter* interp,
                                Value prototype,
                                const Value& receiver,
                                const std::string& key) {
  int depth = 0;
  while (isJSONObjectLike(prototype) && depth++ < 32) {
    OrderedMap<std::string, Value>* props = nullptr;
    if (prototype.isObject()) props = &prototype.getGC<Object>()->properties;
    else if (prototype.isFunction()) props = &prototype.getGC<Function>()->properties;
    else if (prototype.isClass()) props = &prototype.getGC<Class>()->properties;
    else if (prototype.isError()) props = &prototype.getGC<Error>()->properties;
    else if (prototype.isArray()) props = &prototype.getGC<Array>()->properties;
    if (!props) break;

    auto getterIt = props->find("__get_" + key);
    if (getterIt != props->end() && getterIt->second.isFunction()) {
      Value result = interp->callForHarness(getterIt->second, {}, receiver);
      if (interp->hasError()) throwInterpreterError(interp);
      return result;
    }
    auto valueIt = props->find(key);
    if (valueIt != props->end()) return valueIt->second;
    prototype = jsonNextPrototype(prototype);
  }
  return Value(Undefined{});
}

Value jsonGetMethodForBigInt(Interpreter* interp, const Value& value, const std::string& key) {
  if (auto bigIntCtor = interp->resolveVariable("BigInt"); bigIntCtor.has_value()) {
    Value proto = jsonGet(interp, *bigIntCtor, "prototype");
    return jsonGetFromPrototypeChain(interp, proto, value, key);
  }
  return Value(Undefined{});
}

bool jsonIsArrayValue(Interpreter* interp, const Value& value) {
  if (value.isArray()) return true;
  if (value.isProxy()) {
    auto proxy = value.getGC<Proxy>();
    if (proxy && proxy->revoked) {
      throw std::runtime_error("TypeError: Cannot perform 'IsArray' on a revoked Proxy");
    }
    if (proxy && proxy->target) {
      return jsonIsArrayValue(interp, *proxy->target);
    }
  }
  return false;
}

double jsonToLength(Interpreter* interp, Value value) {
  if (isJSONObjectLike(value)) {
    value = interp->toPrimitive(value, false);
    if (interp->hasError()) throwInterpreterError(interp);
  }
  if (value.isSymbol() || value.isBigInt()) {
    throw std::runtime_error("TypeError: Cannot convert value to number");
  }
  double length = value.toNumber();
  if (std::isnan(length) || length <= 0) return 0.0;
  if (length == std::numeric_limits<double>::infinity()) return 9007199254740991.0;
  return std::min(std::floor(length), 9007199254740991.0);
}

double jsonLengthOfArrayLike(Interpreter* interp, const Value& value) {
  return jsonToLength(interp, jsonGet(interp, value, "length"));
}

bool jsonDeleteProperty(Interpreter* interp, const Value& receiver, const std::string& key) {
  if (receiver.isProxy()) {
    auto proxy = receiver.getGC<Proxy>();
    if (proxy && proxy->revoked) {
      throw std::runtime_error("TypeError: Cannot perform 'deleteProperty' on a revoked Proxy");
    }
    if (proxy && proxy->handler && proxy->handler->isObject()) {
      auto handlerObj = proxy->handler->getGC<Object>();
      auto trapIt = handlerObj->properties.find("deleteProperty");
      if (trapIt != handlerObj->properties.end() && trapIt->second.isFunction() && proxy->target) {
        Value result = interp->callForHarness(
          trapIt->second,
          {*proxy->target, Value(key)},
          Value(handlerObj));
        if (interp->hasError()) throwInterpreterError(interp);
        return result.toBool();
      }
    }
    if (proxy && proxy->target) {
      return jsonDeleteProperty(interp, *proxy->target, key);
    }
    return true;
  }

  if (receiver.isArray()) {
    auto arr = receiver.getGC<Array>();
    if (arr->properties.find("__non_configurable_" + key) != arr->properties.end()) return false;
    size_t idx = 0;
    if (parseJSONArrayIndex(key, idx)) {
      if (idx < arr->elements.size()) {
        arr->elements[idx] = Value(Undefined{});
        arr->properties["__deleted_" + key + "__"] = Value(true);
      }
      arr->properties.erase(key);
      arr->properties.erase("__get_" + key);
      arr->properties.erase("__set_" + key);
      return true;
    }
    arr->properties.erase(key);
    arr->properties.erase("__get_" + key);
    arr->properties.erase("__set_" + key);
    return true;
  }

  OrderedMap<std::string, Value>* props = nullptr;
  if (receiver.isObject()) props = &receiver.getGC<Object>()->properties;
  else if (receiver.isError()) props = &receiver.getGC<Error>()->properties;
  if (!props) return true;
  if (props->find("__non_configurable_" + key) != props->end()) return false;
  props->erase(key);
  props->erase("__get_" + key);
  props->erase("__set_" + key);
  return true;
}

bool jsonCreateDataProperty(Interpreter* interp,
                            const Value& receiver,
                            const std::string& key,
                            const Value& value) {
  if (receiver.isProxy()) {
    auto proxy = receiver.getGC<Proxy>();
    if (proxy && proxy->revoked) {
      throw std::runtime_error("TypeError: Cannot perform 'defineProperty' on a revoked Proxy");
    }
    if (proxy && proxy->handler && proxy->handler->isObject()) {
      auto handlerObj = proxy->handler->getGC<Object>();
      auto trapIt = handlerObj->properties.find("defineProperty");
      if (trapIt != handlerObj->properties.end() && trapIt->second.isFunction() && proxy->target) {
        auto descriptor = GarbageCollector::makeGC<Object>();
        descriptor->properties["value"] = value;
        descriptor->properties["writable"] = Value(true);
        descriptor->properties["enumerable"] = Value(true);
        descriptor->properties["configurable"] = Value(true);
        Value result = interp->callForHarness(
          trapIt->second,
          {*proxy->target, Value(key), Value(descriptor)},
          Value(handlerObj));
        if (interp->hasError()) throwInterpreterError(interp);
        return result.toBool();
      }
    }
    if (proxy && proxy->target) {
      return jsonCreateDataProperty(interp, *proxy->target, key, value);
    }
    return false;
  }

  if (receiver.isArray()) {
    auto arr = receiver.getGC<Array>();
    if (arr->properties.find("__non_configurable_" + key) != arr->properties.end()) return false;
    size_t idx = 0;
    if (parseJSONArrayIndex(key, idx)) {
      if (idx >= arr->elements.size()) arr->elements.resize(idx + 1, Value(Undefined{}));
      arr->elements[idx] = value;
      arr->properties.erase("__deleted_" + key + "__");
      arr->properties.erase("__hole_" + key + "__");
      return true;
    }
    arr->properties[key] = value;
    return true;
  }

  OrderedMap<std::string, Value>* props = nullptr;
  if (receiver.isObject()) props = &receiver.getGC<Object>()->properties;
  else if (receiver.isError()) props = &receiver.getGC<Error>()->properties;
  if (!props) return false;
  if (props->find("__non_configurable_" + key) != props->end()) return false;
  (*props)[key] = value;
  return true;
}

bool jsonIsOwnEnumerableStringKey(const Value& value, const std::string& key) {
  if (isSymbolPropertyKey(key)) return false;
  if (value.isObject()) {
    auto obj = value.getGC<Object>();
    return (obj->properties.count(key) || obj->properties.count("__get_" + key) ||
            obj->properties.count("__set_" + key)) &&
           obj->properties.count("__non_enum_" + key) == 0;
  }
  if (value.isError()) {
    auto err = value.getGC<Error>();
    return (err->properties.count(key) || err->properties.count("__get_" + key) ||
            err->properties.count("__set_" + key)) &&
           err->properties.count("__non_enum_" + key) == 0;
  }
  if (value.isArray()) {
    auto arr = value.getGC<Array>();
    size_t idx = 0;
    if (parseJSONArrayIndex(key, idx) && idx < arr->elements.size()) {
      return arr->properties.count("__deleted_" + key + "__") == 0 &&
             arr->properties.count("__hole_" + key + "__") == 0 &&
             arr->properties.count("__non_enum_" + key) == 0;
    }
    return (arr->properties.count(key) || arr->properties.count("__get_" + key) ||
            arr->properties.count("__set_" + key)) &&
           arr->properties.count("__non_enum_" + key) == 0;
  }
  return false;
}

std::vector<std::string> jsonEnumerableOwnKeys(Interpreter* interp, const Value& value) {
  std::vector<std::string> keys;
  if (value.isProxy()) {
    auto proxy = value.getGC<Proxy>();
    if (proxy && proxy->revoked) {
      throw std::runtime_error("TypeError: Cannot perform 'ownKeys' on a revoked Proxy");
    }
    GCPtr<Object> handlerObj = nullptr;
    if (proxy && proxy->handler && proxy->handler->isObject()) {
      handlerObj = proxy->handler->getGC<Object>();
    }

    std::vector<std::string> candidates;
    bool usedTrap = false;
    if (handlerObj) {
      auto ownKeysIt = handlerObj->properties.find("ownKeys");
      if (ownKeysIt != handlerObj->properties.end() && ownKeysIt->second.isFunction() && proxy->target) {
        Value ownKeysResult = interp->callForHarness(
          ownKeysIt->second, {*proxy->target}, Value(handlerObj));
        if (interp->hasError()) throwInterpreterError(interp);
        if (ownKeysResult.isArray()) {
          usedTrap = true;
          for (const auto& keyValue : ownKeysResult.getGC<Array>()->elements) {
            candidates.push_back(valueToPropertyKey(keyValue));
          }
        }
      }
    }
    if (!usedTrap) {
      if (proxy && proxy->target) return jsonEnumerableOwnKeys(interp, *proxy->target);
      return keys;
    }

    for (const auto& key : candidates) {
      if (isSymbolPropertyKey(key)) continue;
      bool present = false;
      bool enumerable = false;
      if (handlerObj) {
        auto gopdIt = handlerObj->properties.find("getOwnPropertyDescriptor");
        if (gopdIt != handlerObj->properties.end() && gopdIt->second.isFunction() && proxy->target) {
          Value descVal = interp->callForHarness(
            gopdIt->second,
            {*proxy->target, Value(key)},
            Value(handlerObj));
          if (interp->hasError()) throwInterpreterError(interp);
          if (descVal.isObject()) {
            present = true;
            enumerable = jsonGet(interp, descVal, "enumerable").toBool();
          }
        } else if (proxy && proxy->target) {
          present = jsonIsOwnEnumerableStringKey(*proxy->target, key);
          enumerable = present;
        }
      }
      if (present && enumerable) keys.push_back(key);
    }
    return keys;
  }

  OrderedMap<std::string, Value>* props = nullptr;
  if (value.isObject()) props = &value.getGC<Object>()->properties;
  else if (value.isError()) props = &value.getGC<Error>()->properties;
  else if (value.isArray()) props = &value.getGC<Array>()->properties;
  if (!props) return keys;

  std::vector<std::pair<uint32_t, std::string>> indexKeys;
  std::vector<std::string> stringKeys;
  for (const auto& key : props->orderedKeys()) {
    if (key.size() >= 2 && key[0] == '_' && key[1] == '_') continue;
    if (isSymbolPropertyKey(key)) continue;
    if (props->find("__non_enum_" + key) != props->end()) continue;
    bool isIdx = false;
    if (!key.empty() &&
        std::all_of(key.begin(), key.end(), [](unsigned char c) { return std::isdigit(c) != 0; }) &&
        (key.size() == 1 || key[0] != '0')) {
      try {
        unsigned long long parsed = std::stoull(key);
        if (parsed < 4294967295ULL) {
          indexKeys.push_back({static_cast<uint32_t>(parsed), key});
          isIdx = true;
        }
      } catch (...) {
      }
    }
    if (!isIdx) stringKeys.push_back(key);
  }
  std::sort(indexKeys.begin(), indexKeys.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& [_, key] : indexKeys) keys.push_back(key);
  for (const auto& key : stringKeys) keys.push_back(key);
  return keys;
}

}  // namespace

class JSONParser {
private:
    const std::string& str_;
    size_t pos_;

    void skipWhitespace() {
        // JSON spec: only SP, HT, LF, CR are valid whitespace
        while (pos_ < str_.size()) {
            char ch = str_[pos_];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                pos_++;
            } else {
                break;
            }
        }
    }

    // Parse a value and optionally capture its source text
    Value parseValue(std::string* outSource = nullptr) {
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
                result = parseString(); isPrimitive = true; break;
            case 't':
                result = parseTrue(); isPrimitive = true; break;
            case 'f':
                result = parseFalse(); isPrimitive = true; break;
            case 'n':
                result = parseNull(); isPrimitive = true; break;
            case '{':
                result = parseObject(); break;
            case '[':
                result = parseArray(); break;
            case '-':
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                result = parseNumber(); isPrimitive = true; break;
            default:
                throw std::runtime_error("Unexpected character in JSON");
        }
        if (outSource && isPrimitive) {
            *outSource = str_.substr(startPos, pos_ - startPos);
        }
        return result;
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
                    case 'u': {
                        // Unicode escape \uXXXX - pos_ is currently on 'u'
                        pos_++; // skip past 'u'
                        if (pos_ + 4 > str_.size()) {
                            throw std::runtime_error("Invalid unicode escape in JSON string");
                        }
                        std::string hex = str_.substr(pos_, 4);
                        pos_ += 3; // advance 3 (the outer loop does pos_++ for the 4th)
                        unsigned int codePoint = 0;
                        for (char c : hex) {
                            codePoint <<= 4;
                            if (c >= '0' && c <= '9') codePoint |= (c - '0');
                            else if (c >= 'a' && c <= 'f') codePoint |= (c - 'a' + 10);
                            else if (c >= 'A' && c <= 'F') codePoint |= (c - 'A' + 10);
                            else throw std::runtime_error("Invalid hex digit in unicode escape");
                        }
                        // Handle surrogate pairs
                        if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
                            // High surrogate - check for low surrogate
                            // pos_ is now on last hex digit of first escape
                            // Need to check pos_+1 = '\\', pos_+2 = 'u'
                            if (pos_ + 2 < str_.size() && str_[pos_ + 1] == '\\' && str_[pos_ + 2] == 'u') {
                                pos_ += 3; // skip past last-hex, backslash, 'u'
                                if (pos_ + 4 > str_.size()) throw std::runtime_error("Invalid surrogate pair");
                                std::string hex2 = str_.substr(pos_, 4);
                                pos_ += 3; // point at last hex (outer loop will advance)
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
                        // Encode as UTF-8
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
                // JSON spec: control characters U+0000-U+001F must be escaped
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

        auto obj = makeObjectWithPrototype();
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

            // Parse value with source tracking
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

        auto arr = makeArrayWithPrototype();
        skipWhitespace();

        if (pos_ < str_.size() && str_[pos_] == ']') {
            pos_++; // Skip closing bracket
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

    Value parse(std::string* outSource = nullptr) {
        Value result = parseValue(outSource);
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
    std::string gap_;  // indentation string per level
    std::string indent_; // current indentation
    std::vector<const void*> stack_; // circular reference detection
    Value replacerFn_ = Value(Undefined{});
    std::vector<std::string> propertyList_;
    bool hasPropertyList_ = false;

    bool isObjectLike(const Value& v) const {
        return v.isObject() || v.isArray() || v.isFunction() || v.isClass() ||
               v.isError() || v.isPromise() || v.isMap() || v.isSet() ||
               v.isRegex() || v.isProxy() || v.isTypedArray() || v.isArrayBuffer() ||
               v.isDataView();
    }

    const void* getPointer(const Value& v) const {
        if (v.isObject()) return v.getGC<Object>().get();
        if (v.isArray()) return v.getGC<Array>().get();
        if (v.isError()) return v.getGC<Error>().get();
        if (v.isFunction()) return v.getGC<Function>().get();
        if (v.isMap()) return v.getGC<Map>().get();
        if (v.isSet()) return v.getGC<Set>().get();
        if (v.isProxy()) return v.getGC<Proxy>().get();
        return nullptr;
    }

    void checkCircular(const Value& v) {
        auto ptr = getPointer(v);
        if (!ptr) return;
        for (auto p : stack_) {
            if (p == ptr) {
                throw std::runtime_error("TypeError: Converting circular structure to JSON");
            }
        }
    }

    // Call toJSON if present and callable (ES spec §24.5.2 step 2)
    Value callToJSON(const Value& value, const std::string& key) {
        auto* interp = getGlobalInterpreter();
        if (!interp) return value;
        // Step 2a: Let toJSON be ? Get(value, "toJSON") — may invoke getter
        Value toJSON = value.isBigInt() ? jsonGetMethodForBigInt(interp, value, "toJSON")
                                        : jsonGet(interp, value, "toJSON");
        // Step 2b: If IsCallable(toJSON), call it
        if (toJSON.isFunction()) {
            Value result = interp->callForHarness(toJSON, {Value(key)}, value);
            if (interp->hasError()) {
                throwInterpreterError(interp);
            }
            return result;
        }
        return value;
    }

    // Apply replacer function
    Value applyReplacer(const Value& holder, const std::string& key, Value value) {
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

    // Unwrap String/Number/Boolean wrapper objects via ToPrimitive
    Value unwrapPrimitive(const Value& value) {
        if (value.isObject()) {
            auto obj = value.getGC<Object>();
            auto primIt = obj->properties.find("__primitive_value__");
            if (primIt != obj->properties.end()) {
                const auto& prim = primIt->second;
                if (prim.isNumber()) {
                    // ToNumber: call valueOf if overridden
                    auto valueOfIt = obj->properties.find("valueOf");
                    if (valueOfIt != obj->properties.end() && valueOfIt->second.isFunction()) {
                        auto* interp = getGlobalInterpreter();
                        if (interp) return interp->callForHarness(valueOfIt->second, {}, value);
                    }
                    return prim;
                } else if (prim.isString()) {
                    // ToString: call toString if overridden
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

    // Returns true if value was written, false if it should be omitted
    bool serializeValue(const Value& holder, const std::string& key, Value value) {
        // ES spec §24.5.2.1 SerializeJSONProperty:
        // Step 2: If Type(value) is Object or BigInt, call toJSON
        value = callToJSON(value, key);
        // Step 3: apply replacer
        value = applyReplacer(holder, key, value);
        // Step 4: Unwrap primitive wrappers (Number/String/Boolean/BigInt objects)
        value = unwrapPrimitive(value);

        if (value.isUndefined() || value.isFunction() || value.isClass() || value.isSymbol()) {
            return false; // omit
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
            // Check for rawJSON marker (JSON.rawJSON result)
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
            // Non-callable object types serialize as plain objects
            out_ << "{}";
        } else {
            out_ << "null";
        }
        return true;
    }

    // Decode a UTF-8 code point from str starting at pos. Returns the code point
    // and advances pos past the sequence. Returns -1 on invalid sequence.
    static int32_t decodeUTF8(const std::string& str, size_t& pos) {
        unsigned char ch = static_cast<unsigned char>(str[pos]);
        if (ch < 0x80) { pos++; return ch; }
        int32_t cp = 0;
        int extra = 0;
        if ((ch & 0xE0) == 0xC0) { cp = ch & 0x1F; extra = 1; }
        else if ((ch & 0xF0) == 0xE0) { cp = ch & 0x0F; extra = 2; }
        else if ((ch & 0xF8) == 0xF0) { cp = ch & 0x07; extra = 3; }
        else { pos++; return -1; }
        if (pos + extra >= str.size()) { pos++; return -1; }
        for (int i = 0; i < extra; i++) {
            pos++;
            unsigned char cont = static_cast<unsigned char>(str[pos]);
            if ((cont & 0xC0) != 0x80) return -1;
            cp = (cp << 6) | (cont & 0x3F);
        }
        pos++;
        return cp;
    }

    void writeEscapedCodePoint(std::ostringstream& os, int32_t cp) {
        // ES spec: unpaired surrogates get \uXXXX escaped
        // Supplementary plane chars (>= 0x10000) get output as UTF-8 directly
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            os << "\\u" << std::hex << std::setfill('0') << std::setw(4) << cp;
            os << std::dec;
        } else if (cp < 0x20) {
            os << "\\u" << std::hex << std::setfill('0') << std::setw(4) << cp;
            os << std::dec;
        } else if (cp < 0x80) {
            os << static_cast<char>(cp);
        } else {
            // Encode as UTF-8
            if (cp <= 0x7FF) {
                os << static_cast<char>(0xC0 | (cp >> 6));
                os << static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp <= 0xFFFF) {
                os << static_cast<char>(0xE0 | (cp >> 12));
                os << static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                os << static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                os << static_cast<char>(0xF0 | (cp >> 18));
                os << static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                os << static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                os << static_cast<char>(0x80 | (cp & 0x3F));
            }
        }
    }

    void stringifyString(const std::string& str) {
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
                            out_ << "\\u" << std::hex << std::setfill('0') << std::setw(4) << (int)ch;
                            out_ << std::dec;
                        } else {
                            out_ << (char)ch;
                        }
                        i++;
                    } else {
                        // Multi-byte UTF-8 - decode to get the code point
                        size_t start = i;
                        int32_t cp = decodeUTF8(str, i);
                        if (cp < 0) {
                            // Invalid UTF-8 - output raw byte
                            out_ << str[start];
                        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                            // Lone surrogate encoded in UTF-8 (CESU-8 style)
                            // ES spec: escape as \uXXXX
                            out_ << "\\u" << std::hex << std::setfill('0') << std::setw(4) << cp;
                            out_ << std::dec;
                        } else {
                            // Valid multi-byte character - output as-is
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

    bool isInternalKey(const std::string& key) const {
        if (key.size() >= 2 && key[0] == '_' && key[1] == '_') return true;
        if (isSymbolPropertyKey(key)) return true;
        return false;
    }

    void serializeArray(const Value& arrayVal) {
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
                out_ << "null"; // undefined/function slots become null in arrays
            }
        }
        indent_ = prevIndent;
        if (!empty && !gap_.empty()) {
            out_ << '\n' << indent_;
        }
        out_ << ']';
        stack_.pop_back();
    }

    void serializeObject(const Value& objVal) {
        auto ptr = getPointer(objVal);
        stack_.push_back(ptr);
        std::string prevIndent = indent_;
        indent_ += gap_;
        out_ << '{';
        bool first = true;

        // Determine which keys to use
        std::vector<std::string> keys;
        if (hasPropertyList_) {
            keys = propertyList_;
        } else {
            keys = jsonEnumerableOwnKeys(getGlobalInterpreter(), objVal);
        }

        for (const auto& key : keys) {
            // ES spec: Get(holder, key) - invoke getters, return undefined for missing
            Value val = jsonGet(getGlobalInterpreter(), objVal, key);

            if (!gap_.empty()) {
                // Build entry with indentation
                std::ostringstream entry;
                entry << '\n' << indent_;
                stringifyStringTo(entry, key);
                entry << ": ";
                // Temporarily redirect output
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

    void stringifyStringTo(std::ostringstream& os, const std::string& str) {
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
                            os << "\\u" << std::hex << std::setfill('0') << std::setw(4) << (int)ch;
                            os << std::dec;
                        } else {
                            os << (char)ch;
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

public:
    void setGap(const std::string& g) { gap_ = g; }
    void setReplacer(const Value& fn) { replacerFn_ = fn; }
    void setPropertyList(const std::vector<std::string>& list) {
        propertyList_ = list;
        hasPropertyList_ = true;
    }

    std::string stringify(const Value& value) {
        out_.str("");
        out_.clear();
        indent_ = "";
        stack_.clear();

        // Wrap in a holder object for the replacer (with Object.prototype)
        auto holder = makeObjectWithPrototype();
        holder->properties[""] = value;
        Value holderVal(holder);

        if (!serializeValue(holderVal, "", value)) {
            return ""; // signals undefined
        }
        return out_.str();
    }
};

// Internalize JSON property (reviver walk) per ES spec §24.5.1.1
static Value internalizeJSONProperty(Interpreter* interp, const Value& holder,
                                      const std::string& name, const Value& reviverFn) {
    // Step 1: Let val be ? Get(holder, name) — uses prototype chain
    Value val = jsonGet(interp, holder, name);

    // Step 2: If val is an Object, walk its properties
    if (isJSONObjectLike(val)) {
        if (jsonIsArrayValue(interp, val)) {
            size_t length = static_cast<size_t>(jsonLengthOfArrayLike(interp, val));
            for (size_t i = 0; i < length; ++i) {
                std::string idxStr = std::to_string(i);
                Value newElement = internalizeJSONProperty(interp, val, idxStr, reviverFn);
                if (newElement.isUndefined()) {
                    jsonDeleteProperty(interp, val, idxStr);
                } else {
                    jsonCreateDataProperty(interp, val, idxStr, newElement);
                }
            }
        } else {
            for (const auto& key : jsonEnumerableOwnKeys(interp, val)) {
                Value newElement = internalizeJSONProperty(interp, val, key, reviverFn);
                if (newElement.isUndefined()) {
                    jsonDeleteProperty(interp, val, key);
                } else {
                    jsonCreateDataProperty(interp, val, key, newElement);
                }
            }
        }
    }

    // Step 3: Call reviver(name, val, context)
    // ES2024: context is {source: <original JSON text>} for primitives, {} for objects/arrays
    auto context = GarbageCollector::makeGC<Object>();
    if (auto objProto = interp->resolveVariable("__object_prototype__"); objProto.has_value()) {
        context->properties["__proto__"] = *objProto;
    }
    // Look up source annotation on the holder — only for primitive values
    // If the value was forward-modified, source should not be included
    if (val.isNumber() || val.isString() || val.isBool() || val.isNull() || val.isBigInt()) {
      std::string sourceKey = "__json_source_" + name;
      OrderedMap<std::string, Value>* holderProps = nullptr;
      if (holder.isObject()) holderProps = &holder.getGC<Object>()->properties;
      else if (holder.isArray()) holderProps = &holder.getGC<Array>()->properties;
      if (holderProps) {
        auto srcIt = holderProps->find(sourceKey);
        if (srcIt != holderProps->end() && srcIt->second.isString()) {
          // Verify source matches current value (detects forward modifications)
          const std::string& src = std::get<std::string>(srcIt->second.data);
          bool matches = false;
          if (val.isNull() && src == "null") matches = true;
          else if (val.isBool() && ((val.toBool() && src == "true") || (!val.toBool() && src == "false"))) matches = true;
          else if (val.isNumber()) {
            // Parse source and compare
            try {
              double parsed = std::stod(src);
              double actual = std::get<double>(val.data);
              if (parsed == actual || (std::isnan(parsed) && std::isnan(actual))) matches = true;
            } catch (...) {}
          } else if (val.isString()) {
            // Source includes quotes; parse it
            if (src.size() >= 2 && src.front() == '"' && src.back() == '"') {
              try {
                JSONParser srcParser(src);
                Value parsed = srcParser.parse();
                if (parsed.isString() && std::get<std::string>(parsed.data) == std::get<std::string>(val.data))
                  matches = true;
              } catch (...) {}
            }
          }
          if (matches) {
            context->properties["source"] = srcIt->second;
          }
        }
      }
    }
    return interp->callForHarness(reviverFn, {Value(name), val, Value(context)}, holder);
}

// JSON object implementation
Value JSON_parse(const std::vector<Value>& args) {
    if (args.empty()) {
        throw std::runtime_error("SyntaxError: Unexpected end of JSON input");
    }

    std::string jsonStr;
    if (args[0].isString()) {
        jsonStr = std::get<std::string>(args[0].data);
    } else {
        // ES spec: ToString(text) - call toString() method for objects
        Interpreter* interp = getGlobalInterpreter();
        if (interp && (args[0].isObject() || args[0].isArray())) {
            auto [found, toStringFn] = interp->getPropertyForExternal(args[0], "toString");
            if (found && toStringFn.isFunction()) {
                Value result = interp->callForHarness(toStringFn, {}, args[0]);
                if (interp->hasError()) {
                    Value err = interp->getError();
                    interp->clearError();
                    throw std::runtime_error(err.toString());
                }
                jsonStr = result.toString();
            } else {
                jsonStr = args[0].toString();
            }
        } else {
            jsonStr = args[0].toString();
        }
    }

    JSONParser parser(jsonStr);
    std::string rootSource;
    Value result = parser.parse(&rootSource);

    // Apply reviver if provided
    if (args.size() > 1 && args[1].isFunction()) {
        Interpreter* interp = getGlobalInterpreter();
        if (interp) {
            auto wrapper = makeObjectWithPrototype();
            wrapper->properties[""] = result;
            if (!rootSource.empty()) {
                wrapper->properties["__json_source_"] = Value(rootSource);
            }
            result = internalizeJSONProperty(interp, Value(wrapper), "", args[1]);
        }
    }

    return result;
}

Value JSON_stringify(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(Undefined{});
    }

    Value val = args[0];

    JSONStringifier stringifier;

    // Process replacer (2nd argument)
    if (args.size() > 1) {
        Value replacer = args[1];
        if (replacer.isFunction()) {
            stringifier.setReplacer(replacer);
        } else if (isJSONObjectLike(replacer) && jsonIsArrayValue(getGlobalInterpreter(), replacer)) {
            std::vector<std::string> propList;
            auto* interp = getGlobalInterpreter();
            size_t length = static_cast<size_t>(jsonLengthOfArrayLike(interp, replacer));
            for (size_t i = 0; i < length; ++i) {
                Value elem = jsonGet(interp, replacer, std::to_string(i));
                std::string item;
                bool hasItem = false;
                if (elem.isString()) {
                    item = std::get<std::string>(elem.data);
                    hasItem = true;
                } else if (elem.isNumber()) {
                    item = elem.toString();
                    hasItem = true;
                } else if (elem.isObject()) {
                    // ES spec: Object with [[StringData]] or [[NumberData]]
                    auto obj = elem.getGC<Object>();
                    auto pvIt = obj->properties.find("__primitive_value__");
                    if (pvIt != obj->properties.end()) {
                        if (pvIt->second.isString()) {
                            // Call ToString on the wrapper (uses toString method)
                            Interpreter* interp = getGlobalInterpreter();
                            if (interp) {
                                auto toStringIt = obj->properties.find("toString");
                                if (toStringIt != obj->properties.end() && toStringIt->second.isFunction()) {
                                    Value result = interp->callForHarness(toStringIt->second, {}, elem);
                                    if (!interp->hasError()) {
                                        item = result.toString();
                                        hasItem = true;
                                    } else {
                                        Value err = interp->getError();
                                        interp->clearError();
                                        throw std::runtime_error(err.toString());
                                    }
                                } else {
                                    item = pvIt->second.toString();
                                    hasItem = true;
                                }
                            } else {
                                item = pvIt->second.toString();
                                hasItem = true;
                            }
                        } else if (pvIt->second.isNumber()) {
                            // Call ToString on the wrapper
                            Interpreter* interp = getGlobalInterpreter();
                            if (interp) {
                                auto toStringIt = obj->properties.find("toString");
                                if (toStringIt != obj->properties.end() && toStringIt->second.isFunction()) {
                                    Value result = interp->callForHarness(toStringIt->second, {}, elem);
                                    if (!interp->hasError()) {
                                        item = result.toString();
                                        hasItem = true;
                                    } else {
                                        Value err = interp->getError();
                                        interp->clearError();
                                        throw std::runtime_error(err.toString());
                                    }
                                } else {
                                    item = pvIt->second.toString();
                                    hasItem = true;
                                }
                            } else {
                                item = pvIt->second.toString();
                                hasItem = true;
                            }
                        }
                    }
                }
                if (hasItem) {
                    // Avoid duplicates
                    bool found = false;
                    for (const auto& p : propList) if (p == item) { found = true; break; }
                    if (!found) propList.push_back(item);
                }
            }
            stringifier.setPropertyList(propList);
        }
    }

    // Process space (3rd argument)
    if (args.size() > 2) {
        Value space = args[2];
        // ES spec: If space is an object, convert via ToNumber or ToString
        if (space.isObject()) {
            auto obj = space.getGC<Object>();
            auto primIt = obj->properties.find("__primitive_value__");
            if (primIt != obj->properties.end()) {
                // Check if it's a Number or String wrapper
                if (primIt->second.isNumber()) {
                    // ToNumber: call valueOf if overridden
                    auto valueOfIt = obj->properties.find("valueOf");
                    if (valueOfIt != obj->properties.end() && valueOfIt->second.isFunction()) {
                        auto* interp = getGlobalInterpreter();
                        if (interp) {
                            space = interp->callForHarness(valueOfIt->second, {}, space);
                        } else {
                            space = primIt->second;
                        }
                    } else {
                        space = primIt->second;
                    }
                } else if (primIt->second.isString()) {
                    // ToString: call toString if overridden
                    auto toStringIt = obj->properties.find("toString");
                    if (toStringIt != obj->properties.end() && toStringIt->second.isFunction()) {
                        auto* interp = getGlobalInterpreter();
                        if (interp) {
                            space = interp->callForHarness(toStringIt->second, {}, space);
                        } else {
                            space = primIt->second;
                        }
                    } else {
                        space = primIt->second;
                    }
                } else {
                    space = primIt->second;
                }
            }
        }
        if (space.isNumber()) {
            int n = std::min(10, std::max(0, static_cast<int>(space.toNumber())));
            if (n > 0) {
                stringifier.setGap(std::string(n, ' '));
            }
        } else if (space.isString()) {
            std::string s = std::get<std::string>(space.data);
            if (s.size() > 10) s = s.substr(0, 10);
            if (!s.empty()) {
                stringifier.setGap(s);
            }
        }
    }

    // Top-level: apply replacer to val first via the holder
    // The stringifier handles this internally

    std::string result = stringifier.stringify(val);
    if (result.empty() && (val.isUndefined() || val.isFunction() || val.isClass() || val.isSymbol())) {
        return Value(Undefined{});
    }
    if (result.empty()) {
        return Value(Undefined{});
    }
    return Value(result);
}

} // namespace lightjs
