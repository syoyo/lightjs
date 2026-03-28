#include "json_internal.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace lightjs::detail {

namespace {

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

}  // namespace

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

}  // namespace lightjs::detail
