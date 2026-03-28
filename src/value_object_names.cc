#include "value_internal.h"

namespace lightjs {

Value Object_getOwnPropertyNames(const std::vector<Value>& args) {
  auto result = makeArrayWithPrototype();

  if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
    throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
  }

  // Primitives: ToObject then get own property names
  if (args[0].isNumber() || args[0].isBool() || args[0].isBigInt() || args[0].isSymbol()) {
    return Value(result); // No own string-keyed properties
  }
  if (args[0].isString()) {
    const auto& str = std::get<std::string>(args[0].data);
    // String objects have indices 0..n-1 and "length"
    size_t cpIdx = 0, bytePos = 0;
    while (bytePos < str.size()) {
      result->elements.push_back(Value(std::to_string(cpIdx)));
      unsigned char c = str[bytePos];
      if (c < 0x80) bytePos += 1;
      else if ((c & 0xE0) == 0xC0) bytePos += 2;
      else if ((c & 0xF0) == 0xE0) bytePos += 3;
      else bytePos += 4;
      cpIdx++;
    }
    result->elements.push_back(Value(std::string("length")));
    return Value(result);
  }

  auto parseArrayIndexKey = [](const std::string& key, uint32_t& out) -> bool {
    if (key.empty()) return false;
    if (key.size() > 1 && key[0] == '0') return false;
    for (char c : key) {
      if (c < '0' || c > '9') return false;
    }
    try {
      unsigned long long parsed = std::stoull(key);
      if (parsed >= static_cast<unsigned long long>(std::numeric_limits<uint32_t>::max())) {
        return false;
      }
      out = static_cast<uint32_t>(parsed);
      return true;
    } catch (...) {
      return false;
    }
  };

  auto appendInOwnPropertyKeyOrder = [&](const std::vector<std::string>& keys,
                                         bool prioritizeLengthName) {
    std::vector<std::pair<uint32_t, std::string>> indexKeys;
    std::vector<std::string> stringKeys;
    for (const auto& k : keys) {
      uint32_t idx = 0;
      if (parseArrayIndexKey(k, idx)) {
        indexKeys.emplace_back(idx, k);
      } else {
        stringKeys.push_back(k);
      }
    }
    std::sort(indexKeys.begin(), indexKeys.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [_, k] : indexKeys) {
      result->elements.push_back(Value(k));
    }
    if (prioritizeLengthName) {
      auto emitFirst = [&](const char* key) {
        for (size_t i = 0; i < stringKeys.size(); i++) {
          if (stringKeys[i] == key) {
            result->elements.push_back(Value(stringKeys[i]));
            stringKeys.erase(stringKeys.begin() + static_cast<long>(i));
            return;
          }
        }
      };
      emitFirst("length");
      emitFirst("name");
    }
    for (const auto& k : stringKeys) {
      result->elements.push_back(Value(k));
    }
  };

  if (args[0].isFunction()) {
    auto fn = args[0].getGC<Function>();
    std::vector<std::string> keys;
    keys.reserve(fn->properties.size());
    for (const auto& rawKey : fn->properties.orderedKeys()) {
      if (isInternalProperty(rawKey)) continue;
      if (isSymbolPropertyKey(rawKey)) continue;
      keys.push_back(rawKey);
    }
    appendInOwnPropertyKeyOrder(keys, true);
    return Value(result);
  }

  if (args[0].isClass()) {
    auto cls = args[0].getGC<Class>();
    std::unordered_set<std::string> seen;
    std::vector<std::string> keys;
    keys.reserve(cls->properties.size());
    for (const auto& rawKey : cls->properties.orderedKeys()) {
      if (rawKey.size() >= 4 && rawKey.substr(0, 2) == "__" &&
          rawKey.substr(rawKey.size() - 2) == "__") {
        continue;
      }
      if (isSymbolPropertyKey(rawKey)) continue;
      if (rawKey.rfind("__non_enum_", 0) == 0 ||
          rawKey.rfind("__non_writable_", 0) == 0 ||
          rawKey.rfind("__non_configurable_", 0) == 0 ||
          rawKey.rfind("__enum_", 0) == 0) {
        continue;
      }
      std::string exposed = rawKey;
      if (rawKey.rfind("__get_", 0) == 0 || rawKey.rfind("__set_", 0) == 0) {
        exposed = rawKey.substr(6);
      }
      if (seen.insert(exposed).second) {
        keys.push_back(exposed);
      }
    }
    appendInOwnPropertyKeyOrder(keys, true);
    return Value(result);
  }

  if (args[0].isArray()) {
    auto arr = args[0].getGC<Array>();
    // Add numeric index keys for non-hole, non-deleted elements
    std::vector<std::string> keys;
    for (size_t i = 0; i < arr->elements.size(); i++) {
      std::string key = std::to_string(i);
      if (arr->properties.find("__deleted_" + key + "__") != arr->properties.end()) continue;
      if (arr->properties.find("__hole_" + key + "__") != arr->properties.end()) continue;
      keys.push_back(key);
    }
    // Add "length"
    if (arr->properties.find("__deleted_length__") == arr->properties.end()) {
      keys.push_back("length");
    }
    // Add non-index, non-internal own properties
    for (const auto& rawKey : arr->properties.orderedKeys()) {
      if (isInternalProperty(rawKey)) continue;
      if (isSymbolPropertyKey(rawKey)) continue;
      // Skip already-added numeric indices and length
      if (rawKey == "length") continue;
      uint32_t idx2;
      if (parseArrayIndexKey(rawKey, idx2)) continue;
      keys.push_back(rawKey);
    }
    for (const auto& k : keys) {
      result->elements.push_back(Value(k));
    }
    return Value(result);
  }

  if (args[0].isError()) {
    auto err = args[0].getGC<Error>();
    std::vector<std::string> keys;
    for (const auto& rawKey : err->properties.orderedKeys()) {
      if (isInternalProperty(rawKey)) continue;
      if (isSymbolPropertyKey(rawKey)) continue;
      keys.push_back(rawKey);
    }
    appendInOwnPropertyKeyOrder(keys, false);
    return Value(result);
  }

  if (args[0].isPromise()) {
    auto p = args[0].getGC<Promise>();
    std::vector<std::string> keys;
    for (const auto& rawKey : p->properties.orderedKeys()) {
      if (isInternalProperty(rawKey)) continue;
      if (isSymbolPropertyKey(rawKey)) continue;
      keys.push_back(rawKey);
    }
    appendInOwnPropertyKeyOrder(keys, false);
    return Value(result);
  }

  if (args[0].isRegex()) {
    auto rx = args[0].getGC<Regex>();
    std::vector<std::string> keys;
    // Add standard regex properties
    keys.push_back("source");
    keys.push_back("flags");
    for (const auto& rawKey : rx->properties.orderedKeys()) {
      if (isInternalProperty(rawKey)) continue;
      if (isSymbolPropertyKey(rawKey)) continue;
      if (rawKey == "source" || rawKey == "flags") continue;
      keys.push_back(rawKey);
    }
    appendInOwnPropertyKeyOrder(keys, false);
    return Value(result);
  }

  if (!args[0].isObject()) {
    return Value(result);
  }

  auto obj = args[0].getGC<Object>();

  if (obj->isModuleNamespace) {
    triggerDeferredNamespaceOwnKeys(obj);
    for (const auto& key : moduleNamespaceExportNames(obj)) {
      result->elements.push_back(Value(key));
    }
    return Value(result);
  }

  std::unordered_set<std::string> seen;
  std::vector<std::string> keys;
  keys.reserve(obj->properties.size());
  for (const auto& rawKey : obj->properties.orderedKeys()) {
    if (rawKey.size() >= 4 && rawKey.substr(0, 2) == "__" &&
        rawKey.substr(rawKey.size() - 2) == "__") {
      continue;
    }
    if (isSymbolPropertyKey(rawKey)) continue;
    if (rawKey.rfind("__non_enum_", 0) == 0 ||
        rawKey.rfind("__non_writable_", 0) == 0 ||
        rawKey.rfind("__non_configurable_", 0) == 0 ||
        rawKey.rfind("__enum_", 0) == 0 ||
        rawKey.rfind("__json_source_", 0) == 0) {
      continue;
    }
    std::string exposed = rawKey;
    if (rawKey.rfind("__get_", 0) == 0 || rawKey.rfind("__set_", 0) == 0) {
      exposed = rawKey.substr(6);
    }
    if (seen.insert(exposed).second) {
      keys.push_back(exposed);
    }
  }
  appendInOwnPropertyKeyOrder(keys, false);

  return Value(result);
}

Value Object_create(const std::vector<Value>& args) {
  auto newObj = GarbageCollector::makeGC<Object>();
  auto isObjectLikePrototype = [](const Value& value) {
    return value.isObject() || value.isArray() || value.isFunction() || value.isRegex() ||
           value.isProxy() || value.isPromise() || value.isGenerator() || value.isClass() ||
           value.isMap() || value.isSet() || value.isWeakMap() || value.isWeakSet() ||
           value.isTypedArray() || value.isArrayBuffer() || value.isDataView() || value.isError();
  };

  // Object.create(proto[, propertiesObject])
  if (!args.empty()) {
    const Value& proto = args[0];
    if (isObjectLikePrototype(proto)) {
      newObj->properties["__proto__"] = proto;
    } else if (proto.isNull()) {
      newObj->properties["__proto__"] = Value(Null{});
    } else if (!proto.isUndefined()) {
      // Spec: TypeError if proto is neither Object nor null.
      throw std::runtime_error("TypeError: Object prototype may only be an Object or null");
    }
  }

  if (args.size() > 1 && args[1].isObject()) {
    // Add properties from the properties descriptor object
    // Use orderedKeys() for spec-compliant property creation order
    auto props = args[1].getGC<Object>();
    for (const auto& key : props->properties.orderedKeys()) {
      // Skip internal property markers
      if (key.size() >= 4 && key.substr(0, 2) == "__" && key.substr(key.size() - 2) == "__") continue;
      if (key.rfind("__non_writable_", 0) == 0 || key.rfind("__non_enum_", 0) == 0 ||
          key.rfind("__non_configurable_", 0) == 0 || key.rfind("__enum_", 0) == 0 ||
          key.rfind("__get_", 0) == 0 || key.rfind("__set_", 0) == 0) continue;
      auto descIt = props->properties.find(key);
      if (descIt == props->properties.end() || !descIt->second.isObject()) continue;
      auto desc = std::get<GCPtr<Object>>(descIt->second.data);
      auto valueIt = desc->properties.find("value");
      auto getIt = desc->properties.find("get");
      auto setIt = desc->properties.find("set");
      if (valueIt != desc->properties.end()) {
        newObj->properties[key] = valueIt->second;
      } else if (getIt != desc->properties.end() || setIt != desc->properties.end()) {
        newObj->properties[key] = Value(Undefined{});
      }
      if (getIt != desc->properties.end() && getIt->second.isFunction()) {
        newObj->properties["__get_" + key] = getIt->second;
      }
      if (setIt != desc->properties.end() && setIt->second.isFunction()) {
        newObj->properties["__set_" + key] = setIt->second;
      }
      // Ensure property key exists even if no value/get/set
      if (newObj->properties.find(key) == newObj->properties.end()) {
        newObj->properties[key] = Value(Undefined{});
      }
      // Validate descriptor
      bool hasGetField = getIt != desc->properties.end();
      bool hasSetField = setIt != desc->properties.end();
      bool hasValueField = valueIt != desc->properties.end();
      auto writableIt = desc->properties.find("writable");
      bool hasWritableField = writableIt != desc->properties.end();
      // Validate: cannot mix data and accessor
      if ((hasGetField || hasSetField) && (hasValueField || hasWritableField)) {
        throw std::runtime_error("TypeError: Invalid property descriptor. Cannot both specify accessors and a value or writable attribute");
      }
      // Validate: getter/setter must be callable or undefined
      if (hasGetField && !getIt->second.isUndefined() && !getIt->second.isFunction()) {
        throw std::runtime_error("TypeError: Getter must be a function: " + getIt->second.toString());
      }
      if (hasSetField && !setIt->second.isUndefined() && !setIt->second.isFunction()) {
        throw std::runtime_error("TypeError: Setter must be a function: " + setIt->second.toString());
      }
      // Attributes default to false for defineProperty/Object.create
      bool isAccessor = hasGetField || hasSetField;
      if (!isAccessor) {
        if (hasWritableField && writableIt->second.toBool()) {
          newObj->properties.erase("__non_writable_" + key);
        } else {
          newObj->properties["__non_writable_" + key] = Value(true);
        }
      }
      auto enumIt = desc->properties.find("enumerable");
      if (enumIt != desc->properties.end() && enumIt->second.toBool()) {
        newObj->properties.erase("__non_enum_" + key);
      } else {
        newObj->properties["__non_enum_" + key] = Value(true);
      }
      auto configIt = desc->properties.find("configurable");
      if (configIt != desc->properties.end() && configIt->second.toBool()) {
        newObj->properties.erase("__non_configurable_" + key);
      } else {
        newObj->properties["__non_configurable_" + key] = Value(true);
      }
    }
  }

  return Value(newObj);
}

Value Object_fromEntries(const std::vector<Value>& args) {
  auto newObj = makeObjectWithPrototype();

  if (args.empty()) {
    throw std::runtime_error("TypeError: Object.fromEntries requires an iterable argument");
  }
  if (args[0].isNull() || args[0].isUndefined()) {
    throw std::runtime_error("TypeError: Object.fromEntries requires an iterable argument");
  }

  auto* interp = getGlobalInterpreter();
  if (!interp) {
    throw std::runtime_error("TypeError: Object.fromEntries requires an interpreter");
  }

  const Value& iterable = args[0];

  // Get @@iterator method
  const auto& iterKey = WellKnownSymbols::iteratorKey();
  auto [hasIter, iterMethod] = interp->getPropertyForExternal(iterable, iterKey);
  if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
  if (!hasIter || !iterMethod.isFunction()) {
    throw std::runtime_error("TypeError: object is not iterable");
  }

  // Call @@iterator to get iterator object
  Value iteratorObj = interp->callForHarness(iterMethod, {}, iterable);
  if (interp->hasError()) {
    Value err = interp->getError(); interp->clearError();
    throw JsValueException(err);
  }

  // Get next method
  Value nextFn;
  bool isGen = iteratorObj.isGenerator();
  if (!isGen) {
    auto [nf, fn] = interp->getPropertyForExternal(iteratorObj, "next");
    if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
    if (!nf || !fn.isFunction()) {
      throw std::runtime_error("TypeError: iterator result is not an object");
    }
    nextFn = fn;
  }

  // Cache return method for iterator closing
  Value returnFn;
  if (!isGen) {
    auto [hr, rf] = interp->getPropertyForExternal(iteratorObj, "return");
    if (interp->hasError()) interp->clearError();
    if (hr && rf.isFunction()) returnFn = rf;
  }
  auto closeIter = [&]() {
    if (returnFn.isFunction()) {
      interp->callForHarness(returnFn, {}, iteratorObj);
      if (interp->hasError()) interp->clearError();
    }
  };

  // Iterate
  for (int i = 0; i < 100000; ++i) {
    Value step;
    if (isGen) {
      step = interp->generatorNext(iteratorObj, Value(Undefined{}));
    } else {
      step = interp->callForHarness(nextFn, {}, iteratorObj);
    }
    if (interp->hasError()) {
      Value err = interp->getError(); interp->clearError();
      throw JsValueException(err);
    }

    // Check done
    auto [hasDone, doneVal] = interp->getPropertyForExternal(step, "done");
    if (interp->hasError()) {
      Value err = interp->getError(); interp->clearError();
      throw JsValueException(err);
    }
    if (hasDone && doneVal.toBool()) break;

    // Get value
    auto [hasVal, entry] = interp->getPropertyForExternal(step, "value");
    if (interp->hasError()) {
      Value err = interp->getError(); interp->clearError();
      throw JsValueException(err);
    }

    // Entry must be an object (array-like with [0] and [1])
    if (entry.isNull() || entry.isUndefined() || entry.isBool() ||
        entry.isNumber() || entry.isBigInt() || entry.isSymbol()) {
      closeIter();
      throw std::runtime_error("TypeError: Iterator value " + entry.toString() + " is not an entry object");
    }

    // String entries: "ab" → key=0, value=1 (char at index 0 and 1)
    if (entry.isString()) {
      const std::string& str = std::get<std::string>(entry.data);
      // Strings are array-like; access [0] and [1] via getPropertyForExternal
      auto [k0, key] = interp->getPropertyForExternal(entry, "0");
      auto [k1, val] = interp->getPropertyForExternal(entry, "1");
      if (interp->hasError()) {
        Value err = interp->getError(); interp->clearError();
        closeIter();
        throw JsValueException(err);
      }
      if (!k0 || !k1) {
        closeIter();
        throw std::runtime_error("TypeError: Iterator value " + str + " is not an entry object");
      }
      newObj->properties[valueToPropertyKey(key)] = val;
      continue;
    }

    // Get key (entry[0]) and value (entry[1]) via property access
    auto [k0, keyVal] = interp->getPropertyForExternal(entry, "0");
    if (interp->hasError()) {
      Value err = interp->getError(); interp->clearError();
      closeIter();
      throw JsValueException(err);
    }
    auto [k1, valVal] = interp->getPropertyForExternal(entry, "1");
    if (interp->hasError()) {
      Value err = interp->getError(); interp->clearError();
      closeIter();
      throw JsValueException(err);
    }

    std::string key = valueToPropertyKey(keyVal);
    newObj->properties[key] = valVal;
  }

  return Value(newObj);
}

}  // namespace lightjs
