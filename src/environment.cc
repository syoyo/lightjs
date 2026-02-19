#include "environment.h"
#include "crypto.h"
#include "http.h"
#include "gc.h"
#include "json.h"
#include "object_methods.h"
#include "array_methods.h"
#include "string_methods.h"
#include "math_object.h"
#include "date_object.h"
#include "event_loop.h"
#include "symbols.h"
#include "module.h"
#include "interpreter.h"
#include "wasm_js.h"
#include "unicode.h"
#include "text_encoding.h"
#include "url.h"
#include "fs.h"
#include "streams.h"
#include "lexer.h"
#include "parser.h"
#include <iostream>
#include <thread>
#include <limits>
#include <cmath>
#include <random>
#include <cctype>
#include <array>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <regex>
#include <unordered_set>

namespace lightjs {

namespace {

std::string trimAsciiWhitespace(const std::string& s) {
  size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
    start++;
  }
  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    end--;
  }
  return s.substr(start, end - start);
}

int digitValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
  return -1;
}

// Parse a JS integer string into int64_t. Supports optional sign and 0x/0o/0b prefixes.
// Values are reduced modulo 2^64 to keep the runtime stable on very large inputs.
bool parseBigIntString64(const std::string& raw, int64_t& out) {
  std::string s = trimAsciiWhitespace(raw);
  if (s.empty()) {
    out = 0;
    return true;
  }

  bool negative = false;
  bool hadSign = false;
  size_t i = 0;
  if (s[i] == '+' || s[i] == '-') {
    hadSign = true;
    negative = (s[i] == '-');
    i++;
  }
  if (i >= s.size()) return false;

  int base = 10;
  if (i + 1 < s.size() && s[i] == '0') {
    if (s[i + 1] == 'x' || s[i + 1] == 'X') {
      if (hadSign) return false;
      base = 16;
      i += 2;
    } else if (s[i + 1] == 'o' || s[i + 1] == 'O') {
      if (hadSign) return false;
      base = 8;
      i += 2;
    } else if (s[i + 1] == 'b' || s[i + 1] == 'B') {
      if (hadSign) return false;
      base = 2;
      i += 2;
    }
  }
  if (i >= s.size()) return false;

  uint64_t value = 0;
  bool sawDigit = false;
  for (; i < s.size(); i++) {
    int d = digitValue(s[i]);
    if (d < 0 || d >= base) return false;
    sawDigit = true;
    value = value * static_cast<uint64_t>(base) + static_cast<uint64_t>(d);
  }
  if (!sawDigit) return false;

  uint64_t signedBits = value;
  if (negative) {
    signedBits = uint64_t{0} - signedBits;
  }
  out = static_cast<int64_t>(signedBits);
  return true;
}

bool isModuleNamespaceExportKey(const std::shared_ptr<Object>& obj, const std::string& key) {
  return std::find(obj->moduleExportNames.begin(), obj->moduleExportNames.end(), key) !=
         obj->moduleExportNames.end();
}

constexpr const char* kImportPhaseSourceSentinel = "__lightjs_import_phase_source__";
constexpr const char* kImportPhaseDeferSentinel = "__lightjs_import_phase_defer__";
constexpr const char* kWithScopeObjectBinding = "__with_scope_object__";

bool isVisibleWithIdentifier(const std::string& name) {
  return !name.empty() && name.rfind("__", 0) != 0;
}

std::optional<Value> lookupWithScopeProperty(const Value& scopeValue, const std::string& name) {
  if (!isVisibleWithIdentifier(name) || !scopeValue.isObject()) {
    return std::nullopt;
  }

  std::unordered_set<Object*> visited;
  auto current = std::get<std::shared_ptr<Object>>(scopeValue.data);
  int depth = 0;
  while (current && depth < 64 && visited.insert(current.get()).second) {
    auto it = current->properties.find(name);
    if (it != current->properties.end()) {
      return it->second;
    }
    auto protoIt = current->properties.find("__proto__");
    if (protoIt == current->properties.end() || !protoIt->second.isObject()) {
      break;
    }
    current = std::get<std::shared_ptr<Object>>(protoIt->second.data);
    depth++;
  }
  return std::nullopt;
}

bool setWithScopeProperty(const Value& scopeValue, const std::string& name, const Value& value) {
  if (!isVisibleWithIdentifier(name) || !scopeValue.isObject()) {
    return false;
  }

  auto receiver = std::get<std::shared_ptr<Object>>(scopeValue.data);
  std::unordered_set<Object*> visited;
  auto current = receiver;
  int depth = 0;
  while (current && depth < 64 && visited.insert(current.get()).second) {
    auto it = current->properties.find(name);
    if (it != current->properties.end()) {
      receiver->properties[name] = value;
      return true;
    }
    auto protoIt = current->properties.find("__proto__");
    if (protoIt == current->properties.end() || !protoIt->second.isObject()) {
      break;
    }
    current = std::get<std::shared_ptr<Object>>(protoIt->second.data);
    depth++;
  }
  return false;
}

bool defineModuleNamespaceProperty(const std::shared_ptr<Object>& obj,
                                   const std::string& key,
                                   const std::shared_ptr<Object>& descriptor) {
  const std::string& toStringTagKey = WellKnownSymbols::toStringTagKey();
  const bool isExport = isModuleNamespaceExportKey(obj, key);
  const bool isToStringTag = (key == toStringTagKey);
  if (!isExport && !isToStringTag) {
    return false;
  }

  auto has = [&](const std::string& name) {
    return descriptor->properties.find(name) != descriptor->properties.end();
  };
  auto boolFieldMatches = [&](const std::string& name, bool expected) {
    if (!has(name)) return true;
    return descriptor->properties.at(name).toBool() == expected;
  };

  if (has("get") || has("set")) {
    return false;
  }

  if (isExport) {
    if (has("value")) {
      return false;
    }
    return boolFieldMatches("writable", true) &&
           boolFieldMatches("enumerable", true) &&
           boolFieldMatches("configurable", false);
  }

  if (has("value") && descriptor->properties.at("value").toString() != "Module") {
    return false;
  }
  return boolFieldMatches("writable", false) &&
         boolFieldMatches("enumerable", false) &&
         boolFieldMatches("configurable", false);
}

std::optional<std::string> readTextFile(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return std::nullopt;
  }
  std::stringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

std::vector<std::string> parseStaticImportSpecifiers(const std::string& source) {
  std::vector<std::string> specifiers;
  static const std::regex kImportRegex(
    R"((?:^|[\n\r])\s*import\s+(?:[^'";\n]*\s+from\s+)?["']([^"']+)["']\s*;)");
  for (std::sregex_iterator it(source.begin(), source.end(), kImportRegex), end; it != end; ++it) {
    specifiers.push_back((*it)[1].str());
  }
  return specifiers;
}

bool hasTopLevelAwaitInSource(const std::string& source) {
  static const std::regex kTopLevelAwaitRegex(R"((?:^|[\n\r])\s*await\b)");
  return std::regex_search(source, kTopLevelAwaitRegex);
}

void gatherAsyncTransitiveDependencies(const std::string& modulePath,
                                       ModuleLoader* loader,
                                       std::unordered_set<std::string>& visitedModules,
                                       std::unordered_set<std::string>& queuedAsyncModules,
                                       std::vector<std::string>& orderedAsyncModules) {
  if (!loader) {
    return;
  }
  if (!visitedModules.insert(modulePath).second) {
    return;
  }

  auto sourceOpt = readTextFile(modulePath);
  if (!sourceOpt.has_value()) {
    return;
  }

  if (hasTopLevelAwaitInSource(*sourceOpt)) {
    if (queuedAsyncModules.insert(modulePath).second) {
      orderedAsyncModules.push_back(modulePath);
    }
    return;
  }

  auto specifiers = parseStaticImportSpecifiers(*sourceOpt);
  for (const auto& specifier : specifiers) {
    std::string resolvedDependency = loader->resolvePath(specifier, modulePath);
    gatherAsyncTransitiveDependencies(
      resolvedDependency, loader, visitedModules, queuedAsyncModules, orderedAsyncModules);
  }
}

}  // namespace

// Global module loader and interpreter for dynamic imports
static std::shared_ptr<ModuleLoader> g_moduleLoader;
static Interpreter* g_interpreter = nullptr;

void setGlobalModuleLoader(std::shared_ptr<ModuleLoader> loader) {
  g_moduleLoader = loader;
}

void setGlobalInterpreter(Interpreter* interpreter) {
  g_interpreter = interpreter;
}

Interpreter* getGlobalInterpreter() {
  return g_interpreter;
}

Environment::Environment(std::shared_ptr<Environment> parent)
  : parent_(parent) {}

void Environment::define(const std::string& name, const Value& value, bool isConst) {
  bindings_[name] = value;
  tdzBindings_.erase(name);  // Remove TDZ when initialized
  if (isConst) {
    constants_[name] = true;
  }
  if (!parent_) {
    auto it = bindings_.find("globalThis");
    if (it != bindings_.end() && it->second.isObject()) {
      auto globalObj = std::get<std::shared_ptr<Object>>(it->second.data);
      globalObj->properties[name] = value;
    }
  }
}

void Environment::defineTDZ(const std::string& name) {
  bindings_[name] = Value(Undefined{});
  tdzBindings_[name] = true;
}

void Environment::removeTDZ(const std::string& name) {
  tdzBindings_.erase(name);
}

bool Environment::isTDZ(const std::string& name) const {
  // Check if binding exists in this scope and is in TDZ
  auto bindIt = bindings_.find(name);
  if (bindIt != bindings_.end()) {
    return tdzBindings_.find(name) != tdzBindings_.end();
  }
  // If not found in this scope, check parent
  if (parent_) {
    return parent_->isTDZ(name);
  }
  return false;
}

std::optional<Value> Environment::get(const std::string& name) const {
  auto it = bindings_.find(name);
  if (it != bindings_.end()) {
    return it->second;
  }
  auto withScopeIt = bindings_.find(kWithScopeObjectBinding);
  if (withScopeIt != bindings_.end()) {
    auto withValue = lookupWithScopeProperty(withScopeIt->second, name);
    if (withValue.has_value()) {
      return withValue;
    }
  }
  if (parent_) {
    return parent_->get(name);
  }
  return std::nullopt;
}

bool Environment::set(const std::string& name, const Value& value) {
  auto it = bindings_.find(name);
  if (it != bindings_.end()) {
    if (constants_.find(name) != constants_.end()) {
      return false;
    }
    bindings_[name] = value;
    return true;
  }
  auto withScopeIt = bindings_.find(kWithScopeObjectBinding);
  if (withScopeIt != bindings_.end() &&
      setWithScopeProperty(withScopeIt->second, name, value)) {
    return true;
  }
  if (parent_) {
    return parent_->set(name, value);
  }
  return false;
}

bool Environment::has(const std::string& name) const {
  if (bindings_.find(name) != bindings_.end()) {
    return true;
  }
  auto withScopeIt = bindings_.find(kWithScopeObjectBinding);
  if (withScopeIt != bindings_.end() &&
      lookupWithScopeProperty(withScopeIt->second, name).has_value()) {
    return true;
  }
  if (parent_) {
    return parent_->has(name);
  }
  return false;
}

bool Environment::isConst(const std::string& name) const {
  if (constants_.find(name) != constants_.end()) {
    return true;
  }
  if (parent_) {
    return parent_->isConst(name);
  }
  return false;
}

std::shared_ptr<Environment> Environment::createChild() {
  return std::make_shared<Environment>(shared_from_this());
}

std::shared_ptr<Environment> Environment::createGlobal() {
  auto env = std::make_shared<Environment>();

  auto consoleFn = std::make_shared<Function>();
  consoleFn->isNative = true;
  consoleFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    for (const auto& arg : args) {
      std::cout << arg.toString() << " ";
    }
    std::cout << std::endl;
    return Value(Undefined{});
  };

  // console.error - writes to stderr
  auto consoleErrorFn = std::make_shared<Function>();
  consoleErrorFn->isNative = true;
  consoleErrorFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    for (const auto& arg : args) {
      std::cerr << arg.toString() << " ";
    }
    std::cerr << std::endl;
    return Value(Undefined{});
  };

  // console.warn - writes to stderr
  auto consoleWarnFn = std::make_shared<Function>();
  consoleWarnFn->isNative = true;
  consoleWarnFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    for (const auto& arg : args) {
      std::cerr << arg.toString() << " ";
    }
    std::cerr << std::endl;
    return Value(Undefined{});
  };

  // console.info - same as log
  auto consoleInfoFn = std::make_shared<Function>();
  consoleInfoFn->isNative = true;
  consoleInfoFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    for (const auto& arg : args) {
      std::cout << arg.toString() << " ";
    }
    std::cout << std::endl;
    return Value(Undefined{});
  };

  // console.debug - same as log
  auto consoleDebugFn = std::make_shared<Function>();
  consoleDebugFn->isNative = true;
  consoleDebugFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    for (const auto& arg : args) {
      std::cout << arg.toString() << " ";
    }
    std::cout << std::endl;
    return Value(Undefined{});
  };

  // console.time/timeEnd - simple timing
  static std::unordered_map<std::string, std::chrono::steady_clock::time_point> consoleTimers;

  auto consoleTimeFn = std::make_shared<Function>();
  consoleTimeFn->isNative = true;
  consoleTimeFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    std::string label = args.empty() ? "default" : args[0].toString();
    consoleTimers[label] = std::chrono::steady_clock::now();
    return Value(Undefined{});
  };

  auto consoleTimeEndFn = std::make_shared<Function>();
  consoleTimeEndFn->isNative = true;
  consoleTimeEndFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    std::string label = args.empty() ? "default" : args[0].toString();
    auto it = consoleTimers.find(label);
    if (it != consoleTimers.end()) {
      auto elapsed = std::chrono::steady_clock::now() - it->second;
      auto ms = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() / 1000.0;
      std::cout << label << ": " << ms << "ms" << std::endl;
      consoleTimers.erase(it);
    }
    return Value(Undefined{});
  };

  // console.assert
  auto consoleAssertFn = std::make_shared<Function>();
  consoleAssertFn->isNative = true;
  consoleAssertFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    bool condition = args.empty() ? false : args[0].toBool();
    if (!condition) {
      std::cerr << "Assertion failed:";
      for (size_t i = 1; i < args.size(); ++i) {
        std::cerr << " " << args[i].toString();
      }
      std::cerr << std::endl;
    }
    return Value(Undefined{});
  };

  auto consoleObj = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  consoleObj->properties["log"] = Value(consoleFn);
  consoleObj->properties["error"] = Value(consoleErrorFn);
  consoleObj->properties["warn"] = Value(consoleWarnFn);
  consoleObj->properties["info"] = Value(consoleInfoFn);
  consoleObj->properties["debug"] = Value(consoleDebugFn);
  consoleObj->properties["time"] = Value(consoleTimeFn);
  consoleObj->properties["timeEnd"] = Value(consoleTimeEndFn);
  consoleObj->properties["assert"] = Value(consoleAssertFn);

  env->define("console", Value(consoleObj));
  env->define("undefined", Value(Undefined{}), true);  // non-writable per spec
  env->define("Infinity", Value(std::numeric_limits<double>::infinity()), true);
  env->define("NaN", Value(std::numeric_limits<double>::quiet_NaN()), true);

  auto evalFn = std::make_shared<Function>();
  evalFn->isNative = true;
  evalFn->nativeFunc = [env](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(Undefined{});
    }
    if (!args[0].isString()) {
      return args[0];
    }

    Interpreter* prevInterpreter = getGlobalInterpreter();
    if (!prevInterpreter) {
      return Value(Undefined{});
    }
    auto evalEnv = env;
    if (prevInterpreter->inDirectEvalInvocation()) {
      auto callerEnv = prevInterpreter->getEnvironment();
      if (callerEnv) {
        evalEnv = callerEnv;
      }
    }

    std::string source = args[0].toString();
    Lexer lexer(source);
    auto tokens = lexer.tokenize();

    for (size_t i = 0; i + 2 < tokens.size(); ++i) {
      if (tokens[i].type == TokenType::Import &&
          !tokens[i].escaped &&
          tokens[i + 1].type == TokenType::Dot &&
          tokens[i + 2].type == TokenType::Identifier &&
          !tokens[i + 2].escaped &&
          tokens[i + 2].value == "meta") {
        throw std::runtime_error("SyntaxError: import.meta is not allowed in eval");
      }
    }

    Parser parser(tokens);
    auto program = parser.parse();
    if (!program) {
      throw std::runtime_error("SyntaxError: Parse error");
    }

    Interpreter evalInterpreter(evalEnv);
    Value result = Value(Undefined{});
    try {
      setGlobalInterpreter(&evalInterpreter);
      auto task = evalInterpreter.evaluate(*program);
      LIGHTJS_RUN_TASK(task, result);
      if (evalInterpreter.hasError()) {
        Value err = evalInterpreter.getError();
        evalInterpreter.clearError();
        setGlobalInterpreter(prevInterpreter);
        throw std::runtime_error(err.toString());
      }
    } catch (...) {
      setGlobalInterpreter(prevInterpreter);
      throw;
    }

    setGlobalInterpreter(prevInterpreter);
    return result;
  };
  evalFn->properties["__is_intrinsic_eval__"] = Value(true);
  env->define("eval", Value(evalFn));

  // Symbol constructor
  auto symbolFn = std::make_shared<Function>();
  symbolFn->isNative = true;
  symbolFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    std::string description = args.empty() ? "" : args[0].toString();
    return Value(Symbol(description));
  };
  symbolFn->properties["iterator"] = WellKnownSymbols::iterator();
  symbolFn->properties["asyncIterator"] = WellKnownSymbols::asyncIterator();
  symbolFn->properties["toStringTag"] = WellKnownSymbols::toStringTag();
  symbolFn->properties["toPrimitive"] = WellKnownSymbols::toPrimitive();
  symbolFn->properties["matchAll"] = WellKnownSymbols::matchAll();
  env->define("Symbol", Value(symbolFn));

  // BigInt constructor/function
  auto bigIntFn = std::make_shared<Function>();
  bigIntFn->isNative = true;
  bigIntFn->isConstructor = true;

  auto arrayToString = [](const std::shared_ptr<Array>& arr) -> std::string {
    std::string out;
    for (size_t i = 0; i < arr->elements.size(); i++) {
      if (i > 0) out += ",";
      out += arr->elements[i].toString();
    }
    return out;
  };

  auto isObjectLike = [](const Value& value) -> bool {
    return value.isObject() || value.isArray() || value.isFunction() || value.isRegex();
  };

  auto callChecked = [](const Value& callee,
                        const std::vector<Value>& callArgs,
                        const Value& thisArg) -> Value {
    if (!callee.isFunction()) {
      return Value(Undefined{});
    }

    auto fn = std::get<std::shared_ptr<Function>>(callee.data);
    if (fn->isNative) {
      auto itUsesThis = fn->properties.find("__uses_this_arg__");
      if (itUsesThis != fn->properties.end() &&
          itUsesThis->second.isBool() &&
          itUsesThis->second.toBool()) {
        std::vector<Value> nativeArgs;
        nativeArgs.reserve(callArgs.size() + 1);
        nativeArgs.push_back(thisArg);
        nativeArgs.insert(nativeArgs.end(), callArgs.begin(), callArgs.end());
        return fn->nativeFunc(nativeArgs);
      }
      return fn->nativeFunc(callArgs);
    }

    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) {
      throw std::runtime_error("TypeError: Interpreter unavailable for callable conversion");
    }
    interpreter->clearError();
    Value result = interpreter->callForHarness(callee, callArgs, thisArg);
    if (interpreter->hasError()) {
      Value err = interpreter->getError();
      interpreter->clearError();
      throw std::runtime_error(err.toString());
    }
    return result;
  };

  auto getObjectProperty = [callChecked](const std::shared_ptr<Object>& obj,
                                         const Value& receiver,
                                         const std::string& key) -> std::pair<bool, Value> {
    auto current = obj;
    int depth = 0;
    while (current && depth <= 16) {
      depth++;

      std::string getterKey = "__get_" + key;
      auto getterIt = current->properties.find(getterKey);
      if (getterIt != current->properties.end()) {
        if (getterIt->second.isFunction()) {
          return {true, callChecked(getterIt->second, {}, receiver)};
        }
        return {true, Value(Undefined{})};
      }

      auto it = current->properties.find(key);
      if (it != current->properties.end()) {
        return {true, it->second};
      }

      auto protoIt = current->properties.find("__proto__");
      if (protoIt == current->properties.end() || !protoIt->second.isObject()) {
        break;
      }
      current = std::get<std::shared_ptr<Object>>(protoIt->second.data);
    }

    return {false, Value(Undefined{})};
  };

  auto getProperty = [getObjectProperty, callChecked](const Value& receiver,
                                                       const std::string& key) -> std::pair<bool, Value> {
    if (receiver.isObject()) {
      return getObjectProperty(std::get<std::shared_ptr<Object>>(receiver.data), receiver, key);
    }
    if (receiver.isFunction()) {
      auto fn = std::get<std::shared_ptr<Function>>(receiver.data);
      auto it = fn->properties.find(key);
      if (it != fn->properties.end()) {
        return {true, it->second};
      }
      return {false, Value(Undefined{})};
    }
    if (receiver.isRegex()) {
      auto regex = std::get<std::shared_ptr<Regex>>(receiver.data);
      std::string getterKey = "__get_" + key;
      auto getterIt = regex->properties.find(getterKey);
      if (getterIt != regex->properties.end()) {
        if (getterIt->second.isFunction()) {
          return {true, callChecked(getterIt->second, {}, receiver)};
        }
        return {true, Value(Undefined{})};
      }
      auto it = regex->properties.find(key);
      if (it != regex->properties.end()) {
        return {true, it->second};
      }
      return {false, Value(Undefined{})};
    }
    return {false, Value(Undefined{})};
  };

  auto toPrimitive = [isObjectLike, arrayToString, getProperty, callChecked](const Value& input,
                                                                              bool preferString) -> Value {
    if (!isObjectLike(input)) {
      return input;
    }

    const std::string& toPrimitiveKey = WellKnownSymbols::toPrimitiveKey();
    auto [hasExotic, exotic] = getProperty(input, toPrimitiveKey);
    if (hasExotic && !exotic.isUndefined() && !exotic.isNull()) {
      if (!exotic.isFunction()) {
        throw std::runtime_error("TypeError: @@toPrimitive is not callable");
      }
      std::string hint = preferString ? "string" : "number";
      Value result = callChecked(exotic, {Value(hint)}, input);
      if (isObjectLike(result)) {
        throw std::runtime_error("TypeError: @@toPrimitive must return a primitive value");
      }
      return result;
    }

    std::array<std::string, 2> methodOrder = preferString
      ? std::array<std::string, 2>{"toString", "valueOf"}
      : std::array<std::string, 2>{"valueOf", "toString"};

    for (const auto& methodName : methodOrder) {
      auto [found, method] = getProperty(input, methodName);
      if (found) {
        if (method.isFunction()) {
          Value result = callChecked(method, {}, input);
          if (!isObjectLike(result)) {
            return result;
          }
        }
        continue;
      }

      if (methodName == "toString") {
        if (input.isArray()) {
          return Value(arrayToString(std::get<std::shared_ptr<Array>>(input.data)));
        }
        if (input.isObject()) {
          return Value(std::string("[object Object]"));
        }
        if (input.isFunction()) {
          return Value(std::string("[Function]"));
        }
        if (input.isRegex()) {
          return Value(input.toString());
        }
      }
    }

    throw std::runtime_error("TypeError: Cannot convert object to primitive value");
  };

  auto toBigIntFromPrimitive = [](const Value& v) -> int64_t {
    if (v.isBigInt()) {
      return v.toBigInt();
    }
    if (v.isBool()) {
      return v.toBool() ? 1 : 0;
    }
    if (v.isString()) {
      int64_t parsed = 0;
      if (!parseBigIntString64(std::get<std::string>(v.data), parsed)) {
        throw std::runtime_error("SyntaxError: Cannot convert string to BigInt");
      }
      return parsed;
    }
    throw std::runtime_error("TypeError: Cannot convert value to BigInt");
  };

  auto toBigIntFromValue = [toPrimitive, toBigIntFromPrimitive](const Value& v) -> int64_t {
    Value primitive = toPrimitive(v, false);
    return toBigIntFromPrimitive(primitive);
  };

  auto toIndex = [toPrimitive](const Value& v) -> uint64_t {
    Value primitive = toPrimitive(v, false);
    if (primitive.isBigInt() || primitive.isSymbol()) {
      throw std::runtime_error("TypeError: Cannot convert to index");
    }
    double n = primitive.toNumber();
    if (std::isnan(n)) return 0;
    if (!std::isfinite(n)) {
      throw std::runtime_error("RangeError: Invalid index");
    }
    n = std::trunc(n);
    if (n < 0.0 || n > 9007199254740991.0) {
      throw std::runtime_error("RangeError: Invalid index");
    }
    return static_cast<uint64_t>(n);
  };

  auto thisBigIntValue = [](const Value& thisValue) -> int64_t {
    if (thisValue.isBigInt()) {
      return thisValue.toBigInt();
    }
    if (thisValue.isObject()) {
      auto obj = std::get<std::shared_ptr<Object>>(thisValue.data);
      auto primitiveIt = obj->properties.find("__primitive_value__");
      if (primitiveIt != obj->properties.end() && primitiveIt->second.isBigInt()) {
        return primitiveIt->second.toBigInt();
      }
    }
    throw std::runtime_error("TypeError: BigInt method called on incompatible receiver");
  };

  auto formatBigInt = [](int64_t value, int radix) -> std::string {
    if (radix == 10) {
      return std::to_string(value);
    }

    bool negative = value < 0;
    uint64_t magnitude = negative
      ? static_cast<uint64_t>(-(value + 1)) + 1
      : static_cast<uint64_t>(value);

    if (magnitude == 0) {
      return "0";
    }

    static const std::string digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::string out;
    while (magnitude > 0) {
      uint64_t digit = magnitude % static_cast<uint64_t>(radix);
      out.push_back(digits[static_cast<size_t>(digit)]);
      magnitude /= static_cast<uint64_t>(radix);
    }
    std::reverse(out.begin(), out.end());
    if (negative) out.insert(out.begin(), '-');
    return out;
  };

  bigIntFn->nativeFunc = [toPrimitive, toBigIntFromPrimitive](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      throw std::runtime_error("TypeError: Cannot convert undefined to BigInt");
    }

    Value primitive = toPrimitive(args[0], false);
    if (primitive.isNumber()) {
      double n = primitive.toNumber();
      if (!std::isfinite(n) || std::floor(n) != n) {
        throw std::runtime_error("RangeError: Cannot convert Number to BigInt");
      }
      return Value(BigInt(static_cast<int64_t>(n)));
    }

    return Value(BigInt(toBigIntFromPrimitive(primitive)));
  };
  bigIntFn->properties["name"] = Value(std::string("BigInt"));
  bigIntFn->properties["length"] = Value(1.0);
  bigIntFn->properties["__throw_on_new__"] = Value(true);

  auto asUintN = std::make_shared<Function>();
  asUintN->isNative = true;
  asUintN->properties["__throw_on_new__"] = Value(true);
  asUintN->properties["name"] = Value(std::string("asUintN"));
  asUintN->properties["length"] = Value(2.0);
  asUintN->nativeFunc = [toIndex, toBigIntFromValue](const std::vector<Value>& args) -> Value {
    uint64_t bits = toIndex(args.empty() ? Value(Undefined{}) : args[0]);
    int64_t n = toBigIntFromValue(args.size() > 1 ? args[1] : Value(Undefined{}));

    if (bits == 0) {
      return Value(BigInt(0));
    }

    uint64_t u = static_cast<uint64_t>(n);
    if (bits < 64) {
      uint64_t mask = (uint64_t{1} << bits) - 1;
      u &= mask;
    }
    return Value(BigInt(static_cast<int64_t>(u)));
  };
  bigIntFn->properties["asUintN"] = Value(asUintN);

  auto asIntN = std::make_shared<Function>();
  asIntN->isNative = true;
  asIntN->properties["__throw_on_new__"] = Value(true);
  asIntN->properties["name"] = Value(std::string("asIntN"));
  asIntN->properties["length"] = Value(2.0);
  asIntN->nativeFunc = [toIndex, toBigIntFromValue](const std::vector<Value>& args) -> Value {
    uint64_t bits = toIndex(args.empty() ? Value(Undefined{}) : args[0]);
    int64_t n = toBigIntFromValue(args.size() > 1 ? args[1] : Value(Undefined{}));

    if (bits == 0) {
      return Value(BigInt(0));
    }

    uint64_t u = static_cast<uint64_t>(n);
    if (bits < 64) {
      uint64_t mask = (uint64_t{1} << bits) - 1;
      u &= mask;
      uint64_t signBit = uint64_t{1} << (bits - 1);
      if (u & signBit) {
        u |= ~mask;
      }
    }
    return Value(BigInt(static_cast<int64_t>(u)));
  };
  bigIntFn->properties["asIntN"] = Value(asIntN);

  auto bigIntProto = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  auto bigIntProtoToString = std::make_shared<Function>();
  bigIntProtoToString->isNative = true;
  bigIntProtoToString->properties["__throw_on_new__"] = Value(true);
  bigIntProtoToString->properties["__uses_this_arg__"] = Value(true);
  bigIntProtoToString->properties["name"] = Value(std::string("toString"));
  bigIntProtoToString->properties["length"] = Value(0.0);
  bigIntProtoToString->nativeFunc = [thisBigIntValue, formatBigInt](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      throw std::runtime_error("TypeError: BigInt.prototype.toString requires BigInt");
    }

    int64_t n = thisBigIntValue(args[0]);
    int radix = 10;
    if (args.size() > 1 && !args[1].isUndefined()) {
      radix = static_cast<int>(std::trunc(args[1].toNumber()));
      if (radix < 2 || radix > 36) {
        throw std::runtime_error("RangeError: radix must be between 2 and 36");
      }
    }
    return Value(formatBigInt(n, radix));
  };
  bigIntProto->properties["toString"] = Value(bigIntProtoToString);

  auto bigIntProtoValueOf = std::make_shared<Function>();
  bigIntProtoValueOf->isNative = true;
  bigIntProtoValueOf->properties["__throw_on_new__"] = Value(true);
  bigIntProtoValueOf->properties["__uses_this_arg__"] = Value(true);
  bigIntProtoValueOf->properties["name"] = Value(std::string("valueOf"));
  bigIntProtoValueOf->properties["length"] = Value(0.0);
  bigIntProtoValueOf->nativeFunc = [thisBigIntValue](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      throw std::runtime_error("TypeError: BigInt.prototype.valueOf requires BigInt");
    }
    return Value(BigInt(thisBigIntValue(args[0])));
  };
  bigIntProto->properties["valueOf"] = Value(bigIntProtoValueOf);

  auto bigIntProtoToLocaleString = std::make_shared<Function>();
  bigIntProtoToLocaleString->isNative = true;
  bigIntProtoToLocaleString->properties["__throw_on_new__"] = Value(true);
  bigIntProtoToLocaleString->properties["__uses_this_arg__"] = Value(true);
  bigIntProtoToLocaleString->properties["name"] = Value(std::string("toLocaleString"));
  bigIntProtoToLocaleString->properties["length"] = Value(0.0);
  bigIntProtoToLocaleString->nativeFunc = [thisBigIntValue](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      throw std::runtime_error("TypeError: BigInt.prototype.toLocaleString requires BigInt");
    }
    return Value(std::to_string(thisBigIntValue(args[0])));
  };
  bigIntProto->properties["toLocaleString"] = Value(bigIntProtoToLocaleString);

  // BigInt.prototype.constructor = BigInt
  bigIntProto->properties["constructor"] = Value(bigIntFn);

  // BigInt.prototype[Symbol.toStringTag] = "BigInt" (non-writable)
  bigIntProto->properties[WellKnownSymbols::toStringTagKey()] = Value(std::string("BigInt"));
  bigIntProto->properties["__non_writable_" + WellKnownSymbols::toStringTagKey()] = Value(true);
  bigIntProto->properties["__non_enum_" + WellKnownSymbols::toStringTagKey()] = Value(true);

  // Mark all BigInt.prototype properties as non-enumerable
  bigIntProto->properties["__non_enum_toString"] = Value(true);
  bigIntProto->properties["__non_enum_valueOf"] = Value(true);
  bigIntProto->properties["__non_enum_toLocaleString"] = Value(true);
  bigIntProto->properties["__non_enum_constructor"] = Value(true);

  bigIntFn->properties["prototype"] = Value(bigIntProto);
  bigIntFn->properties["__non_writable_prototype"] = Value(true);
  bigIntFn->properties["__non_configurable_prototype"] = Value(true);

  env->define("BigInt", Value(bigIntFn));

  auto createTypedArrayConstructor = [](TypedArrayType type) {
    auto func = std::make_shared<Function>();
    func->isNative = true;
    func->isConstructor = true;
    func->nativeFunc = [type](const std::vector<Value>& args) -> Value {
      if (args.empty()) {
        return Value(std::make_shared<TypedArray>(type, 0));
      }

      // Check if first argument is an array
      if (std::holds_alternative<std::shared_ptr<Array>>(args[0].data)) {
        auto arr = std::get<std::shared_ptr<Array>>(args[0].data);
        auto typedArray = std::make_shared<TypedArray>(type, arr->elements.size());

        // Fill the typed array with values from the regular array
        for (size_t i = 0; i < arr->elements.size(); ++i) {
          double val = arr->elements[i].toNumber();
          typedArray->setElement(i, val);
        }

        return Value(typedArray);
      }

      // Otherwise treat as length
      double lengthNum = args[0].toNumber();
      if (std::isnan(lengthNum) || std::isinf(lengthNum) || lengthNum < 0) {
        // Invalid length, return empty array
        return Value(std::make_shared<TypedArray>(type, 0));
      }

      size_t length = static_cast<size_t>(lengthNum);
      // Sanity check: prevent allocating huge arrays
      if (length > 1000000000) { // 1GB limit
        return Value(std::make_shared<TypedArray>(type, 0));
      }

      return Value(std::make_shared<TypedArray>(type, length));
    };
    return Value(func);
  };

  env->define("Int8Array", createTypedArrayConstructor(TypedArrayType::Int8));
  env->define("Uint8Array", createTypedArrayConstructor(TypedArrayType::Uint8));
  env->define("Uint8ClampedArray", createTypedArrayConstructor(TypedArrayType::Uint8Clamped));
  env->define("Int16Array", createTypedArrayConstructor(TypedArrayType::Int16));
  env->define("Uint16Array", createTypedArrayConstructor(TypedArrayType::Uint16));
  env->define("Float16Array", createTypedArrayConstructor(TypedArrayType::Float16));
  env->define("Int32Array", createTypedArrayConstructor(TypedArrayType::Int32));
  env->define("Uint32Array", createTypedArrayConstructor(TypedArrayType::Uint32));
  env->define("Float32Array", createTypedArrayConstructor(TypedArrayType::Float32));
  env->define("Float64Array", createTypedArrayConstructor(TypedArrayType::Float64));
  env->define("BigInt64Array", createTypedArrayConstructor(TypedArrayType::BigInt64));
  env->define("BigUint64Array", createTypedArrayConstructor(TypedArrayType::BigUint64));

  // ArrayBuffer constructor
  auto arrayBufferConstructor = std::make_shared<Function>();
  arrayBufferConstructor->isNative = true;
  arrayBufferConstructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    size_t length = 0;
    if (!args.empty()) {
      length = static_cast<size_t>(args[0].toNumber());
    }
    auto buffer = std::make_shared<ArrayBuffer>(length);
    GarbageCollector::instance().reportAllocation(length);
    return Value(buffer);
  };
  env->define("ArrayBuffer", Value(arrayBufferConstructor));

  // DataView constructor
  auto dataViewConstructor = std::make_shared<Function>();
  dataViewConstructor->isNative = true;
  dataViewConstructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArrayBuffer()) {
      throw std::runtime_error("TypeError: DataView requires an ArrayBuffer");
    }

    auto buffer = std::get<std::shared_ptr<ArrayBuffer>>(args[0].data);
    size_t byteOffset = 0;
    size_t byteLength = 0;

    if (args.size() > 1) {
      byteOffset = static_cast<size_t>(args[1].toNumber());
    }
    if (args.size() > 2) {
      byteLength = static_cast<size_t>(args[2].toNumber());
    }

    auto dataView = std::make_shared<DataView>(buffer, byteOffset, byteLength);
    GarbageCollector::instance().reportAllocation(sizeof(DataView));
    return Value(dataView);
  };
  env->define("DataView", Value(dataViewConstructor));

  auto cryptoObj = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  auto sha256Fn = std::make_shared<Function>();
  sha256Fn->isNative = true;
  sha256Fn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::string(""));
    std::string input = args[0].toString();
    std::string result = crypto::SHA256::hashHex(
      reinterpret_cast<const uint8_t*>(input.c_str()), input.length()
    );
    return Value(result);
  };
  cryptoObj->properties["sha256"] = Value(sha256Fn);

  auto hmacFn = std::make_shared<Function>();
  hmacFn->isNative = true;
  hmacFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(std::string(""));
    std::string key = args[0].toString();
    std::string message = args[1].toString();
    std::string result = crypto::HMAC::computeHex(
      reinterpret_cast<const uint8_t*>(key.c_str()), key.length(),
      reinterpret_cast<const uint8_t*>(message.c_str()), message.length()
    );
    return Value(result);
  };
  cryptoObj->properties["hmac"] = Value(hmacFn);

  auto toHexFn = std::make_shared<Function>();
  toHexFn->isNative = true;
  toHexFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::string(""));
    std::string input = args[0].toString();
    std::string result = crypto::toHex(
      reinterpret_cast<const uint8_t*>(input.c_str()), input.length()
    );
    return Value(result);
  };
  cryptoObj->properties["toHex"] = Value(toHexFn);

  // crypto.randomUUID - generate RFC 4122 version 4 UUID
  auto randomUUIDFn = std::make_shared<Function>();
  randomUUIDFn->isNative = true;
  randomUUIDFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // Generate 16 random bytes
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis(0, 255);

    uint8_t bytes[16];
    for (int i = 0; i < 16; ++i) {
      bytes[i] = static_cast<uint8_t>(dis(gen));
    }

    // Set version (4) and variant (RFC 4122)
    bytes[6] = (bytes[6] & 0x0f) | 0x40;  // Version 4
    bytes[8] = (bytes[8] & 0x3f) | 0x80;  // Variant RFC 4122

    // Format as UUID string
    char uuid[37];
    snprintf(uuid, sizeof(uuid),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11],
             bytes[12], bytes[13], bytes[14], bytes[15]);
    return Value(std::string(uuid));
  };
  cryptoObj->properties["randomUUID"] = Value(randomUUIDFn);

  // crypto.getRandomValues - fill typed array with random values
  auto getRandomValuesFn = std::make_shared<Function>();
  getRandomValuesFn->isNative = true;
  getRandomValuesFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isTypedArray()) {
      return Value(std::make_shared<Error>(ErrorType::TypeError, "getRandomValues requires a TypedArray"));
    }

    auto typedArray = std::get<std::shared_ptr<TypedArray>>(args[0].data);
    std::random_device rd;
    std::mt19937 gen(rd());

    // Fill buffer with random bytes
    std::uniform_int_distribution<uint32_t> dis(0, 255);
    for (size_t i = 0; i < typedArray->buffer.size(); ++i) {
      typedArray->buffer[i] = static_cast<uint8_t>(dis(gen));
    }

    return args[0];  // Return the same array
  };
  cryptoObj->properties["getRandomValues"] = Value(getRandomValuesFn);

  env->define("crypto", Value(cryptoObj));

  auto fetchFn = std::make_shared<Function>();
  fetchFn->isNative = true;
  fetchFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(Undefined{});
    }

    std::string url = args[0].toString();

    auto promise = std::make_shared<Promise>();
    http::HTTPClient client;

    try {
      http::Response httpResp = client.get(url);

      auto respObj = std::make_shared<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      respObj->properties["status"] = Value(static_cast<double>(httpResp.statusCode));
      respObj->properties["statusText"] = Value(httpResp.statusText);
      respObj->properties["ok"] = Value(httpResp.statusCode >= 200 && httpResp.statusCode < 300);

      auto textFn = std::make_shared<Function>();
      textFn->isNative = true;
      std::string bodyText = httpResp.bodyAsString();
      textFn->nativeFunc = [bodyText](const std::vector<Value>&) -> Value {
        return Value(bodyText);
      };
      respObj->properties["text"] = Value(textFn);

      auto headersObj = std::make_shared<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      for (const auto& [key, value] : httpResp.headers) {
        headersObj->properties[key] = Value(value);
      }
      respObj->properties["headers"] = Value(headersObj);

      promise->resolve(Value(respObj));
    } catch (...) {
      promise->reject(Value(std::string("Fetch failed")));
    }

    return Value(promise);
  };
  env->define("fetch", Value(fetchFn));

  // Dynamic import() function - returns a Promise
  auto importFn = std::make_shared<Function>();
  importFn->isNative = true;
  importFn->nativeFunc = [arrayToString](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      auto promise = std::make_shared<Promise>();
      auto err = std::make_shared<Error>(ErrorType::TypeError, "import() requires a module specifier");
      promise->reject(Value(err));
      return Value(promise);
    }

    auto promise = std::make_shared<Promise>();
    std::string specifier;
    enum class ImportPhase {
      Normal,
      Source,
      Defer,
    };
    ImportPhase importPhase = ImportPhase::Normal;
    bool hasImportOptions = false;
    Value importOptions = Value(Undefined{});
    if (args.size() > 1 && args[1].isString()) {
      const std::string& maybePhase = std::get<std::string>(args[1].data);
      if (maybePhase == kImportPhaseSourceSentinel) {
        importPhase = ImportPhase::Source;
      } else if (maybePhase == kImportPhaseDeferSentinel) {
        importPhase = ImportPhase::Defer;
      } else {
        hasImportOptions = true;
        importOptions = args[1];
      }
    } else if (args.size() > 1) {
      hasImportOptions = true;
      importOptions = args[1];
    }

    auto rejectWith = [&](ErrorType fallbackType, const std::string& fallbackMessage, const std::optional<Value>& candidate = std::nullopt) {
      if (candidate.has_value()) {
        promise->reject(*candidate);
      } else {
        auto err = std::make_shared<Error>(fallbackType, fallbackMessage);
        promise->reject(Value(err));
      }
    };

    auto isObjectLikeImport = [](const Value& value) -> bool {
      return value.isObject() || value.isArray() || value.isFunction() || value.isRegex() || value.isProxy();
    };

    auto valueFromErrorMessage = [](std::string message) -> Value {
      ErrorType errorType = ErrorType::Error;
      auto consumePrefix = [&](const std::string& prefix, ErrorType type) {
        if (message.rfind(prefix, 0) == 0) {
          errorType = type;
          message = message.substr(prefix.size());
          return true;
        }
        return false;
      };
      if (!consumePrefix("TypeError: ", ErrorType::TypeError) &&
          !consumePrefix("ReferenceError: ", ErrorType::ReferenceError) &&
          !consumePrefix("RangeError: ", ErrorType::RangeError) &&
          !consumePrefix("SyntaxError: ", ErrorType::SyntaxError) &&
          !consumePrefix("URIError: ", ErrorType::URIError) &&
          !consumePrefix("EvalError: ", ErrorType::EvalError) &&
          !consumePrefix("Error: ", ErrorType::Error)) {
        return Value(message);
      }
      return Value(std::make_shared<Error>(errorType, message));
    };

    auto makeError = [](ErrorType type, const std::string& message) -> Value {
      return Value(std::make_shared<Error>(type, message));
    };

    auto callImportCallable = [&](const Value& callee,
                                  const std::vector<Value>& callArgs,
                                  const Value& thisArg,
                                  Value& out,
                                  Value& abrupt) -> bool {
      if (!callee.isFunction()) {
        out = Value(Undefined{});
        return true;
      }

      auto fn = std::get<std::shared_ptr<Function>>(callee.data);
      if (fn->isNative) {
        try {
          auto itUsesThis = fn->properties.find("__uses_this_arg__");
          if (itUsesThis != fn->properties.end() &&
              itUsesThis->second.isBool() &&
              itUsesThis->second.toBool()) {
            std::vector<Value> nativeArgs;
            nativeArgs.reserve(callArgs.size() + 1);
            nativeArgs.push_back(thisArg);
            nativeArgs.insert(nativeArgs.end(), callArgs.begin(), callArgs.end());
            out = fn->nativeFunc(nativeArgs);
          } else {
            out = fn->nativeFunc(callArgs);
          }
          return true;
        } catch (const std::exception& e) {
          abrupt = valueFromErrorMessage(e.what());
          return false;
        } catch (...) {
          abrupt = makeError(ErrorType::Error, "Unknown native error");
          return false;
        }
      }

      Interpreter* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        abrupt = makeError(ErrorType::TypeError, "Interpreter unavailable for callable conversion");
        return false;
      }
      interpreter->clearError();
      out = interpreter->callForHarness(callee, callArgs, thisArg);
      if (interpreter->hasError()) {
        abrupt = interpreter->getError();
        interpreter->clearError();
        return false;
      }
      return true;
    };

    std::function<bool(const Value&, const std::string&, bool&, Value&, Value&)> getImportProperty;
    getImportProperty =
      [&](const Value& receiver, const std::string& key, bool& found, Value& out, Value& abrupt) -> bool {
      if (receiver.isProxy()) {
        auto proxyPtr = std::get<std::shared_ptr<Proxy>>(receiver.data);
        if (proxyPtr->handler && proxyPtr->handler->isObject()) {
          auto handlerObj = std::get<std::shared_ptr<Object>>(proxyPtr->handler->data);
          auto getTrapIt = handlerObj->properties.find("get");
          if (getTrapIt != handlerObj->properties.end() && getTrapIt->second.isFunction()) {
            Value trapOut = Value(Undefined{});
            if (!callImportCallable(getTrapIt->second, {*proxyPtr->target, Value(key), receiver}, Value(Undefined{}), trapOut, abrupt)) {
              return false;
            }
            found = true;
            out = trapOut;
            return true;
          }
        }
        if (proxyPtr->target) {
          return getImportProperty(*proxyPtr->target, key, found, out, abrupt);
        }
        found = false;
        out = Value(Undefined{});
        return true;
      }

      if (receiver.isObject()) {
        auto current = std::get<std::shared_ptr<Object>>(receiver.data);
        int depth = 0;
        while (current && depth <= 16) {
          depth++;
          std::string getterKey = "__get_" + key;
          auto getterIt = current->properties.find(getterKey);
          if (getterIt != current->properties.end()) {
            if (getterIt->second.isFunction()) {
              Value getterOut = Value(Undefined{});
              if (!callImportCallable(getterIt->second, {}, receiver, getterOut, abrupt)) {
                return false;
              }
              found = true;
              out = getterOut;
              return true;
            }
            found = true;
            out = Value(Undefined{});
            return true;
          }

          auto it = current->properties.find(key);
          if (it != current->properties.end()) {
            found = true;
            out = it->second;
            return true;
          }

          auto protoIt = current->properties.find("__proto__");
          if (protoIt == current->properties.end() || !protoIt->second.isObject()) {
            break;
          }
          current = std::get<std::shared_ptr<Object>>(protoIt->second.data);
        }
        found = false;
        out = Value(Undefined{});
        return true;
      }

      if (receiver.isFunction()) {
        auto fn = std::get<std::shared_ptr<Function>>(receiver.data);
        auto it = fn->properties.find(key);
        if (it != fn->properties.end()) {
          found = true;
          out = it->second;
          return true;
        }
        found = false;
        out = Value(Undefined{});
        return true;
      }

      if (receiver.isRegex()) {
        auto regex = std::get<std::shared_ptr<Regex>>(receiver.data);
        std::string getterKey = "__get_" + key;
        auto getterIt = regex->properties.find(getterKey);
        if (getterIt != regex->properties.end()) {
          if (getterIt->second.isFunction()) {
            Value getterOut = Value(Undefined{});
            if (!callImportCallable(getterIt->second, {}, receiver, getterOut, abrupt)) {
              return false;
            }
            found = true;
            out = getterOut;
            return true;
          }
          found = true;
          out = Value(Undefined{});
          return true;
        }
        auto it = regex->properties.find(key);
        if (it != regex->properties.end()) {
          found = true;
          out = it->second;
          return true;
        }
      }

      found = false;
      out = Value(Undefined{});
      return true;
    };

    auto toPrimitiveImport = [&](const Value& input, bool preferString, Value& out, Value& abrupt) -> bool {
      if (!isObjectLikeImport(input)) {
        out = input;
        return true;
      }

      const std::string& toPrimitiveKey = WellKnownSymbols::toPrimitiveKey();
      bool hasExotic = false;
      Value exotic = Value(Undefined{});
      if (!getImportProperty(input, toPrimitiveKey, hasExotic, exotic, abrupt)) {
        return false;
      }
      if (hasExotic && !exotic.isUndefined() && !exotic.isNull()) {
        if (!exotic.isFunction()) {
          abrupt = makeError(ErrorType::TypeError, "@@toPrimitive is not callable");
          return false;
        }
        std::string hint = preferString ? "string" : "number";
        Value result = Value(Undefined{});
        if (!callImportCallable(exotic, {Value(hint)}, input, result, abrupt)) {
          return false;
        }
        if (isObjectLikeImport(result)) {
          abrupt = makeError(ErrorType::TypeError, "@@toPrimitive must return a primitive value");
          return false;
        }
        out = result;
        return true;
      }

      std::array<std::string, 2> methodOrder = preferString
        ? std::array<std::string, 2>{"toString", "valueOf"}
        : std::array<std::string, 2>{"valueOf", "toString"};
      for (const auto& methodName : methodOrder) {
        bool found = false;
        Value method = Value(Undefined{});
        if (!getImportProperty(input, methodName, found, method, abrupt)) {
          return false;
        }
        if (found && method.isFunction()) {
          Value result = Value(Undefined{});
          if (!callImportCallable(method, {}, input, result, abrupt)) {
            return false;
          }
          if (!isObjectLikeImport(result)) {
            out = result;
            return true;
          }
        }
      }

      if (input.isArray()) {
        out = Value(arrayToString(std::get<std::shared_ptr<Array>>(input.data)));
        return true;
      }
      if (input.isObject() || input.isFunction() || input.isProxy()) {
        out = Value(std::string("[object Object]"));
        return true;
      }
      if (input.isRegex()) {
        out = Value(input.toString());
        return true;
      }

      abrupt = makeError(ErrorType::TypeError, "Cannot convert object to primitive value");
      return false;
    };

    std::function<bool(const Value&, std::vector<std::string>&, Value&)> enumerateImportAttributeKeys;
    enumerateImportAttributeKeys =
      [&](const Value& source, std::vector<std::string>& keys, Value& abrupt) -> bool {
      std::unordered_set<std::string> seen;
      auto pushKey = [&](const std::string& key) {
        if (seen.find(key) != seen.end()) return;
        seen.insert(key);
        keys.push_back(key);
      };

      if (source.isProxy()) {
        auto proxyPtr = std::get<std::shared_ptr<Proxy>>(source.data);
        if (proxyPtr->handler && proxyPtr->handler->isObject()) {
          auto handlerObj = std::get<std::shared_ptr<Object>>(proxyPtr->handler->data);
          auto ownKeysIt = handlerObj->properties.find("ownKeys");
          if (ownKeysIt != handlerObj->properties.end() && ownKeysIt->second.isFunction()) {
            Value ownKeysResult = Value(Undefined{});
            if (!callImportCallable(ownKeysIt->second, {*proxyPtr->target}, Value(Undefined{}), ownKeysResult, abrupt)) {
              return false;
            }

            std::vector<std::string> candidateKeys;
            if (ownKeysResult.isArray()) {
              auto arr = std::get<std::shared_ptr<Array>>(ownKeysResult.data);
              for (const auto& entry : arr->elements) {
                if (entry.isString()) {
                  candidateKeys.push_back(std::get<std::string>(entry.data));
                }
              }
            }

            auto gopdIt = handlerObj->properties.find("getOwnPropertyDescriptor");
            for (const auto& key : candidateKeys) {
              bool enumerable = true;
              if (gopdIt != handlerObj->properties.end() && gopdIt->second.isFunction()) {
                Value desc = Value(Undefined{});
                if (!callImportCallable(gopdIt->second, {*proxyPtr->target, Value(key)}, Value(Undefined{}), desc, abrupt)) {
                  return false;
                }
                if (desc.isUndefined()) {
                  enumerable = false;
                } else {
                  bool foundEnumerable = false;
                  Value enumerableValue = Value(Undefined{});
                  if (!getImportProperty(desc, "enumerable", foundEnumerable, enumerableValue, abrupt)) {
                    return false;
                  }
                  enumerable = foundEnumerable && enumerableValue.toBool();
                }
              }
              if (enumerable) {
                pushKey(key);
              }
            }
            return true;
          }
        }

        if (proxyPtr->target) {
          return enumerateImportAttributeKeys(*proxyPtr->target, keys, abrupt);
        }
        return true;
      }

      if (source.isObject()) {
        auto withObj = std::get<std::shared_ptr<Object>>(source.data);
        for (const auto& [key, _] : withObj->properties) {
          if (key.rfind("__get_", 0) == 0) {
            pushKey(key.substr(6));
            continue;
          }
          if (key.rfind("__", 0) == 0) {
            continue;
          }
          pushKey(key);
        }
      } else if (source.isArray()) {
        auto arr = std::get<std::shared_ptr<Array>>(source.data);
        for (size_t i = 0; i < arr->elements.size(); ++i) {
          pushKey(std::to_string(i));
        }
      } else if (source.isFunction()) {
        auto fn = std::get<std::shared_ptr<Function>>(source.data);
        for (const auto& [key, _] : fn->properties) {
          if (key.rfind("__", 0) == 0) {
            continue;
          }
          pushKey(key);
        }
      } else {
        return false;
      }
      return true;
    };

    try {
      if (args[0].isObject()) {
        auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
        auto importMetaIt = obj->properties.find("__import_meta__");
        if (importMetaIt != obj->properties.end() &&
            importMetaIt->second.isBool() &&
            importMetaIt->second.toBool()) {
          auto err = std::make_shared<Error>(ErrorType::TypeError, "Cannot convert object to primitive value");
          promise->reject(Value(err));
          return Value(promise);
        }
      }
      Value primitiveSpecifier = Value(Undefined{});
      Value abrupt = Value(Undefined{});
      if (!toPrimitiveImport(args[0], true, primitiveSpecifier, abrupt)) {
        promise->reject(abrupt);
        return Value(promise);
      }
      if (primitiveSpecifier.isSymbol()) {
        auto err = std::make_shared<Error>(ErrorType::TypeError, "Cannot convert a Symbol value to a string");
        promise->reject(Value(err));
        return Value(promise);
      }
      specifier = primitiveSpecifier.toString();

      if (importPhase == ImportPhase::Source) {
        auto err = std::make_shared<Error>(ErrorType::SyntaxError, "Source phase import is not available");
        promise->reject(Value(err));
        return Value(promise);
      }

      if (hasImportOptions && !importOptions.isUndefined()) {
        if (!isObjectLikeImport(importOptions)) {
          auto err = std::make_shared<Error>(ErrorType::TypeError, "import() options must be an object");
          promise->reject(Value(err));
          return Value(promise);
        }

        bool hasWith = false;
        Value withValue = Value(Undefined{});
        Value abrupt = Value(Undefined{});
        if (!getImportProperty(importOptions, "with", hasWith, withValue, abrupt)) {
          promise->reject(abrupt);
          return Value(promise);
        }

        if (hasWith && !withValue.isUndefined()) {
          if (!isObjectLikeImport(withValue)) {
            auto err = std::make_shared<Error>(ErrorType::TypeError, "import() options.with must be an object");
            promise->reject(Value(err));
            return Value(promise);
          }

          std::vector<std::string> keys;
          if (!enumerateImportAttributeKeys(withValue, keys, abrupt)) {
            promise->reject(abrupt);
            return Value(promise);
          }

          for (const auto& key : keys) {
            bool foundAttr = false;
            Value attrValue = Value(Undefined{});
            if (!getImportProperty(withValue, key, foundAttr, attrValue, abrupt)) {
              promise->reject(abrupt);
              return Value(promise);
            }
            if (!attrValue.isString()) {
              auto err = std::make_shared<Error>(ErrorType::TypeError, "import() options.with values must be strings");
              promise->reject(Value(err));
              return Value(promise);
            }
          }
        }
      }

      // Use global module loader if available
      if (g_moduleLoader && g_interpreter) {
        // Resolve the module path
        std::string resolvedPath = g_moduleLoader->resolvePath(specifier);

        // Load the module
        auto module = g_moduleLoader->loadModule(resolvedPath);
        if (!module) {
          rejectWith(ErrorType::Error, "Failed to load module: " + specifier, g_moduleLoader->getLastError());
          return Value(promise);
        }

        // Instantiate the module
        if (!module->instantiate(g_moduleLoader.get())) {
          rejectWith(ErrorType::SyntaxError, "Failed to instantiate module: " + specifier, module->getLastError());
          return Value(promise);
        }

        bool deferEvaluationUntilNamespaceAccess = false;
        if (importPhase == ImportPhase::Defer) {
          // Defer phase: eagerly evaluate only asynchronous transitive dependencies.
          std::unordered_set<std::string> visitedModules;
          std::unordered_set<std::string> queuedAsyncModules;
          std::vector<std::string> orderedAsyncModules;
          gatherAsyncTransitiveDependencies(
            resolvedPath, g_moduleLoader.get(), visitedModules, queuedAsyncModules, orderedAsyncModules);

          for (const auto& asyncModulePath : orderedAsyncModules) {
            auto asyncModule = g_moduleLoader->loadModule(asyncModulePath);
            if (!asyncModule) {
              rejectWith(
                ErrorType::Error,
                "Failed to load deferred async dependency: " + asyncModulePath,
                g_moduleLoader->getLastError());
              return Value(promise);
            }
            if (!asyncModule->instantiate(g_moduleLoader.get())) {
              rejectWith(
                ErrorType::SyntaxError,
                "Failed to instantiate deferred async dependency: " + asyncModulePath,
                asyncModule->getLastError());
              return Value(promise);
            }
            if (!asyncModule->evaluate(g_interpreter)) {
              rejectWith(
                ErrorType::Error,
                "Failed to evaluate deferred async dependency: " + asyncModulePath,
                asyncModule->getLastError());
              return Value(promise);
            }
          }

          deferEvaluationUntilNamespaceAccess = (module->getState() != Module::State::Evaluated);
        } else {
          // Normal dynamic import evaluates immediately.
          if (!module->evaluate(g_interpreter)) {
            rejectWith(ErrorType::Error, "Failed to evaluate module: " + specifier, module->getLastError());
            return Value(promise);
          }
        }

        if (module->getState() == Module::State::EvaluatingAsync) {
          auto evalPromise = module->getEvaluationPromise();
          if (evalPromise) {
            evalPromise->then(
              [promise, module](Value) -> Value {
                promise->resolve(Value(module->getNamespaceObject()));
                return Value(Undefined{});
              },
              [promise](Value reason) -> Value {
                promise->reject(reason);
                return Value(Undefined{});
              }
            );
            return Value(promise);
          }
        }

        auto moduleNamespace = module->getNamespaceObject();
        if (deferEvaluationUntilNamespaceAccess) {
          auto deferredEvalFn = std::make_shared<Function>();
          deferredEvalFn->isNative = true;
          deferredEvalFn->properties["__throw_on_new__"] = Value(true);
          auto deferredModule = module;
          deferredEvalFn->nativeFunc = [deferredModule](const std::vector<Value>&) -> Value {
            if (deferredModule->getState() == Module::State::Evaluated) {
              return Value(Undefined{});
            }
            Interpreter* interpreter = getGlobalInterpreter();
            if (!interpreter) {
              throw std::runtime_error("Error: Interpreter unavailable for deferred module evaluation");
            }
            if (!deferredModule->evaluate(interpreter)) {
              if (auto error = deferredModule->getLastError()) {
                throw std::runtime_error(error->toString());
              }
              throw std::runtime_error("Error: Failed to evaluate deferred module");
            }
            return Value(Undefined{});
          };
          moduleNamespace->properties["__deferred_pending__"] = Value(true);
          moduleNamespace->properties["__deferred_eval__"] = Value(deferredEvalFn);
        }

        promise->resolve(Value(moduleNamespace));
      } else {
        // Fallback: create a placeholder namespace (for cases where module loader isn't set up)
        auto moduleNamespace = std::make_shared<Object>();
        GarbageCollector::instance().reportAllocation(sizeof(Object));
        moduleNamespace->properties["__esModule"] = Value(true);
        moduleNamespace->properties["__moduleSpecifier"] = Value(specifier);
        promise->resolve(Value(moduleNamespace));
      }
    } catch (const std::exception& e) {
      std::string message = e.what();
      ErrorType errorType = ErrorType::Error;
      auto consumePrefix = [&](const std::string& prefix, ErrorType type) {
        if (message.rfind(prefix, 0) == 0) {
          errorType = type;
          message = message.substr(prefix.size());
          return true;
        }
        return false;
      };
      if (!consumePrefix("TypeError: ", ErrorType::TypeError) &&
          !consumePrefix("ReferenceError: ", ErrorType::ReferenceError) &&
          !consumePrefix("RangeError: ", ErrorType::RangeError) &&
          !consumePrefix("SyntaxError: ", ErrorType::SyntaxError) &&
          !consumePrefix("URIError: ", ErrorType::URIError) &&
          !consumePrefix("EvalError: ", ErrorType::EvalError) &&
          !consumePrefix("Error: ", ErrorType::Error)) {
        // Keep non-Error abrupt completions (e.g. thrown strings) as-is.
        promise->reject(Value(message));
        return Value(promise);
      }
      auto err = std::make_shared<Error>(errorType, message);
      promise->reject(Value(err));
    } catch (...) {
      auto err = std::make_shared<Error>(ErrorType::Error, "Failed to load module: " + specifier);
      promise->reject(Value(err));
    }

    return Value(promise);
  };
  env->define("import", Value(importFn));

  auto regExpPrototype = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  auto regExpMatchAll = std::make_shared<Function>();
  regExpMatchAll->isNative = true;
  regExpMatchAll->isConstructor = false;
  regExpMatchAll->properties["__uses_this_arg__"] = Value(true);
  regExpMatchAll->properties["__throw_on_new__"] = Value(true);
  regExpMatchAll->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isRegex()) {
      throw std::runtime_error("TypeError: RegExp.prototype[@@matchAll] called on non-RegExp");
    }

    auto regexPtr = std::get<std::shared_ptr<Regex>>(args[0].data);
    std::string input = args.size() > 1 ? args[1].toString() : "";
    bool global = regexPtr->flags.find('g') != std::string::npos;
    bool unicodeMode = regexPtr->flags.find('u') != std::string::npos ||
                       regexPtr->flags.find('v') != std::string::npos;

    auto allMatches = std::make_shared<Array>();
    GarbageCollector::instance().reportAllocation(sizeof(Array));

#if USE_SIMPLE_REGEX
    std::string remaining = input;
    size_t offsetBytes = 0;
    std::vector<simple_regex::Regex::Match> matches;
    while (regexPtr->regex->search(remaining, matches)) {
      if (matches.empty()) break;
      auto matchObj = std::make_shared<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      for (size_t i = 0; i < matches.size(); ++i) {
        matchObj->properties[std::to_string(i)] = Value(matches[i].str);
      }
      size_t matchStartBytes = offsetBytes + matches[0].start;
      double matchIndex = static_cast<double>(unicode::utf8Length(input.substr(0, matchStartBytes)));
      matchObj->properties["index"] = Value(matchIndex);
      matchObj->properties["input"] = Value(input);
      allMatches->elements.push_back(Value(matchObj));

      if (!global) break;

      size_t matchAdvance = matches[0].start + matches[0].str.length();
      if (matchAdvance == 0) {
        if (!remaining.empty()) {
          if (unicodeMode) {
            matchAdvance = unicode::utf8SequenceLength(static_cast<uint8_t>(remaining[0]));
          } else {
            matchAdvance = 1;
          }
        } else {
          break;
        }
      }
      offsetBytes += matchAdvance;
      remaining = remaining.substr(matchAdvance);
      matches.clear();
    }
#else
    std::string::const_iterator searchStart = input.cbegin();
    std::smatch match;
    while (std::regex_search(searchStart, input.cend(), match, regexPtr->regex)) {
      auto matchObj = std::make_shared<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      for (size_t i = 0; i < match.size(); ++i) {
        matchObj->properties[std::to_string(i)] = Value(match[i].str());
      }
      size_t matchStartBytes = static_cast<size_t>(match.position() + (searchStart - input.cbegin()));
      double matchIndex = static_cast<double>(unicode::utf8Length(input.substr(0, matchStartBytes)));
      matchObj->properties["index"] = Value(matchIndex);
      matchObj->properties["input"] = Value(input);
      allMatches->elements.push_back(Value(matchObj));

      if (!global) break;
      searchStart = match.suffix().first;
      if (match[0].length() == 0) {
        if (searchStart != input.cend()) {
          if (unicodeMode) {
            size_t byteOffset = static_cast<size_t>(searchStart - input.cbegin());
            size_t advance = unicode::utf8SequenceLength(static_cast<uint8_t>(input[byteOffset]));
            std::advance(searchStart, static_cast<std::ptrdiff_t>(advance));
          } else {
            ++searchStart;
          }
        } else {
          break;  // empty match at end of string, stop
        }
      }
    }
#endif

    return Value(allMatches);
  };
  regExpPrototype->properties[WellKnownSymbols::matchAllKey()] = Value(regExpMatchAll);

  auto regExpConstructor = std::make_shared<Function>();
  regExpConstructor->isNative = true;
  regExpConstructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(Undefined{});
    std::string pattern = args[0].toString();
    std::string flags = args.size() > 1 ? args[1].toString() : "";
    return Value(std::make_shared<Regex>(pattern, flags));
  };
  regExpConstructor->properties["prototype"] = Value(regExpPrototype);
  env->define("RegExp", Value(regExpConstructor));

  // Error constructors
  auto createErrorConstructor = [](ErrorType type, const std::string& name) {
    auto prototype = std::make_shared<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    auto func = std::make_shared<Function>();
    func->isNative = true;
    func->isConstructor = true;
    func->nativeFunc = [type](const std::vector<Value>& args) -> Value {
      std::string message = args.empty() ? "" : args[0].toString();
      return Value(std::make_shared<Error>(type, message));
    };
    func->properties["__error_type__"] = Value(static_cast<double>(static_cast<int>(type)));
    func->properties["prototype"] = Value(prototype);
    prototype->properties["constructor"] = Value(func);
    prototype->properties["name"] = Value(name);
    return func;
  };

  auto errorCtor = createErrorConstructor(ErrorType::Error, "Error");
  auto typeErrorCtor = createErrorConstructor(ErrorType::TypeError, "TypeError");
  auto referenceErrorCtor = createErrorConstructor(ErrorType::ReferenceError, "ReferenceError");
  auto rangeErrorCtor = createErrorConstructor(ErrorType::RangeError, "RangeError");
  auto syntaxErrorCtor = createErrorConstructor(ErrorType::SyntaxError, "SyntaxError");
  auto uriErrorCtor = createErrorConstructor(ErrorType::URIError, "URIError");
  auto evalErrorCtor = createErrorConstructor(ErrorType::EvalError, "EvalError");

  env->define("Error", Value(errorCtor));
  env->define("TypeError", Value(typeErrorCtor));
  env->define("ReferenceError", Value(referenceErrorCtor));
  env->define("RangeError", Value(rangeErrorCtor));
  env->define("SyntaxError", Value(syntaxErrorCtor));
  env->define("URIError", Value(uriErrorCtor));
  env->define("EvalError", Value(evalErrorCtor));

  // Map constructor
  auto mapConstructor = std::make_shared<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  mapConstructor->isNative = true;
  mapConstructor->isConstructor = true;
  mapConstructor->properties["name"] = Value(std::string("Map"));
  mapConstructor->properties["length"] = Value(0.0);
  mapConstructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto mapObj = std::make_shared<Map>();
    GarbageCollector::instance().reportAllocation(sizeof(Map));

    if (!args.empty() && args[0].isArray()) {
      auto entriesArr = std::get<std::shared_ptr<Array>>(args[0].data);
      for (const auto& entryVal : entriesArr->elements) {
        if (entryVal.isArray()) {
          auto entryArr = std::get<std::shared_ptr<Array>>(entryVal.data);
          if (entryArr->elements.size() >= 2) {
            mapObj->set(entryArr->elements[0], entryArr->elements[1]);
          }
        }
      }
    }

    return Value(mapObj);
  };
  auto mapPrototype = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  mapConstructor->properties["prototype"] = Value(mapPrototype);
  mapPrototype->properties["constructor"] = Value(mapConstructor);
  env->define("Map", Value(mapConstructor));

  // Set constructor
  auto setConstructor = std::make_shared<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  setConstructor->isNative = true;
  setConstructor->isConstructor = true;
  setConstructor->properties["name"] = Value(std::string("Set"));
  setConstructor->properties["length"] = Value(0.0);
  setConstructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto setObj = std::make_shared<Set>();
    GarbageCollector::instance().reportAllocation(sizeof(Set));

    if (!args.empty() && args[0].isArray()) {
      auto entriesArr = std::get<std::shared_ptr<Array>>(args[0].data);
      for (const auto& entryVal : entriesArr->elements) {
        setObj->add(entryVal);
      }
    }

    return Value(setObj);
  };
  auto setPrototype = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  setConstructor->properties["prototype"] = Value(setPrototype);
  setPrototype->properties["constructor"] = Value(setConstructor);
  env->define("Set", Value(setConstructor));

  // WeakMap constructor
  auto weakMapConstructor = std::make_shared<Function>();
  weakMapConstructor->isNative = true;
  weakMapConstructor->nativeFunc = [](const std::vector<Value>&) -> Value {
    auto wm = std::make_shared<WeakMap>();
    GarbageCollector::instance().reportAllocation(sizeof(WeakMap));
    return Value(wm);
  };
  env->define("WeakMap", Value(weakMapConstructor));

  // WeakSet constructor
  auto weakSetConstructor = std::make_shared<Function>();
  weakSetConstructor->isNative = true;
  weakSetConstructor->nativeFunc = [](const std::vector<Value>&) -> Value {
    auto ws = std::make_shared<WeakSet>();
    GarbageCollector::instance().reportAllocation(sizeof(WeakSet));
    return Value(ws);
  };
  env->define("WeakSet", Value(weakSetConstructor));

  // Proxy constructor
  auto proxyConstructor = std::make_shared<Function>();
  proxyConstructor->isNative = true;
  proxyConstructor->isConstructor = true;
  proxyConstructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) {
      // Need target and handler
      return Value(Undefined{});
    }
    auto proxy = std::make_shared<Proxy>(args[0], args[1]);
    GarbageCollector::instance().reportAllocation(sizeof(Proxy));
    return Value(proxy);
  };
  env->define("Proxy", Value(proxyConstructor));

  // Reflect object with static methods
  auto reflectObj = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // Reflect.get(target, property)
  auto reflectGet = std::make_shared<Function>();
  reflectGet->isNative = true;
  reflectGet->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(Undefined{});

    const Value& target = args[0];
    std::string prop = args[1].toString();

    if (target.isObject()) {
      auto obj = std::get<std::shared_ptr<Object>>(target.data);
      if (obj->isModuleNamespace) {
        auto getterIt = obj->properties.find("__get_" + prop);
        if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
          Interpreter* interpreter = getGlobalInterpreter();
          if (interpreter) {
            Value out = interpreter->callForHarness(getterIt->second, {}, target);
            if (interpreter->hasError()) {
              Value err = interpreter->getError();
              interpreter->clearError();
              throw std::runtime_error(err.toString());
            }
            return out;
          }
        }
      }
      auto it = obj->properties.find(prop);
      if (it != obj->properties.end()) {
        return it->second;
      }
    }
    return Value(Undefined{});
  };
  reflectObj->properties["get"] = Value(reflectGet);

  // Reflect.set(target, property, value)
  auto reflectSet = std::make_shared<Function>();
  reflectSet->isNative = true;
  reflectSet->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 3) return Value(false);

    const Value& target = args[0];
    std::string prop = args[1].toString();
    const Value& value = args[2];

    if (target.isObject()) {
      auto obj = std::get<std::shared_ptr<Object>>(target.data);
      if (obj->isModuleNamespace) {
        return Value(false);
      }
      obj->properties[prop] = value;
      return Value(true);
    }
    return Value(false);
  };
  reflectObj->properties["set"] = Value(reflectSet);

  // Reflect.has(target, property)
  auto reflectHas = std::make_shared<Function>();
  reflectHas->isNative = true;
  reflectHas->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(false);

    const Value& target = args[0];
    std::string prop = args[1].toString();

    if (target.isObject()) {
      auto obj = std::get<std::shared_ptr<Object>>(target.data);
      if (obj->isModuleNamespace) {
        if (prop == WellKnownSymbols::toStringTagKey()) {
          return Value(true);
        }
        return Value(isModuleNamespaceExportKey(obj, prop));
      }
      return Value(obj->properties.find(prop) != obj->properties.end());
    }
    return Value(false);
  };
  reflectObj->properties["has"] = Value(reflectHas);

  // Reflect.deleteProperty(target, property)
  auto reflectDelete = std::make_shared<Function>();
  reflectDelete->isNative = true;
  reflectDelete->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(false);

    const Value& target = args[0];
    std::string prop = args[1].toString();

    if (target.isObject()) {
      auto obj = std::get<std::shared_ptr<Object>>(target.data);
      if (obj->isModuleNamespace) {
        if (prop == WellKnownSymbols::toStringTagKey()) {
          return Value(false);
        }
        return Value(!isModuleNamespaceExportKey(obj, prop));
      }
      return Value(obj->properties.erase(prop) > 0);
    }
    return Value(false);
  };
  reflectObj->properties["deleteProperty"] = Value(reflectDelete);

  // Reflect.apply(target, thisArg, argumentsList)
  auto reflectApply = std::make_shared<Function>();
  reflectApply->isNative = true;
  reflectApply->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 3 || !args[0].isFunction()) {
      return Value(Undefined{});
    }

    auto func = std::get<std::shared_ptr<Function>>(args[0].data);
    const Value& thisArg = args[1];

    std::vector<Value> callArgs;
    if (args[2].isArray()) {
      auto arr = std::get<std::shared_ptr<Array>>(args[2].data);
      callArgs = arr->elements;
    }

    if (func->isNative) {
      // For native functions, we can't pass 'this' directly
      // Native functions typically don't use 'this'
      return func->nativeFunc(callArgs);
    }
    // For non-native functions, we'd need interpreter access
    return Value(Undefined{});
  };
  reflectObj->properties["apply"] = Value(reflectApply);

  // Reflect.construct(target, argumentsList, newTarget?)
  auto reflectConstruct = std::make_shared<Function>();
  reflectConstruct->isNative = true;
  reflectConstruct->properties["__reflect_construct__"] = Value(true);
  reflectConstruct->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2 || !args[0].isFunction()) {
      throw std::runtime_error("TypeError: Reflect.construct target is not a function");
    }

    auto func = std::get<std::shared_ptr<Function>>(args[0].data);
    if (!func->isConstructor) {
      throw std::runtime_error("TypeError: target is not a constructor");
    }

    if (args.size() >= 3) {
      if (!args[2].isFunction()) {
        throw std::runtime_error("TypeError: newTarget is not a constructor");
      }
      auto newTarget = std::get<std::shared_ptr<Function>>(args[2].data);
      if (!newTarget->isConstructor) {
        throw std::runtime_error("TypeError: newTarget is not a constructor");
      }
    }

    std::vector<Value> callArgs;
    if (args[1].isArray()) {
      auto arr = std::get<std::shared_ptr<Array>>(args[1].data);
      callArgs = arr->elements;
    }

    if (func->isNative) {
      return func->nativeFunc(callArgs);
    }
    // For non-native functions, we'd need interpreter access
    return Value(Undefined{});
  };
  reflectObj->properties["construct"] = Value(reflectConstruct);

  // Reflect.getPrototypeOf(target) - returns null (limited prototype support)
  auto reflectGetPrototypeOf = std::make_shared<Function>();
  reflectGetPrototypeOf->isNative = true;
  reflectGetPrototypeOf->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(Null{});
    // LightJS has limited prototype support, return null
    return Value(Null{});
  };
  reflectObj->properties["getPrototypeOf"] = Value(reflectGetPrototypeOf);

  // Reflect.setPrototypeOf(target, proto) - returns false (not supported)
  auto reflectSetPrototypeOf = std::make_shared<Function>();
  reflectSetPrototypeOf->isNative = true;
  reflectSetPrototypeOf->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // LightJS doesn't support dynamic prototype modification
    return Value(false);
  };
  reflectObj->properties["setPrototypeOf"] = Value(reflectSetPrototypeOf);

  // Reflect.isExtensible(target) - check if object can be extended
  auto reflectIsExtensible = std::make_shared<Function>();
  reflectIsExtensible->isNative = true;
  reflectIsExtensible->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(false);
    if (args[0].isObject()) {
      auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
      if (obj->isModuleNamespace) {
        return Value(false);
      }
      return Value(!obj->sealed && !obj->frozen);
    }
    return Value(false);
  };
  reflectObj->properties["isExtensible"] = Value(reflectIsExtensible);

  // Reflect.preventExtensions(target) - prevent adding new properties
  auto reflectPreventExtensions = std::make_shared<Function>();
  reflectPreventExtensions->isNative = true;
  reflectPreventExtensions->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isObject()) {
      return Value(false);
    }
    auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
    if (obj->isModuleNamespace) {
      return Value(true);
    }
    obj->sealed = true;
    return Value(true);
  };
  reflectObj->properties["preventExtensions"] = Value(reflectPreventExtensions);

  // Reflect.ownKeys(target) - return array of own property keys
  auto reflectOwnKeys = std::make_shared<Function>();
  reflectOwnKeys->isNative = true;
  reflectOwnKeys->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto result = std::make_shared<Array>();

    if (args.empty()) return Value(result);

    if (args[0].isObject()) {
      auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
      if (obj->isModuleNamespace) {
        for (const auto& key : obj->moduleExportNames) {
          result->elements.push_back(Value(key));
        }
        result->elements.push_back(WellKnownSymbols::toStringTag());
        return Value(result);
      }
      for (const auto& [key, _] : obj->properties) {
        result->elements.push_back(Value(key));
      }
    } else if (args[0].isArray()) {
      auto arr = std::get<std::shared_ptr<Array>>(args[0].data);
      for (size_t i = 0; i < arr->elements.size(); ++i) {
        result->elements.push_back(Value(std::to_string(i)));
      }
      result->elements.push_back(Value("length"));
    }

    return Value(result);
  };
  reflectObj->properties["ownKeys"] = Value(reflectOwnKeys);

  // Reflect.getOwnPropertyDescriptor(target, propertyKey)
  auto reflectGetOwnPropertyDescriptor = std::make_shared<Function>();
  reflectGetOwnPropertyDescriptor->isNative = true;
  reflectGetOwnPropertyDescriptor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(Undefined{});

    std::string prop = args[1].toString();

    if (args[0].isObject()) {
      auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
      if (obj->isModuleNamespace) {
        auto descriptor = std::make_shared<Object>();
        if (prop == WellKnownSymbols::toStringTagKey()) {
          descriptor->properties["value"] = Value(std::string("Module"));
          descriptor->properties["writable"] = Value(false);
          descriptor->properties["enumerable"] = Value(false);
          descriptor->properties["configurable"] = Value(false);
          return Value(descriptor);
        }
        if (!isModuleNamespaceExportKey(obj, prop)) {
          return Value(Undefined{});
        }
        auto getterIt = obj->properties.find("__get_" + prop);
        if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
          Interpreter* interpreter = getGlobalInterpreter();
          if (interpreter) {
            Value out = interpreter->callForHarness(getterIt->second, {}, args[0]);
            if (interpreter->hasError()) {
              Value err = interpreter->getError();
              interpreter->clearError();
              throw std::runtime_error(err.toString());
            }
            descriptor->properties["value"] = out;
          } else {
            descriptor->properties["value"] = Value(Undefined{});
          }
        } else if (auto it = obj->properties.find(prop); it != obj->properties.end()) {
          descriptor->properties["value"] = it->second;
        } else {
          descriptor->properties["value"] = Value(Undefined{});
        }
        descriptor->properties["writable"] = Value(true);
        descriptor->properties["enumerable"] = Value(true);
        descriptor->properties["configurable"] = Value(false);
        return Value(descriptor);
      }
      auto it = obj->properties.find(prop);
      if (it == obj->properties.end()) {
        return Value(Undefined{});
      }
      // Create a simplified property descriptor
      auto descriptor = std::make_shared<Object>();
      descriptor->properties["value"] = it->second;
      descriptor->properties["writable"] = Value(!obj->frozen);
      descriptor->properties["enumerable"] = Value(true);
      descriptor->properties["configurable"] = Value(!obj->sealed);
      return Value(descriptor);
    }

    return Value(Undefined{});
  };
  reflectObj->properties["getOwnPropertyDescriptor"] = Value(reflectGetOwnPropertyDescriptor);

  // Reflect.defineProperty(target, propertyKey, attributes)
  auto reflectDefineProperty = std::make_shared<Function>();
  reflectDefineProperty->isNative = true;
  reflectDefineProperty->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 3) return Value(false);
    if (!args[0].isObject()) return Value(false);

    auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
    std::string prop = args[1].toString();
    if (obj->isModuleNamespace) {
      if (!args[2].isObject()) {
        return Value(false);
      }
      auto descriptor = std::get<std::shared_ptr<Object>>(args[2].data);
      return Value(defineModuleNamespaceProperty(obj, prop, descriptor));
    }

    // Check if object is sealed/frozen
    bool isNewProp = obj->properties.find(prop) == obj->properties.end();
    if ((obj->sealed && isNewProp) || obj->frozen) {
      return Value(false);
    }

    // Get value from descriptor
    if (args[2].isObject()) {
      auto descriptor = std::get<std::shared_ptr<Object>>(args[2].data);
      auto valueIt = descriptor->properties.find("value");
      if (valueIt != descriptor->properties.end()) {
        obj->properties[prop] = valueIt->second;
        return Value(true);
      }
    }

    return Value(false);
  };
  reflectObj->properties["defineProperty"] = Value(reflectDefineProperty);

  env->define("Reflect", Value(reflectObj));

  // Number object with static methods
  auto numberObj = std::make_shared<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  numberObj->isNative = true;
  numberObj->isConstructor = true;
  numberObj->properties["__wrap_primitive__"] = Value(true);
  numberObj->properties["name"] = Value(std::string("Number"));
  numberObj->properties["length"] = Value(1.0);
  numberObj->nativeFunc = [toPrimitive](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(0.0);
    }
    Value primitive = toPrimitive(args[0], false);
    if (primitive.isSymbol()) {
      throw std::runtime_error("TypeError: Cannot convert Symbol to number");
    }
    return Value(primitive.toNumber());
  };

  // Number.parseInt
  auto parseIntFn = std::make_shared<Function>();
  parseIntFn->isNative = true;
  parseIntFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    std::string str = args[0].toString();
    int radix = args.size() > 1 ? static_cast<int>(args[1].toNumber()) : 10;

    if (radix != 0 && (radix < 2 || radix > 36)) {
      return Value(std::numeric_limits<double>::quiet_NaN());
    }

    // Trim whitespace
    size_t start = str.find_first_not_of(" \t\n\r\f\v");
    if (start == std::string::npos) {
      return Value(std::numeric_limits<double>::quiet_NaN());
    }
    str = str.substr(start);

    // Handle sign
    bool negative = false;
    if (!str.empty() && (str[0] == '+' || str[0] == '-')) {
      negative = (str[0] == '-');
      str = str.substr(1);
    }

    // Auto-detect radix if 0
    if (radix == 0) {
      if (str.size() >= 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        radix = 16;
        str = str.substr(2);
      } else {
        radix = 10;
      }
    }

    try {
      size_t idx;
      long long result = std::stoll(str, &idx, radix);
      if (idx == 0) {
        return Value(std::numeric_limits<double>::quiet_NaN());
      }
      return Value(static_cast<double>(negative ? -result : result));
    } catch (...) {
      return Value(std::numeric_limits<double>::quiet_NaN());
    }
  };
  numberObj->properties["parseInt"] = Value(parseIntFn);

  // Number.parseFloat
  auto parseFloatFn = std::make_shared<Function>();
  parseFloatFn->isNative = true;
  parseFloatFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    std::string str = args[0].toString();

    // Trim whitespace
    size_t start = str.find_first_not_of(" \t\n\r\f\v");
    if (start == std::string::npos) {
      return Value(std::numeric_limits<double>::quiet_NaN());
    }
    str = str.substr(start);

    if (str.empty()) {
      return Value(std::numeric_limits<double>::quiet_NaN());
    }

    try {
      size_t idx;
      double result = std::stod(str, &idx);
      if (idx == 0) {
        return Value(std::numeric_limits<double>::quiet_NaN());
      }
      return Value(result);
    } catch (...) {
      return Value(std::numeric_limits<double>::quiet_NaN());
    }
  };
  numberObj->properties["parseFloat"] = Value(parseFloatFn);

  // Number.isNaN
  auto isNaNFn = std::make_shared<Function>();
  isNaNFn->isNative = true;
  isNaNFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isNumber()) return Value(false);
    double num = std::get<double>(args[0].data);
    return Value(std::isnan(num));
  };
  numberObj->properties["isNaN"] = Value(isNaNFn);

  // Number.isFinite
  auto isFiniteFn = std::make_shared<Function>();
  isFiniteFn->isNative = true;
  isFiniteFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isNumber()) return Value(false);
    double num = std::get<double>(args[0].data);
    return Value(std::isfinite(num));
  };
  numberObj->properties["isFinite"] = Value(isFiniteFn);

  // Number.isInteger
  auto isIntegerFn = std::make_shared<Function>();
  isIntegerFn->isNative = true;
  isIntegerFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isNumber()) return Value(false);
    double num = std::get<double>(args[0].data);
    return Value(std::isfinite(num) && std::floor(num) == num);
  };
  numberObj->properties["isInteger"] = Value(isIntegerFn);

  // Number.isSafeInteger
  auto isSafeIntegerFn = std::make_shared<Function>();
  isSafeIntegerFn->isNative = true;
  isSafeIntegerFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isNumber()) return Value(false);
    double num = std::get<double>(args[0].data);
    // Safe integers are integers in range [-(2^53-1), 2^53-1]
    const double MAX_SAFE_INTEGER = 9007199254740991.0;  // 2^53 - 1
    return Value(std::isfinite(num) && std::floor(num) == num &&
                 num >= -MAX_SAFE_INTEGER && num <= MAX_SAFE_INTEGER);
  };
  numberObj->properties["isSafeInteger"] = Value(isSafeIntegerFn);

  // Number constants
  numberObj->properties["MAX_VALUE"] = Value(std::numeric_limits<double>::max());
  numberObj->properties["MIN_VALUE"] = Value(std::numeric_limits<double>::min());
  numberObj->properties["POSITIVE_INFINITY"] = Value(std::numeric_limits<double>::infinity());
  numberObj->properties["NEGATIVE_INFINITY"] = Value(-std::numeric_limits<double>::infinity());
  numberObj->properties["NaN"] = Value(std::numeric_limits<double>::quiet_NaN());
  numberObj->properties["MAX_SAFE_INTEGER"] = Value(9007199254740991.0);  // 2^53 - 1
  numberObj->properties["MIN_SAFE_INTEGER"] = Value(-9007199254740991.0);  // -(2^53 - 1)
  numberObj->properties["EPSILON"] = Value(2.220446049250313e-16);  // 2^-52

  env->define("Number", Value(numberObj));

  // Boolean constructor
  auto booleanObj = std::make_shared<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  booleanObj->isNative = true;
  booleanObj->isConstructor = true;
  booleanObj->properties["__wrap_primitive__"] = Value(true);
  booleanObj->properties["name"] = Value(std::string("Boolean"));
  booleanObj->properties["length"] = Value(1.0);
  booleanObj->nativeFunc = [toPrimitive](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(false);
    }
    Value primitive = toPrimitive(args[0], false);
    return Value(primitive.toBool());
  };
  env->define("Boolean", Value(booleanObj));

  // Global parseInt and parseFloat (aliases)
  env->define("parseInt", Value(parseIntFn));
  env->define("parseFloat", Value(parseFloatFn));

  // Global isNaN (different from Number.isNaN - coerces to number first)
  auto globalIsNaNFn = std::make_shared<Function>();
  globalIsNaNFn->isNative = true;
  globalIsNaNFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(true);
    double num = args[0].toNumber();
    return Value(std::isnan(num));
  };
  env->define("isNaN", Value(globalIsNaNFn));

  // Global isFinite
  auto globalIsFiniteFn = std::make_shared<Function>();
  globalIsFiniteFn->isNative = true;
  globalIsFiniteFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(false);
    double num = args[0].toNumber();
    return Value(std::isfinite(num));
  };
  env->define("isFinite", Value(globalIsFiniteFn));

  // Array constructor with static methods
  auto arrayConstructorFn = std::make_shared<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  arrayConstructorFn->isNative = true;
  arrayConstructorFn->isConstructor = true;
  arrayConstructorFn->properties["name"] = Value(std::string("Array"));
  arrayConstructorFn->properties["length"] = Value(1.0);
  arrayConstructorFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto result = std::make_shared<Array>();
    GarbageCollector::instance().reportAllocation(sizeof(Array));

    if (args.empty()) {
      return Value(result);
    }

    if (args.size() == 1 && args[0].isNumber()) {
      double lengthNum = args[0].toNumber();
      if (!std::isfinite(lengthNum) || lengthNum < 0 || std::floor(lengthNum) != lengthNum) {
        throw std::runtime_error("RangeError: Invalid array length");
      }
      result->elements.resize(static_cast<size_t>(lengthNum), Value(Undefined{}));
      return Value(result);
    }

    result->elements = args;
    return Value(result);
  };

  auto arrayConstructorObj = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  arrayConstructorObj->properties["__callable_object__"] = Value(true);
  arrayConstructorObj->properties["constructor"] = Value(arrayConstructorFn);

  auto arrayPrototype = std::make_shared<Object>();
  arrayConstructorObj->properties["prototype"] = Value(arrayPrototype);
  arrayConstructorFn->properties["prototype"] = Value(arrayPrototype);
  arrayPrototype->properties["constructor"] = Value(arrayConstructorObj);
  env->define("__array_prototype__", Value(arrayPrototype));

  // Array.prototype.push - receives this (array) via __uses_this_arg__
  auto arrayProtoPush = std::make_shared<Function>();
  arrayProtoPush->isNative = true;
  arrayProtoPush->properties["__uses_this_arg__"] = Value(true);
  arrayProtoPush->properties["name"] = Value(std::string("push"));
  arrayProtoPush->properties["length"] = Value(1.0);
  arrayProtoPush->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      throw std::runtime_error("TypeError: Array.prototype.push called on non-array");
    }
    auto arr = std::get<std::shared_ptr<Array>>(args[0].data);
    for (size_t i = 1; i < args.size(); ++i) {
      arr->elements.push_back(args[i]);
    }
    return Value(static_cast<double>(arr->elements.size()));
  };
  arrayPrototype->properties["push"] = Value(arrayProtoPush);

  // Array.prototype.join
  auto arrayProtoJoin = std::make_shared<Function>();
  arrayProtoJoin->isNative = true;
  arrayProtoJoin->properties["__uses_this_arg__"] = Value(true);
  arrayProtoJoin->properties["name"] = Value(std::string("join"));
  arrayProtoJoin->properties["length"] = Value(1.0);
  arrayProtoJoin->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      throw std::runtime_error("TypeError: Array.prototype.join called on non-array");
    }
    auto arr = std::get<std::shared_ptr<Array>>(args[0].data);
    std::string separator = args.size() > 1 && !args[1].isUndefined() ? args[1].toString() : ",";
    std::string result;
    for (size_t i = 0; i < arr->elements.size(); ++i) {
      if (i > 0) result += separator;
      if (!arr->elements[i].isUndefined() && !arr->elements[i].isNull()) {
        result += arr->elements[i].toString();
      }
    }
    return Value(result);
  };
  arrayPrototype->properties["join"] = Value(arrayProtoJoin);

  // Array.isArray
  auto isArrayFn = std::make_shared<Function>();
  isArrayFn->isNative = true;
  isArrayFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(false);
    return Value(args[0].isArray());
  };
  arrayConstructorObj->properties["isArray"] = Value(isArrayFn);

  // Array.from - creates array from array-like or iterable object
  auto fromFn = std::make_shared<Function>();
  fromFn->isNative = true;
  fromFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto result = std::make_shared<Array>();

    if (args.empty()) {
      return Value(result);
    }

    const Value& arrayLike = args[0];
    Value mapFn = args.size() > 1 ? args[1] : Value(Undefined{});
    Value thisArg = args.size() > 2 ? args[2] : Value(Undefined{});
    bool hasMapFn = !mapFn.isUndefined();

    if (hasMapFn && !mapFn.isFunction()) {
      throw std::runtime_error("TypeError: Array.from mapper must be a function");
    }

    auto applyMap = [&](const Value& value, size_t index) -> Value {
      if (!hasMapFn) {
        return value;
      }

      std::vector<Value> mapperArgs = {value, Value(static_cast<double>(index))};
      Interpreter* interpreter = getGlobalInterpreter();
      if (interpreter) {
        Value mapped = interpreter->callForHarness(mapFn, mapperArgs, thisArg);
        if (interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw std::runtime_error(err.toString());
        }
        return mapped;
      }

      auto mapper = std::get<std::shared_ptr<Function>>(mapFn.data);
      if (mapper->isNative) {
        auto itUsesThis = mapper->properties.find("__uses_this_arg__");
        if (itUsesThis != mapper->properties.end() && itUsesThis->second.isBool() && itUsesThis->second.toBool()) {
          std::vector<Value> nativeArgs;
          nativeArgs.reserve(mapperArgs.size() + 1);
          nativeArgs.push_back(thisArg);
          nativeArgs.insert(nativeArgs.end(), mapperArgs.begin(), mapperArgs.end());
          return mapper->nativeFunc(nativeArgs);
        }
        return mapper->nativeFunc(mapperArgs);
      }

      return value;
    };

    // If it's already an array, copy it
    if (arrayLike.isArray()) {
      auto srcArray = std::get<std::shared_ptr<Array>>(arrayLike.data);
      size_t index = 0;
      for (const auto& elem : srcArray->elements) {
        result->elements.push_back(applyMap(elem, index++));
      }
      return Value(result);
    }

    // If it's a string, convert each character to array element
    if (arrayLike.isString()) {
      std::string str = std::get<std::string>(arrayLike.data);
      size_t index = 0;
      for (char c : str) {
        result->elements.push_back(applyMap(Value(std::string(1, c)), index++));
      }
      return Value(result);
    }

    // If it's an iterator object, consume it
    if (arrayLike.isObject()) {
      Value iteratorValue = arrayLike;
      auto srcObj = std::get<std::shared_ptr<Object>>(arrayLike.data);
      const auto& iteratorKey = WellKnownSymbols::iteratorKey();

      auto iteratorMethodIt = srcObj->properties.find(iteratorKey);
      if (iteratorMethodIt != srcObj->properties.end() && iteratorMethodIt->second.isFunction()) {
        Interpreter* interpreter = getGlobalInterpreter();
        if (interpreter) {
          iteratorValue = interpreter->callForHarness(iteratorMethodIt->second, {}, arrayLike);
          if (interpreter->hasError()) {
            Value err = interpreter->getError();
            interpreter->clearError();
            throw std::runtime_error(err.toString());
          }
        } else {
          auto iterMethod = std::get<std::shared_ptr<Function>>(iteratorMethodIt->second.data);
          iteratorValue = iterMethod->isNative ? iterMethod->nativeFunc({}) : Value(Undefined{});
        }
      }

      if (iteratorValue.isObject()) {
        auto iterObj = std::get<std::shared_ptr<Object>>(iteratorValue.data);
        auto nextIt = iterObj->properties.find("next");
        if (nextIt != iterObj->properties.end() && nextIt->second.isFunction()) {
          size_t index = 0;
          while (true) {
            Value stepResult;
            Interpreter* interpreter = getGlobalInterpreter();
            if (interpreter) {
              stepResult = interpreter->callForHarness(nextIt->second, {}, iteratorValue);
              if (interpreter->hasError()) {
                Value err = interpreter->getError();
                interpreter->clearError();
                throw std::runtime_error(err.toString());
              }
            } else {
              auto nextFn = std::get<std::shared_ptr<Function>>(nextIt->second.data);
              stepResult = nextFn->isNative ? nextFn->nativeFunc({}) : Value(Undefined{});
            }

            if (!stepResult.isObject()) break;
            auto stepObj = std::get<std::shared_ptr<Object>>(stepResult.data);
            bool done = false;
            if (auto doneIt = stepObj->properties.find("done"); doneIt != stepObj->properties.end()) {
              done = doneIt->second.toBool();
            }
            if (done) break;

            Value element = Value(Undefined{});
            if (auto valueIt = stepObj->properties.find("value"); valueIt != stepObj->properties.end()) {
              element = valueIt->second;
            }
            result->elements.push_back(applyMap(element, index++));
          }
          return Value(result);
        }
      }
    }

    // Otherwise return empty array
    return Value(result);
  };
  arrayConstructorObj->properties["from"] = Value(fromFn);

  // Array.of - creates array from arguments
  auto ofFn = std::make_shared<Function>();
  ofFn->isNative = true;
  ofFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto result = std::make_shared<Array>();
    result->elements = args;
    return Value(result);
  };
  arrayConstructorObj->properties["of"] = Value(ofFn);

  env->define("Array", Value(arrayConstructorObj));

  // Promise constructor
  auto promiseFunc = std::make_shared<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  promiseFunc->isNative = true;
  promiseFunc->isConstructor = true;
  promiseFunc->properties["name"] = Value(std::string("Promise"));
  promiseFunc->properties["length"] = Value(1.0);

  auto promiseConstructor = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  auto promisePrototype = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  promiseConstructor->properties["prototype"] = Value(promisePrototype);
  promisePrototype->properties["constructor"] = Value(promiseFunc);

  auto invokePromiseCallback = [](const std::shared_ptr<Function>& callback, const Value& arg) -> Value {
    if (!callback) {
      return arg;
    }
    if (callback->isNative) {
      return callback->nativeFunc({arg});
    }
    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) {
      throw std::runtime_error("TypeError: Interpreter unavailable");
    }
    interpreter->clearError();
    Value out = interpreter->callForHarness(Value(callback), {arg}, Value(Undefined{}));
    if (interpreter->hasError()) {
      Value err = interpreter->getError();
      interpreter->clearError();
      throw std::runtime_error(err.toString());
    }
    return out;
  };

  auto promiseProtoThen = std::make_shared<Function>();
  promiseProtoThen->isNative = true;
  promiseProtoThen->properties["__uses_this_arg__"] = Value(true);
  promiseProtoThen->properties["name"] = Value(std::string("then"));
  promiseProtoThen->properties["length"] = Value(2.0);
  promiseProtoThen->nativeFunc = [invokePromiseCallback](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isPromise()) {
      throw std::runtime_error("TypeError: Promise.prototype.then called on non-Promise");
    }

    auto promise = std::get<std::shared_ptr<Promise>>(args[0].data);
    std::function<Value(Value)> onFulfilled = nullptr;
    std::function<Value(Value)> onRejected = nullptr;

    if (args.size() > 1 && args[1].isFunction()) {
      auto callback = std::get<std::shared_ptr<Function>>(args[1].data);
      onFulfilled = [callback, invokePromiseCallback](Value v) -> Value {
        return invokePromiseCallback(callback, v);
      };
    }
    if (args.size() > 2 && args[2].isFunction()) {
      auto callback = std::get<std::shared_ptr<Function>>(args[2].data);
      onRejected = [callback, invokePromiseCallback](Value v) -> Value {
        return invokePromiseCallback(callback, v);
      };
    }

    auto chained = promise->then(onFulfilled, onRejected);
    auto ctorIt = promise->properties.find("__constructor__");
    if (ctorIt != promise->properties.end()) {
      chained->properties["__constructor__"] = ctorIt->second;
    }
    return Value(chained);
  };
  promisePrototype->properties["then"] = Value(promiseProtoThen);
  promisePrototype->properties["__non_enum_then"] = Value(true);

  auto promiseProtoCatch = std::make_shared<Function>();
  promiseProtoCatch->isNative = true;
  promiseProtoCatch->properties["__uses_this_arg__"] = Value(true);
  promiseProtoCatch->properties["name"] = Value(std::string("catch"));
  promiseProtoCatch->properties["length"] = Value(1.0);
  promiseProtoCatch->nativeFunc = [invokePromiseCallback](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isPromise()) {
      throw std::runtime_error("TypeError: Promise.prototype.catch called on non-Promise");
    }

    auto promise = std::get<std::shared_ptr<Promise>>(args[0].data);
    std::function<Value(Value)> onRejected = nullptr;
    if (args.size() > 1 && args[1].isFunction()) {
      auto callback = std::get<std::shared_ptr<Function>>(args[1].data);
      onRejected = [callback, invokePromiseCallback](Value v) -> Value {
        return invokePromiseCallback(callback, v);
      };
    }

    auto chained = promise->catch_(onRejected);
    auto ctorIt = promise->properties.find("__constructor__");
    if (ctorIt != promise->properties.end()) {
      chained->properties["__constructor__"] = ctorIt->second;
    }
    return Value(chained);
  };
  promisePrototype->properties["catch"] = Value(promiseProtoCatch);
  promisePrototype->properties["__non_enum_catch"] = Value(true);

  auto promiseProtoFinally = std::make_shared<Function>();
  promiseProtoFinally->isNative = true;
  promiseProtoFinally->properties["__uses_this_arg__"] = Value(true);
  promiseProtoFinally->properties["name"] = Value(std::string("finally"));
  promiseProtoFinally->properties["length"] = Value(1.0);
  promiseProtoFinally->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isPromise()) {
      throw std::runtime_error("TypeError: Promise.prototype.finally called on non-Promise");
    }

    auto promise = std::get<std::shared_ptr<Promise>>(args[0].data);
    std::function<Value()> onFinally = nullptr;
    if (args.size() > 1 && args[1].isFunction()) {
      auto callback = std::get<std::shared_ptr<Function>>(args[1].data);
      onFinally = [callback]() -> Value {
        if (callback->isNative) {
          return callback->nativeFunc({});
        }
        Interpreter* interpreter = getGlobalInterpreter();
        if (!interpreter) {
          throw std::runtime_error("TypeError: Interpreter unavailable");
        }
        interpreter->clearError();
        Value out = interpreter->callForHarness(Value(callback), {}, Value(Undefined{}));
        if (interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw std::runtime_error(err.toString());
        }
        return out;
      };
    }

    auto chained = promise->finally(onFinally);
    auto ctorIt = promise->properties.find("__constructor__");
    if (ctorIt != promise->properties.end()) {
      chained->properties["__constructor__"] = ctorIt->second;
    }
    return Value(chained);
  };
  promisePrototype->properties["finally"] = Value(promiseProtoFinally);
  promisePrototype->properties["__non_enum_finally"] = Value(true);

  // Promise.resolve
  auto promiseResolve = std::make_shared<Function>();
  promiseResolve->isNative = true;
  promiseResolve->properties["__uses_this_arg__"] = Value(true);
  promiseResolve->properties["name"] = Value(std::string("resolve"));
  promiseResolve->properties["length"] = Value(1.0);
  promiseResolve->nativeFunc = [callChecked, getProperty, env, promiseFunc](const std::vector<Value>& args) -> Value {
    Value resolution = args.size() > 1 ? args[1] : Value(Undefined{});
    if (resolution.isPromise()) {
      return resolution;
    }

    auto promise = std::make_shared<Promise>();
    GarbageCollector::instance().reportAllocation(sizeof(Promise));
    promise->properties["__constructor__"] = Value(promiseFunc);

    auto getThenProperty = [&](const Value& candidate) -> std::pair<bool, Value> {
      if (candidate.isArray()) {
        auto arr = std::get<std::shared_ptr<Array>>(candidate.data);
        auto getterIt = arr->properties.find("__get_then");
        if (getterIt != arr->properties.end()) {
          if (getterIt->second.isFunction()) {
            return {true, callChecked(getterIt->second, {}, candidate)};
          }
          return {true, Value(Undefined{})};
        }
        auto ownIt = arr->properties.find("then");
        if (ownIt != arr->properties.end()) {
          return {true, ownIt->second};
        }
        if (auto arrayCtor = env->get("Array"); arrayCtor && arrayCtor->isObject()) {
          auto arrayObjPtr = std::get<std::shared_ptr<Object>>(arrayCtor->data);
          auto protoIt = arrayObjPtr->properties.find("prototype");
          if (protoIt != arrayObjPtr->properties.end() && protoIt->second.isObject()) {
            auto protoObj = std::get<std::shared_ptr<Object>>(protoIt->second.data);
            auto protoGetterIt = protoObj->properties.find("__get_then");
            if (protoGetterIt != protoObj->properties.end()) {
              if (protoGetterIt->second.isFunction()) {
                return {true, callChecked(protoGetterIt->second, {}, candidate)};
              }
              return {true, Value(Undefined{})};
            }
            auto protoThenIt = protoObj->properties.find("then");
            if (protoThenIt != protoObj->properties.end()) {
              return {true, protoThenIt->second};
            }
          }
        }
        return {false, Value(Undefined{})};
      }

      return getProperty(candidate, "then");
    };

    auto resolveSelf = std::make_shared<std::function<void(const Value&)>>();
    *resolveSelf = [promise, resolveSelf, getThenProperty, callChecked](const Value& value) {
      if (promise->state != PromiseState::Pending) {
        return;
      }

      if (value.isPromise()) {
        auto nested = std::get<std::shared_ptr<Promise>>(value.data);
        if (nested.get() == promise.get()) {
          promise->reject(Value(std::make_shared<Error>(ErrorType::TypeError, "Cannot resolve promise with itself")));
          return;
        }
        if (nested->state == PromiseState::Fulfilled) {
          (*resolveSelf)(nested->result);
          return;
        }
        if (nested->state == PromiseState::Rejected) {
          promise->reject(nested->result);
          return;
        }
        nested->then(
          [resolveSelf](Value fulfilled) -> Value {
            (*resolveSelf)(fulfilled);
            return fulfilled;
          },
          [promise](Value reason) -> Value {
            promise->reject(reason);
            return reason;
          });
        return;
      }

      if (value.isObject() || value.isArray() || value.isFunction() || value.isRegex() || value.isProxy()) {
        try {
          auto [foundThen, thenValue] = getThenProperty(value);
          if (foundThen && thenValue.isFunction()) {
            auto alreadyCalled = std::make_shared<bool>(false);
            auto resolveFn = std::make_shared<Function>();
            resolveFn->isNative = true;
            resolveFn->nativeFunc = [resolveSelf, alreadyCalled](const std::vector<Value>& innerArgs) -> Value {
              if (*alreadyCalled) {
                return Value(Undefined{});
              }
              *alreadyCalled = true;
              Value next = innerArgs.empty() ? Value(Undefined{}) : innerArgs[0];
              (*resolveSelf)(next);
              return Value(Undefined{});
            };

            auto rejectFn = std::make_shared<Function>();
            rejectFn->isNative = true;
            rejectFn->nativeFunc = [promise, alreadyCalled](const std::vector<Value>& innerArgs) -> Value {
              if (*alreadyCalled) {
                return Value(Undefined{});
              }
              *alreadyCalled = true;
              Value reason = innerArgs.empty() ? Value(Undefined{}) : innerArgs[0];
              promise->reject(reason);
              return Value(Undefined{});
            };

            callChecked(thenValue, {Value(resolveFn), Value(rejectFn)}, value);
            return;
          }
        } catch (const std::exception& e) {
          promise->reject(Value(std::string(e.what())));
          return;
        }
      }

      promise->resolve(value);
    };

    (*resolveSelf)(resolution);
    return Value(promise);
  };
  promiseConstructor->properties["resolve"] = Value(promiseResolve);

  // Promise.reject
  auto promiseReject = std::make_shared<Function>();
  promiseReject->isNative = true;
  promiseReject->properties["name"] = Value(std::string("reject"));
  promiseReject->properties["length"] = Value(1.0);
  promiseReject->nativeFunc = [promiseFunc](const std::vector<Value>& args) -> Value {
    auto promise = std::make_shared<Promise>();
    GarbageCollector::instance().reportAllocation(sizeof(Promise));
    promise->properties["__constructor__"] = Value(promiseFunc);
    if (!args.empty()) {
      promise->reject(args[0]);
    } else {
      promise->reject(Value(Undefined{}));
    }
    return Value(promise);
  };
  promiseConstructor->properties["reject"] = Value(promiseReject);

  // Promise.withResolvers
  auto promiseWithResolvers = std::make_shared<Function>();
  promiseWithResolvers->isNative = true;
  promiseWithResolvers->nativeFunc = [promiseFunc](const std::vector<Value>&) -> Value {
    auto promise = std::make_shared<Promise>();
    GarbageCollector::instance().reportAllocation(sizeof(Promise));
    promise->properties["__constructor__"] = Value(promiseFunc);

    auto resolveFunc = std::make_shared<Function>();
    resolveFunc->isNative = true;
    auto promisePtr = promise;
    resolveFunc->nativeFunc = [promisePtr](const std::vector<Value>& args) -> Value {
      if (!args.empty()) {
        promisePtr->resolve(args[0]);
      } else {
        promisePtr->resolve(Value(Undefined{}));
      }
      return Value(Undefined{});
    };

    auto rejectFunc = std::make_shared<Function>();
    rejectFunc->isNative = true;
    rejectFunc->nativeFunc = [promisePtr](const std::vector<Value>& args) -> Value {
      if (!args.empty()) {
        promisePtr->reject(args[0]);
      } else {
        promisePtr->reject(Value(Undefined{}));
      }
      return Value(Undefined{});
    };

    auto result = std::make_shared<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    result->properties["promise"] = Value(promise);
    result->properties["resolve"] = Value(resolveFunc);
    result->properties["reject"] = Value(rejectFunc);
    return Value(result);
  };
  promiseConstructor->properties["withResolvers"] = Value(promiseWithResolvers);

  // Promise.all
  auto promiseAll = std::make_shared<Function>();
  promiseAll->isNative = true;
  promiseAll->properties["name"] = Value(std::string("all"));
  promiseAll->properties["length"] = Value(1.0);
  promiseAll->nativeFunc = [promiseFunc](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      auto promise = std::make_shared<Promise>();
      GarbageCollector::instance().reportAllocation(sizeof(Promise));
      promise->properties["__constructor__"] = Value(promiseFunc);
      promise->reject(Value(std::string("Promise.all expects an array")));
      return Value(promise);
    }

    auto arr = std::get<std::shared_ptr<Array>>(args[0].data);
    auto resultPromise = std::make_shared<Promise>();
    GarbageCollector::instance().reportAllocation(sizeof(Promise));
    resultPromise->properties["__constructor__"] = Value(promiseFunc);
    auto results = std::make_shared<Array>();

    bool hasRejection = false;
    for (const auto& elem : arr->elements) {
      if (elem.isPromise()) {
        auto p = std::get<std::shared_ptr<Promise>>(elem.data);
        if (p->state == PromiseState::Rejected) {
          hasRejection = true;
          resultPromise->reject(p->result);
          break;
        } else if (p->state == PromiseState::Fulfilled) {
          results->elements.push_back(p->result);
        }
      } else {
        results->elements.push_back(elem);
      }
    }

    if (!hasRejection) {
      resultPromise->resolve(Value(results));
    }

    return Value(resultPromise);
  };
  promiseConstructor->properties["all"] = Value(promiseAll);

  // Promise.allSettled - waits for all promises to settle (resolve or reject)
  auto promiseAllSettled = std::make_shared<Function>();
  promiseAllSettled->isNative = true;
  promiseAllSettled->properties["__uses_this_arg__"] = Value(true);
  promiseAllSettled->properties["name"] = Value(std::string("allSettled"));
  promiseAllSettled->properties["length"] = Value(1.0);
  promiseAllSettled->nativeFunc = [callChecked, getProperty, env, promiseFunc](const std::vector<Value>& args) -> Value {
    Value constructor = args.empty() ? Value(Undefined{}) : args[0];
    Value iterable = args.size() > 1 ? args[1] : Value(Undefined{});
    auto resultPromise = std::make_shared<Promise>();
    GarbageCollector::instance().reportAllocation(sizeof(Promise));
    resultPromise->properties["__constructor__"] = Value(promiseFunc);

    auto typeErrorValue = [](const std::string& msg) -> Value {
      return Value(std::make_shared<Error>(ErrorType::TypeError, msg));
    };

    auto rejectAndReturn = [&](const Value& reason) -> Value {
      resultPromise->reject(reason);
      return Value(resultPromise);
    };

    auto callWithThis = [](const Value& callee,
                           const std::vector<Value>& callArgs,
                           const Value& thisArg,
                           Value& out,
                           Value& thrown) -> bool {
      if (!callee.isFunction()) {
        thrown = Value(std::make_shared<Error>(ErrorType::TypeError, "Value is not callable"));
        return false;
      }

      auto fn = std::get<std::shared_ptr<Function>>(callee.data);
      if (fn->isNative) {
        try {
          auto itUsesThis = fn->properties.find("__uses_this_arg__");
          if (itUsesThis != fn->properties.end() &&
              itUsesThis->second.isBool() &&
              itUsesThis->second.toBool()) {
            std::vector<Value> nativeArgs;
            nativeArgs.reserve(callArgs.size() + 1);
            nativeArgs.push_back(thisArg);
            nativeArgs.insert(nativeArgs.end(), callArgs.begin(), callArgs.end());
            out = fn->nativeFunc(nativeArgs);
          } else {
            out = fn->nativeFunc(callArgs);
          }
          return true;
        } catch (const std::exception& e) {
          thrown = Value(std::string(e.what()));
          return false;
        }
      }

      Interpreter* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        thrown = Value(std::string("TypeError: Interpreter unavailable"));
        return false;
      }
      interpreter->clearError();
      out = interpreter->callForHarness(callee, callArgs, thisArg);
      if (interpreter->hasError()) {
        thrown = interpreter->getError();
        interpreter->clearError();
        return false;
      }
      return true;
    };

    auto getPropertyWithThrow = [&](const Value& receiver,
                                    const std::string& key,
                                    Value& out,
                                    bool& found,
                                    Value& thrown) -> bool {
      auto resolveFromObject = [&](const std::shared_ptr<Object>& obj,
                                   const Value& originalReceiver) -> bool {
        auto current = obj;
        int depth = 0;
        while (current && depth <= 32) {
          depth++;

          auto getterIt = current->properties.find("__get_" + key);
          if (getterIt != current->properties.end()) {
            found = true;
            if (!getterIt->second.isFunction()) {
              out = Value(Undefined{});
              return true;
            }
            Value getterOut;
            if (!callWithThis(getterIt->second, {}, originalReceiver, getterOut, thrown)) {
              return false;
            }
            out = getterOut;
            return true;
          }

          auto it = current->properties.find(key);
          if (it != current->properties.end()) {
            found = true;
            out = it->second;
            return true;
          }

          auto protoIt = current->properties.find("__proto__");
          if (protoIt == current->properties.end() || !protoIt->second.isObject()) {
            break;
          }
          current = std::get<std::shared_ptr<Object>>(protoIt->second.data);
        }
        return true;
      };

      found = false;
      out = Value(Undefined{});

      if (receiver.isObject()) {
        return resolveFromObject(std::get<std::shared_ptr<Object>>(receiver.data), receiver);
      }
      if (receiver.isFunction()) {
        auto fn = std::get<std::shared_ptr<Function>>(receiver.data);
        auto getterIt = fn->properties.find("__get_" + key);
        if (getterIt != fn->properties.end()) {
          found = true;
          if (!getterIt->second.isFunction()) {
            out = Value(Undefined{});
            return true;
          }
          return callWithThis(getterIt->second, {}, receiver, out, thrown);
        }
        auto it = fn->properties.find(key);
        if (it != fn->properties.end()) {
          found = true;
          out = it->second;
        }
        return true;
      }
      if (receiver.isArray()) {
        auto arr = std::get<std::shared_ptr<Array>>(receiver.data);
        auto getterIt = arr->properties.find("__get_" + key);
        if (getterIt != arr->properties.end()) {
          found = true;
          if (!getterIt->second.isFunction()) {
            out = Value(Undefined{});
            return true;
          }
          return callWithThis(getterIt->second, {}, receiver, out, thrown);
        }
        auto it = arr->properties.find(key);
        if (it != arr->properties.end()) {
          found = true;
          out = it->second;
          return true;
        }
        if (key == "length") {
          found = true;
          out = Value(static_cast<double>(arr->elements.size()));
          return true;
        }
        if (auto arrayCtor = env->get("Array"); arrayCtor && arrayCtor->isObject()) {
          auto arrayObjPtr = std::get<std::shared_ptr<Object>>(arrayCtor->data);
          auto protoIt = arrayObjPtr->properties.find("prototype");
          if (protoIt != arrayObjPtr->properties.end() && protoIt->second.isObject()) {
            return resolveFromObject(std::get<std::shared_ptr<Object>>(protoIt->second.data), receiver);
          }
        }
        return true;
      }

      return true;
    };

    Value promiseResolveMethod = Value(Undefined{});
    bool hasResolve = false;
    Value resolveThrown;
    if (!getPropertyWithThrow(constructor, "resolve", promiseResolveMethod, hasResolve, resolveThrown)) {
      return rejectAndReturn(resolveThrown);
    }
    if (!hasResolve || !promiseResolveMethod.isFunction()) {
      return rejectAndReturn(typeErrorValue("Promise.resolve is not callable"));
    }

    auto results = std::make_shared<Array>();
    auto remaining = std::make_shared<size_t>(1);

    auto resolveResultPromise = std::make_shared<std::function<void(const Value&)>>();
    *resolveResultPromise = [resultPromise, resolveResultPromise, getPropertyWithThrow, callWithThis](const Value& finalValue) {
      if (resultPromise->state != PromiseState::Pending) {
        return;
      }

      if (finalValue.isPromise()) {
        auto nested = std::get<std::shared_ptr<Promise>>(finalValue.data);
        if (nested.get() == resultPromise.get()) {
          resultPromise->reject(Value(std::make_shared<Error>(ErrorType::TypeError, "Cannot resolve promise with itself")));
          return;
        }
        if (nested->state == PromiseState::Fulfilled) {
          (*resolveResultPromise)(nested->result);
          return;
        }
        if (nested->state == PromiseState::Rejected) {
          resultPromise->reject(nested->result);
          return;
        }
        nested->then(
          [resolveResultPromise](Value fulfilled) -> Value {
            (*resolveResultPromise)(fulfilled);
            return fulfilled;
          },
          [resultPromise](Value reason) -> Value {
            resultPromise->reject(reason);
            return reason;
          });
        return;
      }

      if (finalValue.isObject() || finalValue.isArray() || finalValue.isFunction() ||
          finalValue.isRegex() || finalValue.isProxy()) {
        Value thenValue = Value(Undefined{});
        bool hasThen = false;
        Value thenThrown;
        if (!getPropertyWithThrow(finalValue, "then", thenValue, hasThen, thenThrown)) {
          resultPromise->reject(thenThrown);
          return;
        }
        if (hasThen && thenValue.isFunction()) {
          auto alreadyCalled = std::make_shared<bool>(false);
          auto resolveFn = std::make_shared<Function>();
          resolveFn->isNative = true;
          resolveFn->nativeFunc = [resolveResultPromise, alreadyCalled](const std::vector<Value>& innerArgs) -> Value {
            if (*alreadyCalled) {
              return Value(Undefined{});
            }
            *alreadyCalled = true;
            Value next = innerArgs.empty() ? Value(Undefined{}) : innerArgs[0];
            (*resolveResultPromise)(next);
            return Value(Undefined{});
          };
          auto rejectFn = std::make_shared<Function>();
          rejectFn->isNative = true;
          rejectFn->nativeFunc = [resultPromise, alreadyCalled](const std::vector<Value>& innerArgs) -> Value {
            if (*alreadyCalled) {
              return Value(Undefined{});
            }
            *alreadyCalled = true;
            Value reason = innerArgs.empty() ? Value(Undefined{}) : innerArgs[0];
            resultPromise->reject(reason);
            return Value(Undefined{});
          };

          Value ignored;
          Value thenCallThrown;
          if (!callWithThis(thenValue, {Value(resolveFn), Value(rejectFn)}, finalValue, ignored, thenCallThrown) &&
              !*alreadyCalled) {
            resultPromise->reject(thenCallThrown);
          }
          return;
        }
      }

      resultPromise->resolve(finalValue);
    };

    auto finalizeIfDone = [remaining, results, resolveResultPromise]() {
      if (*remaining == 0) {
        auto valuesArray = std::make_shared<Array>();
        valuesArray->elements = results->elements;
        (*resolveResultPromise)(Value(valuesArray));
      }
    };

    auto processElement = [&](size_t index, const Value& nextValue, Value& failureReason) -> bool {
      if (results->elements.size() <= index) {
        results->elements.resize(index + 1, Value(Undefined{}));
      }

      (*remaining)++;
      Value nextPromise = Value(Undefined{});
      Value resolveCallThrown;
      if (!callWithThis(promiseResolveMethod, {nextValue}, constructor, nextPromise, resolveCallThrown)) {
        failureReason = resolveCallThrown;
        return false;
      }

      auto alreadyCalled = std::make_shared<bool>(false);
      auto resolveElement = std::make_shared<Function>();
      resolveElement->isNative = true;
      resolveElement->properties["length"] = Value(1.0);
      resolveElement->properties["name"] = Value(std::string(""));
      resolveElement->nativeFunc = [results, remaining, index, alreadyCalled, finalizeIfDone](const std::vector<Value>& innerArgs) -> Value {
        if (*alreadyCalled) {
          return Value(Undefined{});
        }
        *alreadyCalled = true;
        Value settledValue = innerArgs.empty() ? Value(Undefined{}) : innerArgs[0];
        auto entry = std::make_shared<Object>();
        entry->properties["status"] = Value(std::string("fulfilled"));
        entry->properties["value"] = settledValue;
        results->elements[index] = Value(entry);
        (*remaining)--;
        finalizeIfDone();
        return Value(Undefined{});
      };

      auto rejectElement = std::make_shared<Function>();
      rejectElement->isNative = true;
      rejectElement->properties["length"] = Value(1.0);
      rejectElement->properties["name"] = Value(std::string(""));
      rejectElement->nativeFunc = [results, remaining, index, alreadyCalled, finalizeIfDone](const std::vector<Value>& innerArgs) -> Value {
        if (*alreadyCalled) {
          return Value(Undefined{});
        }
        *alreadyCalled = true;
        Value settledReason = innerArgs.empty() ? Value(Undefined{}) : innerArgs[0];
        auto entry = std::make_shared<Object>();
        entry->properties["status"] = Value(std::string("rejected"));
        entry->properties["reason"] = settledReason;
        results->elements[index] = Value(entry);
        (*remaining)--;
        finalizeIfDone();
        return Value(Undefined{});
      };

      if (nextPromise.isPromise()) {
        auto promisePtr = std::get<std::shared_ptr<Promise>>(nextPromise.data);
        Value overriddenThen = Value(Undefined{});
        bool hasOverriddenThen = false;

        auto getterIt = promisePtr->properties.find("__get_then");
        if (getterIt != promisePtr->properties.end()) {
          hasOverriddenThen = true;
          if (getterIt->second.isFunction()) {
            Value getterOut = Value(Undefined{});
            Value getterThrown;
            if (!callWithThis(getterIt->second, {}, nextPromise, getterOut, getterThrown)) {
              failureReason = getterThrown;
              return false;
            }
            overriddenThen = getterOut;
          }
        } else if (auto thenIt = promisePtr->properties.find("then");
                   thenIt != promisePtr->properties.end()) {
          hasOverriddenThen = true;
          overriddenThen = thenIt->second;
        }

        if (hasOverriddenThen) {
          if (!overriddenThen.isFunction()) {
            failureReason = typeErrorValue("Promise resolve result is not thenable");
            return false;
          }
          Value ignored;
          Value thenInvokeThrown;
          if (!callWithThis(overriddenThen, {Value(resolveElement), Value(rejectElement)},
                            nextPromise, ignored, thenInvokeThrown) &&
              !*alreadyCalled) {
            failureReason = thenInvokeThrown;
            return false;
          }
          return true;
        }

        promisePtr->then(
          [resolveElement](Value v) -> Value {
            return resolveElement->nativeFunc({v});
          },
          [rejectElement](Value reason) -> Value {
            return rejectElement->nativeFunc({reason});
          });
        return true;
      }

      Value thenMethod = Value(Undefined{});
      bool hasThenMethod = false;
      Value thenLookupThrown;
      if (!getPropertyWithThrow(nextPromise, "then", thenMethod, hasThenMethod, thenLookupThrown)) {
        failureReason = thenLookupThrown;
        return false;
      }
      if (!hasThenMethod || !thenMethod.isFunction()) {
        failureReason = typeErrorValue("Promise resolve result is not thenable");
        return false;
      }

      Value ignored;
      Value thenInvokeThrown;
      if (!callWithThis(thenMethod, {Value(resolveElement), Value(rejectElement)}, nextPromise, ignored, thenInvokeThrown) &&
          !*alreadyCalled) {
        failureReason = thenInvokeThrown;
        return false;
      }
      return true;
    };

    auto closeIterator = [&](const Value& iteratorValue, Value& closeFailure) -> bool {
      Value returnMethod = Value(Undefined{});
      bool hasReturn = false;
      Value returnLookupThrown;
      if (!getPropertyWithThrow(iteratorValue, "return", returnMethod, hasReturn, returnLookupThrown)) {
        closeFailure = returnLookupThrown;
        return false;
      }
      if (!hasReturn || returnMethod.isUndefined() || returnMethod.isNull()) {
        return true;
      }
      if (!returnMethod.isFunction()) {
        closeFailure = typeErrorValue("Iterator return is not callable");
        return false;
      }
      Value ignored;
      Value returnThrown;
      if (!callWithThis(returnMethod, {}, iteratorValue, ignored, returnThrown)) {
        closeFailure = returnThrown;
        return false;
      }
      return true;
    };

    const auto& iteratorKey = WellKnownSymbols::iteratorKey();
    size_t nextIndex = 0;
    if (iterable.isArray()) {
      auto arr = std::get<std::shared_ptr<Array>>(iterable.data);
      for (const auto& value : arr->elements) {
        Value failureReason;
        if (!processElement(nextIndex++, value, failureReason)) {
          return rejectAndReturn(failureReason);
        }
      }
    } else if (iterable.isString()) {
      const auto& str = std::get<std::string>(iterable.data);
      for (char c : str) {
        Value failureReason;
        if (!processElement(nextIndex++, Value(std::string(1, c)), failureReason)) {
          return rejectAndReturn(failureReason);
        }
      }
    } else {
      Value iteratorMethod = Value(Undefined{});
      bool hasIteratorMethod = false;
      Value iteratorLookupThrown;
      if (!getPropertyWithThrow(iterable, iteratorKey, iteratorMethod, hasIteratorMethod, iteratorLookupThrown)) {
        return rejectAndReturn(iteratorLookupThrown);
      }
      if (!hasIteratorMethod || iteratorMethod.isUndefined() || iteratorMethod.isNull()) {
        return rejectAndReturn(typeErrorValue("Value is not iterable"));
      }
      if (!iteratorMethod.isFunction()) {
        return rejectAndReturn(typeErrorValue("Symbol.iterator is not callable"));
      }

      Value iteratorValue = Value(Undefined{});
      Value iteratorThrown;
      if (!callWithThis(iteratorMethod, {}, iterable, iteratorValue, iteratorThrown)) {
        return rejectAndReturn(iteratorThrown);
      }
      if (!iteratorValue.isObject()) {
        return rejectAndReturn(typeErrorValue("Iterator must be an object"));
      }

      while (true) {
        Value nextMethod = Value(Undefined{});
        bool hasNext = false;
        Value nextLookupThrown;
        if (!getPropertyWithThrow(iteratorValue, "next", nextMethod, hasNext, nextLookupThrown)) {
          return rejectAndReturn(nextLookupThrown);
        }
        if (!hasNext || !nextMethod.isFunction()) {
          return rejectAndReturn(typeErrorValue("Iterator next is not callable"));
        }

        Value stepResult = Value(Undefined{});
        Value nextThrown;
        if (!callWithThis(nextMethod, {}, iteratorValue, stepResult, nextThrown)) {
          return rejectAndReturn(nextThrown);
        }
        if (!stepResult.isObject()) {
          return rejectAndReturn(typeErrorValue("Iterator result is not an object"));
        }

        Value doneValue = Value(false);
        bool hasDone = false;
        Value doneThrown;
        if (!getPropertyWithThrow(stepResult, "done", doneValue, hasDone, doneThrown)) {
          return rejectAndReturn(doneThrown);
        }
        if (hasDone && doneValue.toBool()) {
          break;
        }

        Value itemValue = Value(Undefined{});
        bool hasItem = false;
        Value itemThrown;
        if (!getPropertyWithThrow(stepResult, "value", itemValue, hasItem, itemThrown)) {
          return rejectAndReturn(itemThrown);
        }

        Value failureReason;
        if (!processElement(nextIndex++, hasItem ? itemValue : Value(Undefined{}), failureReason)) {
          Value closeFailure;
          if (!closeIterator(iteratorValue, closeFailure)) {
            return rejectAndReturn(closeFailure);
          }
          return rejectAndReturn(failureReason);
        }
      }
    }

    (*remaining)--;
    finalizeIfDone();
    return Value(resultPromise);
  };
  promiseConstructor->properties["allSettled"] = Value(promiseAllSettled);

  // Promise.any - resolves when any promise fulfills, rejects if all reject
  auto promiseAny = std::make_shared<Function>();
  promiseAny->isNative = true;
  promiseAny->properties["name"] = Value(std::string("any"));
  promiseAny->properties["length"] = Value(1.0);
  promiseAny->nativeFunc = [promiseFunc](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      auto promise = std::make_shared<Promise>();
      GarbageCollector::instance().reportAllocation(sizeof(Promise));
      promise->properties["__constructor__"] = Value(promiseFunc);
      promise->reject(Value(std::string("Promise.any expects an array")));
      return Value(promise);
    }

    auto arr = std::get<std::shared_ptr<Array>>(args[0].data);
    auto resultPromise = std::make_shared<Promise>();
    GarbageCollector::instance().reportAllocation(sizeof(Promise));
    resultPromise->properties["__constructor__"] = Value(promiseFunc);

    if (arr->elements.empty()) {
      // Empty array - reject with AggregateError
      auto err = std::make_shared<Error>(ErrorType::Error, "All promises were rejected");
      resultPromise->reject(Value(err));
      return Value(resultPromise);
    }

    auto errors = std::make_shared<Array>();
    bool hasResolved = false;

    for (const auto& elem : arr->elements) {
      if (elem.isPromise()) {
        auto p = std::get<std::shared_ptr<Promise>>(elem.data);
        if (p->state == PromiseState::Fulfilled && !hasResolved) {
          hasResolved = true;
          resultPromise->resolve(p->result);
          break;
        } else if (p->state == PromiseState::Rejected) {
          errors->elements.push_back(p->result);
        }
      } else {
        // Non-promise values are treated as fulfilled
        if (!hasResolved) {
          hasResolved = true;
          resultPromise->resolve(elem);
          break;
        }
      }
    }

    if (!hasResolved) {
      // All rejected - create AggregateError-like object
      auto err = std::make_shared<Error>(ErrorType::Error, "All promises were rejected");
      resultPromise->reject(Value(err));
    }

    return Value(resultPromise);
  };
  promiseConstructor->properties["any"] = Value(promiseAny);

  // Promise.race - resolves or rejects with the first settled promise
  auto promiseRace = std::make_shared<Function>();
  promiseRace->isNative = true;
  promiseRace->properties["name"] = Value(std::string("race"));
  promiseRace->properties["length"] = Value(1.0);
  promiseRace->nativeFunc = [promiseFunc](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      auto promise = std::make_shared<Promise>();
      GarbageCollector::instance().reportAllocation(sizeof(Promise));
      promise->properties["__constructor__"] = Value(promiseFunc);
      promise->reject(Value(std::string("Promise.race expects an array")));
      return Value(promise);
    }

    auto arr = std::get<std::shared_ptr<Array>>(args[0].data);
    auto resultPromise = std::make_shared<Promise>();
    GarbageCollector::instance().reportAllocation(sizeof(Promise));
    resultPromise->properties["__constructor__"] = Value(promiseFunc);

    for (const auto& elem : arr->elements) {
      if (elem.isPromise()) {
        auto p = std::get<std::shared_ptr<Promise>>(elem.data);
        if (p->state == PromiseState::Fulfilled) {
          resultPromise->resolve(p->result);
          break;
        } else if (p->state == PromiseState::Rejected) {
          resultPromise->reject(p->result);
          break;
        }
      } else {
        // Non-promise value settles immediately
        resultPromise->resolve(elem);
        break;
      }
    }

    return Value(resultPromise);
  };
  promiseConstructor->properties["race"] = Value(promiseRace);

  // Promise constructor function
  promiseFunc->nativeFunc = [promiseFunc](const std::vector<Value>& args) -> Value {
    auto promise = std::make_shared<Promise>();
    GarbageCollector::instance().reportAllocation(sizeof(Promise));
    promise->properties["__constructor__"] = Value(promiseFunc);

    if (!args.empty() && args[0].isFunction()) {
      auto executor = std::get<std::shared_ptr<Function>>(args[0].data);

      // Create resolve and reject functions
      auto resolveFunc = std::make_shared<Function>();
      resolveFunc->isNative = true;
      auto promisePtr = promise;
      resolveFunc->nativeFunc = [promisePtr](const std::vector<Value>& args) -> Value {
        if (!args.empty()) {
          promisePtr->resolve(args[0]);
        } else {
          promisePtr->resolve(Value(Undefined{}));
        }
        return Value(Undefined{});
      };

      auto rejectFunc = std::make_shared<Function>();
      rejectFunc->isNative = true;
      rejectFunc->nativeFunc = [promisePtr](const std::vector<Value>& args) -> Value {
        if (!args.empty()) {
          promisePtr->reject(args[0]);
        } else {
          promisePtr->reject(Value(Undefined{}));
        }
        return Value(Undefined{});
      };

      // Call executor with resolve and reject
      if (executor->isNative) {
        try {
          executor->nativeFunc({Value(resolveFunc), Value(rejectFunc)});
        } catch (const std::exception& e) {
          promise->reject(Value(std::string(e.what())));
        }
      } else {
        Interpreter* interpreter = getGlobalInterpreter();
        if (interpreter) {
          interpreter->clearError();
          interpreter->callForHarness(Value(executor), {Value(resolveFunc), Value(rejectFunc)}, Value(Undefined{}));
          if (interpreter->hasError()) {
            Value err = interpreter->getError();
            interpreter->clearError();
            promise->reject(err);
          }
        } else {
          promise->reject(Value(std::make_shared<Error>(ErrorType::TypeError, "Interpreter unavailable")));
        }
      }
    }

    return Value(promise);
  };

  // Expose Promise as a callable constructor with static methods.
  promiseFunc->properties = promiseConstructor->properties;
  env->define("Promise", Value(promiseFunc));
  // Keep intrinsic Promise reachable even if global Promise is overwritten.
  env->define("__intrinsic_Promise__", Value(promiseFunc));

  // JSON object
  auto jsonObj = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // JSON.parse
  auto jsonParse = std::make_shared<Function>();
  jsonParse->isNative = true;
  jsonParse->nativeFunc = JSON_parse;
  jsonObj->properties["parse"] = Value(jsonParse);

  // JSON.stringify
  auto jsonStringify = std::make_shared<Function>();
  jsonStringify->isNative = true;
  jsonStringify->nativeFunc = JSON_stringify;
  jsonObj->properties["stringify"] = Value(jsonStringify);

  env->define("JSON", Value(jsonObj));

  // Object static methods
  auto objectConstructor = std::make_shared<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  objectConstructor->isNative = true;
  objectConstructor->isConstructor = true;
  objectConstructor->properties["name"] = Value(std::string("Object"));
  objectConstructor->properties["length"] = Value(1.0);
  objectConstructor->nativeFunc = [env](const std::vector<Value>& args) -> Value {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
      auto obj = std::make_shared<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      return Value(obj);
    }

    const Value& value = args[0];
    if (!value.isBool() && !value.isNumber() && !value.isString() &&
        !value.isBigInt() && !value.isSymbol()) {
      return value;
    }

    auto wrapped = std::make_shared<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    wrapped->properties["__primitive_value__"] = value;

    if (!value.isBigInt()) {
      auto valueOf = std::make_shared<Function>();
      valueOf->isNative = true;
      valueOf->properties["__uses_this_arg__"] = Value(true);
      valueOf->nativeFunc = [](const std::vector<Value>& callArgs) -> Value {
        if (!callArgs.empty() && callArgs[0].isObject()) {
          auto obj = std::get<std::shared_ptr<Object>>(callArgs[0].data);
          auto it = obj->properties.find("__primitive_value__");
          if (it != obj->properties.end()) {
            return it->second;
          }
        }
        return callArgs.empty() ? Value(Undefined{}) : callArgs[0];
      };
      wrapped->properties["valueOf"] = Value(valueOf);

      auto toString = std::make_shared<Function>();
      toString->isNative = true;
      toString->properties["__uses_this_arg__"] = Value(true);
      toString->nativeFunc = [](const std::vector<Value>& callArgs) -> Value {
        if (!callArgs.empty() && callArgs[0].isObject()) {
          auto obj = std::get<std::shared_ptr<Object>>(callArgs[0].data);
          auto it = obj->properties.find("__primitive_value__");
          if (it != obj->properties.end()) {
            return Value(it->second.toString());
          }
        }
        return Value(callArgs.empty() ? std::string("undefined") : callArgs[0].toString());
      };
      wrapped->properties["toString"] = Value(toString);
    }

    if (value.isBigInt()) {
      if (auto bigIntCtor = env->get("BigInt")) {
        if (bigIntCtor->isFunction()) {
          auto ctor = std::get<std::shared_ptr<Function>>(bigIntCtor->data);
          auto protoIt = ctor->properties.find("prototype");
          if (protoIt != ctor->properties.end() && protoIt->second.isObject()) {
            wrapped->properties["__proto__"] = protoIt->second;
          }
        }
      }
    }

    return Value(wrapped);
  };

  auto objectPrototype = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  objectConstructor->properties["prototype"] = Value(objectPrototype);

  auto objectProtoHasOwnProperty = std::make_shared<Function>();
  objectProtoHasOwnProperty->isNative = true;
  objectProtoHasOwnProperty->properties["__uses_this_arg__"] = Value(true);
  objectProtoHasOwnProperty->nativeFunc = Object_hasOwnProperty;
  objectPrototype->properties["hasOwnProperty"] = Value(objectProtoHasOwnProperty);

  auto objectProtoToString = std::make_shared<Function>();
  objectProtoToString->isNative = true;
  objectProtoToString->properties["__uses_this_arg__"] = Value(true);
  objectProtoToString->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || args[0].isUndefined()) {
      return Value(std::string("[object Undefined]"));
    }
    if (args[0].isNull()) {
      return Value(std::string("[object Null]"));
    }

    std::string tag = "Object";
    if (args[0].isObject()) {
      auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
      auto toStringTagIt = obj->properties.find(WellKnownSymbols::toStringTagKey());
      if (toStringTagIt != obj->properties.end()) {
        tag = toStringTagIt->second.toString();
      }
    } else if (args[0].isArray()) {
      tag = "Array";
    } else if (args[0].isFunction()) {
      tag = "Function";
    }
    return Value(std::string("[object ") + tag + "]");
  };
  objectPrototype->properties["toString"] = Value(objectProtoToString);

  // Object.prototype.propertyIsEnumerable
  auto objectProtoPropertyIsEnumerable = std::make_shared<Function>();
  objectProtoPropertyIsEnumerable->isNative = true;
  objectProtoPropertyIsEnumerable->properties["__uses_this_arg__"] = Value(true);
  objectProtoPropertyIsEnumerable->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(false);
    Value thisVal = args[0];
    std::string key = args[1].toString();

    if (thisVal.isFunction()) {
      auto fn = std::get<std::shared_ptr<Function>>(thisVal.data);
      // name, length, prototype are non-enumerable on functions
      if (key == "name" || key == "length" || key == "prototype") return Value(false);
      // Internal properties
      if (key.size() >= 4 && key.substr(0, 2) == "__" &&
          key.substr(key.size() - 2) == "__") return Value(false);
      auto it = fn->properties.find(key);
      if (it == fn->properties.end()) return Value(false);
      // Check enum marker: built-in function props default non-enumerable
      auto enumIt = fn->properties.find("__enum_" + key);
      return Value(enumIt != fn->properties.end());
    }
    if (thisVal.isObject()) {
      auto obj = std::get<std::shared_ptr<Object>>(thisVal.data);
      // Internal properties
      if (key.size() >= 4 && key.substr(0, 2) == "__" &&
          key.substr(key.size() - 2) == "__") return Value(false);
      auto it = obj->properties.find(key);
      if (it == obj->properties.end()) return Value(false);
      auto neIt = obj->properties.find("__non_enum_" + key);
      return Value(neIt == obj->properties.end());
    }
    if (thisVal.isArray()) {
      auto arr = std::get<std::shared_ptr<Array>>(thisVal.data);
      // Check if it's a valid index
      try {
        size_t idx = std::stoull(key);
        if (idx < arr->elements.size()) return Value(true);
      } catch (...) {}
      auto it = arr->properties.find(key);
      if (it == arr->properties.end()) return Value(false);
      auto neIt = arr->properties.find("__non_enum_" + key);
      return Value(neIt == arr->properties.end());
    }
    return Value(false);
  };
  objectPrototype->properties["propertyIsEnumerable"] = Value(objectProtoPropertyIsEnumerable);

  // Object.keys
  auto objectKeys = std::make_shared<Function>();
  objectKeys->isNative = true;
  objectKeys->nativeFunc = Object_keys;
  objectConstructor->properties["keys"] = Value(objectKeys);

  // Object.values
  auto objectValues = std::make_shared<Function>();
  objectValues->isNative = true;
  objectValues->nativeFunc = Object_values;
  objectConstructor->properties["values"] = Value(objectValues);

  // Object.entries
  auto objectEntries = std::make_shared<Function>();
  objectEntries->isNative = true;
  objectEntries->nativeFunc = Object_entries;
  objectConstructor->properties["entries"] = Value(objectEntries);

  // Object.assign
  auto objectAssign = std::make_shared<Function>();
  objectAssign->isNative = true;
  objectAssign->nativeFunc = Object_assign;
  objectConstructor->properties["assign"] = Value(objectAssign);

  // Object.hasOwnProperty (for prototypal access)
  auto objectHasOwnProperty = std::make_shared<Function>();
  objectHasOwnProperty->isNative = true;
  objectHasOwnProperty->nativeFunc = Object_hasOwnProperty;
  objectConstructor->properties["hasOwnProperty"] = Value(objectHasOwnProperty);

  // Object.getOwnPropertyNames
  auto objectGetOwnPropertyNames = std::make_shared<Function>();
  objectGetOwnPropertyNames->isNative = true;
  objectGetOwnPropertyNames->nativeFunc = Object_getOwnPropertyNames;
  objectConstructor->properties["getOwnPropertyNames"] = Value(objectGetOwnPropertyNames);

  // Object.create
  auto objectCreate = std::make_shared<Function>();
  objectCreate->isNative = true;
  objectCreate->nativeFunc = Object_create;
  objectConstructor->properties["create"] = Value(objectCreate);

  // Object.fromEntries - converts array of [key, value] pairs to object
  auto objectFromEntries = std::make_shared<Function>();
  objectFromEntries->isNative = true;
  objectFromEntries->nativeFunc = Object_fromEntries;
  objectConstructor->properties["fromEntries"] = Value(objectFromEntries);

  // Object.hasOwn - checks if object has own property
  auto objectHasOwn = std::make_shared<Function>();
  objectHasOwn->isNative = true;
  objectHasOwn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(false);
    std::string key = args[1].toString();
    if (args[0].isObject()) {
      auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
      if (obj->isModuleNamespace) {
        if (key == WellKnownSymbols::toStringTagKey()) {
          return Value(true);
        }
        return Value(isModuleNamespaceExportKey(obj, key));
      }
      return Value(obj->properties.find(key) != obj->properties.end());
    }
    if (args[0].isArray()) {
      auto arr = std::get<std::shared_ptr<Array>>(args[0].data);
      // Check numeric index
      try {
        size_t idx = std::stoul(key);
        return Value(idx < arr->elements.size());
      } catch (...) {
        return Value(false);
      }
    }
    return Value(false);
  };
  objectConstructor->properties["hasOwn"] = Value(objectHasOwn);

  auto objectGetPrototypeOf = std::make_shared<Function>();
  objectGetPrototypeOf->isNative = true;
  objectGetPrototypeOf->nativeFunc = [promisePrototype, env](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(Null{});
    }
    if (args[0].isPromise()) {
      return Value(promisePrototype);
    }
    if (args[0].isArray()) {
      if (auto arrayCtor = env->get("Array"); arrayCtor && arrayCtor->isObject()) {
        auto arrayObj = std::get<std::shared_ptr<Object>>(arrayCtor->data);
        auto protoIt = arrayObj->properties.find("prototype");
        if (protoIt != arrayObj->properties.end()) {
          return protoIt->second;
        }
      }
      if (auto hiddenProto = env->get("__array_prototype__"); hiddenProto.has_value()) {
        return *hiddenProto;
      }
      return Value(Null{});
    }
    if (args[0].isError()) {
      auto err = std::get<std::shared_ptr<Error>>(args[0].data);
      std::string ctorName = "Error";
      switch (err->type) {
        case ErrorType::TypeError:
          ctorName = "TypeError";
          break;
        case ErrorType::ReferenceError:
          ctorName = "ReferenceError";
          break;
        case ErrorType::RangeError:
          ctorName = "RangeError";
          break;
        case ErrorType::SyntaxError:
          ctorName = "SyntaxError";
          break;
        case ErrorType::URIError:
          ctorName = "URIError";
          break;
        case ErrorType::EvalError:
          ctorName = "EvalError";
          break;
        case ErrorType::Error:
        default:
          ctorName = "Error";
          break;
      }
      if (auto ctor = env->get(ctorName); ctor && ctor->isFunction()) {
        auto fn = std::get<std::shared_ptr<Function>>(ctor->data);
        auto protoIt = fn->properties.find("prototype");
        if (protoIt != fn->properties.end()) {
          return protoIt->second;
        }
      }
      return Value(Null{});
    }
    if (args[0].isFunction()) {
      auto fn = std::get<std::shared_ptr<Function>>(args[0].data);
      auto protoIt = fn->properties.find("__proto__");
      if (protoIt != fn->properties.end()) {
        return protoIt->second;
      }
      // Default: Function.prototype (we don't have a formal one, return null)
      return Value(Null{});
    }
    if (args[0].isBigInt()) {
      // Return BigInt.prototype
      if (auto bigIntCtor = env->get("BigInt"); bigIntCtor && bigIntCtor->isFunction()) {
        auto fn = std::get<std::shared_ptr<Function>>(bigIntCtor->data);
        auto protoIt = fn->properties.find("prototype");
        if (protoIt != fn->properties.end()) {
          return protoIt->second;
        }
      }
      return Value(Null{});
    }
    if (!args[0].isObject()) {
      return Value(Null{});
    }
    auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
    if (obj->isModuleNamespace) {
      return Value(Null{});
    }
    auto it = obj->properties.find("__proto__");
    if (it != obj->properties.end()) {
      return it->second;
    }
    return Value(Null{});
  };
  objectConstructor->properties["getPrototypeOf"] = Value(objectGetPrototypeOf);

  auto objectSetPrototypeOf = std::make_shared<Function>();
  objectSetPrototypeOf->isNative = true;
  objectSetPrototypeOf->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2 || !args[0].isObject()) {
      return args.empty() ? Value(Undefined{}) : args[0];
    }
    auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
    if (obj->isModuleNamespace) {
      if (args[1].isNull()) {
        return args[0];
      }
      throw std::runtime_error("TypeError: Cannot set prototype of module namespace object");
    }
    obj->properties["__proto__"] = args[1];
    return args[0];
  };
  objectConstructor->properties["setPrototypeOf"] = Value(objectSetPrototypeOf);

  auto objectIsExtensible = std::make_shared<Function>();
  objectIsExtensible->isNative = true;
  objectIsExtensible->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isObject()) {
      return Value(false);
    }
    auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
    if (obj->isModuleNamespace) {
      return Value(false);
    }
    return Value(!obj->sealed && !obj->frozen);
  };
  objectConstructor->properties["isExtensible"] = Value(objectIsExtensible);

  auto objectPreventExtensions = std::make_shared<Function>();
  objectPreventExtensions->isNative = true;
  objectPreventExtensions->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isObject()) {
      return args.empty() ? Value(Undefined{}) : args[0];
    }
    auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
    if (!obj->isModuleNamespace) {
      obj->sealed = true;
    }
    return args[0];
  };
  objectConstructor->properties["preventExtensions"] = Value(objectPreventExtensions);

  // Object.freeze - makes an object immutable
  auto objectFreeze = std::make_shared<Function>();
  objectFreeze->isNative = true;
  objectFreeze->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isObject()) {
      return args.empty() ? Value(Undefined{}) : args[0];
    }
    auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
    obj->frozen = true;
    obj->sealed = true;  // Frozen objects are also sealed
    return args[0];
  };
  objectConstructor->properties["freeze"] = Value(objectFreeze);

  // Object.seal - prevents adding or removing properties
  auto objectSeal = std::make_shared<Function>();
  objectSeal->isNative = true;
  objectSeal->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isObject()) {
      return args.empty() ? Value(Undefined{}) : args[0];
    }
    auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
    obj->sealed = true;
    return args[0];
  };
  objectConstructor->properties["seal"] = Value(objectSeal);

  // Object.isFrozen - check if object is frozen
  auto objectIsFrozen = std::make_shared<Function>();
  objectIsFrozen->isNative = true;
  objectIsFrozen->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isObject()) {
      return Value(true);  // Non-objects are considered frozen
    }
    auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
    return Value(obj->frozen);
  };
  objectConstructor->properties["isFrozen"] = Value(objectIsFrozen);

  // Object.isSealed - check if object is sealed
  auto objectIsSealed = std::make_shared<Function>();
  objectIsSealed->isNative = true;
  objectIsSealed->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isObject()) {
      return Value(true);  // Non-objects are considered sealed
    }
    auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
    return Value(obj->sealed);
  };
  objectConstructor->properties["isSealed"] = Value(objectIsSealed);

  // Object.is - SameValue comparison (handles NaN and -0 correctly)
  auto objectIs = std::make_shared<Function>();
  objectIs->isNative = true;
  objectIs->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(false);
    const Value& a = args[0];
    const Value& b = args[1];

    // Check for same type
    if (a.data.index() != b.data.index()) return Value(false);

    // Handle numbers specially (NaN === NaN, +0 !== -0)
    if (a.isNumber() && b.isNumber()) {
      double x = a.toNumber();
      double y = b.toNumber();
      // NaN is equal to NaN in Object.is
      if (std::isnan(x) && std::isnan(y)) return Value(true);
      // +0 and -0 are different in Object.is
      if (x == 0 && y == 0) {
        return Value(std::signbit(x) == std::signbit(y));
      }
      return Value(x == y);
    }

    // For other types, use regular equality
    if (a.isUndefined() && b.isUndefined()) return Value(true);
    if (a.isNull() && b.isNull()) return Value(true);
    if (a.isBool() && b.isBool()) {
      return Value(std::get<bool>(a.data) == std::get<bool>(b.data));
    }
    if (a.isString() && b.isString()) {
      return Value(std::get<std::string>(a.data) == std::get<std::string>(b.data));
    }
    // For objects, check reference equality
    if (a.isObject() && b.isObject()) {
      return Value(std::get<std::shared_ptr<Object>>(a.data).get() ==
                   std::get<std::shared_ptr<Object>>(b.data).get());
    }
    if (a.isArray() && b.isArray()) {
      return Value(std::get<std::shared_ptr<Array>>(a.data).get() ==
                   std::get<std::shared_ptr<Array>>(b.data).get());
    }
    if (a.isFunction() && b.isFunction()) {
      return Value(std::get<std::shared_ptr<Function>>(a.data).get() ==
                   std::get<std::shared_ptr<Function>>(b.data).get());
    }

    return Value(false);
  };
  objectConstructor->properties["is"] = Value(objectIs);

  // Object.getOwnPropertyDescriptor - get property descriptor
  auto objectGetOwnPropertyDescriptor = std::make_shared<Function>();
  objectGetOwnPropertyDescriptor->isNative = true;
  objectGetOwnPropertyDescriptor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2 ||
        (!args[0].isObject() && !args[0].isFunction() && !args[0].isPromise() && !args[0].isRegex())) {
      return Value(Undefined{});
    }
    std::string key = args[1].toString();

    auto descriptor = std::make_shared<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));

    if (args[0].isFunction()) {
      auto fn = std::get<std::shared_ptr<Function>>(args[0].data);

      // Internal properties are not visible
      if (key.size() >= 4 && key.substr(0, 2) == "__" &&
          key.substr(key.size() - 2) == "__") {
        return Value(Undefined{});
      }

      auto getterIt = fn->properties.find("__get_" + key);
      if (getterIt != fn->properties.end() && getterIt->second.isFunction()) {
        descriptor->properties["get"] = getterIt->second;
      }

      auto it = fn->properties.find(key);
      if (it == fn->properties.end() && getterIt == fn->properties.end()) {
        return Value(Undefined{});
      }
      if (it != fn->properties.end()) {
        descriptor->properties["value"] = it->second;
      }

      // name and length: non-writable, non-enumerable, configurable
      if (key == "name" || key == "length") {
        descriptor->properties["writable"] = Value(false);
        descriptor->properties["enumerable"] = Value(false);
        descriptor->properties["configurable"] = Value(true);
      } else if (key == "prototype") {
        bool protoWritable = fn->properties.find("__non_writable_prototype") == fn->properties.end();
        bool protoConfigurable = fn->properties.find("__non_configurable_prototype") == fn->properties.end();
        descriptor->properties["writable"] = Value(protoWritable);
        descriptor->properties["enumerable"] = Value(false);
        descriptor->properties["configurable"] = Value(protoConfigurable);
      } else {
        // Check for per-property attribute markers
        bool writable = true;
        bool enumerable = false; // Built-in function properties default to non-enumerable
        bool configurable = true;
        auto nwIt = fn->properties.find("__non_writable_" + key);
        if (nwIt != fn->properties.end()) writable = false;
        auto neIt = fn->properties.find("__enum_" + key);
        if (neIt != fn->properties.end()) enumerable = true;
        auto ncIt = fn->properties.find("__non_configurable_" + key);
        if (ncIt != fn->properties.end()) configurable = false;
        descriptor->properties["writable"] = Value(writable);
        descriptor->properties["enumerable"] = Value(enumerable);
        descriptor->properties["configurable"] = Value(configurable);
      }
      return Value(descriptor);
    }

    if (args[0].isPromise()) {
      auto promise = std::get<std::shared_ptr<Promise>>(args[0].data);
      auto getterIt = promise->properties.find("__get_" + key);
      if (getterIt != promise->properties.end() && getterIt->second.isFunction()) {
        descriptor->properties["get"] = getterIt->second;
      }

      auto it = promise->properties.find(key);
      if (it == promise->properties.end() && getterIt == promise->properties.end()) {
        return Value(Undefined{});
      }
      if (it != promise->properties.end()) {
        descriptor->properties["value"] = it->second;
      }
      descriptor->properties["writable"] = Value(true);
      descriptor->properties["enumerable"] = Value(true);
      descriptor->properties["configurable"] = Value(true);
      return Value(descriptor);
    }

    if (args[0].isObject()) {
      auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
      if (obj->isModuleNamespace) {
        if (key == WellKnownSymbols::toStringTagKey()) {
          descriptor->properties["value"] = Value(std::string("Module"));
          descriptor->properties["writable"] = Value(false);
          descriptor->properties["enumerable"] = Value(false);
          descriptor->properties["configurable"] = Value(false);
          return Value(descriptor);
        }
        if (!isModuleNamespaceExportKey(obj, key)) {
          return Value(Undefined{});
        }
        auto getterIt = obj->properties.find("__get_" + key);
        if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
          Interpreter* interpreter = getGlobalInterpreter();
          if (interpreter) {
            Value out = interpreter->callForHarness(getterIt->second, {}, args[0]);
            if (interpreter->hasError()) {
              Value err = interpreter->getError();
              interpreter->clearError();
              throw std::runtime_error(err.toString());
            }
            descriptor->properties["value"] = out;
          } else {
            descriptor->properties["value"] = Value(Undefined{});
          }
        } else if (auto it = obj->properties.find(key); it != obj->properties.end()) {
          descriptor->properties["value"] = it->second;
        } else {
          descriptor->properties["value"] = Value(Undefined{});
        }
        descriptor->properties["writable"] = Value(true);
        descriptor->properties["enumerable"] = Value(true);
        descriptor->properties["configurable"] = Value(false);
        return Value(descriptor);
      }
      // Internal properties are not visible
      if (key.size() >= 4 && key.substr(0, 2) == "__" &&
          key.substr(key.size() - 2) == "__") {
        return Value(Undefined{});
      }

      auto getterIt2 = obj->properties.find("__get_" + key);
      auto it = obj->properties.find(key);
      if (it == obj->properties.end() && getterIt2 == obj->properties.end()) {
        return Value(Undefined{});
      }
      if (getterIt2 != obj->properties.end() && getterIt2->second.isFunction()) {
        descriptor->properties["get"] = getterIt2->second;
      }
      if (it != obj->properties.end()) {
        descriptor->properties["value"] = it->second;
      }

      // Check for per-property attribute markers
      bool writable = !obj->frozen;
      bool enumerable = true;
      bool configurable = !obj->sealed;
      auto nwIt = obj->properties.find("__non_writable_" + key);
      if (nwIt != obj->properties.end()) writable = false;
      auto neIt = obj->properties.find("__non_enum_" + key);
      if (neIt != obj->properties.end()) enumerable = false;
      auto ncIt = obj->properties.find("__non_configurable_" + key);
      if (ncIt != obj->properties.end()) configurable = false;
      descriptor->properties["writable"] = Value(writable);
      descriptor->properties["enumerable"] = Value(enumerable);
      descriptor->properties["configurable"] = Value(configurable);
      return Value(descriptor);
    }

    auto regex = std::get<std::shared_ptr<Regex>>(args[0].data);
    auto getterIt = regex->properties.find("__get_" + key);
    if (getterIt != regex->properties.end() && getterIt->second.isFunction()) {
      descriptor->properties["get"] = getterIt->second;
    }

    auto it = regex->properties.find(key);
    if (it != regex->properties.end()) {
      descriptor->properties["value"] = it->second;
    } else if (key == "source") {
      descriptor->properties["value"] = Value(regex->pattern);
    } else if (key == "flags") {
      descriptor->properties["value"] = Value(regex->flags);
    } else if (getterIt == regex->properties.end()) {
      return Value(Undefined{});
    }

    descriptor->properties["writable"] = Value(true);
    descriptor->properties["enumerable"] = Value(true);
    descriptor->properties["configurable"] = Value(true);
    return Value(descriptor);
  };
  objectConstructor->properties["getOwnPropertyDescriptor"] = Value(objectGetOwnPropertyDescriptor);

  // Object.defineProperty - define property with descriptor
  auto objectDefineProperty = std::make_shared<Function>();
  objectDefineProperty->isNative = true;
  objectDefineProperty->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 3 ||
        (!args[0].isObject() && !args[0].isFunction() && !args[0].isPromise() && !args[0].isRegex() && !args[0].isArray())) {
      return args.empty() ? Value(Undefined{}) : args[0];
    }
    std::string key = args[1].toString();

    if (args[2].isObject()) {
      auto descriptor = std::get<std::shared_ptr<Object>>(args[2].data);
      auto readDescriptorField = [&](const std::string& name) -> std::optional<Value> {
        auto it = descriptor->properties.find(name);
        if (it != descriptor->properties.end()) {
          return it->second;
        }
        if (descriptor->shape) {
          int offset = descriptor->shape->getPropertyOffset(name);
          if (offset >= 0) {
            Value slotValue;
            if (descriptor->getSlot(offset, slotValue)) {
              return slotValue;
            }
          }
        }
        return std::nullopt;
      };
      if (args[0].isObject()) {
        auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
        if (obj->isModuleNamespace) {
          if (!defineModuleNamespaceProperty(obj, key, descriptor)) {
            throw std::runtime_error("TypeError: Cannot redefine module namespace property");
          }
          return args[0];
        }

        if (obj->frozen) {
          return args[0];
        }
        if (obj->sealed && obj->properties.find(key) == obj->properties.end()) {
          return args[0];
        }

        if (auto valueField = readDescriptorField("value"); valueField.has_value()) {
          obj->properties[key] = *valueField;
        }

        if (auto getField = readDescriptorField("get");
            getField.has_value() && getField->isFunction()) {
          obj->properties["__get_" + key] = *getField;
        }

        if (auto setField = readDescriptorField("set");
            setField.has_value() && setField->isFunction()) {
          obj->properties["__set_" + key] = *setField;
        }

        // Handle writable descriptor
        if (auto writableField = readDescriptorField("writable"); writableField.has_value()) {
          if (!writableField->toBool()) {
            obj->properties["__non_writable_" + key] = Value(true);
          } else {
            obj->properties.erase("__non_writable_" + key);
          }
        }

        // Handle enumerable descriptor
        if (auto enumField = readDescriptorField("enumerable"); enumField.has_value()) {
          if (!enumField->toBool()) {
            obj->properties["__non_enum_" + key] = Value(true);
          } else {
            obj->properties.erase("__non_enum_" + key);
          }
        }

        // Handle configurable descriptor
        if (auto configField = readDescriptorField("configurable"); configField.has_value()) {
          if (!configField->toBool()) {
            obj->properties["__non_configurable_" + key] = Value(true);
          } else {
            obj->properties.erase("__non_configurable_" + key);
          }
        }
      } else if (args[0].isFunction()) {
        auto fn = std::get<std::shared_ptr<Function>>(args[0].data);
        if (auto valueField = readDescriptorField("value"); valueField.has_value()) {
          fn->properties[key] = *valueField;
        }

        if (auto getField = readDescriptorField("get");
            getField.has_value() && getField->isFunction()) {
          fn->properties["__get_" + key] = *getField;
        }

        if (auto setField = readDescriptorField("set");
            setField.has_value() && setField->isFunction()) {
          fn->properties["__set_" + key] = *setField;
        }
      } else if (args[0].isPromise()) {
        auto promise = std::get<std::shared_ptr<Promise>>(args[0].data);
        if (auto valueField = readDescriptorField("value"); valueField.has_value()) {
          promise->properties[key] = *valueField;
        }

        if (auto getField = readDescriptorField("get");
            getField.has_value() && getField->isFunction()) {
          promise->properties["__get_" + key] = *getField;
        }

        if (auto setField = readDescriptorField("set");
            setField.has_value() && setField->isFunction()) {
          promise->properties["__set_" + key] = *setField;
        }
      } else if (args[0].isArray()) {
        auto arr = std::get<std::shared_ptr<Array>>(args[0].data);
        if (auto valueField = readDescriptorField("value"); valueField.has_value()) {
          // For numeric keys, set in elements array; for others, use properties
          bool isNumeric = true;
          size_t idx = 0;
          try { idx = std::stoul(key); } catch (...) { isNumeric = false; }
          if (isNumeric && idx < arr->elements.size()) {
            arr->elements[idx] = *valueField;
          } else {
            arr->properties[key] = *valueField;
          }
        }

        if (auto getField = readDescriptorField("get");
            getField.has_value() && getField->isFunction()) {
          arr->properties["__get_" + key] = *getField;
          // For numeric indices, ensure elements array covers this index
          // so iteration sees it (getter takes priority over element value)
          bool isNumIdx = true;
          size_t gIdx = 0;
          try { gIdx = std::stoul(key); } catch (...) { isNumIdx = false; }
          if (isNumIdx && gIdx >= arr->elements.size()) {
            arr->elements.resize(gIdx + 1, Value(Undefined{}));
          }
        }

        if (auto setField = readDescriptorField("set");
            setField.has_value() && setField->isFunction()) {
          arr->properties["__set_" + key] = *setField;
        }

        // Handle writable descriptor
        if (auto writableField = readDescriptorField("writable"); writableField.has_value()) {
          if (!writableField->toBool()) {
            arr->properties["__non_writable_" + key] = Value(true);
          } else {
            arr->properties.erase("__non_writable_" + key);
          }
        }

        // Handle enumerable descriptor
        if (auto enumField = readDescriptorField("enumerable"); enumField.has_value()) {
          if (!enumField->toBool()) {
            arr->properties["__non_enum_" + key] = Value(true);
          } else {
            arr->properties.erase("__non_enum_" + key);
          }
        }

        // Handle configurable descriptor
        if (auto configField = readDescriptorField("configurable"); configField.has_value()) {
          if (!configField->toBool()) {
            arr->properties["__non_configurable_" + key] = Value(true);
          } else {
            arr->properties.erase("__non_configurable_" + key);
          }
        }
      } else if (args[0].isRegex()) {
        auto regex = std::get<std::shared_ptr<Regex>>(args[0].data);
        if (auto valueField = readDescriptorField("value"); valueField.has_value()) {
          regex->properties[key] = *valueField;
        }

        if (auto getField = readDescriptorField("get");
            getField.has_value() && getField->isFunction()) {
          regex->properties["__get_" + key] = *getField;
        }

        if (auto setField = readDescriptorField("set");
            setField.has_value() && setField->isFunction()) {
          regex->properties["__set_" + key] = *setField;
        }
      }
    }
    return args[0];
  };
  objectConstructor->properties["defineProperty"] = Value(objectDefineProperty);

  // Object.defineProperties - define multiple properties
  auto objectDefineProperties = std::make_shared<Function>();
  objectDefineProperties->isNative = true;
  objectDefineProperties->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2 || !args[0].isObject() || !args[1].isObject()) {
      return args.empty() ? Value(Undefined{}) : args[0];
    }
    auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
    auto props = std::get<std::shared_ptr<Object>>(args[1].data);

    if (obj->frozen) {
      return args[0];
    }

    for (const auto& [key, descriptor] : props->properties) {
      if (obj->sealed && obj->properties.find(key) == obj->properties.end()) {
        continue;  // Skip new properties on sealed object
      }
      if (descriptor.isObject()) {
        auto descObj = std::get<std::shared_ptr<Object>>(descriptor.data);
        auto valueIt = descObj->properties.find("value");
        if (valueIt != descObj->properties.end()) {
          obj->properties[key] = valueIt->second;
        }
      }
    }
    return args[0];
  };
  objectConstructor->properties["defineProperties"] = Value(objectDefineProperties);

  env->define("Object", Value(objectConstructor));

  // Math object
  auto mathObj = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // Math constants
  mathObj->properties["PI"] = Value(3.141592653589793);
  mathObj->properties["E"] = Value(2.718281828459045);
  mathObj->properties["LN2"] = Value(0.6931471805599453);
  mathObj->properties["LN10"] = Value(2.302585092994046);
  mathObj->properties["LOG2E"] = Value(1.4426950408889634);
  mathObj->properties["LOG10E"] = Value(0.4342944819032518);
  mathObj->properties["SQRT1_2"] = Value(0.7071067811865476);
  mathObj->properties["SQRT2"] = Value(1.4142135623730951);

  // Math methods
  auto mathAbs = std::make_shared<Function>();
  mathAbs->isNative = true;
  mathAbs->nativeFunc = Math_abs;
  mathObj->properties["abs"] = Value(mathAbs);

  auto mathCeil = std::make_shared<Function>();
  mathCeil->isNative = true;
  mathCeil->nativeFunc = Math_ceil;
  mathObj->properties["ceil"] = Value(mathCeil);

  auto mathFloor = std::make_shared<Function>();
  mathFloor->isNative = true;
  mathFloor->nativeFunc = Math_floor;
  mathObj->properties["floor"] = Value(mathFloor);

  auto mathRound = std::make_shared<Function>();
  mathRound->isNative = true;
  mathRound->nativeFunc = Math_round;
  mathObj->properties["round"] = Value(mathRound);

  auto mathTrunc = std::make_shared<Function>();
  mathTrunc->isNative = true;
  mathTrunc->nativeFunc = Math_trunc;
  mathObj->properties["trunc"] = Value(mathTrunc);

  auto mathMax = std::make_shared<Function>();
  mathMax->isNative = true;
  mathMax->nativeFunc = Math_max;
  mathObj->properties["max"] = Value(mathMax);

  auto mathMin = std::make_shared<Function>();
  mathMin->isNative = true;
  mathMin->nativeFunc = Math_min;
  mathObj->properties["min"] = Value(mathMin);

  auto mathPow = std::make_shared<Function>();
  mathPow->isNative = true;
  mathPow->nativeFunc = Math_pow;
  mathObj->properties["pow"] = Value(mathPow);

  auto mathSqrt = std::make_shared<Function>();
  mathSqrt->isNative = true;
  mathSqrt->nativeFunc = Math_sqrt;
  mathObj->properties["sqrt"] = Value(mathSqrt);

  auto mathSin = std::make_shared<Function>();
  mathSin->isNative = true;
  mathSin->nativeFunc = Math_sin;
  mathObj->properties["sin"] = Value(mathSin);

  auto mathCos = std::make_shared<Function>();
  mathCos->isNative = true;
  mathCos->nativeFunc = Math_cos;
  mathObj->properties["cos"] = Value(mathCos);

  auto mathTan = std::make_shared<Function>();
  mathTan->isNative = true;
  mathTan->nativeFunc = Math_tan;
  mathObj->properties["tan"] = Value(mathTan);

  auto mathRandom = std::make_shared<Function>();
  mathRandom->isNative = true;
  mathRandom->nativeFunc = Math_random;
  mathObj->properties["random"] = Value(mathRandom);

  auto mathSign = std::make_shared<Function>();
  mathSign->isNative = true;
  mathSign->nativeFunc = Math_sign;
  mathObj->properties["sign"] = Value(mathSign);

  auto mathLog = std::make_shared<Function>();
  mathLog->isNative = true;
  mathLog->nativeFunc = Math_log;
  mathObj->properties["log"] = Value(mathLog);

  auto mathLog10 = std::make_shared<Function>();
  mathLog10->isNative = true;
  mathLog10->nativeFunc = Math_log10;
  mathObj->properties["log10"] = Value(mathLog10);

  auto mathExp = std::make_shared<Function>();
  mathExp->isNative = true;
  mathExp->nativeFunc = Math_exp;
  mathObj->properties["exp"] = Value(mathExp);

  auto mathCbrt = std::make_shared<Function>();
  mathCbrt->isNative = true;
  mathCbrt->nativeFunc = Math_cbrt;
  mathObj->properties["cbrt"] = Value(mathCbrt);

  auto mathLog2 = std::make_shared<Function>();
  mathLog2->isNative = true;
  mathLog2->nativeFunc = Math_log2;
  mathObj->properties["log2"] = Value(mathLog2);

  auto mathHypot = std::make_shared<Function>();
  mathHypot->isNative = true;
  mathHypot->nativeFunc = Math_hypot;
  mathObj->properties["hypot"] = Value(mathHypot);

  auto mathExpm1 = std::make_shared<Function>();
  mathExpm1->isNative = true;
  mathExpm1->nativeFunc = Math_expm1;
  mathObj->properties["expm1"] = Value(mathExpm1);

  auto mathLog1p = std::make_shared<Function>();
  mathLog1p->isNative = true;
  mathLog1p->nativeFunc = Math_log1p;
  mathObj->properties["log1p"] = Value(mathLog1p);

  auto mathFround = std::make_shared<Function>();
  mathFround->isNative = true;
  mathFround->nativeFunc = Math_fround;
  mathObj->properties["fround"] = Value(mathFround);

  auto mathClz32 = std::make_shared<Function>();
  mathClz32->isNative = true;
  mathClz32->nativeFunc = Math_clz32;
  mathObj->properties["clz32"] = Value(mathClz32);

  auto mathImul = std::make_shared<Function>();
  mathImul->isNative = true;
  mathImul->nativeFunc = Math_imul;
  mathObj->properties["imul"] = Value(mathImul);

  env->define("Math", Value(mathObj));

  // Date constructor
  auto dateConstructor = std::make_shared<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  dateConstructor->isNative = true;
  dateConstructor->isConstructor = true;
  dateConstructor->properties["name"] = Value(std::string("Date"));
  dateConstructor->properties["length"] = Value(1.0);
  dateConstructor->nativeFunc = [toPrimitive](const std::vector<Value>& args) -> Value {
    if (args.size() == 1) {
      Value primitive = toPrimitive(args[0], false);
      if (primitive.isSymbol() || primitive.isBigInt()) {
        throw std::runtime_error("TypeError: Cannot convert to number");
      }
      if (primitive.isString()) {
        return Date_constructor({primitive});
      }
      return Date_constructor({Value(primitive.toNumber())});
    }
    return Date_constructor(args);
  };

  // Date static methods
  auto dateNow = std::make_shared<Function>();
  dateNow->isNative = true;
  dateNow->nativeFunc = Date_now;
  dateConstructor->properties["now"] = Value(dateNow);

  auto dateParse = std::make_shared<Function>();
  dateParse->isNative = true;
  dateParse->nativeFunc = Date_parse;
  dateConstructor->properties["parse"] = Value(dateParse);

  env->define("Date", Value(dateConstructor));

  // String constructor with static methods
  auto stringConstructorFn = std::make_shared<Function>();
  stringConstructorFn->isNative = true;
  stringConstructorFn->isConstructor = true;
  stringConstructorFn->properties["__wrap_primitive__"] = Value(true);
  stringConstructorFn->nativeFunc = [toPrimitive](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(std::string(""));
    }
    Value primitive = toPrimitive(args[0], true);
    return Value(primitive.toString());
  };

  // Wrap in an Object to hold static methods
  auto stringConstructorObj = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // The constructor itself
  stringConstructorObj->properties["__callable_object__"] = Value(true);
  stringConstructorObj->properties["constructor"] = Value(stringConstructorFn);

  // String.fromCharCode
  auto fromCharCode = std::make_shared<Function>();
  fromCharCode->isNative = true;
  fromCharCode->nativeFunc = String_fromCharCode;
  stringConstructorObj->properties["fromCharCode"] = Value(fromCharCode);

  // String.fromCodePoint
  auto fromCodePoint = std::make_shared<Function>();
  fromCodePoint->isNative = true;
  fromCodePoint->nativeFunc = String_fromCodePoint;
  stringConstructorObj->properties["fromCodePoint"] = Value(fromCodePoint);

  auto stringPrototype = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  auto stringMatchAll = std::make_shared<Function>();
  stringMatchAll->isNative = true;
  stringMatchAll->isConstructor = false;
  stringMatchAll->properties["name"] = Value(std::string("matchAll"));
  stringMatchAll->properties["length"] = Value(1.0);
  stringMatchAll->properties["__uses_this_arg__"] = Value(true);
  stringMatchAll->properties["__throw_on_new__"] = Value(true);
  stringMatchAll->nativeFunc = [regExpPrototype](const std::vector<Value>& args) -> Value {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
      throw std::runtime_error("TypeError: String.prototype.matchAll called on null or undefined");
    }

    Value thisValue = args[0];
    Value regexp = args.size() > 1 ? args[1] : Value(Undefined{});
    Interpreter* interpreter = getGlobalInterpreter();
    const std::string& matchAllKey = WellKnownSymbols::matchAllKey();

    auto callChecked = [&](const Value& callee,
                           const std::vector<Value>& callArgs,
                           const Value& thisArg) -> Value {
      if (!callee.isFunction()) {
        return Value(Undefined{});
      }

      if (interpreter) {
        Value out = interpreter->callForHarness(callee, callArgs, thisArg);
        if (interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw std::runtime_error(err.toString());
        }
        return out;
      }

      auto fn = std::get<std::shared_ptr<Function>>(callee.data);
      if (fn->isNative) {
        auto itUsesThis = fn->properties.find("__uses_this_arg__");
        if (itUsesThis != fn->properties.end() && itUsesThis->second.isBool() && itUsesThis->second.toBool()) {
          std::vector<Value> nativeArgs;
          nativeArgs.reserve(callArgs.size() + 1);
          nativeArgs.push_back(thisArg);
          nativeArgs.insert(nativeArgs.end(), callArgs.begin(), callArgs.end());
          return fn->nativeFunc(nativeArgs);
        }
        return fn->nativeFunc(callArgs);
      }

      return Value(Undefined{});
    };

    auto getObjectValue = [&](const std::shared_ptr<Object>& obj,
                              const Value& thisArg,
                              const std::string& key) -> Value {
      std::string getterName = "__get_" + key;
      auto getterIt = obj->properties.find(getterName);
      if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
        return callChecked(getterIt->second, {}, thisArg);
      }

      auto it = obj->properties.find(key);
      if (it != obj->properties.end()) {
        return it->second;
      }
      return Value(Undefined{});
    };

    auto getRegexValue = [&](const std::shared_ptr<Regex>& regex,
                             const std::string& key) -> Value {
      Value regexValue(regex);

      std::string getterName = "__get_" + key;
      auto getterIt = regex->properties.find(getterName);
      if (getterIt != regex->properties.end() && getterIt->second.isFunction()) {
        return callChecked(getterIt->second, {}, regexValue);
      }

      auto it = regex->properties.find(key);
      if (it != regex->properties.end()) {
        return it->second;
      }

      if (regExpPrototype) {
        std::string protoGetterName = "__get_" + key;
        auto protoGetterIt = regExpPrototype->properties.find(protoGetterName);
        if (protoGetterIt != regExpPrototype->properties.end() && protoGetterIt->second.isFunction()) {
          return callChecked(protoGetterIt->second, {}, regexValue);
        }

        auto protoIt = regExpPrototype->properties.find(key);
        if (protoIt != regExpPrototype->properties.end()) {
          return protoIt->second;
        }
      }

      if (key == "source") {
        return Value(regex->pattern);
      }
      if (key == "flags") {
        return Value(regex->flags);
      }

      return Value(Undefined{});
    };

    std::string input;
    if (thisValue.isObject()) {
      auto obj = std::get<std::shared_ptr<Object>>(thisValue.data);
      Value toPrimitive = Value(Undefined{});
      auto toPrimitiveIt = obj->properties.find("Symbol(Symbol.toPrimitive)");
      if (toPrimitiveIt != obj->properties.end()) {
        toPrimitive = toPrimitiveIt->second;
      } else {
        auto fallbackIt = obj->properties.find("undefined");
        if (fallbackIt != obj->properties.end()) {
          toPrimitive = fallbackIt->second;
        }
      }
      if (toPrimitive.isFunction()) {
        input = callChecked(toPrimitive, {}, thisValue).toString();
      } else {
        input = thisValue.toString();
      }
    } else {
      input = thisValue.toString();
    }

    if (!regexp.isUndefined() && !regexp.isNull()) {
      if (regexp.isRegex()) {
        auto regex = std::get<std::shared_ptr<Regex>>(regexp.data);
        Value flagsValue = getRegexValue(regex, "flags");
        if (flagsValue.isUndefined() || flagsValue.isNull()) {
          throw std::runtime_error("TypeError: RegExp flags is undefined or null");
        }
        if (flagsValue.toString().find('g') == std::string::npos) {
          throw std::runtime_error("TypeError: String.prototype.matchAll requires a global RegExp");
        }
      }

      Value matcher(Undefined{});
      if (regexp.isRegex()) {
        matcher = getRegexValue(std::get<std::shared_ptr<Regex>>(regexp.data), matchAllKey);
      } else if (regexp.isObject()) {
        matcher = getObjectValue(std::get<std::shared_ptr<Object>>(regexp.data), regexp, matchAllKey);
      } else if (regexp.isFunction()) {
        auto fn = std::get<std::shared_ptr<Function>>(regexp.data);
        auto it = fn->properties.find(matchAllKey);
        if (it != fn->properties.end()) {
          matcher = it->second;
        }
      }

      if (!matcher.isUndefined() && !matcher.isNull()) {
        if (!matcher.isFunction()) {
          throw std::runtime_error("TypeError: @@matchAll is not callable");
        }
        return callChecked(matcher, {Value(input)}, regexp);
      }
    }

    std::string pattern;
    if (regexp.isRegex()) {
      pattern = std::get<std::shared_ptr<Regex>>(regexp.data)->pattern;
    } else if (regexp.isUndefined()) {
      pattern = "";  // RegExp(undefined) uses empty pattern
    } else {
      pattern = regexp.toString();
    }

    auto rx = std::make_shared<Regex>(pattern, "g");
    Value rxValue(rx);
    Value matcher = getRegexValue(rx, matchAllKey);
    if (!matcher.isFunction()) {
      throw std::runtime_error("TypeError: RegExp @@matchAll is not callable");
    }
    return callChecked(matcher, {Value(input)}, rxValue);
  };
  stringPrototype->properties["matchAll"] = Value(stringMatchAll);
  stringPrototype->properties["__non_enum_matchAll"] = Value(true);
  stringConstructorObj->properties["prototype"] = Value(stringPrototype);

  // For simplicity, we can make the Object callable by storing the function
  env->define("String", Value(stringConstructorObj));

  // WebAssembly global object
  env->define("WebAssembly", wasm_js::createWebAssemblyGlobal());

  // globalThis - reference to the global object
  // Create a proxy object that reflects the current global environment
  auto globalThisObj = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // Copy all current global bindings into globalThis
  for (const auto& [name, value] : env->bindings_) {
    globalThisObj->properties[name] = value;
  }

  // Define globalThis pointing to the global object
  env->define("globalThis", Value(globalThisObj));
  // 'this' at global scope should be globalThis
  env->define("this", Value(globalThisObj));

  // Also add globalThis to itself
  globalThisObj->properties["globalThis"] = Value(globalThisObj);

  // Timer functions - setTimeout
  auto setTimeoutFn = std::make_shared<Function>();
  setTimeoutFn->isNative = true;
  setTimeoutFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isFunction()) {
      return Value(Undefined{});
    }

    auto callback = std::get<std::shared_ptr<Function>>(args[0].data);
    int64_t delayMs = args.size() > 1 ? static_cast<int64_t>(args[1].toNumber()) : 0;

    // Create a timer callback that executes the JS function
    auto& loop = EventLoopContext::instance().getLoop();
    TimerId id = loop.setTimeout([callback]() -> Value {
      // Execute the callback function
      if (callback->isNative && callback->nativeFunc) {
        return callback->nativeFunc({});
      }
      // For non-native functions, we can't easily execute them here
      // This will be improved when we integrate with the interpreter
      return Value(Undefined{});
    }, delayMs);

    return Value(static_cast<double>(id));
  };
  env->define("setTimeout", Value(setTimeoutFn));

  // Timer functions - setInterval
  auto setIntervalFn = std::make_shared<Function>();
  setIntervalFn->isNative = true;
  setIntervalFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isFunction()) {
      return Value(Undefined{});
    }

    auto callback = std::get<std::shared_ptr<Function>>(args[0].data);
    int64_t intervalMs = args.size() > 1 ? static_cast<int64_t>(args[1].toNumber()) : 0;

    // Create an interval timer callback
    auto& loop = EventLoopContext::instance().getLoop();
    TimerId id = loop.setInterval([callback]() -> Value {
      // Execute the callback function
      if (callback->isNative && callback->nativeFunc) {
        return callback->nativeFunc({});
      }
      return Value(Undefined{});
    }, intervalMs);

    return Value(static_cast<double>(id));
  };
  env->define("setInterval", Value(setIntervalFn));

  // Timer functions - clearTimeout
  auto clearTimeoutFn = std::make_shared<Function>();
  clearTimeoutFn->isNative = true;
  clearTimeoutFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(Undefined{});
    }

    TimerId id = static_cast<TimerId>(args[0].toNumber());
    auto& loop = EventLoopContext::instance().getLoop();
    loop.clearTimer(id);

    return Value(Undefined{});
  };
  env->define("clearTimeout", Value(clearTimeoutFn));

  // Timer functions - clearInterval (same implementation as clearTimeout)
  auto clearIntervalFn = std::make_shared<Function>();
  clearIntervalFn->isNative = true;
  clearIntervalFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(Undefined{});
    }

    TimerId id = static_cast<TimerId>(args[0].toNumber());
    auto& loop = EventLoopContext::instance().getLoop();
    loop.clearTimer(id);

    return Value(Undefined{});
  };
  env->define("clearInterval", Value(clearIntervalFn));

  // queueMicrotask function
  auto queueMicrotaskFn = std::make_shared<Function>();
  queueMicrotaskFn->isNative = true;
  queueMicrotaskFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isFunction()) {
      return Value(Undefined{});
    }

    auto callback = std::get<std::shared_ptr<Function>>(args[0].data);

    // Queue the microtask
    auto& loop = EventLoopContext::instance().getLoop();
    loop.queueMicrotask([callback]() {
      // Execute the callback function
      if (callback->isNative && callback->nativeFunc) {
        callback->nativeFunc({});
      }
    });

    return Value(Undefined{});
  };
  env->define("queueMicrotask", Value(queueMicrotaskFn));

  // TextEncoder and TextDecoder
  env->define("TextEncoder", Value(createTextEncoderConstructor()));
  env->define("TextDecoder", Value(createTextDecoderConstructor()));

  // URL and URLSearchParams
  env->define("URL", Value(createURLConstructor()));
  env->define("URLSearchParams", Value(createURLSearchParamsConstructor()));

  // AbortController and AbortSignal
  auto abortControllerCtor = std::make_shared<Function>();
  abortControllerCtor->isNative = true;
  abortControllerCtor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto controller = std::make_shared<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));

    // Create AbortSignal
    auto signal = std::make_shared<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    signal->properties["aborted"] = Value(false);
    signal->properties["reason"] = Value(Undefined{});

    // Event listeners storage
    auto listeners = std::make_shared<Array>();
    signal->properties["_listeners"] = Value(listeners);

    // addEventListener method
    auto addEventListenerFn = std::make_shared<Function>();
    addEventListenerFn->isNative = true;
    addEventListenerFn->nativeFunc = [listeners](const std::vector<Value>& args) -> Value {
      if (args.size() >= 2 && args[0].toString() == "abort" && args[1].isFunction()) {
        listeners->elements.push_back(args[1]);
      }
      return Value(Undefined{});
    };
    signal->properties["addEventListener"] = Value(addEventListenerFn);

    // removeEventListener method
    auto removeEventListenerFn = std::make_shared<Function>();
    removeEventListenerFn->isNative = true;
    removeEventListenerFn->nativeFunc = [listeners](const std::vector<Value>& args) -> Value {
      // Simple implementation - just mark for removal
      return Value(Undefined{});
    };
    signal->properties["removeEventListener"] = Value(removeEventListenerFn);

    controller->properties["signal"] = Value(signal);

    // abort method
    auto abortFn = std::make_shared<Function>();
    abortFn->isNative = true;
    abortFn->nativeFunc = [signal, listeners](const std::vector<Value>& args) -> Value {
      // Check if already aborted
      if (signal->properties["aborted"].toBool()) {
        return Value(Undefined{});
      }

      signal->properties["aborted"] = Value(true);
      signal->properties["reason"] = args.empty() ?
          Value(std::string("AbortError: The operation was aborted")) : args[0];

      // Call all abort listeners
      for (const auto& listener : listeners->elements) {
        if (listener.isFunction()) {
          auto fn = std::get<std::shared_ptr<Function>>(listener.data);
          if (fn->isNative && fn->nativeFunc) {
            fn->nativeFunc({});
          }
        }
      }

      return Value(Undefined{});
    };
    controller->properties["abort"] = Value(abortFn);

    return Value(controller);
  };
  env->define("AbortController", Value(abortControllerCtor));

  // AbortSignal.abort() static method
  auto abortSignalObj = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  auto abortStaticFn = std::make_shared<Function>();
  abortStaticFn->isNative = true;
  abortStaticFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto signal = std::make_shared<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    signal->properties["aborted"] = Value(true);
    signal->properties["reason"] = args.empty() ?
        Value(std::string("AbortError: The operation was aborted")) : args[0];
    return Value(signal);
  };
  abortSignalObj->properties["abort"] = Value(abortStaticFn);

  // AbortSignal.timeout() static method
  auto timeoutStaticFn = std::make_shared<Function>();
  timeoutStaticFn->isNative = true;
  timeoutStaticFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto signal = std::make_shared<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    signal->properties["aborted"] = Value(false);
    signal->properties["reason"] = Value(Undefined{});
    // Note: Actual timeout implementation would require event loop integration
    return Value(signal);
  };
  abortSignalObj->properties["timeout"] = Value(timeoutStaticFn);

  env->define("AbortSignal", Value(abortSignalObj));

  // Streams API - ReadableStream
  auto readableStreamCtor = std::make_shared<Function>();
  readableStreamCtor->isNative = true;
  readableStreamCtor->isConstructor = true;
  readableStreamCtor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // Create underlying source callbacks from argument
    std::shared_ptr<Function> startFn = nullptr;
    std::shared_ptr<Function> pullFn = nullptr;
    std::shared_ptr<Function> cancelFn = nullptr;
    double highWaterMark = 1.0;

    if (!args.empty() && args[0].isObject()) {
      auto srcObj = std::get<std::shared_ptr<Object>>(args[0].data);

      // Get start callback
      auto startIt = srcObj->properties.find("start");
      if (startIt != srcObj->properties.end() && startIt->second.isFunction()) {
        startFn = std::get<std::shared_ptr<Function>>(startIt->second.data);
      }

      // Get pull callback
      auto pullIt = srcObj->properties.find("pull");
      if (pullIt != srcObj->properties.end() && pullIt->second.isFunction()) {
        pullFn = std::get<std::shared_ptr<Function>>(pullIt->second.data);
      }

      // Get cancel callback
      auto cancelIt = srcObj->properties.find("cancel");
      if (cancelIt != srcObj->properties.end() && cancelIt->second.isFunction()) {
        cancelFn = std::get<std::shared_ptr<Function>>(cancelIt->second.data);
      }
    }

    // Create the stream
    auto stream = createReadableStream(startFn, pullFn, cancelFn, highWaterMark);
    return Value(stream);
  };
  env->define("ReadableStream", Value(readableStreamCtor));

  // Streams API - WritableStream
  auto writableStreamCtor = std::make_shared<Function>();
  writableStreamCtor->isNative = true;
  writableStreamCtor->isConstructor = true;
  writableStreamCtor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // Create underlying sink callbacks from argument
    std::shared_ptr<Function> startFn = nullptr;
    std::shared_ptr<Function> writeFn = nullptr;
    std::shared_ptr<Function> closeFn = nullptr;
    std::shared_ptr<Function> abortFn = nullptr;
    double highWaterMark = 1.0;

    if (!args.empty() && args[0].isObject()) {
      auto sinkObj = std::get<std::shared_ptr<Object>>(args[0].data);

      // Get start callback
      auto startIt = sinkObj->properties.find("start");
      if (startIt != sinkObj->properties.end() && startIt->second.isFunction()) {
        startFn = std::get<std::shared_ptr<Function>>(startIt->second.data);
      }

      // Get write callback
      auto writeIt = sinkObj->properties.find("write");
      if (writeIt != sinkObj->properties.end() && writeIt->second.isFunction()) {
        writeFn = std::get<std::shared_ptr<Function>>(writeIt->second.data);
      }

      // Get close callback
      auto closeIt = sinkObj->properties.find("close");
      if (closeIt != sinkObj->properties.end() && closeIt->second.isFunction()) {
        closeFn = std::get<std::shared_ptr<Function>>(closeIt->second.data);
      }

      // Get abort callback
      auto abortIt = sinkObj->properties.find("abort");
      if (abortIt != sinkObj->properties.end() && abortIt->second.isFunction()) {
        abortFn = std::get<std::shared_ptr<Function>>(abortIt->second.data);
      }
    }

    // Create the stream
    auto stream = createWritableStream(startFn, writeFn, closeFn, abortFn, highWaterMark);
    return Value(stream);
  };
  env->define("WritableStream", Value(writableStreamCtor));

  // Streams API - TransformStream
  auto transformStreamCtor = std::make_shared<Function>();
  transformStreamCtor->isNative = true;
  transformStreamCtor->isConstructor = true;
  transformStreamCtor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // Create transformer callbacks from argument
    std::shared_ptr<Function> startFn = nullptr;
    std::shared_ptr<Function> transformFn = nullptr;
    std::shared_ptr<Function> flushFn = nullptr;

    if (!args.empty() && args[0].isObject()) {
      auto transformerObj = std::get<std::shared_ptr<Object>>(args[0].data);

      // Get start callback
      auto startIt = transformerObj->properties.find("start");
      if (startIt != transformerObj->properties.end() && startIt->second.isFunction()) {
        startFn = std::get<std::shared_ptr<Function>>(startIt->second.data);
      }

      // Get transform callback
      auto transformIt = transformerObj->properties.find("transform");
      if (transformIt != transformerObj->properties.end() && transformIt->second.isFunction()) {
        transformFn = std::get<std::shared_ptr<Function>>(transformIt->second.data);
      }

      // Get flush callback
      auto flushIt = transformerObj->properties.find("flush");
      if (flushIt != transformerObj->properties.end() && flushIt->second.isFunction()) {
        flushFn = std::get<std::shared_ptr<Function>>(flushIt->second.data);
      }
    }

    // Create the stream
    auto stream = createTransformStream(startFn, transformFn, flushFn);
    return Value(stream);
  };
  env->define("TransformStream", Value(transformStreamCtor));

  // File System module (fs)
  globalThisObj->properties["fs"] = Value(createFSModule());

  // performance.now() - high-resolution timing
  static auto startTime = std::chrono::steady_clock::now();

  auto performanceNowFn = std::make_shared<Function>();
  performanceNowFn->isNative = true;
  performanceNowFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - startTime).count();
    return Value(static_cast<double>(elapsed) / 1000.0);  // Return milliseconds
  };

  auto performanceObj = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  performanceObj->properties["now"] = Value(performanceNowFn);
  env->define("performance", Value(performanceObj));
  globalThisObj->properties["performance"] = Value(performanceObj);

  // structuredClone - deep clone objects, arrays, and primitives
  std::function<Value(const Value&)> deepClone;
  deepClone = [&deepClone](const Value& val) -> Value {
    // Primitives are returned as-is (they're already copies)
    if (val.isUndefined() || val.isNull() || val.isBool() ||
        val.isNumber() || val.isString() || val.isBigInt() || val.isSymbol()) {
      return val;
    }

    // Clone arrays
    if (val.isArray()) {
      auto arr = std::get<std::shared_ptr<Array>>(val.data);
      auto newArr = std::make_shared<Array>();
      GarbageCollector::instance().reportAllocation(sizeof(Array));
      for (const auto& elem : arr->elements) {
        newArr->elements.push_back(deepClone(elem));
      }
      return Value(newArr);
    }

    // Clone objects
    if (val.isObject()) {
      auto obj = std::get<std::shared_ptr<Object>>(val.data);
      auto newObj = std::make_shared<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      for (const auto& [key, value] : obj->properties) {
        newObj->properties[key] = deepClone(value);
      }
      return Value(newObj);
    }

    // Functions, Promises, etc. cannot be cloned - return as-is
    return val;
  };

  auto structuredCloneFn = std::make_shared<Function>();
  structuredCloneFn->isNative = true;
  structuredCloneFn->nativeFunc = [deepClone](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(Undefined{});
    return deepClone(args[0]);
  };
  env->define("structuredClone", Value(structuredCloneFn));
  globalThisObj->properties["structuredClone"] = Value(structuredCloneFn);

  // Base64 encoding table
  static const char base64Chars[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  // btoa - encode string to Base64
  auto btoaFn = std::make_shared<Function>();
  btoaFn->isNative = true;
  btoaFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::string(""));
    std::string input = args[0].toString();
    std::string result;
    result.reserve((input.size() + 2) / 3 * 4);

    for (size_t i = 0; i < input.size(); i += 3) {
      uint32_t n = static_cast<uint8_t>(input[i]) << 16;
      if (i + 1 < input.size()) n |= static_cast<uint8_t>(input[i + 1]) << 8;
      if (i + 2 < input.size()) n |= static_cast<uint8_t>(input[i + 2]);

      result += base64Chars[(n >> 18) & 0x3F];
      result += base64Chars[(n >> 12) & 0x3F];
      result += (i + 1 < input.size()) ? base64Chars[(n >> 6) & 0x3F] : '=';
      result += (i + 2 < input.size()) ? base64Chars[n & 0x3F] : '=';
    }
    return Value(result);
  };
  env->define("btoa", Value(btoaFn));
  globalThisObj->properties["btoa"] = Value(btoaFn);

  // atob - decode Base64 to string
  auto atobFn = std::make_shared<Function>();
  atobFn->isNative = true;
  atobFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::string(""));
    std::string input = args[0].toString();

    // Build decode lookup table
    static int decodeTable[256] = {-1};
    static bool tableInit = false;
    if (!tableInit) {
      for (int i = 0; i < 256; ++i) decodeTable[i] = -1;
      for (int i = 0; i < 64; ++i) decodeTable[static_cast<uint8_t>(base64Chars[i])] = i;
      tableInit = true;
    }

    std::string result;
    result.reserve(input.size() * 3 / 4);

    int bits = 0;
    int bitCount = 0;
    for (char c : input) {
      if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
      int value = decodeTable[static_cast<uint8_t>(c)];
      if (value == -1) continue;  // Skip invalid characters

      bits = (bits << 6) | value;
      bitCount += 6;

      if (bitCount >= 8) {
        bitCount -= 8;
        result += static_cast<char>((bits >> bitCount) & 0xFF);
      }
    }
    return Value(result);
  };
  env->define("atob", Value(atobFn));
  globalThisObj->properties["atob"] = Value(atobFn);

  // encodeURIComponent - encode URI component
  auto encodeURIComponentFn = std::make_shared<Function>();
  encodeURIComponentFn->isNative = true;
  encodeURIComponentFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::string("undefined"));
    std::string input = args[0].toString();
    std::string result;
    result.reserve(input.size() * 3);  // Worst case

    for (unsigned char c : input) {
      // Unreserved characters (RFC 3986): A-Z a-z 0-9 - _ . ~
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
        result += static_cast<char>(c);
      } else {
        char hex[4];
        snprintf(hex, sizeof(hex), "%%%02X", c);
        result += hex;
      }
    }
    return Value(result);
  };
  env->define("encodeURIComponent", Value(encodeURIComponentFn));
  globalThisObj->properties["encodeURIComponent"] = Value(encodeURIComponentFn);

  // decodeURIComponent - decode URI component
  auto decodeURIComponentFn = std::make_shared<Function>();
  decodeURIComponentFn->isNative = true;
  decodeURIComponentFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::string("undefined"));
    std::string input = args[0].toString();
    std::string result;
    result.reserve(input.size());

    for (size_t i = 0; i < input.size(); ++i) {
      if (input[i] == '%' && i + 2 < input.size()) {
        int value = 0;
        if (std::sscanf(input.c_str() + i + 1, "%2x", &value) == 1) {
          result += static_cast<char>(value);
          i += 2;
          continue;
        }
      }
      result += input[i];
    }
    return Value(result);
  };
  env->define("decodeURIComponent", Value(decodeURIComponentFn));
  globalThisObj->properties["decodeURIComponent"] = Value(decodeURIComponentFn);

  // encodeURI - encode full URI (leaves more characters unencoded)
  auto encodeURIFn = std::make_shared<Function>();
  encodeURIFn->isNative = true;
  encodeURIFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::string("undefined"));
    std::string input = args[0].toString();
    std::string result;
    result.reserve(input.size() * 3);

    for (unsigned char c : input) {
      // Reserved and unreserved characters that should NOT be encoded in full URI
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' ||
          c == ':' || c == '/' || c == '?' || c == '#' || c == '[' || c == ']' ||
          c == '@' || c == '!' || c == '$' || c == '&' || c == '\'' ||
          c == '(' || c == ')' || c == '*' || c == '+' || c == ',' || c == ';' || c == '=') {
        result += static_cast<char>(c);
      } else {
        char hex[4];
        snprintf(hex, sizeof(hex), "%%%02X", c);
        result += hex;
      }
    }
    return Value(result);
  };
  env->define("encodeURI", Value(encodeURIFn));
  globalThisObj->properties["encodeURI"] = Value(encodeURIFn);

  // decodeURI - decode full URI
  auto decodeURIFn = std::make_shared<Function>();
  decodeURIFn->isNative = true;
  decodeURIFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::string("undefined"));
    std::string input = args[0].toString();
    std::string result;
    result.reserve(input.size());

    for (size_t i = 0; i < input.size(); ++i) {
      if (input[i] == '%' && i + 2 < input.size()) {
        int value = 0;
        if (std::sscanf(input.c_str() + i + 1, "%2x", &value) == 1) {
          result += static_cast<char>(value);
          i += 2;
          continue;
        }
      }
      result += input[i];
    }
    return Value(result);
  };
  env->define("decodeURI", Value(decodeURIFn));
  globalThisObj->properties["decodeURI"] = Value(decodeURIFn);

  // ===== Function constructor =====
  auto functionConstructor = std::make_shared<Function>();
  functionConstructor->isNative = true;
  functionConstructor->isConstructor = true;
  functionConstructor->properties["name"] = Value(std::string("Function"));
  functionConstructor->properties["length"] = Value(1.0);
  functionConstructor->nativeFunc = [env, objectPrototype](const std::vector<Value>& args) -> Value {
    std::vector<std::string> params;
    std::string body;

    if (!args.empty()) {
      for (size_t i = 0; i + 1 < args.size(); ++i) {
        params.push_back(args[i].toString());
      }
      body = args.back().toString();
    }

    std::string source = "function anonymous(";
    for (size_t i = 0; i < params.size(); ++i) {
      if (i > 0) source += ",";
      source += params[i];
    }
    source += ") {\n";
    source += body;
    source += "\n}";

    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    for (size_t i = 0; i + 2 < tokens.size(); ++i) {
      if (tokens[i].type == TokenType::Import &&
          !tokens[i].escaped &&
          tokens[i + 1].type == TokenType::Dot &&
          tokens[i + 2].type == TokenType::Identifier &&
          !tokens[i + 2].escaped &&
          tokens[i + 2].value == "meta") {
        throw std::runtime_error("SyntaxError: Cannot use import.meta outside a module");
      }
    }
    Parser parser(tokens);
    auto program = parser.parse();
    if (!program || program->body.empty()) {
      throw std::runtime_error("SyntaxError: Function constructor parse error");
    }

    auto compiledProgram = std::make_shared<Program>(std::move(*program));
    auto* fnDecl = std::get_if<FunctionDeclaration>(&compiledProgram->body[0]->node);
    if (!fnDecl) {
      throw std::runtime_error("SyntaxError: Function constructor parse error");
    }

    auto fn = std::make_shared<Function>();
    fn->isNative = false;
    fn->isAsync = fnDecl->isAsync;
    fn->isGenerator = fnDecl->isGenerator;
    fn->isConstructor = true;
    fn->closure = env;

    auto hasUseStrictDirective = [](const std::vector<StmtPtr>& bodyStmts) -> bool {
      for (const auto& stmt : bodyStmts) {
        if (!stmt) break;
        auto* exprStmt = std::get_if<ExpressionStmt>(&stmt->node);
        if (!exprStmt || !exprStmt->expression) break;
        auto* str = std::get_if<StringLiteral>(&exprStmt->expression->node);
        if (!str) break;
        if (str->value == "use strict") return true;
      }
      return false;
    };
    fn->isStrict = hasUseStrictDirective(fnDecl->body);

    for (const auto& param : fnDecl->params) {
      FunctionParam funcParam;
      funcParam.name = param.name.name;
      if (param.defaultValue) {
        funcParam.defaultValue = std::shared_ptr<void>(
          const_cast<Expression*>(param.defaultValue.get()),
          [](void*) {});
      }
      fn->params.push_back(funcParam);
    }

    if (fnDecl->restParam.has_value()) {
      fn->restParam = fnDecl->restParam->name;
    }

    fn->body = std::shared_ptr<void>(
      const_cast<std::vector<StmtPtr>*>(&fnDecl->body),
      [](void*) {});
    fn->astOwner = compiledProgram;
    fn->properties["name"] = Value(std::string("anonymous"));
    fn->properties["length"] = Value(static_cast<double>(fn->params.size()));

    auto fnPrototype = std::make_shared<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    fnPrototype->properties["constructor"] = Value(fn);
    fnPrototype->properties["__proto__"] = Value(objectPrototype);
    fn->properties["prototype"] = Value(fnPrototype);

    return Value(fn);
  };

  // Function.prototype - a minimal prototype with call/apply/bind
  // (actual call/apply/bind are handled dynamically in the interpreter)
  auto functionPrototype = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  functionPrototype->properties["__proto__"] = Value(objectPrototype);

  // Function.prototype.call - uses __uses_this_arg__ so args[0] = this (the function to call)
  auto fpCall = std::make_shared<Function>();
  fpCall->isNative = true;
  fpCall->properties["name"] = Value(std::string("call"));
  fpCall->properties["length"] = Value(1.0);
  fpCall->properties["__uses_this_arg__"] = Value(true);
  fpCall->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // args[0] = this (the function to invoke), args[1] = thisArg, args[2+] = call args
    if (args.empty() || !args[0].isFunction()) {
      throw std::runtime_error("TypeError: Function.prototype.call called on non-function");
    }
    auto fn = std::get<std::shared_ptr<Function>>(args[0].data);
    Value thisArg = args.size() > 1 ? args[1] : Value(Undefined{});
    std::vector<Value> callArgs;
    if (args.size() > 2) {
      callArgs.insert(callArgs.end(), args.begin() + 2, args.end());
    }
    if (fn->isNative) {
      // For native functions that use this_arg, prepend thisArg
      auto itUsesThis = fn->properties.find("__uses_this_arg__");
      if (itUsesThis != fn->properties.end() && itUsesThis->second.isBool() && itUsesThis->second.toBool()) {
        std::vector<Value> nativeArgs;
        nativeArgs.push_back(thisArg);
        nativeArgs.insert(nativeArgs.end(), callArgs.begin(), callArgs.end());
        return fn->nativeFunc(nativeArgs);
      }
      return fn->nativeFunc(callArgs);
    }
    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) {
      throw std::runtime_error("TypeError: Interpreter unavailable");
    }
    return interpreter->callForHarness(Value(fn), callArgs, thisArg);
  };
  functionPrototype->properties["call"] = Value(fpCall);
  functionPrototype->properties["__non_enum_call"] = Value(true);

  // Function.prototype.apply
  auto fpApply = std::make_shared<Function>();
  fpApply->isNative = true;
  fpApply->properties["name"] = Value(std::string("apply"));
  fpApply->properties["length"] = Value(2.0);
  fpApply->properties["__uses_this_arg__"] = Value(true);
  fpApply->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isFunction()) {
      throw std::runtime_error("TypeError: Function.prototype.apply called on non-function");
    }
    auto fn = std::get<std::shared_ptr<Function>>(args[0].data);
    Value thisArg = args.size() > 1 ? args[1] : Value(Undefined{});
    std::vector<Value> callArgs;
    if (args.size() > 2 && args[2].isArray()) {
      auto arr = std::get<std::shared_ptr<Array>>(args[2].data);
      callArgs = arr->elements;
    }
    if (fn->isNative) {
      auto itUsesThis = fn->properties.find("__uses_this_arg__");
      if (itUsesThis != fn->properties.end() && itUsesThis->second.isBool() && itUsesThis->second.toBool()) {
        std::vector<Value> nativeArgs;
        nativeArgs.push_back(thisArg);
        nativeArgs.insert(nativeArgs.end(), callArgs.begin(), callArgs.end());
        return fn->nativeFunc(nativeArgs);
      }
      return fn->nativeFunc(callArgs);
    }
    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) {
      throw std::runtime_error("TypeError: Interpreter unavailable");
    }
    return interpreter->callForHarness(Value(fn), callArgs, thisArg);
  };
  functionPrototype->properties["apply"] = Value(fpApply);
  functionPrototype->properties["__non_enum_apply"] = Value(true);

  // Function.prototype.bind
  auto fpBind = std::make_shared<Function>();
  fpBind->isNative = true;
  fpBind->properties["name"] = Value(std::string("bind"));
  fpBind->properties["length"] = Value(1.0);
  fpBind->properties["__uses_this_arg__"] = Value(true);
  fpBind->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // args[0] = this (the function to bind), args[1] = boundThis, args[2+] = bound args
    if (args.empty() || !args[0].isFunction()) {
      throw std::runtime_error("TypeError: Function.prototype.bind called on non-function");
    }
    auto targetFn = std::get<std::shared_ptr<Function>>(args[0].data);
    Value boundThis = args.size() > 1 ? args[1] : Value(Undefined{});
    std::vector<Value> boundArgs;
    if (args.size() > 2) {
      boundArgs.insert(boundArgs.end(), args.begin() + 2, args.end());
    }
    auto boundFn = std::make_shared<Function>();
    boundFn->isNative = true;
    std::string targetName = targetFn->properties.count("name") ? targetFn->properties["name"].toString() : "";
    boundFn->properties["name"] = Value(std::string("bound " + targetName));
    boundFn->nativeFunc = [targetFn, boundThis, boundArgs](const std::vector<Value>& callArgs) -> Value {
      std::vector<Value> finalArgs = boundArgs;
      finalArgs.insert(finalArgs.end(), callArgs.begin(), callArgs.end());
      if (targetFn->isNative) {
        auto itUsesThis = targetFn->properties.find("__uses_this_arg__");
        if (itUsesThis != targetFn->properties.end() && itUsesThis->second.isBool() && itUsesThis->second.toBool()) {
          std::vector<Value> nativeArgs;
          nativeArgs.push_back(boundThis);
          nativeArgs.insert(nativeArgs.end(), finalArgs.begin(), finalArgs.end());
          return targetFn->nativeFunc(nativeArgs);
        }
        return targetFn->nativeFunc(finalArgs);
      }
      Interpreter* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        throw std::runtime_error("TypeError: Interpreter unavailable");
      }
      return interpreter->callForHarness(Value(targetFn), finalArgs, boundThis);
    };
    return Value(boundFn);
  };
  functionPrototype->properties["bind"] = Value(fpBind);
  functionPrototype->properties["__non_enum_bind"] = Value(true);

  functionConstructor->properties["prototype"] = Value(functionPrototype);
  env->define("Function", Value(functionConstructor));
  globalThisObj->properties["Function"] = Value(functionConstructor);

  // ===== Deferred prototype chain setup =====
  // These must happen after all constructors are defined.

  // BigInt.prototype.__proto__ = Object.prototype
  if (auto bigIntCtor = env->get("BigInt"); bigIntCtor && bigIntCtor->isFunction()) {
    auto bigIntFnPtr = std::get<std::shared_ptr<Function>>(bigIntCtor->data);
    auto protoIt = bigIntFnPtr->properties.find("prototype");
    if (protoIt != bigIntFnPtr->properties.end() && protoIt->second.isObject()) {
      auto bigIntProtoPtr = std::get<std::shared_ptr<Object>>(protoIt->second.data);
      bigIntProtoPtr->properties["__proto__"] = Value(objectPrototype);
    }
    // BigInt.__proto__ = Function.prototype
    bigIntFnPtr->properties["__proto__"] = Value(functionPrototype);
  }

  // Promise.prototype.__proto__ = Object.prototype (already set via promisePrototype)
  // Promise.__proto__ = Function.prototype
  if (auto promiseCtor = env->get("Promise"); promiseCtor && promiseCtor->isFunction()) {
    auto promiseFnPtr = std::get<std::shared_ptr<Function>>(promiseCtor->data);
    promiseFnPtr->properties["__proto__"] = Value(functionPrototype);
  }

  // Mark built-in constructors as non-enumerable on globalThis (per ES spec)
  static const char* builtinNames[] = {
    "BigInt", "Object", "Array", "String", "Number", "Boolean", "Symbol",
    "Promise", "RegExp", "Map", "Set", "Proxy", "Reflect", "Error",
    "TypeError", "RangeError", "ReferenceError", "SyntaxError", "URIError",
    "EvalError", "Function", "Date", "Math", "JSON", "console",
    "ArrayBuffer", "DataView", "Int8Array", "Uint8Array", "Uint8ClampedArray",
    "Int16Array", "Uint16Array", "Int32Array", "Uint32Array",
    "Float16Array", "Float32Array", "Float64Array",
    "BigInt64Array", "BigUint64Array", "WeakRef", "FinalizationRegistry",
    "globalThis", "undefined", "NaN", "Infinity",
    "eval", "parseInt", "parseFloat", "isNaN", "isFinite",
    "encodeURIComponent", "decodeURIComponent", "encodeURI", "decodeURI",
    "setTimeout", "clearTimeout", "setInterval", "clearInterval",
    "queueMicrotask", "structuredClone", "btoa", "atob",
    "fetch", "crypto", "WebAssembly", "performance"
  };
  for (const char* name : builtinNames) {
    if (globalThisObj->properties.count(name)) {
      globalThisObj->properties["__non_enum_" + std::string(name)] = Value(true);
    }
  }

  return env;
}

Environment* Environment::getRoot() {
  Environment* current = this;
  while (current->parent_) {
    current = current->parent_.get();
  }
  return current;
}

std::shared_ptr<Object> Environment::getGlobal() const {
  auto globalObj = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // Walk up to the root environment
  const Environment* current = this;
  while (current->parent_) {
    current = current->parent_.get();
  }

  // Add all global bindings to the object
  for (const auto& [name, value] : current->bindings_) {
    globalObj->properties[name] = value;
  }

  return globalObj;
}

}
