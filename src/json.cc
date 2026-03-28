#include "json_internal.h"
#include <cmath>
#include <stdexcept>

namespace lightjs {

namespace {

Value internalizeJSONProperty(Interpreter* interp,
                              const Value& holder,
                              const std::string& name,
                              const Value& reviverFn) {
    Value val = detail::jsonGet(interp, holder, name);

    if (detail::isJSONObjectLike(val)) {
        if (detail::jsonIsArrayValue(interp, val)) {
            size_t length = static_cast<size_t>(detail::jsonLengthOfArrayLike(interp, val));
            for (size_t i = 0; i < length; ++i) {
                std::string idxStr = std::to_string(i);
                Value newElement = internalizeJSONProperty(interp, val, idxStr, reviverFn);
                if (newElement.isUndefined()) {
                    detail::jsonDeleteProperty(interp, val, idxStr);
                } else {
                    detail::jsonCreateDataProperty(interp, val, idxStr, newElement);
                }
            }
        } else {
            for (const auto& key : detail::jsonEnumerableOwnKeys(interp, val)) {
                Value newElement = internalizeJSONProperty(interp, val, key, reviverFn);
                if (newElement.isUndefined()) {
                    detail::jsonDeleteProperty(interp, val, key);
                } else {
                    detail::jsonCreateDataProperty(interp, val, key, newElement);
                }
            }
        }
    }

    auto context = GarbageCollector::makeGC<Object>();
    if (auto objProto = interp->resolveVariable("__object_prototype__"); objProto.has_value()) {
        context->properties["__proto__"] = *objProto;
    }
    if (val.isNumber() || val.isString() || val.isBool() || val.isNull() || val.isBigInt()) {
        std::string sourceKey = "__json_source_" + name;
        OrderedMap<std::string, Value>* holderProps = nullptr;
        if (holder.isObject()) holderProps = &holder.getGC<Object>()->properties;
        else if (holder.isArray()) holderProps = &holder.getGC<Array>()->properties;
        if (holderProps) {
            auto srcIt = holderProps->find(sourceKey);
            if (srcIt != holderProps->end() && srcIt->second.isString()) {
                const std::string& src = std::get<std::string>(srcIt->second.data);
                bool matches = false;
                if (val.isNull() && src == "null") matches = true;
                else if (val.isBool() && ((val.toBool() && src == "true") || (!val.toBool() && src == "false"))) matches = true;
                else if (val.isNumber()) {
                    try {
                        double parsed = std::stod(src);
                        double actual = std::get<double>(val.data);
                        if (parsed == actual || (std::isnan(parsed) && std::isnan(actual))) matches = true;
                    } catch (...) {
                    }
                } else if (val.isString()) {
                    if (src.size() >= 2 && src.front() == '"' && src.back() == '"') {
                        try {
                            detail::JSONParser srcParser(src);
                            Value parsed = srcParser.parse();
                            if (parsed.isString() &&
                                std::get<std::string>(parsed.data) == std::get<std::string>(val.data)) {
                                matches = true;
                            }
                        } catch (...) {
                        }
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

}  // namespace

Value JSON_parse(const std::vector<Value>& args) {
    if (args.empty()) {
        throw std::runtime_error("SyntaxError: Unexpected end of JSON input");
    }

    std::string jsonStr;
    if (args[0].isString()) {
        jsonStr = std::get<std::string>(args[0].data);
    } else {
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

    detail::JSONParser parser(jsonStr);
    std::string rootSource;
    Value result = parser.parse(&rootSource);

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
    detail::JSONStringifier stringifier;

    if (args.size() > 1) {
        Value replacer = args[1];
        if (replacer.isFunction()) {
            stringifier.setReplacer(replacer);
        } else if (detail::isJSONObjectLike(replacer) && detail::jsonIsArrayValue(getGlobalInterpreter(), replacer)) {
            std::vector<std::string> propList;
            auto* interp = getGlobalInterpreter();
            size_t length = static_cast<size_t>(detail::jsonLengthOfArrayLike(interp, replacer));
            for (size_t i = 0; i < length; ++i) {
                Value elem = detail::jsonGet(interp, replacer, std::to_string(i));
                std::string item;
                bool hasItem = false;
                if (elem.isString()) {
                    item = std::get<std::string>(elem.data);
                    hasItem = true;
                } else if (elem.isNumber()) {
                    item = elem.toString();
                    hasItem = true;
                } else if (elem.isObject()) {
                    auto obj = elem.getGC<Object>();
                    auto pvIt = obj->properties.find("__primitive_value__");
                    if (pvIt != obj->properties.end()) {
                        if (pvIt->second.isString()) {
                            Interpreter* innerInterp = getGlobalInterpreter();
                            if (innerInterp) {
                                auto toStringIt = obj->properties.find("toString");
                                if (toStringIt != obj->properties.end() && toStringIt->second.isFunction()) {
                                    Value result = innerInterp->callForHarness(toStringIt->second, {}, elem);
                                    if (!innerInterp->hasError()) {
                                        item = result.toString();
                                        hasItem = true;
                                    } else {
                                        Value err = innerInterp->getError();
                                        innerInterp->clearError();
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
                            Interpreter* innerInterp = getGlobalInterpreter();
                            if (innerInterp) {
                                auto toStringIt = obj->properties.find("toString");
                                if (toStringIt != obj->properties.end() && toStringIt->second.isFunction()) {
                                    Value result = innerInterp->callForHarness(toStringIt->second, {}, elem);
                                    if (!innerInterp->hasError()) {
                                        item = result.toString();
                                        hasItem = true;
                                    } else {
                                        Value err = innerInterp->getError();
                                        innerInterp->clearError();
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
                    bool found = false;
                    for (const auto& p : propList) {
                        if (p == item) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) propList.push_back(item);
                }
            }
            stringifier.setPropertyList(propList);
        }
    }

    if (args.size() > 2) {
        Value space = args[2];
        if (space.isObject()) {
            auto obj = space.getGC<Object>();
            auto primIt = obj->properties.find("__primitive_value__");
            if (primIt != obj->properties.end()) {
                if (primIt->second.isNumber()) {
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

    std::string result = stringifier.stringify(val);
    if (result.empty() && (val.isUndefined() || val.isFunction() || val.isClass() || val.isSymbol())) {
        return Value(Undefined{});
    }
    if (result.empty()) {
        return Value(Undefined{});
    }
    return Value(result);
}

}  // namespace lightjs
