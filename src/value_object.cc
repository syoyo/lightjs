#include "value_internal.h"

namespace lightjs {

Value Object_keys(const std::vector<Value>& args) {
  auto result = makeArrayWithPrototype();
  if (args.empty()) {
    throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
  }

  const auto& arg = args[0];
  // Throw TypeError for null/undefined
  if (arg.isNull() || arg.isUndefined()) {
    throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
  }

  // Handle string primitives: Object.keys("abc") → ["0","1","2"]
  if (arg.isString()) {
    const std::string& str = std::get<std::string>(arg.data);
    // Count code points for proper Unicode support
    size_t cpIdx = 0;
    size_t bytePos = 0;
    while (bytePos < str.size()) {
      result->elements.push_back(Value(std::to_string(cpIdx)));
      // Skip UTF-8 bytes
      unsigned char c = str[bytePos];
      if (c < 0x80) bytePos += 1;
      else if ((c & 0xE0) == 0xC0) bytePos += 2;
      else if ((c & 0xF0) == 0xE0) bytePos += 3;
      else bytePos += 4;
      cpIdx++;
    }
    return Value(result);
  }

  // Handle Object with module namespace
  if (arg.isObject()) {
    auto obj = arg.getGC<Object>();
    if (obj->isModuleNamespace) {
      for (const auto& key : moduleNamespaceExportNames(obj)) {
        auto getterIt = obj->properties.find("__get_" + key);
        if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
          auto getter = getterIt->second.getGC<Function>();
          if (getter && getter->isNative) {
            getter->nativeFunc({});
          }
        }
        result->elements.push_back(Value(key));
      }
      return Value(result);
    }
  }

  // Handle number/boolean primitives - no enumerable own properties
  if (arg.isNumber() || arg.isBool()) {
    return Value(result);
  }

  // For Arrays, include numeric indices first
  if (arg.isArray()) {
    auto arr = arg.getGC<Array>();
    for (size_t i = 0; i < arr->elements.size(); ++i) {
      std::string key = std::to_string(i);
      // Skip non-enumerable
      if (arr->properties.count("__non_enum_" + key)) continue;
      // Skip holes (deleted or never-set elements)
      if (arr->properties.count("__deleted_" + key + "__")) continue;
      if (arr->properties.count("__hole_" + key + "__")) continue;
      result->elements.push_back(Value(key));
    }
    // Then add non-index enumerable properties (including large indices stored as properties)
    for (const auto& key : arr->properties.orderedKeys()) {
      if (arr->properties.count("__non_enum_" + key)) continue;
      if (key.find("__") == 0) continue; // skip internal markers
      // Skip numeric indices already added from elements array
      uint32_t idx = 0;
      if (isArrayIndex(key, idx) && idx < arr->elements.size()) continue;
      if (key == "length") continue; // length is not enumerable
      result->elements.push_back(Value(key));
    }
    return Value(result);
  }

  auto* props = getPropertiesMap(arg);
  if (!props) return Value(result);

  for (const auto& key : sortOwnPropertyKeys(props->orderedKeys())) {
    if (props->count("__non_enum_" + key)) continue;
    result->elements.push_back(Value(key));
  }

  return Value(result);
}

Value Object_values(const std::vector<Value>& args) {
  auto result = makeArrayWithPrototype();
  if (args.empty()) {
    throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
  }

  const auto& arg = args[0];
  if (arg.isNull() || arg.isUndefined()) {
    throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
  }

  // Handle string primitives: Object.values("abc") → ["a","b","c"]
  if (arg.isString()) {
    const std::string& str = std::get<std::string>(arg.data);
    size_t bytePos = 0;
    while (bytePos < str.size()) {
      unsigned char c = str[bytePos];
      size_t len = 1;
      if (c >= 0x80) {
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else len = 4;
      }
      result->elements.push_back(Value(str.substr(bytePos, len)));
      bytePos += len;
    }
    return Value(result);
  }

  if (arg.isNumber() || arg.isBool()) {
    return Value(result);
  }

  // For Arrays, include element values
  if (arg.isArray()) {
    auto arr = arg.getGC<Array>();
    Interpreter* interp = getGlobalInterpreter();
    for (size_t i = 0; i < arr->elements.size(); ++i) {
      std::string key = std::to_string(i);
      if (!arr->properties.count("__non_enum_" + key)) {
        if (interp) {
          auto [found, val] = interp->getPropertyForExternal(arg, key);
          if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
          if (found) result->elements.push_back(val);
        } else {
          result->elements.push_back(arr->elements[i]);
        }
      }
    }
    return Value(result);
  }

  auto* props = getPropertiesMap(arg);
  if (!props) return Value(result);

  Interpreter* interp = getGlobalInterpreter();
  for (const auto& key : sortOwnPropertyKeys(props->orderedKeys())) {
    if (props->count("__non_enum_" + key)) continue;
    if (interp) {
      auto [found, val] = interp->getPropertyForExternal(arg, key);
      if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
      if (found) result->elements.push_back(val);
    } else {
      auto it = props->find(key);
      result->elements.push_back(it->second);
    }
  }

  return Value(result);
}

Value Object_entries(const std::vector<Value>& args) {
  auto result = makeArrayWithPrototype();
  if (args.empty()) {
    throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
  }

  const auto& arg = args[0];
  if (arg.isNull() || arg.isUndefined()) {
    throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
  }

  // Handle string primitives: Object.entries("abc") → [["0","a"],["1","b"],["2","c"]]
  if (arg.isString()) {
    const std::string& str = std::get<std::string>(arg.data);
    size_t cpIdx = 0;
    size_t bytePos = 0;
    while (bytePos < str.size()) {
      unsigned char c = str[bytePos];
      size_t len = 1;
      if (c >= 0x80) {
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else len = 4;
      }
      auto entry = makeArrayWithPrototype();
      entry->elements.push_back(Value(std::to_string(cpIdx)));
      entry->elements.push_back(Value(str.substr(bytePos, len)));
      result->elements.push_back(Value(entry));
      bytePos += len;
      cpIdx++;
    }
    return Value(result);
  }

  if (arg.isNumber() || arg.isBool()) {
    return Value(result);
  }

  // For Arrays, include element entries
  if (arg.isArray()) {
    auto arr = arg.getGC<Array>();
    Interpreter* interp = getGlobalInterpreter();
    for (size_t i = 0; i < arr->elements.size(); ++i) {
      std::string key = std::to_string(i);
      if (!arr->properties.count("__non_enum_" + key)) {
        auto entry = makeArrayWithPrototype();
        entry->elements.push_back(Value(key));
        if (interp) {
          auto [found, val] = interp->getPropertyForExternal(arg, key);
          if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
          entry->elements.push_back(found ? val : Value(Undefined{}));
        } else {
          entry->elements.push_back(arr->elements[i]);
        }
        result->elements.push_back(Value(entry));
      }
    }
    return Value(result);
  }

  auto* props = getPropertiesMap(arg);
  if (!props) return Value(result);

  Interpreter* interp = getGlobalInterpreter();
  for (const auto& key : sortOwnPropertyKeys(props->orderedKeys())) {
    if (props->count("__non_enum_" + key)) continue;
    auto entry = makeArrayWithPrototype();
    entry->elements.push_back(Value(key));
    if (interp) {
      auto [found, val] = interp->getPropertyForExternal(arg, key);
      if (interp->hasError()) {
        Value err = interp->getError();
        interp->clearError();
        throw JsValueException(err);
      }
      entry->elements.push_back(found ? val : Value(Undefined{}));
    } else {
      auto it = props->find(key);
      entry->elements.push_back(it->second);
    }
    result->elements.push_back(Value(entry));
  }

  return Value(result);
}

// Helper: get enumerable own string-keyed properties from a source value
// Returns pairs of (key, value) in ES property order
static std::vector<std::pair<std::string, Value>> getEnumerableOwnProperties(const Value& source) {
  std::vector<std::pair<std::string, Value>> result;

  // String sources: each character is an enumerable indexed property
  if (source.isString()) {
    const std::string& str = std::get<std::string>(source.data);
    // Iterate by UTF-16 code units for ES compat
    size_t i = 0;
    size_t idx = 0;
    while (i < str.size()) {
      unsigned char c = str[i];
      size_t charLen = 1;
      if ((c & 0x80) == 0) charLen = 1;
      else if ((c & 0xE0) == 0xC0) charLen = 2;
      else if ((c & 0xF0) == 0xE0) charLen = 3;
      else if ((c & 0xF8) == 0xF0) charLen = 4;
      std::string ch = str.substr(i, charLen);
      result.push_back({std::to_string(idx), Value(ch)});
      i += charLen;
      idx++;
    }
    return result;
  }

  // Number, Boolean, Symbol, BigInt: no own enumerable string-keyed properties
  if (source.isNumber() || source.isBool() || source.isSymbol() || source.isBigInt()) {
    return result;
  }

  // Array sources: indexed elements + own properties
  if (source.isArray()) {
    auto arr = source.getGC<Array>();
    // Add array elements (indexed, enumerable by default)
    for (size_t i = 0; i < arr->elements.size(); i++) {
      std::string key = std::to_string(i);
      // Skip deleted elements and holes
      if (arr->properties.find("__deleted_" + key + "__") != arr->properties.end()) continue;
      if (arr->properties.find("__hole_" + key + "__") != arr->properties.end()) continue;
      if (arr->properties.find("__non_enum_" + key) != arr->properties.end()) continue;
      result.push_back({key, arr->elements[i]});
    }
    // Add non-indexed own properties
    for (const auto& key : arr->properties.orderedKeys()) {
      if (isInternalProperty(key)) continue;
      if (isSymbolPropertyKey(key)) continue;
      if (arr->properties.count("__non_enum_" + key)) continue;
      // Skip numeric indices already handled
      uint32_t idx2;
      if (isArrayIndex(key, idx2) && idx2 < arr->elements.size()) continue;
      if (key == "length") {
        result.push_back({key, Value(static_cast<double>(arr->elements.size()))});
        continue;
      }
      auto it = arr->properties.find(key);
      if (it != arr->properties.end()) {
        result.push_back({key, it->second});
      }
    }
    return result;
  }

  // Generic object-like types: Object, Function, Class, etc.
  OrderedMap<std::string, Value>* props = nullptr;
  if (source.isObject()) props = &source.getGC<Object>()->properties;
  else if (source.isFunction()) props = &source.getGC<Function>()->properties;
  else if (source.isClass()) props = &source.getGC<Class>()->properties;
  else if (source.isPromise()) props = &source.getGC<Promise>()->properties;
  else if (source.isRegex()) props = &source.getGC<Regex>()->properties;
  else if (source.isError()) props = &source.getGC<Error>()->properties;
  else if (source.isMap()) props = &source.getGC<Map>()->properties;
  else if (source.isSet()) props = &source.getGC<Set>()->properties;

  if (!props) return result;

  auto* interp = getGlobalInterpreter();
  // Helper: get value via [[Get]] (handles accessor properties)
  auto getValue = [&](const std::string& key) -> Value {
    // Check for getter
    auto getterIt = props->find("__get_" + key);
    if (getterIt != props->end() && getterIt->second.isFunction() && interp) {
      Value val = interp->callForHarness(getterIt->second, {}, source);
      if (interp->hasError()) { Value err = interp->getError(); interp->clearError(); throw JsValueException(err); }
      return val;
    }
    auto it = props->find(key);
    return (it != props->end()) ? it->second : Value(Undefined{});
  };

  // String keys first (in property order: integer indices, then strings)
  for (const auto& key : sortOwnPropertyKeys(props->orderedKeys())) {
    if (isInternalProperty(key)) continue;
    if (isSymbolPropertyKey(key)) continue;
    if (props->count("__non_enum_" + key)) continue;
    auto it = props->find(key);
    if (it != props->end()) {
      result.push_back({key, getValue(key)});
    }
  }
  // Also check for accessor-only properties (have __get_ but no data entry)
  for (const auto& rawKey : props->orderedKeys()) {
    if (rawKey.size() <= 6 || rawKey.substr(0, 6) != "__get_") continue;
    std::string key = rawKey.substr(6);
    if (isInternalProperty(key)) continue;
    if (isSymbolPropertyKey(key)) continue;
    if (props->count("__non_enum_" + key)) continue;
    // Skip if already added as data property
    if (props->find(key) != props->end()) continue;
    result.push_back({key, getValue(key)});
  }
  // Symbol keys last (in insertion order)
  for (const auto& key : props->orderedKeys()) {
    if (!isSymbolPropertyKey(key)) continue;
    if (isInternalProperty(key)) continue;
    if (props->count("__non_enum_" + key)) continue;
    auto it = props->find(key);
    if (it != props->end()) {
      result.push_back({key, getValue(key)});
    }
  }
  // Symbol accessor-only properties
  for (const auto& rawKey : props->orderedKeys()) {
    if (rawKey.size() <= 6 || rawKey.substr(0, 6) != "__get_") continue;
    std::string key = rawKey.substr(6);
    if (!isSymbolPropertyKey(key)) continue;
    if (isInternalProperty(key)) continue;
    if (props->count("__non_enum_" + key)) continue;
    if (props->find(key) != props->end()) continue;
    result.push_back({key, getValue(key)});
  }
  return result;
}

// Helper: assign a property value to a target, respecting non-writable constraints
static void assignToTarget(Value& target, const std::string& key, const Value& value) {
  if (target.isArray()) {
    auto arr = target.getGC<Array>();
    // Check non-writable
    if (arr->properties.count("__non_writable_" + key)) {
      throw std::runtime_error("TypeError: Cannot assign to read only property '" + key + "'");
    }
    // Handle "length" specially for arrays
    if (key == "length" && value.isNumber()) {
      double newLen = value.toNumber();
      uint32_t len = static_cast<uint32_t>(newLen);
      if (len < arr->elements.size()) {
        arr->elements.resize(len);
      } else if (len > arr->elements.size()) {
        arr->elements.resize(len, Value(Undefined{}));
      }
      return;
    }
    // Handle numeric index
    uint32_t idx;
    if (isArrayIndex(key, idx)) {
      if (idx >= arr->elements.size()) {
        arr->elements.resize(idx + 1, Value(Undefined{}));
      }
      arr->elements[idx] = value;
      return;
    }
    arr->properties[key] = value;
    return;
  }

  OrderedMap<std::string, Value>* props = nullptr;
  if (target.isObject()) props = &target.getGC<Object>()->properties;
  else if (target.isFunction()) props = &target.getGC<Function>()->properties;
  else if (target.isClass()) props = &target.getGC<Class>()->properties;
  else if (target.isPromise()) props = &target.getGC<Promise>()->properties;
  else if (target.isError()) props = &target.getGC<Error>()->properties;

  if (!props) return;

  // Check if target has a setter for this property (accessor property)
  // Setter invocation happens even on sealed/frozen objects per spec
  auto setterIt = props->find("__set_" + key);
  if (setterIt != props->end() && setterIt->second.isFunction()) {
    auto* interp = getGlobalInterpreter();
    if (interp) {
      interp->callForHarness(setterIt->second, {value}, target);
      if (interp->hasError()) {
        Value err = interp->getError();
        interp->clearError();
        throw JsValueException(err);
      }
    }
    return;
  }

  // Data property: check non-writable
  if (props->count("__non_writable_" + key)) {
    throw std::runtime_error("TypeError: Cannot assign to read only property '" + key + "'");
  }

  // Check non-extensible: reject new properties
  if (props->find(key) == props->end() &&
      props->find("__get_" + key) == props->end() &&
      props->find("__set_" + key) == props->end()) {
    if (props->count("__non_extensible__") ||
        (target.isObject() && (target.getGC<Object>()->sealed || target.getGC<Object>()->nonExtensible || target.getGC<Object>()->frozen))) {
      throw std::runtime_error("TypeError: Cannot add property " + key + ", object is not extensible");
    }
  }

  (*props)[key] = value;
}

Value Object_assign(const std::vector<Value>& args) {
  if (args.empty()) {
    throw std::runtime_error("TypeError: Object.assign requires at least 1 argument");
  }

  Value target = args[0];
  if (target.isNull() || target.isUndefined()) {
    throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
  }

  // ToObject: if target is already an object-like type, keep it; otherwise wrap
  Value targetVal = target;
  if (target.isString() || target.isNumber() || target.isBool() ||
      target.isBigInt() || target.isSymbol()) {
    // Wrap primitive to object
    auto wrapped = GarbageCollector::makeGC<Object>();
    wrapped->properties["__primitive_value__"] = target;

    // For string targets, add indexed characters as non-writable, non-configurable, enumerable
    if (target.isString()) {
      const std::string& str = std::get<std::string>(target.data);
      size_t i = 0, idx = 0;
      while (i < str.size()) {
        unsigned char c = str[i];
        size_t charLen = 1;
        if ((c & 0x80) == 0) charLen = 1;
        else if ((c & 0xE0) == 0xC0) charLen = 2;
        else if ((c & 0xF0) == 0xE0) charLen = 3;
        else if ((c & 0xF8) == 0xF0) charLen = 4;
        std::string ch = str.substr(i, charLen);
        std::string key = std::to_string(idx);
        wrapped->properties[key] = Value(ch);
        wrapped->properties["__non_writable_" + key] = Value(true);
        wrapped->properties["__non_configurable_" + key] = Value(true);
        i += charLen;
        idx++;
      }
      wrapped->properties["length"] = Value(static_cast<double>(idx));
      wrapped->properties["__non_writable_length"] = Value(true);
      wrapped->properties["__non_enum_length"] = Value(true);
      wrapped->properties["__non_configurable_length"] = Value(true);
    }

    // Set __proto__ to the appropriate prototype for valueOf/toString inheritance
    auto* interp = getGlobalInterpreter();
    if (interp) {
      auto env = interp->getEnvironment();
      // Walk to global env
      while (env && env->getParent()) env = env->getParentPtr();
      if (env) {
        std::string ctorName;
        if (target.isNumber()) ctorName = "Number";
        else if (target.isBool()) ctorName = "Boolean";
        else if (target.isString()) ctorName = "String";
        else if (target.isSymbol()) ctorName = "Symbol";
        else if (target.isBigInt()) ctorName = "BigInt";
        if (!ctorName.empty()) {
          if (auto ctor = env->get(ctorName)) {
            OrderedMap<std::string, Value>* ctorProps = nullptr;
            if (ctor->isFunction()) {
              ctorProps = &ctor->getGC<Function>()->properties;
            } else if (ctor->isObject()) {
              ctorProps = &ctor->getGC<Object>()->properties;
            } else if (ctor->isClass()) {
              ctorProps = &ctor->getGC<Class>()->properties;
            }
            if (ctorProps) {
              auto protoIt = ctorProps->find("prototype");
              if (protoIt != ctorProps->end()) {
                wrapped->properties["__proto__"] = protoIt->second;
              }
            }
          }
        }
      }
    }

    targetVal = Value(wrapped);
  }

  // Copy properties from each source
  for (size_t i = 1; i < args.size(); ++i) {
    const Value& source = args[i];
    if (source.isNull() || source.isUndefined()) {
      continue; // Skip null/undefined sources
    }

    auto ownProps = getEnumerableOwnProperties(source);
    for (const auto& [key, value] : ownProps) {
      assignToTarget(targetVal, key, value);
    }
  }

  return targetVal;
}

Value Object_hasOwnProperty(const std::vector<Value>& args) {
  if (args.size() < 2) {
    // Still need to check this value
    if (!args.empty() && (args[0].isUndefined() || args[0].isNull())) {
      throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
    }
    return Value(false);
  }

  // Step 1: ToPropertyKey(V) - must happen before ToObject(this)
  std::string key = valueToPropertyKey(args[1]);

  // Step 2: ToObject(this) - throw TypeError for undefined/null
  if (args[0].isUndefined() || args[0].isNull()) {
    throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
  }

  // Handle Function objects
  if (args[0].isFunction()) {
    auto fn = args[0].getGC<Function>();
    // Internal properties are not own properties
    if (isInternalProperty(key)) return Value(false);
    if (fn->properties.find(key) != fn->properties.end()) return Value(true);
    if (fn->properties.find("__get_" + key) != fn->properties.end()) return Value(true);
    if (fn->properties.find("__set_" + key) != fn->properties.end()) return Value(true);
    return Value(false);
  }

  // Handle Class objects
  if (args[0].isClass()) {
    auto cls = args[0].getGC<Class>();
    if (isInternalProperty(key)) return Value(false);
    if (cls->properties.find(key) != cls->properties.end()) return Value(true);
    if (cls->properties.find("__get_" + key) != cls->properties.end()) return Value(true);
    if (cls->properties.find("__set_" + key) != cls->properties.end()) return Value(true);
    return Value(false);
  }

  // Handle Array objects
  if (args[0].isArray()) {
    auto arr = args[0].getGC<Array>();
    if (isInternalProperty(key)) return Value(false);
    // Arrays always have an own `length` data property (unless deleted from arguments).
    if (key == "length") {
      if (arr->properties.find("__deleted_length__") != arr->properties.end()) {
        return Value(false);
      }
      return Value(true);
    }
    try {
      size_t idx = std::stoull(key);
      if (idx < arr->elements.size()) {
        // Check if this index was deleted or is a hole
        if (arr->properties.find("__deleted_" + key + "__") != arr->properties.end()) {
          return Value(false);
        }
        if (arr->properties.find("__hole_" + key + "__") != arr->properties.end()) {
          return Value(false);
        }
        return Value(true);
      }
    } catch (...) {}
    if (arr->properties.find(key) != arr->properties.end()) return Value(true);
    if (arr->properties.find("__get_" + key) != arr->properties.end()) return Value(true);
    if (arr->properties.find("__set_" + key) != arr->properties.end()) return Value(true);
    return Value(false);
  }

  if (args[0].isRegex()) {
    auto rx = args[0].getGC<Regex>();
    if (isInternalProperty(key)) return Value(false);
    if (rx->properties.find(key) != rx->properties.end()) return Value(true);
    if (rx->properties.find("__get_" + key) != rx->properties.end()) return Value(true);
    if (rx->properties.find("__set_" + key) != rx->properties.end()) return Value(true);
    return Value(false);
  }

  if (args[0].isPromise()) {
    auto p = args[0].getGC<Promise>();
    if (isInternalProperty(key)) return Value(false);
    if (p->properties.find(key) != p->properties.end()) return Value(true);
    if (p->properties.find("__get_" + key) != p->properties.end()) return Value(true);
    if (p->properties.find("__set_" + key) != p->properties.end()) return Value(true);
    return Value(false);
  }

  if (args[0].isError()) {
    auto e = args[0].getGC<Error>();
    if (isInternalProperty(key)) return Value(false);
    if (e->properties.find(key) != e->properties.end()) return Value(true);
    if (e->properties.find("__get_" + key) != e->properties.end()) return Value(true);
    if (e->properties.find("__set_" + key) != e->properties.end()) return Value(true);
    return Value(false);
  }

  if (args[0].isMap()) {
    auto m = args[0].getGC<Map>();
    if (isInternalProperty(key)) return Value(false);
    if (m->properties.find(key) != m->properties.end()) return Value(true);
    if (m->properties.find("__get_" + key) != m->properties.end()) return Value(true);
    if (m->properties.find("__set_" + key) != m->properties.end()) return Value(true);
    return Value(false);
  }

  if (args[0].isSet()) {
    auto s = args[0].getGC<Set>();
    if (isInternalProperty(key)) return Value(false);
    if (s->properties.find(key) != s->properties.end()) return Value(true);
    if (s->properties.find("__get_" + key) != s->properties.end()) return Value(true);
    if (s->properties.find("__set_" + key) != s->properties.end()) return Value(true);
    return Value(false);
  }

  if (args[0].isWeakMap()) {
    auto wm = args[0].getGC<WeakMap>();
    if (isInternalProperty(key)) return Value(false);
    if (wm->properties.find(key) != wm->properties.end()) return Value(true);
    if (wm->properties.find("__get_" + key) != wm->properties.end()) return Value(true);
    if (wm->properties.find("__set_" + key) != wm->properties.end()) return Value(true);
    return Value(false);
  }

  if (args[0].isWeakSet()) {
    auto ws = args[0].getGC<WeakSet>();
    if (isInternalProperty(key)) return Value(false);
    if (ws->properties.find(key) != ws->properties.end()) return Value(true);
    if (ws->properties.find("__get_" + key) != ws->properties.end()) return Value(true);
    if (ws->properties.find("__set_" + key) != ws->properties.end()) return Value(true);
    return Value(false);
  }

  if (args[0].isTypedArray()) {
    auto ta = args[0].getGC<TypedArray>();
    if (isInternalProperty(key)) return Value(false);
    if (key == "length" || key == "byteLength" || key == "buffer" || key == "byteOffset") return Value(true);
    if (!key.empty() && std::all_of(key.begin(), key.end(), ::isdigit)) {
      try {
        size_t idx = std::stoull(key);
        if (idx < ta->currentLength()) return Value(true);
      } catch (...) {
      }
    }
    if (ta->properties.find(key) != ta->properties.end()) return Value(true);
    if (ta->properties.find("__get_" + key) != ta->properties.end()) return Value(true);
    if (ta->properties.find("__set_" + key) != ta->properties.end()) return Value(true);
    return Value(false);
  }

  if (args[0].isArrayBuffer()) {
    auto b = args[0].getGC<ArrayBuffer>();
    if (isInternalProperty(key)) return Value(false);
    if (key == "byteLength") return Value(true);
    if (b->properties.find(key) != b->properties.end()) return Value(true);
    if (b->properties.find("__get_" + key) != b->properties.end()) return Value(true);
    if (b->properties.find("__set_" + key) != b->properties.end()) return Value(true);
    return Value(false);
  }

  if (args[0].isDataView()) {
    auto v = args[0].getGC<DataView>();
    if (isInternalProperty(key)) return Value(false);
    if (v->properties.find(key) != v->properties.end()) return Value(true);
    if (v->properties.find("__get_" + key) != v->properties.end()) return Value(true);
    if (v->properties.find("__set_" + key) != v->properties.end()) return Value(true);
    return Value(false);
  }

  if (args[0].isGenerator()) {
    auto g = args[0].getGC<Generator>();
    if (isInternalProperty(key)) return Value(false);
    if (g->properties.find(key) != g->properties.end()) return Value(true);
    if (g->properties.find("__get_" + key) != g->properties.end()) return Value(true);
    if (g->properties.find("__set_" + key) != g->properties.end()) return Value(true);
    return Value(false);
  }

  if (!args[0].isObject()) {
    return Value(false);
  }

  auto obj = args[0].getGC<Object>();

  if (obj->isModuleNamespace) {
    if (key == WellKnownSymbols::toStringTagKey()) {
      return Value(true);
    }
    auto exportNames = moduleNamespaceExportNames(obj);
    bool isExport = std::find(exportNames.begin(), exportNames.end(), key) !=
                    exportNames.end();
    if (!isExport) {
      return Value(false);
    }
    auto getterIt = obj->properties.find("__get_" + key);
    if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
      auto getter = getterIt->second.getGC<Function>();
      if (getter && getter->isNative) {
        getter->nativeFunc({});
      }
    }
    return Value(true);
  }

  // Internal properties are not own properties
  if (key == "__proto__" && obj->properties.find("__own_prop___proto__") != obj->properties.end()) {
    return Value(true);
  }
  // Check for accessor properties before isInternalProperty check
  // (e.g., Object.prototype has __get___proto__ / __set___proto__)
  if (obj->properties.find("__get_" + key) != obj->properties.end()) return Value(true);
  if (obj->properties.find("__set_" + key) != obj->properties.end()) return Value(true);
  if (isInternalProperty(key)) return Value(false);
  if (obj->properties.find(key) != obj->properties.end()) return Value(true);
  return Value(false);
}

}  // namespace lightjs
