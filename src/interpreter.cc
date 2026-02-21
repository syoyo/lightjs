#include "interpreter.h"
#include "module.h"
#include "array_methods.h"
#include "string_methods.h"
#include "unicode.h"
#include "gc.h"
#include "event_loop.h"
#include "symbols.h"
#include "streams.h"
#include <iostream>
#include <cmath>
#include <climits>
#include <sstream>
#include <iomanip>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <limits>

namespace lightjs {

namespace {
int32_t toInt32(double value) {
  if (!std::isfinite(value) || value == 0.0) {
    return 0;
  }

  double intPart = std::trunc(value);
  constexpr double kTwo32 = 4294967296.0;
  double wrapped = std::fmod(intPart, kTwo32);
  if (wrapped < 0) {
    wrapped += kTwo32;
  }
  if (wrapped >= 2147483648.0) {
    wrapped -= kTwo32;
  }
  return static_cast<int32_t>(wrapped);
}

std::string numberToPropertyKey(double value) {
  if (std::isnan(value)) return "NaN";
  if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
  if (value == 0.0) return "0";

  double integral = std::trunc(value);
  if (integral == value) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(0) << value;
    return oss.str();
  }

  std::ostringstream oss;
  oss << std::setprecision(15) << value;
  std::string out = oss.str();
  auto dot = out.find('.');
  if (dot != std::string::npos) {
    while (!out.empty() && out.back() == '0') out.pop_back();
    if (!out.empty() && out.back() == '.') out.pop_back();
  }
  return out;
}

std::string toPropertyKeyString(const Value& value) {
  if (value.isNumber()) {
    return numberToPropertyKey(value.toNumber());
  }
  return value.toString();
}

bool parseArrayIndex(const std::string& key, size_t& index) {
  if (key.empty()) return false;
  if (key.size() > 1 && key[0] == '0') return false;
  for (char c : key) {
    if (c < '0' || c > '9') return false;
  }

  try {
    size_t parsed = std::stoull(key);
    if (parsed == static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
      return false;
    }
    index = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

bool hasUseStrictDirective(const std::vector<StmtPtr>& body) {
  for (const auto& stmt : body) {
    if (!stmt) {
      break;
    }
    auto* exprStmt = std::get_if<ExpressionStmt>(&stmt->node);
    if (!exprStmt || !exprStmt->expression) {
      break;
    }
    auto* str = std::get_if<StringLiteral>(&exprStmt->expression->node);
    if (!str) {
      break;
    }
    if (str->value == "use strict") {
      return true;
    }
  }
  return false;
}
}  // namespace

// Forward declaration for TDZ initialization
static void collectVarHoistNames(const Expression& expr, std::vector<std::string>& names);

Interpreter::Interpreter(std::shared_ptr<Environment> env) : env_(env) {
  setGlobalInterpreter(this);
}

bool Interpreter::hasError() const {
  return flow_.type == ControlFlow::Type::Throw;
}

Value Interpreter::getError() const {
  return flow_.value;
}

void Interpreter::clearError() {
  flow_.type = ControlFlow::Type::None;
  flow_.value = Value(Undefined{});
}

Value Interpreter::callForHarness(const Value& callee,
                                  const std::vector<Value>& args,
                                  const Value& thisValue) {
  return callFunction(callee, args, thisValue);
}

Value Interpreter::constructFromNative(const Value& constructor,
                                       const std::vector<Value>& args) {
  auto task = constructValue(constructor, args);
  Value result;
  LIGHTJS_RUN_TASK(task, result);
  return result;
}

bool Interpreter::isObjectLike(const Value& value) const {
  return value.isObject() || value.isArray() || value.isFunction() || value.isRegex() || value.isProxy() || value.isPromise();
}

std::pair<bool, Value> Interpreter::getPropertyForPrimitive(const Value& receiver, const std::string& key) {
  if (receiver.isObject()) {
    auto current = std::get<std::shared_ptr<Object>>(receiver.data);
    int depth = 0;
    while (current && depth <= 16) {
      depth++;

      std::string getterKey = "__get_" + key;
      auto getterIt = current->properties.find(getterKey);
      if (getterIt != current->properties.end()) {
        if (getterIt->second.isFunction()) {
          return {true, callFunction(getterIt->second, {}, receiver)};
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
  }

  if (receiver.isFunction()) {
    auto fn = std::get<std::shared_ptr<Function>>(receiver.data);
    auto it = fn->properties.find(key);
    if (it != fn->properties.end()) {
      return {true, it->second};
    }
    // Walk prototype chain for functions
    auto protoIt = fn->properties.find("__proto__");
    if (protoIt != fn->properties.end() && protoIt->second.isObject()) {
      auto proto = std::get<std::shared_ptr<Object>>(protoIt->second.data);
      int depth = 0;
      while (proto && depth < 16) {
        auto found = proto->properties.find(key);
        if (found != proto->properties.end()) {
          return {true, found->second};
        }
        auto nextProto = proto->properties.find("__proto__");
        if (nextProto == proto->properties.end() || !nextProto->second.isObject()) break;
        proto = std::get<std::shared_ptr<Object>>(nextProto->second.data);
        depth++;
      }
    }
    return {false, Value(Undefined{})};
  }

  if (receiver.isRegex()) {
    auto regex = std::get<std::shared_ptr<Regex>>(receiver.data);
    std::string getterKey = "__get_" + key;
    auto getterIt = regex->properties.find(getterKey);
    if (getterIt != regex->properties.end()) {
      if (getterIt->second.isFunction()) {
        return {true, callFunction(getterIt->second, {}, receiver)};
      }
      return {true, Value(Undefined{})};
    }
    auto it = regex->properties.find(key);
    if (it != regex->properties.end()) {
      return {true, it->second};
    }
    return {false, Value(Undefined{})};
  }

  if (receiver.isProxy()) {
    auto proxy = std::get<std::shared_ptr<Proxy>>(receiver.data);
    if (proxy->target) {
      return getPropertyForPrimitive(*proxy->target, key);
    }
  }

  return {false, Value(Undefined{})};
}

Value Interpreter::toPrimitiveValue(const Value& input, bool preferString) {
  if (!isObjectLike(input)) {
    return input;
  }

  const std::string& toPrimitiveKey = WellKnownSymbols::toPrimitiveKey();
  auto [hasExotic, exotic] = getPropertyForPrimitive(input, toPrimitiveKey);
  if (hasError()) {
    return Value(Undefined{});
  }
  if (hasExotic && !exotic.isUndefined() && !exotic.isNull()) {
    if (!exotic.isFunction()) {
      throwError(ErrorType::TypeError, "@@toPrimitive is not callable");
      return Value(Undefined{});
    }
    Value hint = Value(std::string(preferString ? "string" : "number"));
    Value result = callFunction(exotic, {hint}, input);
    if (hasError()) {
      return Value(Undefined{});
    }
    if (isObjectLike(result)) {
      throwError(ErrorType::TypeError, "@@toPrimitive must return a primitive");
      return Value(Undefined{});
    }
    return result;
  }

  const char* firstMethod = preferString ? "toString" : "valueOf";
  const char* secondMethod = preferString ? "valueOf" : "toString";
  const char* methods[2] = {firstMethod, secondMethod};

  for (const char* methodName : methods) {
    auto [found, method] = getPropertyForPrimitive(input, methodName);
    if (hasError()) {
      return Value(Undefined{});
    }
    if (found) {
      if (method.isFunction()) {
        Value result = callFunction(method, {}, input);
        if (hasError()) {
          return Value(Undefined{});
        }
        if (!isObjectLike(result)) {
          return result;
        }
      }
      continue;
    }

    if (std::string(methodName) == "toString") {
      if (input.isArray()) {
        auto arr = std::get<std::shared_ptr<Array>>(input.data);
        std::string out;
        for (size_t i = 0; i < arr->elements.size(); i++) {
          if (i > 0) out += ",";
          out += arr->elements[i].toString();
        }
        return Value(out);
      }
      if (input.isObject()) return Value(std::string("[object Object]"));
      if (input.isFunction()) return Value(std::string("[Function]"));
      if (input.isRegex()) return Value(input.toString());
    }
  }

  throwError(ErrorType::TypeError, "Cannot convert object to primitive value");
  return Value(Undefined{});
}

bool Interpreter::checkMemoryLimit(size_t additionalBytes) {
  auto& gc = GarbageCollector::instance();

  if (!gc.checkHeapLimit(additionalBytes)) {
    // Try to free memory first
    gc.collect();

    // Check again after collection
    if (!gc.checkHeapLimit(additionalBytes)) {
      // Format memory statistics for error message
      size_t currentUsage = gc.getCurrentMemoryUsage();
      size_t heapLimit = gc.getHeapLimit();

      std::stringstream ss;
      ss << "JavaScript heap out of memory (";
      ss << (currentUsage / (1024 * 1024)) << " MB used, ";
      ss << (heapLimit / (1024 * 1024)) << " MB limit)";

      throwError(ErrorType::RangeError, ss.str());
      return false;
    }
  }
  return true;
}

Task Interpreter::evaluate(const Program& program) {
  bool previousStrictMode = strictMode_;
  strictMode_ = hasUseStrictDirective(program.body) || program.isModule;

  if (program.isModule) {
    for (const auto& stmt : program.body) {
      if (std::holds_alternative<ImportDeclaration>(stmt->node)) {
        auto task = evaluate(*stmt);
        LIGHTJS_RUN_TASK_VOID(task);
        if (flow_.type != ControlFlow::Type::None) {
          break;
        }
      }
    }
  }

  // Hoisting phase 0: Initialize TDZ for let/const declarations (non-recursive)
  for (const auto& stmt : program.body) {
    if (auto* varDecl = std::get_if<VarDeclaration>(&stmt->node)) {
      if (varDecl->kind == VarDeclaration::Kind::Let ||
          varDecl->kind == VarDeclaration::Kind::Const) {
        for (const auto& declarator : varDecl->declarations) {
          std::vector<std::string> names;
          collectVarHoistNames(*declarator.pattern, names);
          for (const auto& name : names) {
            env_->defineTDZ(name);
          }
        }
      }
    }
  }

  // Hoisting phase 1: Hoist var declarations (recursive scan)
  hoistVarDeclarations(program.body);

  // Hoisting phase 2: Hoist function declarations (top-level only)
  for (const auto& stmt : program.body) {
    if (std::holds_alternative<FunctionDeclaration>(stmt->node)) {
      auto task = evaluate(*stmt);
      LIGHTJS_RUN_TASK_VOID(task);
    }
  }

  Value result = Value(Undefined{});
  for (const auto& stmt : program.body) {
    if (program.isModule && std::holds_alternative<ImportDeclaration>(stmt->node)) {
      continue;
    }
    // Skip function declarations - already hoisted
    if (std::holds_alternative<FunctionDeclaration>(stmt->node)) {
      continue;
    }
    auto task = evaluate(*stmt);
  LIGHTJS_RUN_TASK(task, result);

    if (flow_.type != ControlFlow::Type::None) {
      break;
    }
  }
  strictMode_ = previousStrictMode;
  LIGHTJS_RETURN(result);
}

Task Interpreter::evaluate(const Statement& stmt) {
  // Stack overflow protection
  StackGuard guard(stackDepth_, MAX_STACK_DEPTH);
  if (guard.overflowed()) {
    throwError(ErrorType::RangeError, "Maximum call stack size exceeded");
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  if (auto* node = std::get_if<VarDeclaration>(&stmt.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateVarDecl(*node)));
  } else if (auto* node = std::get_if<FunctionDeclaration>(&stmt.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateFuncDecl(*node)));
  } else if (auto* node = std::get_if<ClassDeclaration>(&stmt.node)) {
    // Create the class directly
    auto cls = std::make_shared<Class>(node->id.name);
    GarbageCollector::instance().reportAllocation(sizeof(Class));
    cls->closure = env_;

    // Handle superclass
    if (node->superClass) {
      auto superTask = evaluate(*node->superClass);
  Value superVal;
  LIGHTJS_RUN_TASK(superTask, superVal);
      if (superVal.isClass()) {
        cls->superClass = std::get<std::shared_ptr<Class>>(superVal.data);
      } else if (superVal.isFunction()) {
        cls->properties["__super_constructor__"] = superVal;
        // Inherit static properties from Function super class
        auto superFunc = std::get<std::shared_ptr<Function>>(superVal.data);
        for (const auto& [key, val] : superFunc->properties) {
          if (key.size() >= 2 && key[0] == '_' && key[1] == '_') continue;
          if (key == "name" || key == "length" || key == "prototype" ||
              key == "caller" || key == "arguments") continue;
          if (cls->properties.find(key) == cls->properties.end()) {
            cls->properties[key] = val;
          }
        }
      }
    }

    // Process methods
    for (const auto& method : node->methods) {
      auto func = std::make_shared<Function>();
      func->isNative = false;
      func->isAsync = method.isAsync;
      func->isStrict = true;  // Class bodies are always strict.
      func->closure = env_;

      for (const auto& param : method.params) {
        FunctionParam funcParam;
        funcParam.name = param.name;
        func->params.push_back(funcParam);
      }

      // Store the body reference
      func->body = std::shared_ptr<void>(
        const_cast<std::vector<StmtPtr>*>(&method.body),
        [](void*){} // No-op deleter
      );
      if (method.kind == MethodDefinition::Kind::Constructor) {
        func->properties["name"] = Value(std::string("constructor"));
      } else {
        func->properties["name"] = Value(method.key.name);
      }
      if (cls->superClass) {
        func->properties["__super_class__"] = Value(cls->superClass);
      } else if (cls->properties.find("__super_constructor__") != cls->properties.end()) {
        func->properties["__super_class__"] = cls->properties["__super_constructor__"];
      } else if (auto objectCtor = env_->get("Object")) {
        func->properties["__super_class__"] = *objectCtor;
      }

      if (method.kind == MethodDefinition::Kind::Constructor) {
        cls->constructor = func;
      } else if (method.isStatic) {
        cls->staticMethods[method.key.name] = func;
      } else if (method.kind == MethodDefinition::Kind::Get) {
        cls->getters[method.key.name] = func;
      } else if (method.kind == MethodDefinition::Kind::Set) {
        cls->setters[method.key.name] = func;
      } else {
        cls->methods[method.key.name] = func;
      }
    }

    Value classVal = Value(cls);
    env_->define(node->id.name, classVal);
    LIGHTJS_RETURN(classVal);
  } else if (auto* node = std::get_if<ReturnStmt>(&stmt.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateReturn(*node)));
  } else if (auto* node = std::get_if<ExpressionStmt>(&stmt.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateExprStmt(*node)));
  } else if (auto* node = std::get_if<BlockStmt>(&stmt.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateBlock(*node)));
  } else if (auto* node = std::get_if<IfStmt>(&stmt.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateIf(*node)));
  } else if (auto* node = std::get_if<WhileStmt>(&stmt.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateWhile(*node)));
  } else if (auto* node = std::get_if<WithStmt>(&stmt.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateWith(*node)));
  } else if (auto* node = std::get_if<DoWhileStmt>(&stmt.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateDoWhile(*node)));
  } else if (auto* node = std::get_if<ForStmt>(&stmt.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateFor(*node)));
  } else if (auto* node = std::get_if<ForInStmt>(&stmt.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateForIn(*node)));
  } else if (auto* node = std::get_if<ForOfStmt>(&stmt.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateForOf(*node)));
  } else if (auto* node = std::get_if<SwitchStmt>(&stmt.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateSwitch(*node)));
  } else if (auto* node = std::get_if<BreakStmt>(&stmt.node)) {
    flow_.type = ControlFlow::Type::Break;
    flow_.label = node->label;
    LIGHTJS_RETURN(Value(Undefined{}));
  } else if (auto* node = std::get_if<ContinueStmt>(&stmt.node)) {
    flow_.type = ControlFlow::Type::Continue;
    flow_.label = node->label;
    LIGHTJS_RETURN(Value(Undefined{}));
  } else if (auto* labelNode = std::get_if<LabelledStmt>(&stmt.node)) {
    // Set pending label so the next iteration statement can consume matching continues
    auto prevLabel = pendingIterationLabel_;
    pendingIterationLabel_ = labelNode->label;
    auto task = evaluate(*labelNode->body);
    Value labelResult;
    LIGHTJS_RUN_TASK(task, labelResult);
    pendingIterationLabel_ = prevLabel;
    // If break targets this label, consume it
    if (flow_.type == ControlFlow::Type::Break && flow_.label == labelNode->label) {
      flow_.type = ControlFlow::Type::None;
      flow_.label.clear();
    }
    LIGHTJS_RETURN(labelResult);
  } else if (auto* node = std::get_if<ThrowStmt>(&stmt.node)) {
    auto task = evaluate(*node->argument);
    LIGHTJS_RUN_TASK_VOID(task);
    if (flow_.type == ControlFlow::Type::Throw) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    flow_.type = ControlFlow::Type::Throw;
    flow_.value = task.result();
    LIGHTJS_RETURN(Value(Undefined{}));
  } else if (auto* node = std::get_if<TryStmt>(&stmt.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateTry(*node)));
  } else if (auto* node = std::get_if<ImportDeclaration>(&stmt.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateImport(*node)));
  } else if (auto* node = std::get_if<ExportNamedDeclaration>(&stmt.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateExportNamed(*node)));
  } else if (auto* node = std::get_if<ExportDefaultDeclaration>(&stmt.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateExportDefault(*node)));
  } else if (auto* node = std::get_if<ExportAllDeclaration>(&stmt.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateExportAll(*node)));
  }
  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::evaluate(const Expression& expr) {
  // Stack overflow protection
  StackGuard guard(stackDepth_, MAX_STACK_DEPTH);
  if (guard.overflowed()) {
    throwError(ErrorType::RangeError, "Maximum call stack size exceeded");
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  if (auto* node = std::get_if<Identifier>(&expr.node)) {
    // Check for temporal dead zone
    if (env_->isTDZ(node->name)) {
      throwError(ErrorType::ReferenceError,
                 formatError("Cannot access '" + node->name + "' before initialization", expr.loc));
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (auto val = env_->get(node->name)) {
      if (val->isModuleBinding()) {
        const auto& binding = std::get<ModuleBinding>(val->data);
        auto module = binding.module.lock();
        if (!module) {
          throwError(ErrorType::ReferenceError,
                     formatError("Cannot access '" + node->name + "' before initialization", expr.loc));
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        auto exportValue = module->getExport(binding.exportName);
        if (!exportValue) {
          throwError(ErrorType::ReferenceError,
                     formatError("Cannot access '" + node->name + "' before initialization", expr.loc));
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        LIGHTJS_RETURN(*exportValue);
      }
      LIGHTJS_RETURN(*val);
    }
    for (auto it = activeNamedExpressionStack_.rbegin();
         it != activeNamedExpressionStack_.rend();
         ++it) {
      const auto& fn = *it;
      auto nameIt = fn->properties.find("name");
      if (nameIt != fn->properties.end() &&
          nameIt->second.isString() &&
          nameIt->second.toString() == node->name) {
        LIGHTJS_RETURN(Value(fn));
      }
    }
    // Throw ReferenceError for undefined variables with line info
    throwError(ErrorType::ReferenceError, formatError("'" + node->name + "' is not defined", expr.loc));
    LIGHTJS_RETURN(Value(Undefined{}));
  } else if (auto* node = std::get_if<NumberLiteral>(&expr.node)) {
    // Use cached value for small integers
    if (SmallIntCache::inRange(node->value)) {
      LIGHTJS_RETURN(SmallIntCache::get(static_cast<int>(node->value)));
    }
    LIGHTJS_RETURN(Value(node->value));
  } else if (auto* node = std::get_if<BigIntLiteral>(&expr.node)) {
    LIGHTJS_RETURN(Value(BigInt(node->value)));
  } else if (auto* node = std::get_if<StringLiteral>(&expr.node)) {
    LIGHTJS_RETURN(Value(node->value));
  } else if (auto* node = std::get_if<TemplateLiteral>(&expr.node)) {
    // Evaluate template literal with interpolation
    std::string result;
    for (size_t i = 0; i < node->quasis.size(); i++) {
      result += node->quasis[i];
      if (i < node->expressions.size()) {
        auto exprTask = evaluate(*node->expressions[i]);
        LIGHTJS_RUN_TASK_VOID(exprTask);
        Value interpolated = exprTask.result();
        if (isObjectLike(interpolated)) {
          interpolated = toPrimitiveValue(interpolated, true);
          if (hasError()) {
            LIGHTJS_RETURN(Value(Undefined{}));
          }
        }
        result += interpolated.toString();
      }
    }
    LIGHTJS_RETURN(Value(result));
  } else if (auto* node = std::get_if<RegexLiteral>(&expr.node)) {
    auto regex = std::make_shared<Regex>(node->pattern, node->flags);
    LIGHTJS_RETURN(Value(regex));
  } else if (auto* node = std::get_if<BoolLiteral>(&expr.node)) {
    LIGHTJS_RETURN(Value(node->value));
  } else if (std::holds_alternative<NullLiteral>(expr.node)) {
    LIGHTJS_RETURN(Value(Null{}));
  } else if (auto* node = std::get_if<BinaryExpr>(&expr.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateBinary(*node)));
  } else if (auto* node = std::get_if<UnaryExpr>(&expr.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateUnary(*node)));
  } else if (auto* node = std::get_if<AssignmentExpr>(&expr.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateAssignment(*node)));
  } else if (auto* node = std::get_if<UpdateExpr>(&expr.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateUpdate(*node)));
  } else if (auto* node = std::get_if<CallExpr>(&expr.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateCall(*node)));
  } else if (auto* node = std::get_if<MemberExpr>(&expr.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateMember(*node)));
  } else if (auto* node = std::get_if<ConditionalExpr>(&expr.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateConditional(*node)));
  } else if (auto* node = std::get_if<SequenceExpr>(&expr.node)) {
    Value last = Value(Undefined{});
    for (const auto& sequenceExpr : node->expressions) {
      if (!sequenceExpr) {
        continue;
      }
      last = LIGHTJS_AWAIT(evaluate(*sequenceExpr));
      if (flow_.type != ControlFlow::Type::None) {
        break;
      }
    }
    LIGHTJS_RETURN(last);
  } else if (auto* node = std::get_if<ArrayExpr>(&expr.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateArray(*node)));
  } else if (auto* node = std::get_if<ObjectExpr>(&expr.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateObject(*node)));
  } else if (auto* node = std::get_if<FunctionExpr>(&expr.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateFunction(*node)));
  } else if (auto* node = std::get_if<AwaitExpr>(&expr.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateAwait(*node)));
  } else if (auto* node = std::get_if<YieldExpr>(&expr.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateYield(*node)));
  } else if (auto* node = std::get_if<NewExpr>(&expr.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateNew(*node)));
  } else if (auto* node = std::get_if<ClassExpr>(&expr.node)) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluateClass(*node)));
  } else if (std::holds_alternative<ThisExpr>(expr.node)) {
    // Look up 'this' in the current environment
    if (auto thisVal = env_->get("this")) {
      LIGHTJS_RETURN(*thisVal);
    }
    LIGHTJS_RETURN(Value(Undefined{}));
  } else if (std::holds_alternative<SuperExpr>(expr.node)) {
    // Look up '__super__' in the current environment (set by class constructor)
    if (auto superVal = env_->get("__super__")) {
      LIGHTJS_RETURN(*superVal);
    }
    throwError(ErrorType::ReferenceError, formatError("'super' keyword is not valid here", expr.loc));
    LIGHTJS_RETURN(Value(Undefined{}));
  } else if (auto* node = std::get_if<MetaProperty>(&expr.node)) {
    if (node->meta == "new" && node->property == "target") {
      if (auto newTarget = env_->get("__new_target__")) {
        LIGHTJS_RETURN(*newTarget);
      }
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    // import.meta - ES2020
    if (node->meta == "meta") {
      if (auto cached = env_->get("__import_meta_object__")) {
        LIGHTJS_RETURN(*cached);
      }

      // Create import.meta object with common properties.
      auto metaObj = std::make_shared<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));

      // import.meta.url - the URL of the current module
      if (auto moduleUrl = env_->get("__module_url__")) {
        metaObj->properties["url"] = *moduleUrl;
      } else {
        metaObj->properties["url"] = Value(std::string(""));
      }

      // import.meta.resolve - function to resolve module specifiers
      auto resolveFn = std::make_shared<Function>();
      resolveFn->isNative = true;
      resolveFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(std::string(""));
        // Simple implementation - just return the specifier as-is
        return Value(args[0].toString());
      };
      metaObj->properties["resolve"] = Value(resolveFn);
      metaObj->properties["__import_meta__"] = Value(true);

      Value metaValue(metaObj);
      env_->define("__import_meta_object__", metaValue);
      LIGHTJS_RETURN(metaValue);
    }
    LIGHTJS_RETURN(Value(Undefined{}));
  }
  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::evaluateBinary(const BinaryExpr& expr) {
  auto leftTask = evaluate(*expr.left);
  Value left;
  LIGHTJS_RUN_TASK(leftTask, left);

  // Check for throw flow from left operand evaluation
  if (flow_.type == ControlFlow::Type::Throw) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  // Short-circuit evaluation for logical operators
  if (expr.op == BinaryExpr::Op::LogicalAnd) {
    if (!left.toBool()) {
      LIGHTJS_RETURN(left);
    }
    auto rTask = evaluate(*expr.right);
    Value rVal;
    LIGHTJS_RUN_TASK(rTask, rVal);
    LIGHTJS_RETURN(rVal);
  }
  if (expr.op == BinaryExpr::Op::LogicalOr) {
    if (left.toBool()) {
      LIGHTJS_RETURN(left);
    }
    auto rTask = evaluate(*expr.right);
    Value rVal;
    LIGHTJS_RUN_TASK(rTask, rVal);
    LIGHTJS_RETURN(rVal);
  }
  if (expr.op == BinaryExpr::Op::NullishCoalescing) {
    if (!left.isNull() && !left.isUndefined()) {
      LIGHTJS_RETURN(left);
    }
    auto rTask = evaluate(*expr.right);
    Value rVal;
    LIGHTJS_RUN_TASK(rTask, rVal);
    LIGHTJS_RETURN(rVal);
  }

  auto rightTask = evaluate(*expr.right);
  Value right;
  LIGHTJS_RUN_TASK(rightTask, right);

  // Check for throw flow from operand evaluation
  if (flow_.type == ControlFlow::Type::Throw) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  // Fast path: both operands are numbers (most common case in loops)
  const bool leftIsNum = left.isNumber();
  const bool rightIsNum = right.isNumber();

  if (leftIsNum && rightIsNum) {
    double l = std::get<double>(left.data);
    double r = std::get<double>(right.data);
    switch (expr.op) {
      case BinaryExpr::Op::Add: LIGHTJS_RETURN(Value(l + r));
      case BinaryExpr::Op::Sub: LIGHTJS_RETURN(Value(l - r));
      case BinaryExpr::Op::Mul: LIGHTJS_RETURN(Value(l * r));
      case BinaryExpr::Op::Div: LIGHTJS_RETURN(Value(l / r));
      case BinaryExpr::Op::Mod: LIGHTJS_RETURN(Value(std::fmod(l, r)));
      case BinaryExpr::Op::BitwiseAnd: LIGHTJS_RETURN(Value(static_cast<double>(toInt32(l) & toInt32(r))));
      case BinaryExpr::Op::BitwiseOr: LIGHTJS_RETURN(Value(static_cast<double>(toInt32(l) | toInt32(r))));
      case BinaryExpr::Op::BitwiseXor: LIGHTJS_RETURN(Value(static_cast<double>(toInt32(l) ^ toInt32(r))));
      case BinaryExpr::Op::Less: LIGHTJS_RETURN(Value(l < r));
      case BinaryExpr::Op::Greater: LIGHTJS_RETURN(Value(l > r));
      case BinaryExpr::Op::LessEqual: LIGHTJS_RETURN(Value(l <= r));
      case BinaryExpr::Op::GreaterEqual: LIGHTJS_RETURN(Value(l >= r));
      case BinaryExpr::Op::Equal:
      case BinaryExpr::Op::StrictEqual: LIGHTJS_RETURN(Value(l == r));
      case BinaryExpr::Op::NotEqual:
      case BinaryExpr::Op::StrictNotEqual: LIGHTJS_RETURN(Value(l != r));
      default: break;  // Fall through for other ops
    }
  }

  switch (expr.op) {
    case BinaryExpr::Op::Add: {
      Value lhs = isObjectLike(left) ? toPrimitiveValue(left, false) : left;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs = isObjectLike(right) ? toPrimitiveValue(right, false) : right;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));

      if (lhs.isString() || rhs.isString()) {
        LIGHTJS_RETURN(Value(lhs.toString() + rhs.toString()));
      }
      if (lhs.isBigInt() && rhs.isBigInt()) {
        LIGHTJS_RETURN(Value(BigInt(lhs.toBigInt() + rhs.toBigInt())));
      }
      if (lhs.isBigInt() != rhs.isBigInt()) {
        throwError(ErrorType::TypeError, "Cannot mix BigInt and other types");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(lhs.toNumber() + rhs.toNumber()));
    }
    case BinaryExpr::Op::Sub: {
      Value lhs = isObjectLike(left) ? toPrimitiveValue(left, false) : left;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs = isObjectLike(right) ? toPrimitiveValue(right, false) : right;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));

      if (lhs.isBigInt() && rhs.isBigInt()) {
        LIGHTJS_RETURN(Value(BigInt(lhs.toBigInt() - rhs.toBigInt())));
      }
      if (lhs.isBigInt() != rhs.isBigInt()) {
        throwError(ErrorType::TypeError, "Cannot mix BigInt and other types");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(lhs.toNumber() - rhs.toNumber()));
    }
    case BinaryExpr::Op::Mul: {
      Value lhs = isObjectLike(left) ? toPrimitiveValue(left, false) : left;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs = isObjectLike(right) ? toPrimitiveValue(right, false) : right;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));

      if (lhs.isBigInt() && rhs.isBigInt()) {
        LIGHTJS_RETURN(Value(BigInt(lhs.toBigInt() * rhs.toBigInt())));
      }
      if (lhs.isBigInt() != rhs.isBigInt()) {
        throwError(ErrorType::TypeError, "Cannot mix BigInt and other types");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(lhs.toNumber() * rhs.toNumber()));
    }
    case BinaryExpr::Op::Div: {
      Value lhs = isObjectLike(left) ? toPrimitiveValue(left, false) : left;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs = isObjectLike(right) ? toPrimitiveValue(right, false) : right;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));

      if (lhs.isBigInt() && rhs.isBigInt()) {
        LIGHTJS_RETURN(Value(BigInt(lhs.toBigInt() / rhs.toBigInt())));
      }
      if (lhs.isBigInt() != rhs.isBigInt()) {
        throwError(ErrorType::TypeError, "Cannot mix BigInt and other types");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(lhs.toNumber() / rhs.toNumber()));
    }
    case BinaryExpr::Op::Mod: {
      Value lhs = isObjectLike(left) ? toPrimitiveValue(left, false) : left;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs = isObjectLike(right) ? toPrimitiveValue(right, false) : right;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));

      if (lhs.isBigInt() && rhs.isBigInt()) {
        LIGHTJS_RETURN(Value(BigInt(lhs.toBigInt() % rhs.toBigInt())));
      }
      if (lhs.isBigInt() != rhs.isBigInt()) {
        throwError(ErrorType::TypeError, "Cannot mix BigInt and other types");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(std::fmod(lhs.toNumber(), rhs.toNumber())));
    }
    case BinaryExpr::Op::BitwiseAnd:
      if (left.isBigInt() && right.isBigInt()) {
        LIGHTJS_RETURN(Value(BigInt(left.toBigInt() & right.toBigInt())));
      }
      if (left.isBigInt() != right.isBigInt()) {
        throwError(ErrorType::TypeError, "Cannot mix BigInt and other types in bitwise operations");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(static_cast<double>(toInt32(left.toNumber()) & toInt32(right.toNumber()))));
    case BinaryExpr::Op::BitwiseOr:
      if (left.isBigInt() && right.isBigInt()) {
        LIGHTJS_RETURN(Value(BigInt(left.toBigInt() | right.toBigInt())));
      }
      if (left.isBigInt() != right.isBigInt()) {
        throwError(ErrorType::TypeError, "Cannot mix BigInt and other types in bitwise operations");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(static_cast<double>(toInt32(left.toNumber()) | toInt32(right.toNumber()))));
    case BinaryExpr::Op::BitwiseXor:
      if (left.isBigInt() && right.isBigInt()) {
        LIGHTJS_RETURN(Value(BigInt(left.toBigInt() ^ right.toBigInt())));
      }
      if (left.isBigInt() != right.isBigInt()) {
        throwError(ErrorType::TypeError, "Cannot mix BigInt and other types in bitwise operations");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(static_cast<double>(toInt32(left.toNumber()) ^ toInt32(right.toNumber()))));
    case BinaryExpr::Op::Exp: {
      Value lhs = isObjectLike(left) ? toPrimitiveValue(left, false) : left;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs = isObjectLike(right) ? toPrimitiveValue(right, false) : right;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));

      if (lhs.isBigInt() && rhs.isBigInt()) {
        // BigInt exponentiation
        int64_t base = lhs.toBigInt();
        int64_t exp = rhs.toBigInt();
        if (exp < 0) {
          LIGHTJS_RETURN(Value(0.0));  // Negative exponents for BigInt return 0
        }
        int64_t result = 1;
        for (int64_t i = 0; i < exp; ++i) {
          result *= base;
        }
        LIGHTJS_RETURN(Value(BigInt(result)));
      }
      if (lhs.isBigInt() != rhs.isBigInt()) {
        throwError(ErrorType::TypeError, "Cannot mix BigInt and other types");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(std::pow(lhs.toNumber(), rhs.toNumber())));
    }
    case BinaryExpr::Op::Less: {
      Value lhs = isObjectLike(left) ? toPrimitiveValue(left, false) : left;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs = isObjectLike(right) ? toPrimitiveValue(right, false) : right;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      if (lhs.isBigInt() && rhs.isBigInt()) {
        LIGHTJS_RETURN(Value(lhs.toBigInt() < rhs.toBigInt()));
      }
      LIGHTJS_RETURN(Value(lhs.toNumber() < rhs.toNumber()));
    }
    case BinaryExpr::Op::Greater: {
      Value lhs = isObjectLike(left) ? toPrimitiveValue(left, false) : left;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs = isObjectLike(right) ? toPrimitiveValue(right, false) : right;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      if (lhs.isBigInt() && rhs.isBigInt()) {
        LIGHTJS_RETURN(Value(lhs.toBigInt() > rhs.toBigInt()));
      }
      LIGHTJS_RETURN(Value(lhs.toNumber() > rhs.toNumber()));
    }
    case BinaryExpr::Op::LessEqual: {
      Value lhs = isObjectLike(left) ? toPrimitiveValue(left, false) : left;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs = isObjectLike(right) ? toPrimitiveValue(right, false) : right;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      if (lhs.isBigInt() && rhs.isBigInt()) {
        LIGHTJS_RETURN(Value(lhs.toBigInt() <= rhs.toBigInt()));
      }
      LIGHTJS_RETURN(Value(lhs.toNumber() <= rhs.toNumber()));
    }
    case BinaryExpr::Op::GreaterEqual: {
      Value lhs = isObjectLike(left) ? toPrimitiveValue(left, false) : left;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs = isObjectLike(right) ? toPrimitiveValue(right, false) : right;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      if (lhs.isBigInt() && rhs.isBigInt()) {
        LIGHTJS_RETURN(Value(lhs.toBigInt() >= rhs.toBigInt()));
      }
      LIGHTJS_RETURN(Value(lhs.toNumber() >= rhs.toNumber()));
    }
    case BinaryExpr::Op::Equal: {
      Value lhs = isObjectLike(left) ? toPrimitiveValue(left, false) : left;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs = isObjectLike(right) ? toPrimitiveValue(right, false) : right;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      if (lhs.isBigInt() && rhs.isBigInt()) {
        LIGHTJS_RETURN(Value(lhs.toBigInt() == rhs.toBigInt()));
      }
      LIGHTJS_RETURN(Value(lhs.toNumber() == rhs.toNumber()));
    }
    case BinaryExpr::Op::NotEqual: {
      Value lhs = isObjectLike(left) ? toPrimitiveValue(left, false) : left;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      Value rhs = isObjectLike(right) ? toPrimitiveValue(right, false) : right;
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
      if (lhs.isBigInt() && rhs.isBigInt()) {
        LIGHTJS_RETURN(Value(lhs.toBigInt() != rhs.toBigInt()));
      }
      LIGHTJS_RETURN(Value(lhs.toNumber() != rhs.toNumber()));
    }
    case BinaryExpr::Op::StrictEqual: {
      // Strict equality requires same type
      if (left.data.index() != right.data.index()) {
        LIGHTJS_RETURN(Value(false));
      }

      if (left.isSymbol() && right.isSymbol()) {
        auto& lsym = std::get<Symbol>(left.data);
        auto& rsym = std::get<Symbol>(right.data);
        LIGHTJS_RETURN(Value(lsym.id == rsym.id));
      }

      if (left.isBigInt() && right.isBigInt()) {
        LIGHTJS_RETURN(Value(left.toBigInt() == right.toBigInt()));
      }

      if (left.isNumber() && right.isNumber()) {
        LIGHTJS_RETURN(Value(left.toNumber() == right.toNumber()));
      }

      if (left.isString() && right.isString()) {
        LIGHTJS_RETURN(Value(std::get<std::string>(left.data) == std::get<std::string>(right.data)));
      }

      if (left.isBool() && right.isBool()) {
        LIGHTJS_RETURN(Value(std::get<bool>(left.data) == std::get<bool>(right.data)));
      }

      if ((left.isNull() && right.isNull()) || (left.isUndefined() && right.isUndefined())) {
        LIGHTJS_RETURN(Value(true));
      }

      if (left.isObject() && right.isObject()) {
        LIGHTJS_RETURN(Value(std::get<std::shared_ptr<Object>>(left.data).get() ==
                             std::get<std::shared_ptr<Object>>(right.data).get()));
      }
      if (left.isArray() && right.isArray()) {
        LIGHTJS_RETURN(Value(std::get<std::shared_ptr<Array>>(left.data).get() ==
                             std::get<std::shared_ptr<Array>>(right.data).get()));
      }
      if (left.isFunction() && right.isFunction()) {
        LIGHTJS_RETURN(Value(std::get<std::shared_ptr<Function>>(left.data).get() ==
                             std::get<std::shared_ptr<Function>>(right.data).get()));
      }
      if (left.isTypedArray() && right.isTypedArray()) {
        LIGHTJS_RETURN(Value(std::get<std::shared_ptr<TypedArray>>(left.data).get() ==
                             std::get<std::shared_ptr<TypedArray>>(right.data).get()));
      }
      if (left.isPromise() && right.isPromise()) {
        LIGHTJS_RETURN(Value(std::get<std::shared_ptr<Promise>>(left.data).get() ==
                             std::get<std::shared_ptr<Promise>>(right.data).get()));
      }
      if (left.isRegex() && right.isRegex()) {
        LIGHTJS_RETURN(Value(std::get<std::shared_ptr<Regex>>(left.data).get() ==
                             std::get<std::shared_ptr<Regex>>(right.data).get()));
      }
      if (left.isMap() && right.isMap()) {
        LIGHTJS_RETURN(Value(std::get<std::shared_ptr<Map>>(left.data).get() ==
                             std::get<std::shared_ptr<Map>>(right.data).get()));
      }
      if (left.isSet() && right.isSet()) {
        LIGHTJS_RETURN(Value(std::get<std::shared_ptr<Set>>(left.data).get() ==
                             std::get<std::shared_ptr<Set>>(right.data).get()));
      }
      if (left.isError() && right.isError()) {
        LIGHTJS_RETURN(Value(std::get<std::shared_ptr<Error>>(left.data).get() ==
                             std::get<std::shared_ptr<Error>>(right.data).get()));
      }
      if (left.isGenerator() && right.isGenerator()) {
        LIGHTJS_RETURN(Value(std::get<std::shared_ptr<Generator>>(left.data).get() ==
                             std::get<std::shared_ptr<Generator>>(right.data).get()));
      }
      if (left.isProxy() && right.isProxy()) {
        LIGHTJS_RETURN(Value(std::get<std::shared_ptr<Proxy>>(left.data).get() ==
                             std::get<std::shared_ptr<Proxy>>(right.data).get()));
      }
      if (left.isWeakMap() && right.isWeakMap()) {
        LIGHTJS_RETURN(Value(std::get<std::shared_ptr<WeakMap>>(left.data).get() ==
                             std::get<std::shared_ptr<WeakMap>>(right.data).get()));
      }
      if (left.isWeakSet() && right.isWeakSet()) {
        LIGHTJS_RETURN(Value(std::get<std::shared_ptr<WeakSet>>(left.data).get() ==
                             std::get<std::shared_ptr<WeakSet>>(right.data).get()));
      }
      if (left.isArrayBuffer() && right.isArrayBuffer()) {
        LIGHTJS_RETURN(Value(std::get<std::shared_ptr<ArrayBuffer>>(left.data).get() ==
                             std::get<std::shared_ptr<ArrayBuffer>>(right.data).get()));
      }
      if (left.isDataView() && right.isDataView()) {
        LIGHTJS_RETURN(Value(std::get<std::shared_ptr<DataView>>(left.data).get() ==
                             std::get<std::shared_ptr<DataView>>(right.data).get()));
      }
      if (left.isClass() && right.isClass()) {
        LIGHTJS_RETURN(Value(std::get<std::shared_ptr<Class>>(left.data).get() ==
                             std::get<std::shared_ptr<Class>>(right.data).get()));
      }
      if (left.isWasmInstance() && right.isWasmInstance()) {
        LIGHTJS_RETURN(Value(std::get<std::shared_ptr<WasmInstanceJS>>(left.data).get() ==
                             std::get<std::shared_ptr<WasmInstanceJS>>(right.data).get()));
      }
      if (left.isWasmMemory() && right.isWasmMemory()) {
        LIGHTJS_RETURN(Value(std::get<std::shared_ptr<WasmMemoryJS>>(left.data).get() ==
                             std::get<std::shared_ptr<WasmMemoryJS>>(right.data).get()));
      }
      if (left.isReadableStream() && right.isReadableStream()) {
        LIGHTJS_RETURN(Value(std::get<std::shared_ptr<ReadableStream>>(left.data).get() ==
                             std::get<std::shared_ptr<ReadableStream>>(right.data).get()));
      }
      if (left.isWritableStream() && right.isWritableStream()) {
        LIGHTJS_RETURN(Value(std::get<std::shared_ptr<WritableStream>>(left.data).get() ==
                             std::get<std::shared_ptr<WritableStream>>(right.data).get()));
      }
      if (left.isTransformStream() && right.isTransformStream()) {
        LIGHTJS_RETURN(Value(std::get<std::shared_ptr<TransformStream>>(left.data).get() ==
                             std::get<std::shared_ptr<TransformStream>>(right.data).get()));
      }

      LIGHTJS_RETURN(Value(false));
    }
    case BinaryExpr::Op::StrictNotEqual: {
      // Reuse StrictEqual logic
      if (left.data.index() != right.data.index()) {
        LIGHTJS_RETURN(Value(true));
      }

      if (left.isSymbol() && right.isSymbol()) {
        auto& lsym = std::get<Symbol>(left.data);
        auto& rsym = std::get<Symbol>(right.data);
        LIGHTJS_RETURN(Value(lsym.id != rsym.id));
      }

      if (left.isBigInt() && right.isBigInt()) {
        LIGHTJS_RETURN(Value(left.toBigInt() != right.toBigInt()));
      }

      if (left.isNumber() && right.isNumber()) {
        LIGHTJS_RETURN(Value(left.toNumber() != right.toNumber()));
      }

      if (left.isString() && right.isString()) {
        LIGHTJS_RETURN(Value(std::get<std::string>(left.data) != std::get<std::string>(right.data)));
      }

      if (left.isBool() && right.isBool()) {
        LIGHTJS_RETURN(Value(std::get<bool>(left.data) != std::get<bool>(right.data)));
      }

      if ((left.isNull() && right.isNull()) || (left.isUndefined() && right.isUndefined())) {
        LIGHTJS_RETURN(Value(false));
      }

      // For all reference types, strict inequality is pointer inequality.
      auto equalByPointer = [&](const Value& a, const Value& b) -> bool {
        if (a.isObject() && b.isObject()) {
          return std::get<std::shared_ptr<Object>>(a.data).get() ==
                 std::get<std::shared_ptr<Object>>(b.data).get();
        }
        if (a.isArray() && b.isArray()) {
          return std::get<std::shared_ptr<Array>>(a.data).get() ==
                 std::get<std::shared_ptr<Array>>(b.data).get();
        }
        if (a.isFunction() && b.isFunction()) {
          return std::get<std::shared_ptr<Function>>(a.data).get() ==
                 std::get<std::shared_ptr<Function>>(b.data).get();
        }
        if (a.isTypedArray() && b.isTypedArray()) {
          return std::get<std::shared_ptr<TypedArray>>(a.data).get() ==
                 std::get<std::shared_ptr<TypedArray>>(b.data).get();
        }
        if (a.isPromise() && b.isPromise()) {
          return std::get<std::shared_ptr<Promise>>(a.data).get() ==
                 std::get<std::shared_ptr<Promise>>(b.data).get();
        }
        if (a.isRegex() && b.isRegex()) {
          return std::get<std::shared_ptr<Regex>>(a.data).get() ==
                 std::get<std::shared_ptr<Regex>>(b.data).get();
        }
        if (a.isMap() && b.isMap()) {
          return std::get<std::shared_ptr<Map>>(a.data).get() ==
                 std::get<std::shared_ptr<Map>>(b.data).get();
        }
        if (a.isSet() && b.isSet()) {
          return std::get<std::shared_ptr<Set>>(a.data).get() ==
                 std::get<std::shared_ptr<Set>>(b.data).get();
        }
        if (a.isError() && b.isError()) {
          return std::get<std::shared_ptr<Error>>(a.data).get() ==
                 std::get<std::shared_ptr<Error>>(b.data).get();
        }
        if (a.isGenerator() && b.isGenerator()) {
          return std::get<std::shared_ptr<Generator>>(a.data).get() ==
                 std::get<std::shared_ptr<Generator>>(b.data).get();
        }
        if (a.isProxy() && b.isProxy()) {
          return std::get<std::shared_ptr<Proxy>>(a.data).get() ==
                 std::get<std::shared_ptr<Proxy>>(b.data).get();
        }
        if (a.isWeakMap() && b.isWeakMap()) {
          return std::get<std::shared_ptr<WeakMap>>(a.data).get() ==
                 std::get<std::shared_ptr<WeakMap>>(b.data).get();
        }
        if (a.isWeakSet() && b.isWeakSet()) {
          return std::get<std::shared_ptr<WeakSet>>(a.data).get() ==
                 std::get<std::shared_ptr<WeakSet>>(b.data).get();
        }
        if (a.isArrayBuffer() && b.isArrayBuffer()) {
          return std::get<std::shared_ptr<ArrayBuffer>>(a.data).get() ==
                 std::get<std::shared_ptr<ArrayBuffer>>(b.data).get();
        }
        if (a.isDataView() && b.isDataView()) {
          return std::get<std::shared_ptr<DataView>>(a.data).get() ==
                 std::get<std::shared_ptr<DataView>>(b.data).get();
        }
        if (a.isClass() && b.isClass()) {
          return std::get<std::shared_ptr<Class>>(a.data).get() ==
                 std::get<std::shared_ptr<Class>>(b.data).get();
        }
        if (a.isWasmInstance() && b.isWasmInstance()) {
          return std::get<std::shared_ptr<WasmInstanceJS>>(a.data).get() ==
                 std::get<std::shared_ptr<WasmInstanceJS>>(b.data).get();
        }
        if (a.isWasmMemory() && b.isWasmMemory()) {
          return std::get<std::shared_ptr<WasmMemoryJS>>(a.data).get() ==
                 std::get<std::shared_ptr<WasmMemoryJS>>(b.data).get();
        }
        if (a.isReadableStream() && b.isReadableStream()) {
          return std::get<std::shared_ptr<ReadableStream>>(a.data).get() ==
                 std::get<std::shared_ptr<ReadableStream>>(b.data).get();
        }
        if (a.isWritableStream() && b.isWritableStream()) {
          return std::get<std::shared_ptr<WritableStream>>(a.data).get() ==
                 std::get<std::shared_ptr<WritableStream>>(b.data).get();
        }
        if (a.isTransformStream() && b.isTransformStream()) {
          return std::get<std::shared_ptr<TransformStream>>(a.data).get() ==
                 std::get<std::shared_ptr<TransformStream>>(b.data).get();
        }
        return false;
      };
      LIGHTJS_RETURN(Value(!equalByPointer(left, right)));
    }
    // LogicalAnd, LogicalOr, and NullishCoalescing are handled above
    // with short-circuit evaluation (right side not evaluated if not needed)
    case BinaryExpr::Op::In: {
      // 'prop in obj' - check if property exists in object
      std::string propName = toPropertyKeyString(left);

      // Handle Proxy has trap
      if (right.isProxy()) {
        auto proxyPtr = std::get<std::shared_ptr<Proxy>>(right.data);
        if (proxyPtr->handler && proxyPtr->handler->isObject()) {
          auto handlerObj = std::get<std::shared_ptr<Object>>(proxyPtr->handler->data);
          auto trapIt = handlerObj->properties.find("has");
          if (trapIt != handlerObj->properties.end() && trapIt->second.isFunction()) {
            auto trap = std::get<std::shared_ptr<Function>>(trapIt->second.data);
            if (trap->isNative) {
              std::vector<Value> trapArgs = {*proxyPtr->target, Value(propName)};
              LIGHTJS_RETURN(trap->nativeFunc(trapArgs));
            }
          }
        }
        // Fall through to check target
        if (proxyPtr->target && proxyPtr->target->isObject()) {
          auto targetObj = std::get<std::shared_ptr<Object>>(proxyPtr->target->data);
          LIGHTJS_RETURN(Value(targetObj->properties.find(propName) != targetObj->properties.end()));
        }
        LIGHTJS_RETURN(Value(false));
      }

      // Helper lambda to walk prototype chain for 'in' operator
      auto hasPropertyInChain = [&](const std::unordered_map<std::string, Value>& props) -> bool {
        if (props.find(propName) != props.end()) return true;
        auto protoIt = props.find("__proto__");
        if (protoIt != props.end() && protoIt->second.isObject()) {
          auto proto = std::get<std::shared_ptr<Object>>(protoIt->second.data);
          int depth = 0;
          while (proto && depth < 50) {
            if (proto->properties.find(propName) != proto->properties.end()) return true;
            auto nextProto = proto->properties.find("__proto__");
            if (nextProto == proto->properties.end() || !nextProto->second.isObject()) break;
            proto = std::get<std::shared_ptr<Object>>(nextProto->second.data);
            depth++;
          }
        }
        return false;
      };

      if (right.isObject()) {
        auto objPtr = std::get<std::shared_ptr<Object>>(right.data);
        LIGHTJS_RETURN(Value(hasPropertyInChain(objPtr->properties)));
      }

      if (right.isArray()) {
        auto arrPtr = std::get<std::shared_ptr<Array>>(right.data);
        size_t idx = 0;
        if (parseArrayIndex(propName, idx)) {
          LIGHTJS_RETURN(Value(idx < arrPtr->elements.size()));
        }
        if (propName == "length") LIGHTJS_RETURN(Value(true));
        LIGHTJS_RETURN(Value(hasPropertyInChain(arrPtr->properties)));
      }

      if (right.isFunction()) {
        auto fnPtr = std::get<std::shared_ptr<Function>>(right.data);
        LIGHTJS_RETURN(Value(hasPropertyInChain(fnPtr->properties)));
      }

      // 'in' on primitives returns false
      LIGHTJS_RETURN(Value(false));
    }
    case BinaryExpr::Op::Instanceof: {
      // Per spec: if RHS is not callable, throw TypeError
      // Objects that are not functions/classes don't have [[HasInstance]]
      if (!right.isFunction() && !right.isClass()) {
        // Check for callable objects (e.g., Proxy with apply trap)
        bool isCallable = false;
        if (right.isObject()) {
          auto obj = std::get<std::shared_ptr<Object>>(right.data);
          auto callableIt = obj->properties.find("__callable_object__");
          if (callableIt != obj->properties.end() &&
              callableIt->second.isBool() && callableIt->second.toBool()) {
            isCallable = true;
          }
        }
        if (!isCallable) {
          throwError(ErrorType::TypeError, "Right-hand side of instanceof is not callable");
          LIGHTJS_RETURN(Value(false));
        }
      }
      // Per spec: if LHS is a primitive (not object-like), return false
      if (!left.isObject() && !left.isArray() && !left.isFunction() &&
          !left.isRegex() && !left.isPromise() && !left.isError() &&
          !left.isClass() && !left.isProxy()) {
        LIGHTJS_RETURN(Value(false));
      }
      auto unwrapConstructor = [&](const Value& ctor) -> Value {
        if (ctor.isObject()) {
          auto obj = std::get<std::shared_ptr<Object>>(ctor.data);
          auto callableIt = obj->properties.find("__callable_object__");
          if (callableIt != obj->properties.end() &&
              callableIt->second.isBool() && callableIt->second.toBool()) {
            auto ctorIt = obj->properties.find("constructor");
            if (ctorIt != obj->properties.end() &&
                (ctorIt->second.isFunction() || ctorIt->second.isClass())) {
              return ctorIt->second;
            }
          }
        }
        return ctor;
      };

      auto sameCtor = [&](const Value& candidate, const Value& ctor) -> bool {
        if (candidate.data.index() != ctor.data.index()) {
          return false;
        }
        if (candidate.isFunction()) {
          return std::get<std::shared_ptr<Function>>(candidate.data) ==
                 std::get<std::shared_ptr<Function>>(ctor.data);
        }
        if (candidate.isClass()) {
          return std::get<std::shared_ptr<Class>>(candidate.data) ==
                 std::get<std::shared_ptr<Class>>(ctor.data);
        }
        return false;
      };

      auto matchesConstructor = [&](const Value& instance, const Value& ctor) -> bool {
        if (instance.isObject()) {
          auto obj = std::get<std::shared_ptr<Object>>(instance.data);
          auto it = obj->properties.find("__constructor__");
          return it != obj->properties.end() && sameCtor(it->second, ctor);
        }
        if (instance.isArray()) {
          auto arr = std::get<std::shared_ptr<Array>>(instance.data);
          auto it = arr->properties.find("__constructor__");
          return it != arr->properties.end() && sameCtor(it->second, ctor);
        }
        if (instance.isFunction()) {
          auto fn = std::get<std::shared_ptr<Function>>(instance.data);
          auto it = fn->properties.find("__constructor__");
          return it != fn->properties.end() && sameCtor(it->second, ctor);
        }
        if (instance.isRegex()) {
          auto regex = std::get<std::shared_ptr<Regex>>(instance.data);
          auto it = regex->properties.find("__constructor__");
          return it != regex->properties.end() && sameCtor(it->second, ctor);
        }
        if (instance.isPromise()) {
          auto promise = std::get<std::shared_ptr<Promise>>(instance.data);
          auto it = promise->properties.find("__constructor__");
          return it != promise->properties.end() && sameCtor(it->second, ctor);
        }
        return false;
      };

      Value ctorValue = unwrapConstructor(right);

      if (left.isError() && ctorValue.isFunction()) {
        auto ctor = std::get<std::shared_ptr<Function>>(ctorValue.data);
        auto tagIt = ctor->properties.find("__error_type__");
        if (tagIt != ctor->properties.end() && tagIt->second.isNumber()) {
          auto err = std::get<std::shared_ptr<Error>>(left.data);
          int expected = static_cast<int>(std::get<double>(tagIt->second.data));
          // Exact match (e.g., new TypeError instanceof TypeError)
          if (static_cast<int>(err->type) == expected) {
            LIGHTJS_RETURN(Value(true));
          }
          // Base Error constructor matches all error types (inheritance)
          if (expected == static_cast<int>(ErrorType::Error)) {
            LIGHTJS_RETURN(Value(true));
          }
          LIGHTJS_RETURN(Value(false));
        }
      }

      // OrdinaryHasInstance: walk the prototype chain
      // Get the constructor's .prototype property
      auto getCtorPrototype = [&](const Value& ctor) -> std::shared_ptr<Object> {
        std::unordered_map<std::string, Value>* props = nullptr;
        if (ctor.isFunction()) {
          props = &std::get<std::shared_ptr<Function>>(ctor.data)->properties;
        } else if (ctor.isObject()) {
          props = &std::get<std::shared_ptr<Object>>(ctor.data)->properties;
        } else if (ctor.isClass()) {
          auto cls = std::get<std::shared_ptr<Class>>(ctor.data);
          if (cls->constructor) {
            props = &cls->constructor->properties;
          }
        }
        if (props) {
          auto protoIt = props->find("prototype");
          if (protoIt != props->end() && protoIt->second.isObject()) {
            return std::get<std::shared_ptr<Object>>(protoIt->second.data);
          }
        }
        return nullptr;
      };

      // Use the original RHS (right) for prototype chain walk, not the unwrapped ctorValue
      // This is important when RHS is a callable object like Function.prototype
      auto ctorProto = getCtorPrototype(right);
      if (!ctorProto) {
        // Check if prototype property exists but is not an object → TypeError
        std::unordered_map<std::string, Value>* checkProps = nullptr;
        if (right.isFunction()) {
          checkProps = &std::get<std::shared_ptr<Function>>(right.data)->properties;
        } else if (right.isObject()) {
          checkProps = &std::get<std::shared_ptr<Object>>(right.data)->properties;
        }
        if (checkProps) {
          auto protoIt = checkProps->find("prototype");
          if (protoIt != checkProps->end() && !protoIt->second.isObject()) {
            throwError(ErrorType::TypeError, "Function has non-object prototype in instanceof check");
            LIGHTJS_RETURN(Value(false));
          }
        }
      }
      if (ctorProto) {
        // Get the instance's __proto__ chain
        auto getProto = [](const Value& val) -> std::shared_ptr<Object> {
          std::unordered_map<std::string, Value>* props = nullptr;
          if (val.isObject()) {
            props = &std::get<std::shared_ptr<Object>>(val.data)->properties;
          } else if (val.isArray()) {
            props = &std::get<std::shared_ptr<Array>>(val.data)->properties;
          } else if (val.isFunction()) {
            props = &std::get<std::shared_ptr<Function>>(val.data)->properties;
          } else if (val.isRegex()) {
            props = &std::get<std::shared_ptr<Regex>>(val.data)->properties;
          } else if (val.isPromise()) {
            props = &std::get<std::shared_ptr<Promise>>(val.data)->properties;
          }
          if (props) {
            auto it = props->find("__proto__");
            if (it != props->end() && it->second.isObject()) {
              return std::get<std::shared_ptr<Object>>(it->second.data);
            }
          }
          return nullptr;
        };

        auto proto = getProto(left);
        int depth = 0;
        while (proto && depth < 100) {
          if (proto == ctorProto) {
            LIGHTJS_RETURN(Value(true));
          }
          auto protoIt = proto->properties.find("__proto__");
          if (protoIt == proto->properties.end() || !protoIt->second.isObject()) {
            break;
          }
          proto = std::get<std::shared_ptr<Object>>(protoIt->second.data);
          depth++;
        }
      }

      // Fallback: check __constructor__ tag (for objects without proper __proto__ chain)
      if ((ctorValue.isFunction() || ctorValue.isClass()) && matchesConstructor(left, ctorValue)) {
        LIGHTJS_RETURN(Value(true));
      }

      // Built-in type checks for instanceof when __proto__ chain not available
      if (ctorValue.isFunction()) {
        auto ctor = std::get<std::shared_ptr<Function>>(ctorValue.data);
        auto nameIt = ctor->properties.find("name");
        if (nameIt != ctor->properties.end() && nameIt->second.isString()) {
          std::string ctorName = nameIt->second.toString();
          if (ctorName == "Object") {
            // Any object, array, function, regex is instanceof Object
            if (left.isObject()) {
              auto obj = std::get<std::shared_ptr<Object>>(left.data);
              if (obj->isModuleNamespace) {
                LIGHTJS_RETURN(Value(false));
              }
            }
            if (left.isObject() || left.isArray() || left.isFunction() ||
                left.isRegex() || left.isPromise() || left.isError()) {
              LIGHTJS_RETURN(Value(true));
            }
          } else if (ctorName == "Array") {
            if (left.isArray()) {
              LIGHTJS_RETURN(Value(true));
            }
          } else if (ctorName == "Function") {
            if (left.isFunction()) {
              LIGHTJS_RETURN(Value(true));
            }
          } else if (ctorName == "RegExp") {
            if (left.isRegex()) {
              LIGHTJS_RETURN(Value(true));
            }
          } else if (ctorName == "Promise") {
            if (left.isPromise()) {
              LIGHTJS_RETURN(Value(true));
            }
          }
        }
      }
      LIGHTJS_RETURN(Value(false));
    }
  }

  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::evaluateUnary(const UnaryExpr& expr) {
  // Handle delete operator specially - it needs direct access to the member expression
  if (expr.op == UnaryExpr::Op::Delete) {
    // delete only works on member expressions
    if (auto* member = std::get_if<MemberExpr>(&expr.argument->node)) {
      auto objTask = evaluate(*member->object);
      Value obj;
      LIGHTJS_RUN_TASK(objTask, obj);

      std::string propName;
      if (member->computed) {
        auto propTask = evaluate(*member->property);
        LIGHTJS_RUN_TASK_VOID(propTask);
        propName = toPropertyKeyString(propTask.result());
      } else {
        if (auto* id = std::get_if<Identifier>(&member->property->node)) {
          propName = id->name;
        }
      }

      // Handle Proxy deleteProperty trap
      if (obj.isProxy()) {
        auto proxyPtr = std::get<std::shared_ptr<Proxy>>(obj.data);
        if (proxyPtr->handler && proxyPtr->handler->isObject()) {
          auto handlerObj = std::get<std::shared_ptr<Object>>(proxyPtr->handler->data);
          auto trapIt = handlerObj->properties.find("deleteProperty");
          if (trapIt != handlerObj->properties.end() && trapIt->second.isFunction()) {
            auto trap = std::get<std::shared_ptr<Function>>(trapIt->second.data);
            if (trap->isNative) {
              std::vector<Value> trapArgs = {*proxyPtr->target, Value(propName)};
              LIGHTJS_RETURN(trap->nativeFunc(trapArgs));
            }
          }
        }
        // Fall through to delete from target
        if (proxyPtr->target && proxyPtr->target->isObject()) {
          auto targetObj = std::get<std::shared_ptr<Object>>(proxyPtr->target->data);
          bool deleted = false;
          deleted = targetObj->properties.erase(propName) > 0 || deleted;
          deleted = targetObj->properties.erase("__get_" + propName) > 0 || deleted;
          deleted = targetObj->properties.erase("__set_" + propName) > 0 || deleted;
          if (deleted && targetObj->shape) {
            targetObj->shape = nullptr; // Invalidate shape on delete
          }
          LIGHTJS_RETURN(Value(true));
        }
        LIGHTJS_RETURN(Value(false));
      }

      // Delete on function properties
      if (obj.isFunction()) {
        auto fnPtr = std::get<std::shared_ptr<Function>>(obj.data);
        // name, length are non-configurable in some contexts; prototype is non-configurable
        if (propName == "prototype") {
          LIGHTJS_RETURN(Value(false));
        }
        // name, length are configurable per spec - allow delete
        if (fnPtr->properties.count("__non_configurable_" + propName)) {
          LIGHTJS_RETURN(Value(false));
        }
        fnPtr->properties.erase(propName);
        LIGHTJS_RETURN(Value(true));
      }

      if (obj.isObject()) {
        auto objPtr = std::get<std::shared_ptr<Object>>(obj.data);
        if (objPtr->isModuleNamespace) {
          const std::string& toStringTagKey = WellKnownSymbols::toStringTagKey();
          bool isExport = std::find(
            objPtr->moduleExportNames.begin(),
            objPtr->moduleExportNames.end(),
            propName
          ) != objPtr->moduleExportNames.end();
          if (propName == toStringTagKey) {
            isExport = true;
          }
          if (isExport && strictMode_) {
            throwError(ErrorType::TypeError, "Cannot delete property '" + propName + "' of module namespace object");
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          LIGHTJS_RETURN(Value(!isExport));
        }
        if (objPtr->frozen || objPtr->sealed) {
          LIGHTJS_RETURN(Value(false));
        }
        // Check __non_configurable_ marker
        if (objPtr->properties.count("__non_configurable_" + propName)) {
          LIGHTJS_RETURN(Value(false));
        }
        bool deleted = false;
        deleted = objPtr->properties.erase(propName) > 0 || deleted;
        deleted = objPtr->properties.erase("__get_" + propName) > 0 || deleted;
        deleted = objPtr->properties.erase("__set_" + propName) > 0 || deleted;
        if (deleted && objPtr->shape) {
          objPtr->shape = nullptr; // Invalidate shape on delete
        }
        LIGHTJS_RETURN(Value(true));
      }

      if (obj.isArray()) {
        auto arrPtr = std::get<std::shared_ptr<Array>>(obj.data);
        size_t idx = 0;
        if (parseArrayIndex(propName, idx) && idx < arrPtr->elements.size()) {
          arrPtr->elements[idx] = Value(Undefined{});
          LIGHTJS_RETURN(Value(true));
        }
        arrPtr->properties.erase(propName);
        arrPtr->properties.erase("__get_" + propName);
        arrPtr->properties.erase("__set_" + propName);
        LIGHTJS_RETURN(Value(true));
      }

      if (obj.isPromise()) {
        auto promisePtr = std::get<std::shared_ptr<Promise>>(obj.data);
        promisePtr->properties.erase(propName);
        promisePtr->properties.erase("__get_" + propName);
        promisePtr->properties.erase("__set_" + propName);
        LIGHTJS_RETURN(Value(true));
      }

      // Delete on class properties
      if (obj.isClass()) {
        auto clsPtr = std::get<std::shared_ptr<Class>>(obj.data);
        if (clsPtr->properties.count("__non_configurable_" + propName)) {
          LIGHTJS_RETURN(Value(false));
        }
        clsPtr->properties.erase(propName);
        clsPtr->properties.erase("__non_writable_" + propName);
        clsPtr->properties.erase("__non_enum_" + propName);
        clsPtr->properties.erase("__enum_" + propName);
        LIGHTJS_RETURN(Value(true));
      }

      // Can't delete from primitives
      LIGHTJS_RETURN(Value(false));
    }

    // delete on identifier (not allowed in strict mode, but we return true)
    if (std::holds_alternative<Identifier>(expr.argument->node)) {
      // In non-strict mode, delete on variables returns false
      LIGHTJS_RETURN(Value(false));
    }

    // delete on other expressions always returns true
    LIGHTJS_RETURN(Value(true));
  }

  // For typeof, handle undeclared identifiers specially (return "undefined" instead of throwing)
  if (expr.op == UnaryExpr::Op::Typeof) {
    if (auto* id = std::get_if<Identifier>(&expr.argument->node)) {
      auto val = env_->get(id->name);
      if (!val) {
        // Unresolvable reference: typeof returns "undefined"
        LIGHTJS_RETURN(Value("undefined"));
      }
    }
  }

  // For other unary operators, evaluate the argument first
  auto argTask = evaluate(*expr.argument);
  Value arg;
  LIGHTJS_RUN_TASK(argTask, arg);

  switch (expr.op) {
    case UnaryExpr::Op::Not:
      LIGHTJS_RETURN(Value(!arg.toBool()));
    case UnaryExpr::Op::Minus: {
      Value prim = isObjectLike(arg) ? toPrimitiveValue(arg, false) : arg;
      if (hasError()) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (prim.isBigInt()) {
        LIGHTJS_RETURN(Value(BigInt(-prim.toBigInt())));
      }
      if (prim.isSymbol()) {
        throwError(ErrorType::TypeError, "Cannot convert Symbol to number");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(-prim.toNumber()));
    }
    case UnaryExpr::Op::Plus: {
      Value prim = isObjectLike(arg) ? toPrimitiveValue(arg, false) : arg;
      if (hasError()) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (prim.isBigInt()) {
        throwError(ErrorType::TypeError, "Cannot convert BigInt value to number");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (prim.isSymbol()) {
        throwError(ErrorType::TypeError, "Cannot convert Symbol to number");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(prim.toNumber()));
    }
    case UnaryExpr::Op::Typeof: {
      if (arg.isUndefined()) LIGHTJS_RETURN(Value("undefined"));
      if (arg.isNull()) LIGHTJS_RETURN(Value("object"));
      if (arg.isBool()) LIGHTJS_RETURN(Value("boolean"));
      if (arg.isNumber()) LIGHTJS_RETURN(Value("number"));
      if (arg.isBigInt()) LIGHTJS_RETURN(Value("bigint"));
      if (arg.isSymbol()) LIGHTJS_RETURN(Value("symbol"));
      if (arg.isString()) LIGHTJS_RETURN(Value("string"));
      if (arg.isFunction()) LIGHTJS_RETURN(Value("function"));
      LIGHTJS_RETURN(Value("object"));
    }
    case UnaryExpr::Op::Void:
      LIGHTJS_RETURN(Value(Undefined{}));
    case UnaryExpr::Op::BitNot: {
      Value prim = isObjectLike(arg) ? toPrimitiveValue(arg, false) : arg;
      if (hasError()) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (prim.isBigInt()) {
        LIGHTJS_RETURN(Value(BigInt(~prim.toBigInt())));
      }
      if (prim.isSymbol()) {
        throwError(ErrorType::TypeError, "Cannot convert Symbol to number");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      int32_t number = static_cast<int32_t>(prim.toNumber());
      LIGHTJS_RETURN(Value(static_cast<double>(~number)));
    }
    case UnaryExpr::Op::Delete:
      // Already handled above
      break;
  }

  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::evaluateAssignment(const AssignmentExpr& expr) {
  // For logical assignment operators, evaluate left first and potentially short-circuit
  if (expr.op == AssignmentExpr::Op::AndAssign ||
      expr.op == AssignmentExpr::Op::OrAssign ||
      expr.op == AssignmentExpr::Op::NullishAssign) {

    if (auto* id = std::get_if<Identifier>(&expr.left->node)) {
      if (auto current = env_->get(id->name)) {
        // Short-circuit evaluation
        bool shouldAssign = false;
        if (expr.op == AssignmentExpr::Op::AndAssign) {
          shouldAssign = current->toBool();  // Only assign if left is truthy
        } else if (expr.op == AssignmentExpr::Op::OrAssign) {
          shouldAssign = !current->toBool();  // Only assign if left is falsy
        } else if (expr.op == AssignmentExpr::Op::NullishAssign) {
          shouldAssign = (current->isNull() || current->isUndefined());  // Only assign if left is nullish
        }

        if (shouldAssign) {
          auto rightTask = evaluate(*expr.right);
  Value right;
  LIGHTJS_RUN_TASK(rightTask, right);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
          // Named evaluation: set name on anonymous function/class
          if (right.isFunction()) {
            auto fn = std::get<std::shared_ptr<Function>>(right.data);
            auto nameIt = fn->properties.find("name");
            if (nameIt != fn->properties.end() && nameIt->second.isString() && nameIt->second.toString().empty()) {
              fn->properties["name"] = Value(id->name);
            }
          } else if (right.isClass()) {
            auto cls = std::get<std::shared_ptr<Class>>(right.data);
            // Per spec: HasOwnProperty(v, "name") check before SetFunctionName
            if (cls->properties.find("name") == cls->properties.end()) {
              cls->name = id->name;
              cls->properties["name"] = Value(id->name);
              cls->properties["__non_writable_name"] = Value(true);
              cls->properties["__non_enum_name"] = Value(true);
            }
          }
          env_->set(id->name, right);
          LIGHTJS_RETURN(right);
        } else {
          LIGHTJS_RETURN(*current);
        }
      } else {
        // Unresolved LHS: throw ReferenceError
        throwError(ErrorType::ReferenceError, "'" + id->name + "' is not defined");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
    }

    // Logical assignment on member expressions - short-circuit
    if (auto* member = std::get_if<MemberExpr>(&expr.left->node)) {
      auto objTask = evaluate(*member->object);
      Value obj;
      LIGHTJS_RUN_TASK(objTask, obj);
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));

      // Throw TypeError for null/undefined base before evaluating property key
      if (obj.isNull() || obj.isUndefined()) {
        if (member->computed) {
          // Evaluate computed property key first (spec: LHS before RHS)
          auto propTask = evaluate(*member->property);
          LIGHTJS_RUN_TASK_VOID(propTask);
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
        }
        throwError(ErrorType::TypeError, "Cannot read properties of " + std::string(obj.isNull() ? "null" : "undefined"));
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      std::string propName;
      if (member->computed) {
        auto propTask = evaluate(*member->property);
        LIGHTJS_RUN_TASK_VOID(propTask);
        if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
        propName = toPropertyKeyString(propTask.result());
      } else {
        if (auto* id = std::get_if<Identifier>(&member->property->node)) {
          propName = id->name;
        }
      }

      // Get current value (with getter support)
      Value current(Undefined{});
      bool hasGetter = false;
      bool hasSetter = true; // assume settable by default
      bool isWritable = true;
      bool isExtensible = true;
      bool propExists = false;
      if (obj.isObject()) {
        auto objPtr = std::get<std::shared_ptr<Object>>(obj.data);
        // Check for getter
        auto getterIt = objPtr->properties.find("__get_" + propName);
        if (getterIt != objPtr->properties.end() && getterIt->second.isFunction()) {
          hasGetter = true;
          current = callFunction(getterIt->second, {}, obj);
          propExists = true;
        } else {
          auto it = objPtr->properties.find(propName);
          if (it != objPtr->properties.end()) {
            current = it->second;
            propExists = true;
          }
        }
        // Check setter status
        auto setterIt = objPtr->properties.find("__set_" + propName);
        if (hasGetter && setterIt == objPtr->properties.end()) {
          // Getter exists but no setter - assignment will fail
          hasSetter = false;
        }
        // Check non-writable marker
        auto nwIt = objPtr->properties.find("__non_writable_" + propName);
        if (nwIt != objPtr->properties.end() && nwIt->second.toBool()) {
          isWritable = false;
        }
        // Check extensible (Object.preventExtensions sets sealed=true)
        if (objPtr->sealed || objPtr->frozen) {
          isExtensible = false;
        }
      } else if (obj.isFunction()) {
        auto fnPtr = std::get<std::shared_ptr<Function>>(obj.data);
        auto it = fnPtr->properties.find(propName);
        if (it != fnPtr->properties.end()) {
          current = it->second;
          propExists = true;
        }
      } else if (obj.isArray()) {
        auto arrPtr = std::get<std::shared_ptr<Array>>(obj.data);
        size_t idx = 0;
        bool isIdx = false;
        try { idx = std::stoull(propName); isIdx = true; } catch (...) {}
        if (isIdx && idx < arrPtr->elements.size()) {
          current = arrPtr->elements[idx];
          propExists = true;
        }
      }

      // Short-circuit check
      bool shouldAssign = false;
      if (expr.op == AssignmentExpr::Op::AndAssign) {
        shouldAssign = current.toBool();
      } else if (expr.op == AssignmentExpr::Op::OrAssign) {
        shouldAssign = !current.toBool();
      } else if (expr.op == AssignmentExpr::Op::NullishAssign) {
        shouldAssign = (current.isNull() || current.isUndefined());
      }

      if (!shouldAssign) {
        LIGHTJS_RETURN(current);
      }

      // Check if assignment is allowed before evaluating RHS
      if (!isWritable && propExists) {
        // Evaluate RHS first (spec requires it)
        auto rightTask2 = evaluate(*expr.right);
        LIGHTJS_RUN_TASK_VOID(rightTask2);
        throwError(ErrorType::TypeError, "Cannot assign to read only property '" + propName + "'");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (!hasSetter && propExists) {
        auto rightTask2 = evaluate(*expr.right);
        LIGHTJS_RUN_TASK_VOID(rightTask2);
        throwError(ErrorType::TypeError, "Cannot set property " + propName + " which has only a getter");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (!isExtensible && !propExists) {
        auto rightTask2 = evaluate(*expr.right);
        LIGHTJS_RUN_TASK_VOID(rightTask2);
        throwError(ErrorType::TypeError, "Cannot add property " + propName + ", object is not extensible");
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      // Evaluate RHS only if we need to assign
      auto rightTask2 = evaluate(*expr.right);
      Value right2;
      LIGHTJS_RUN_TASK(rightTask2, right2);
      if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));

      // Assign (with setter support)
      if (obj.isObject()) {
        auto objPtr = std::get<std::shared_ptr<Object>>(obj.data);
        auto setterIt = objPtr->properties.find("__set_" + propName);
        if (setterIt != objPtr->properties.end() && setterIt->second.isFunction()) {
          callFunction(setterIt->second, {right2}, obj);
        } else {
          objPtr->properties[propName] = right2;
        }
      } else if (obj.isFunction()) {
        auto fnPtr = std::get<std::shared_ptr<Function>>(obj.data);
        fnPtr->properties[propName] = right2;
      } else if (obj.isArray()) {
        auto arrPtr = std::get<std::shared_ptr<Array>>(obj.data);
        size_t idx = 0;
        try { idx = std::stoull(propName); } catch (...) {}
        if (idx < arrPtr->elements.size()) arrPtr->elements[idx] = right2;
      }
      LIGHTJS_RETURN(right2);
    }
  }

  auto rightTask = evaluate(*expr.right);
  Value right;
  LIGHTJS_RUN_TASK(rightTask, right);

  if (auto* id = std::get_if<Identifier>(&expr.left->node)) {
    // Check TDZ before any assignment to identifier
    if (env_->isTDZ(id->name)) {
      throwError(ErrorType::ReferenceError,
                 "Cannot access '" + id->name + "' before initialization");
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (expr.op == AssignmentExpr::Op::Assign) {
      if (!env_->set(id->name, right)) {
        if (env_->isConst(id->name)) {
          throwError(ErrorType::TypeError, "Assignment to constant variable '" + id->name + "'");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        // In strict mode, assignment to undeclared variable is a ReferenceError
        if (strictMode_) {
          throwError(ErrorType::ReferenceError, "'" + id->name + "' is not defined");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        // In sloppy mode, create global
        env_->define(id->name, right);
      }
      LIGHTJS_RETURN(right);
    }

    if (auto current = env_->get(id->name)) {
      Value result;
      switch (expr.op) {
        case AssignmentExpr::Op::AddAssign: {
          Value lhs = isObjectLike(*current) ? toPrimitiveValue(*current, false) : *current;
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
          Value rhs = isObjectLike(right) ? toPrimitiveValue(right, false) : right;
          if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
          if (lhs.isString() || rhs.isString()) {
            result = Value(lhs.toString() + rhs.toString());
          } else if (lhs.isBigInt() && rhs.isBigInt()) {
            result = Value(BigInt(lhs.toBigInt() + rhs.toBigInt()));
          } else if (lhs.isBigInt() != rhs.isBigInt()) {
            throwError(ErrorType::TypeError, "Cannot mix BigInt and other types");
            LIGHTJS_RETURN(Value(Undefined{}));
          } else {
            result = Value(lhs.toNumber() + rhs.toNumber());
          }
          break;
        }
        case AssignmentExpr::Op::SubAssign:
          result = Value(current->toNumber() - right.toNumber());
          break;
        case AssignmentExpr::Op::MulAssign:
          result = Value(current->toNumber() * right.toNumber());
          break;
        case AssignmentExpr::Op::DivAssign:
          result = Value(current->toNumber() / right.toNumber());
          break;
        default:
          result = right;
      }
      env_->set(id->name, result);
      LIGHTJS_RETURN(result);
    }
  }

  if (auto* member = std::get_if<MemberExpr>(&expr.left->node)) {
    auto objTask = evaluate(*member->object);
  Value obj;
  LIGHTJS_RUN_TASK(objTask, obj);

    std::string propName;
    if (member->computed) {
      auto propTask = evaluate(*member->property);
      LIGHTJS_RUN_TASK_VOID(propTask);
      propName = toPropertyKeyString(propTask.result());
    } else {
      if (auto* id = std::get_if<Identifier>(&member->property->node)) {
        propName = id->name;
      }
    }

    // Handle Proxy set trap
    if (obj.isProxy()) {
      auto proxyPtr = std::get<std::shared_ptr<Proxy>>(obj.data);
      if (proxyPtr->handler && proxyPtr->handler->isObject()) {
        auto handlerObj = std::get<std::shared_ptr<Object>>(proxyPtr->handler->data);
        auto trapIt = handlerObj->properties.find("set");
        if (trapIt != handlerObj->properties.end() && trapIt->second.isFunction()) {
          auto trap = std::get<std::shared_ptr<Function>>(trapIt->second.data);
          if (trap->isNative) {
            // Call set trap: handler.set(target, property, value, receiver)
            std::vector<Value> trapArgs = {*proxyPtr->target, Value(propName), right, obj};
            Value result = trap->nativeFunc(trapArgs);
            if (!result.toBool()) {
              // Set trap returned false - throw in strict mode, but we'll just return
            }
            LIGHTJS_RETURN(right);
          } else {
            // Non-native set trap - call as JS function
            std::vector<Value> trapArgs = {*proxyPtr->target, Value(propName), right, obj};
            Value result = invokeFunction(trap, trapArgs, Value(Undefined{}));
            if (!result.toBool()) {
              // Set trap returned false
            }
            LIGHTJS_RETURN(right);
          }
        }
      }
      // No set trap - fall through to set on target
      if (proxyPtr->target && proxyPtr->target->isObject()) {
        auto targetObj = std::get<std::shared_ptr<Object>>(proxyPtr->target->data);
        targetObj->properties[propName] = right;
        LIGHTJS_RETURN(right);
      }
    }

    if (obj.isObject()) {
      auto objPtr = std::get<std::shared_ptr<Object>>(obj.data);
      if (objPtr->isModuleNamespace) {
        // Module namespace exotic objects reject writes.
        if (strictMode_) {
          throwError(ErrorType::TypeError, "Cannot assign to property '" + propName + "' of module namespace object");
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        LIGHTJS_RETURN(right);
      }

      // Check if object is frozen (can't modify any properties)
      if (objPtr->frozen) {
        // In strict mode this would throw, but we'll silently fail
        LIGHTJS_RETURN(right);
      }

      // Check __non_writable_ marker
      if (objPtr->properties.count("__non_writable_" + propName)) {
        LIGHTJS_RETURN(right);
      }

      // Check if object is sealed (can't add new properties)
      bool isNewProperty = objPtr->properties.find(propName) == objPtr->properties.end();
      if (objPtr->sealed && isNewProperty) {
        // Can't add new properties to sealed object
        LIGHTJS_RETURN(right);
      }

      // Shape tracking disabled - using direct hash map access for simplicity

      // Check for setter
      std::string setterName = "__set_" + propName;
      auto setterIt = objPtr->properties.find(setterName);
      if (setterIt != objPtr->properties.end() && setterIt->second.isFunction()) {
        auto setter = std::get<std::shared_ptr<Function>>(setterIt->second.data);
        // Call the setter with 'this' bound to the object and the value as argument
        invokeFunction(setter, {right}, obj);
        LIGHTJS_RETURN(right);
      }

      if (expr.op == AssignmentExpr::Op::Assign) {
        objPtr->properties[propName] = right;
      } else {
        Value current = objPtr->properties[propName];
        switch (expr.op) {
          case AssignmentExpr::Op::AddAssign: {
            Value lhs = isObjectLike(current) ? toPrimitiveValue(current, false) : current;
            if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
            Value rhs = isObjectLike(right) ? toPrimitiveValue(right, false) : right;
            if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
            if (lhs.isString() || rhs.isString()) {
              objPtr->properties[propName] = Value(lhs.toString() + rhs.toString());
            } else if (lhs.isBigInt() && rhs.isBigInt()) {
              objPtr->properties[propName] = Value(BigInt(lhs.toBigInt() + rhs.toBigInt()));
            } else if (lhs.isBigInt() != rhs.isBigInt()) {
              throwError(ErrorType::TypeError, "Cannot mix BigInt and other types");
              LIGHTJS_RETURN(Value(Undefined{}));
            } else {
              objPtr->properties[propName] = Value(lhs.toNumber() + rhs.toNumber());
            }
            break;
          }
          case AssignmentExpr::Op::SubAssign:
            objPtr->properties[propName] = Value(current.toNumber() - right.toNumber());
            break;
          case AssignmentExpr::Op::MulAssign:
            objPtr->properties[propName] = Value(current.toNumber() * right.toNumber());
            break;
          case AssignmentExpr::Op::DivAssign:
            objPtr->properties[propName] = Value(current.toNumber() / right.toNumber());
            break;
          default:
            objPtr->properties[propName] = right;
        }
      }
      LIGHTJS_RETURN(right);
    }

    if (obj.isFunction()) {
      auto funcPtr = std::get<std::shared_ptr<Function>>(obj.data);
      // name and length are non-writable on functions - silently ignore assignment
      if (propName == "name" || propName == "length") {
        LIGHTJS_RETURN(right);
      }
      // Check __non_writable_ marker
      if (funcPtr->properties.count("__non_writable_" + propName)) {
        LIGHTJS_RETURN(right);
      }
      if (expr.op == AssignmentExpr::Op::Assign) {
        funcPtr->properties[propName] = right;
      } else {
        Value current = funcPtr->properties[propName];
        switch (expr.op) {
          case AssignmentExpr::Op::AddAssign:
            funcPtr->properties[propName] = Value(current.toNumber() + right.toNumber());
            break;
          case AssignmentExpr::Op::SubAssign:
            funcPtr->properties[propName] = Value(current.toNumber() - right.toNumber());
            break;
          case AssignmentExpr::Op::MulAssign:
            funcPtr->properties[propName] = Value(current.toNumber() * right.toNumber());
            break;
          case AssignmentExpr::Op::DivAssign:
            funcPtr->properties[propName] = Value(current.toNumber() / right.toNumber());
            break;
          default:
            funcPtr->properties[propName] = right;
        }
      }
      LIGHTJS_RETURN(right);
    }

    if (obj.isPromise()) {
      auto promisePtr = std::get<std::shared_ptr<Promise>>(obj.data);
      if (expr.op == AssignmentExpr::Op::Assign) {
        promisePtr->properties[propName] = right;
      } else {
        Value current = promisePtr->properties[propName];
        switch (expr.op) {
          case AssignmentExpr::Op::AddAssign:
            promisePtr->properties[propName] = Value(current.toNumber() + right.toNumber());
            break;
          case AssignmentExpr::Op::SubAssign:
            promisePtr->properties[propName] = Value(current.toNumber() - right.toNumber());
            break;
          case AssignmentExpr::Op::MulAssign:
            promisePtr->properties[propName] = Value(current.toNumber() * right.toNumber());
            break;
          case AssignmentExpr::Op::DivAssign:
            promisePtr->properties[propName] = Value(current.toNumber() / right.toNumber());
            break;
          default:
            promisePtr->properties[propName] = right;
            break;
        }
      }
      LIGHTJS_RETURN(right);
    }

    if (obj.isArray()) {
      auto arrPtr = std::get<std::shared_ptr<Array>>(obj.data);
      size_t idx = 0;
      if (parseArrayIndex(propName, idx)) {
        if (expr.op == AssignmentExpr::Op::Assign) {
          if (idx >= arrPtr->elements.size()) {
            arrPtr->elements.resize(idx + 1, Value(Undefined{}));
          }
          arrPtr->elements[idx] = right;
          LIGHTJS_RETURN(right);
        }
        // Compound assignment: read current, compute, write back
        Value current = (idx < arrPtr->elements.size()) ? arrPtr->elements[idx] : Value(Undefined{});
        Value result;
        switch (expr.op) {
          case AssignmentExpr::Op::AddAssign: {
            Value lhs = isObjectLike(current) ? toPrimitiveValue(current, false) : current;
            if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
            Value rhs = isObjectLike(right) ? toPrimitiveValue(right, false) : right;
            if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
            if (lhs.isString() || rhs.isString()) {
              result = Value(lhs.toString() + rhs.toString());
            } else if (lhs.isBigInt() && rhs.isBigInt()) {
              result = Value(BigInt(lhs.toBigInt() + rhs.toBigInt()));
            } else if (lhs.isBigInt() != rhs.isBigInt()) {
              throwError(ErrorType::TypeError, "Cannot mix BigInt and other types");
              LIGHTJS_RETURN(Value(Undefined{}));
            } else {
              result = Value(lhs.toNumber() + rhs.toNumber());
            }
            break;
          }
          case AssignmentExpr::Op::SubAssign:
            result = Value(current.toNumber() - right.toNumber());
            break;
          case AssignmentExpr::Op::MulAssign:
            result = Value(current.toNumber() * right.toNumber());
            break;
          case AssignmentExpr::Op::DivAssign:
            result = Value(current.toNumber() / right.toNumber());
            break;
          default:
            result = right;
        }
        // Only extend array if index is in bounds (compound assignment on
        // out-of-bounds index matches arguments object behavior where length
        // doesn't change)
        if (idx < arrPtr->elements.size()) {
          arrPtr->elements[idx] = result;
        }
        LIGHTJS_RETURN(result);
      }
      if (expr.op == AssignmentExpr::Op::Assign) {
        arrPtr->properties[propName] = right;
      } else {
        Value current = arrPtr->properties[propName];
        switch (expr.op) {
          case AssignmentExpr::Op::AddAssign: {
            Value lhs = isObjectLike(current) ? toPrimitiveValue(current, false) : current;
            if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
            Value rhs = isObjectLike(right) ? toPrimitiveValue(right, false) : right;
            if (hasError()) LIGHTJS_RETURN(Value(Undefined{}));
            if (lhs.isString() || rhs.isString()) {
              arrPtr->properties[propName] = Value(lhs.toString() + rhs.toString());
            } else {
              arrPtr->properties[propName] = Value(lhs.toNumber() + rhs.toNumber());
            }
            break;
          }
          case AssignmentExpr::Op::SubAssign:
            arrPtr->properties[propName] = Value(current.toNumber() - right.toNumber());
            break;
          case AssignmentExpr::Op::MulAssign:
            arrPtr->properties[propName] = Value(current.toNumber() * right.toNumber());
            break;
          case AssignmentExpr::Op::DivAssign:
            arrPtr->properties[propName] = Value(current.toNumber() / right.toNumber());
            break;
          default:
            arrPtr->properties[propName] = right;
        }
      }
      LIGHTJS_RETURN(arrPtr->properties[propName]);
    }

    if (obj.isTypedArray()) {
      auto taPtr = std::get<std::shared_ptr<TypedArray>>(obj.data);
      size_t idx = 0;
      if (parseArrayIndex(propName, idx)) {
        if (taPtr->type == TypedArrayType::BigInt64 || taPtr->type == TypedArrayType::BigUint64) {
          taPtr->setBigIntElement(idx, right.toBigInt());
        } else {
          taPtr->setElement(idx, right.toNumber());
        }
        LIGHTJS_RETURN(right);
      }
    }

    if (obj.isRegex()) {
      auto regexPtr = std::get<std::shared_ptr<Regex>>(obj.data);
      if (expr.op == AssignmentExpr::Op::Assign) {
        regexPtr->properties[propName] = right;
      } else {
        Value current = regexPtr->properties[propName];
        switch (expr.op) {
          case AssignmentExpr::Op::AddAssign:
            regexPtr->properties[propName] = Value(current.toNumber() + right.toNumber());
            break;
          case AssignmentExpr::Op::SubAssign:
            regexPtr->properties[propName] = Value(current.toNumber() - right.toNumber());
            break;
          case AssignmentExpr::Op::MulAssign:
            regexPtr->properties[propName] = Value(current.toNumber() * right.toNumber());
            break;
          case AssignmentExpr::Op::DivAssign:
            regexPtr->properties[propName] = Value(current.toNumber() / right.toNumber());
            break;
          default:
            regexPtr->properties[propName] = right;
        }
      }
      LIGHTJS_RETURN(right);
    }

    if (obj.isClass()) {
      auto clsPtr = std::get<std::shared_ptr<Class>>(obj.data);
      // Check __non_writable_ marker
      if (clsPtr->properties.count("__non_writable_" + propName)) {
        LIGHTJS_RETURN(right);
      }
      if (expr.op == AssignmentExpr::Op::Assign) {
        clsPtr->properties[propName] = right;
      } else {
        Value current = clsPtr->properties[propName];
        switch (expr.op) {
          case AssignmentExpr::Op::AddAssign:
            clsPtr->properties[propName] = Value(current.toNumber() + right.toNumber());
            break;
          case AssignmentExpr::Op::SubAssign:
            clsPtr->properties[propName] = Value(current.toNumber() - right.toNumber());
            break;
          case AssignmentExpr::Op::MulAssign:
            clsPtr->properties[propName] = Value(current.toNumber() * right.toNumber());
            break;
          case AssignmentExpr::Op::DivAssign:
            clsPtr->properties[propName] = Value(current.toNumber() / right.toNumber());
            break;
          default:
            clsPtr->properties[propName] = right;
        }
      }
      LIGHTJS_RETURN(right);
    }
  }

  LIGHTJS_RETURN(right);
}

Task Interpreter::evaluateUpdate(const UpdateExpr& expr) {
  if (auto* id = std::get_if<Identifier>(&expr.argument->node)) {
    if (auto current = env_->get(id->name)) {
      if (current->isBigInt()) {
        int64_t oldVal = current->toBigInt();
        int64_t newVal = (expr.op == UpdateExpr::Op::Increment) ? oldVal + 1 : oldVal - 1;
        env_->set(id->name, Value(BigInt(newVal)));
        LIGHTJS_RETURN(expr.prefix ? Value(BigInt(newVal)) : Value(BigInt(oldVal)));
      }

      double num = current->toNumber();
      double newVal = (expr.op == UpdateExpr::Op::Increment) ? num + 1 : num - 1;
      env_->set(id->name, Value(newVal));
      LIGHTJS_RETURN(expr.prefix ? Value(newVal) : Value(num));
    }
  }

  if (auto* member = std::get_if<MemberExpr>(&expr.argument->node)) {
    Value obj = LIGHTJS_AWAIT(evaluate(*member->object));
    if (hasError()) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    std::string propName;
    if (member->computed) {
      Value prop = LIGHTJS_AWAIT(evaluate(*member->property));
      if (hasError()) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      propName = toPropertyKeyString(prop);
    } else if (auto* idProp = std::get_if<Identifier>(&member->property->node)) {
      propName = idProp->name;
    } else {
      Value prop = LIGHTJS_AWAIT(evaluate(*member->property));
      if (hasError()) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      propName = toPropertyKeyString(prop);
    }

    auto applyNumericUpdate = [&](const Value& currentValue) -> std::pair<Value, Value> {
      if (currentValue.isBigInt()) {
        int64_t oldVal = currentValue.toBigInt();
        int64_t newVal = (expr.op == UpdateExpr::Op::Increment) ? oldVal + 1 : oldVal - 1;
        return {Value(BigInt(oldVal)), Value(BigInt(newVal))};
      }
      double oldNum = currentValue.toNumber();
      double newNum = (expr.op == UpdateExpr::Op::Increment) ? oldNum + 1.0 : oldNum - 1.0;
      return {Value(oldNum), Value(newNum)};
    };

    if (obj.isObject()) {
      auto objPtr = std::get<std::shared_ptr<Object>>(obj.data);
      Value currentValue = Value(Undefined{});
      auto it = objPtr->properties.find(propName);
      if (it != objPtr->properties.end()) {
        currentValue = it->second;
      }
      auto [oldValue, newValue] = applyNumericUpdate(currentValue);
      objPtr->properties[propName] = newValue;
      LIGHTJS_RETURN(expr.prefix ? newValue : oldValue);
    }

    if (obj.isArray()) {
      auto arrPtr = std::get<std::shared_ptr<Array>>(obj.data);
      size_t index = 0;
      if (parseArrayIndex(propName, index)) {
        Value currentValue = index < arrPtr->elements.size()
          ? arrPtr->elements[index]
          : Value(Undefined{});
        auto [oldValue, newValue] = applyNumericUpdate(currentValue);
        if (index >= arrPtr->elements.size()) {
          arrPtr->elements.resize(index + 1, Value(Undefined{}));
        }
        arrPtr->elements[index] = newValue;
        LIGHTJS_RETURN(expr.prefix ? newValue : oldValue);
      }

      Value currentValue = Value(Undefined{});
      auto it = arrPtr->properties.find(propName);
      if (it != arrPtr->properties.end()) {
        currentValue = it->second;
      }
      auto [oldValue, newValue] = applyNumericUpdate(currentValue);
      arrPtr->properties[propName] = newValue;
      LIGHTJS_RETURN(expr.prefix ? newValue : oldValue);
    }
  }

  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::evaluateCall(const CallExpr& expr) {
  // Stack overflow protection
  StackGuard guard(stackDepth_, MAX_STACK_DEPTH);
  if (guard.overflowed()) {
    throwError(ErrorType::RangeError, "Maximum call stack size exceeded");
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  // Special handling for dynamic import()
  if (auto* id = std::get_if<Identifier>(&expr.callee->node)) {
    if (id->name == "import") {
      // This is a dynamic import - call the global import function
      auto importFunc = env_->get("import");

      if (importFunc && importFunc->isFunction()) {
        std::vector<Value> args;
        for (const auto& arg : expr.arguments) {
          auto argTask = evaluate(*arg);
          LIGHTJS_RUN_TASK_VOID(argTask);
          if (flow_.type != ControlFlow::Type::None) {
            LIGHTJS_RETURN(Value(Undefined{}));
          }
          args.push_back(argTask.result());
        }

        auto func = std::get<std::shared_ptr<Function>>(importFunc->data);
	  if (func->isNative) {
          LIGHTJS_RETURN(func->nativeFunc(args));
        }
      }

      // If we couldn't find the import function, return an error Promise
      auto promise = std::make_shared<Promise>();
      auto err = std::make_shared<Error>(ErrorType::ReferenceError, "import is not defined");
      promise->reject(Value(err));
      LIGHTJS_RETURN(Value(promise));
    }
  }

  // Track the 'this' value for method calls
  Value thisValue = Value(Undefined{});
  Value callee;

  // Check if this is a method call (obj.method())
  if (std::holds_alternative<MemberExpr>(expr.callee->node)) {
    // Evaluate the member expression once and capture the base object from evaluateMember.
    hasLastMemberBase_ = false;
    callee = LIGHTJS_AWAIT(evaluate(*expr.callee));
    if (flow_.type != ControlFlow::Type::None) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (hasLastMemberBase_) {
      thisValue = lastMemberBase_;
    }
  } else {
    callee = LIGHTJS_AWAIT(evaluate(*expr.callee));
    if (flow_.type != ControlFlow::Type::None) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }
  }

  // Optional call short-circuits without evaluating arguments.
  // inOptionalChain propagates through the entire chain (e.g., a?.b.c(x))
  if ((expr.optional || expr.inOptionalChain) && (callee.isNull() || callee.isUndefined())) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  std::vector<Value> args;
  for (const auto& arg : expr.arguments) {
    // Check if this is a spread element
    if (auto* spread = std::get_if<SpreadElement>(&arg->node)) {
      // Evaluate the argument
      Value val = LIGHTJS_AWAIT(evaluate(*spread->argument));
      if (flow_.type != ControlFlow::Type::None) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }

      // Spread the value into args
      if (val.isArray()) {
        auto srcArr = std::get<std::shared_ptr<Array>>(val.data);
        for (const auto& item : srcArr->elements) {
          args.push_back(item);
        }
      } else {
        args.push_back(val);
      }
    } else {
      Value argVal = LIGHTJS_AWAIT(evaluate(*arg));
      if (flow_.type != ControlFlow::Type::None) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      args.push_back(argVal);
    }
  }

  // Handle super() calls as construction (ES2020: super() calls [[Construct]])
  if (std::holds_alternative<SuperExpr>(expr.callee->node)) {
    Value newTarget = Value(Undefined{});
    if (auto nt = env_->get("__new_target__")) {
      newTarget = *nt;
    }
    Value result = LIGHTJS_AWAIT(constructValue(callee, args, newTarget));
    if (flow_.type != ControlFlow::Type::None) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    // Update 'this' binding to the super-constructed value
    env_->set("this", result);
    LIGHTJS_RETURN(result);
  }

  if (inTailPosition_ && strictMode_ && callee.isFunction() && activeFunction_) {
    auto calleeFunc = std::get<std::shared_ptr<Function>>(callee.data);
    if (!calleeFunc->isNative &&
        !calleeFunc->isAsync &&
        !calleeFunc->isGenerator &&
        calleeFunc.get() == activeFunction_.get()) {
      pendingSelfTailCall_ = true;
      pendingSelfTailArgs_ = std::move(args);
      pendingSelfTailThis_ = thisValue;
      LIGHTJS_RETURN(Value(Undefined{}));
    }
  }

  bool isDirectEvalCall = false;
  if (!expr.optional &&
      !expr.inOptionalChain &&
      callee.isFunction() &&
      std::holds_alternative<Identifier>(expr.callee->node)) {
    auto* id = std::get_if<Identifier>(&expr.callee->node);
    if (id && id->name == "eval") {
      auto evalFn = std::get<std::shared_ptr<Function>>(callee.data);
      auto intrinsicEvalIt = evalFn->properties.find("__is_intrinsic_eval__");
      isDirectEvalCall = intrinsicEvalIt != evalFn->properties.end() &&
                         intrinsicEvalIt->second.isBool() &&
                         intrinsicEvalIt->second.toBool();
    }
  }

  // Handle Proxy apply trap for callable proxies
  if (callee.isProxy()) {
    auto proxyPtr = std::get<std::shared_ptr<Proxy>>(callee.data);
    if (proxyPtr->handler && proxyPtr->handler->isObject()) {
      auto handlerObj = std::get<std::shared_ptr<Object>>(proxyPtr->handler->data);
      auto trapIt = handlerObj->properties.find("apply");
      if (trapIt != handlerObj->properties.end() && trapIt->second.isFunction()) {
        auto trap = std::get<std::shared_ptr<Function>>(trapIt->second.data);
        // Create args array
        auto argsArray = std::make_shared<Array>();
        argsArray->elements = args;
        // Call apply trap: handler.apply(target, thisArg, argumentsList)
        std::vector<Value> trapArgs = {*proxyPtr->target, thisValue, Value(argsArray)};
        if (trap->isNative) {
          LIGHTJS_RETURN(trap->nativeFunc(trapArgs));
        } else {
          LIGHTJS_RETURN(invokeFunction(trap, trapArgs, Value(Undefined{})));
        }
      }
    }
    // No apply trap - call the target directly if it's a function
    if (proxyPtr->target && proxyPtr->target->isFunction()) {
      LIGHTJS_RETURN(callFunction(*proxyPtr->target, args, thisValue));
    }
  }

  if (callee.isFunction()) {
    bool prevPendingDirectEvalCall = pendingDirectEvalCall_;
    pendingDirectEvalCall_ = isDirectEvalCall;
    Value callResult = callFunction(callee, args, thisValue);
    pendingDirectEvalCall_ = prevPendingDirectEvalCall;
    LIGHTJS_RETURN(callResult);
  }

  // Some built-ins are represented as wrapper objects with a callable constructor.
  if (callee.isObject()) {
    auto objPtr = std::get<std::shared_ptr<Object>>(callee.data);
    auto callableIt = objPtr->properties.find("__callable_object__");
    bool isCallableWrapper = callableIt != objPtr->properties.end() &&
                             callableIt->second.isBool() &&
                             callableIt->second.toBool();
    if (isCallableWrapper) {
      auto ctorIt = objPtr->properties.find("constructor");
      if (ctorIt != objPtr->properties.end() && ctorIt->second.isFunction()) {
        LIGHTJS_RETURN(callFunction(ctorIt->second, args, thisValue));
      }
    }
  }

  // Throw TypeError if trying to call a non-function
  throwError(ErrorType::TypeError, callee.toString() + " is not a function");
  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::evaluateMember(const MemberExpr& expr) {
  auto objTask = evaluate(*expr.object);
  Value obj;
  LIGHTJS_RUN_TASK(objTask, obj);

  // Optional chaining: if object is null or undefined, return undefined.
  // This check must happen before evaluating computed keys to preserve short-circuiting.
  // inOptionalChain propagates short-circuiting through the entire chain (e.g., a?.b.c.d)
  if ((expr.optional || expr.inOptionalChain) && (obj.isNull() || obj.isUndefined())) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  // TypeError for property access on null/undefined (non-optional)
  if (!expr.optional && !expr.inOptionalChain && (obj.isNull() || obj.isUndefined())) {
    // Evaluate computed property key first per spec (LHS before RHS)
    if (expr.computed) {
      auto propTask = evaluate(*expr.property);
      LIGHTJS_RUN_TASK_VOID(propTask);
    }
    std::string propName;
    if (expr.computed) {
      // Already evaluated above, re-evaluate to get name for error message
    } else if (auto* id = std::get_if<Identifier>(&expr.property->node)) {
      propName = id->name;
    }
    throwError(ErrorType::TypeError,
      "Cannot read properties of " + std::string(obj.isNull() ? "null" : "undefined") +
      (propName.empty() ? "" : " (reading '" + propName + "')"));
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  std::string propName;
  if (expr.computed) {
    auto propTask = evaluate(*expr.property);
    LIGHTJS_RUN_TASK_VOID(propTask);
    Value key = propTask.result();
    if (isObjectLike(key)) {
      key = toPrimitiveValue(key, true);
      if (hasError()) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
    }
    propName = toPropertyKeyString(key);
  } else {
    if (auto* id = std::get_if<Identifier>(&expr.property->node)) {
      propName = id->name;
    }
  }

  // Remember the current base object so method calls can bind `this` without
  // re-evaluating side-effectful member objects.
  bool isSuperAccess = expr.object && std::holds_alternative<SuperExpr>(expr.object->node);
  if (isSuperAccess) {
    if (auto thisValue = env_->get("this")) {
      lastMemberBase_ = *thisValue;
    } else {
      lastMemberBase_ = Value(Undefined{});
    }
  } else {
    lastMemberBase_ = obj;
  }
  hasLastMemberBase_ = true;

  // BigInt primitive member access
  if (obj.isBigInt()) {
    int64_t bigintValue = obj.toBigInt();

    if (propName == "constructor") {
      if (auto ctor = env_->get("BigInt")) {
        LIGHTJS_RETURN(*ctor);
      }
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    if (propName == "valueOf") {
      auto valueOfFn = std::make_shared<Function>();
      valueOfFn->isNative = true;
      valueOfFn->properties["__throw_on_new__"] = Value(true);
      valueOfFn->nativeFunc = [bigintValue](const std::vector<Value>&) -> Value {
        return Value(BigInt(bigintValue));
      };
      LIGHTJS_RETURN(Value(valueOfFn));
    }

    if (propName == "toLocaleString") {
      auto toLocaleStringFn = std::make_shared<Function>();
      toLocaleStringFn->isNative = true;
      toLocaleStringFn->properties["__throw_on_new__"] = Value(true);
      toLocaleStringFn->nativeFunc = [bigintValue](const std::vector<Value>&) -> Value {
        return Value(std::to_string(bigintValue));
      };
      LIGHTJS_RETURN(Value(toLocaleStringFn));
    }

    if (propName == "toString") {
      auto toStringFn = std::make_shared<Function>();
      toStringFn->isNative = true;
      toStringFn->properties["__throw_on_new__"] = Value(true);
      toStringFn->nativeFunc = [bigintValue](const std::vector<Value>& args) -> Value {
        int radix = 10;
        if (!args.empty() && !args[0].isUndefined()) {
          int r = static_cast<int>(std::trunc(args[0].toNumber()));
          if (r < 2 || r > 36) {
            throw std::runtime_error("RangeError: radix must be between 2 and 36");
          }
          radix = r;
        }

        bool negative = bigintValue < 0;
        uint64_t magnitude = negative
          ? static_cast<uint64_t>(-(bigintValue + 1)) + 1
          : static_cast<uint64_t>(bigintValue);

        if (magnitude == 0) {
          return Value(std::string("0"));
        }

        std::string digits = "0123456789abcdefghijklmnopqrstuvwxyz";
        std::string out;
        while (magnitude > 0) {
          uint64_t digit = magnitude % static_cast<uint64_t>(radix);
          out.push_back(digits[static_cast<size_t>(digit)]);
          magnitude /= static_cast<uint64_t>(radix);
        }
        std::reverse(out.begin(), out.end());
        if (negative) out.insert(out.begin(), '-');
        return Value(out);
      };
      LIGHTJS_RETURN(Value(toStringFn));
    }
  }

  // Symbol primitive member access
  if (obj.isSymbol()) {
    Symbol symbolValue = std::get<Symbol>(obj.data);

    if (propName == "constructor") {
      if (auto ctor = env_->get("Symbol")) {
        LIGHTJS_RETURN(*ctor);
      }
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    if (propName == "toString") {
      auto toStringFn = std::make_shared<Function>();
      toStringFn->isNative = true;
      toStringFn->properties["__throw_on_new__"] = Value(true);
      toStringFn->nativeFunc = [symbolValue](const std::vector<Value>&) -> Value {
        return Value(std::string("Symbol(") + symbolValue.description + ")");
      };
      LIGHTJS_RETURN(Value(toStringFn));
    }

    if (propName == "valueOf") {
      auto valueOfFn = std::make_shared<Function>();
      valueOfFn->isNative = true;
      valueOfFn->properties["__throw_on_new__"] = Value(true);
      valueOfFn->nativeFunc = [symbolValue](const std::vector<Value>&) -> Value {
        return Value(symbolValue);
      };
      LIGHTJS_RETURN(Value(valueOfFn));
    }

    if (propName == "description") {
      if (symbolValue.description.empty()) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      LIGHTJS_RETURN(Value(symbolValue.description));
    }
  }

  // Proxy trap handling - intercept get operations
  if (obj.isProxy()) {
    auto proxyPtr = std::get<std::shared_ptr<Proxy>>(obj.data);

    // Compute property name
    std::string propName;
    if (expr.computed) {
      Value propVal = LIGHTJS_AWAIT(evaluate(*expr.property));
      propName = toPropertyKeyString(propVal);
    } else {
      if (auto* id = std::get_if<Identifier>(&expr.property->node)) {
        propName = id->name;
      }
    }

    // Check if handler has a 'get' trap
    if (proxyPtr->handler && proxyPtr->handler->isObject()) {
      auto handlerObj = std::get<std::shared_ptr<Object>>(proxyPtr->handler->data);
      auto getTrapIt = handlerObj->properties.find("get");

      if (getTrapIt != handlerObj->properties.end() && getTrapIt->second.isFunction()) {
        // Call the get trap: handler.get(target, property, receiver)
        auto getTrap = std::get<std::shared_ptr<Function>>(getTrapIt->second.data);
        std::vector<Value> trapArgs = {
          *proxyPtr->target,
          Value(propName),
          obj  // receiver is the proxy itself
        };
        if (getTrap->isNative) {
          LIGHTJS_RETURN(getTrap->nativeFunc(trapArgs));
        } else {
          LIGHTJS_RETURN(invokeFunction(getTrap, trapArgs, Value(Undefined{})));
        }
      }
    }

    // No trap, fall through to default behavior on target
    if (proxyPtr->target && proxyPtr->target->isObject()) {
      auto targetObj = std::get<std::shared_ptr<Object>>(proxyPtr->target->data);
      auto it = targetObj->properties.find(propName);
      if (it != targetObj->properties.end()) {
        LIGHTJS_RETURN(it->second);
      }
    }
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  const auto& iteratorKey = WellKnownSymbols::iteratorKey();

  if (obj.isPromise()) {
    auto promisePtr = std::get<std::shared_ptr<Promise>>(obj.data);

    // Promise own accessor/data properties can override built-in behavior.
    std::string promiseGetterName = "__get_" + propName;
    auto promiseGetterIt = promisePtr->properties.find(promiseGetterName);
    if (promiseGetterIt != promisePtr->properties.end() && promiseGetterIt->second.isFunction()) {
      LIGHTJS_RETURN(callFunction(promiseGetterIt->second, {}, obj));
    }
    auto promiseOwnIt = promisePtr->properties.find(propName);
    if (promiseOwnIt != promisePtr->properties.end()) {
      LIGHTJS_RETURN(promiseOwnIt->second);
    }

    auto getIntrinsicPromisePrototype = [&]() -> std::shared_ptr<Object> {
      Value promiseCtorValue = Value(Undefined{});
      if (auto intrinsicPromise = env_->get("__intrinsic_Promise__")) {
        promiseCtorValue = *intrinsicPromise;
      } else if (auto promiseCtor = env_->get("Promise")) {
        promiseCtorValue = *promiseCtor;
      }
      if (!promiseCtorValue.isFunction()) {
        return nullptr;
      }
      auto ctorFn = std::get<std::shared_ptr<Function>>(promiseCtorValue.data);
      auto protoIt = ctorFn->properties.find("prototype");
      if (protoIt == ctorFn->properties.end() || !protoIt->second.isObject()) {
        return nullptr;
      }
      return std::get<std::shared_ptr<Object>>(protoIt->second.data);
    };

    if (auto promiseProto = getIntrinsicPromisePrototype()) {
      auto getterIt = promiseProto->properties.find("__get_" + propName);
      if (getterIt != promiseProto->properties.end() && getterIt->second.isFunction()) {
        LIGHTJS_RETURN(callFunction(getterIt->second, {}, obj));
      }
      auto protoIt = promiseProto->properties.find(propName);
      if (protoIt != promiseProto->properties.end()) {
        LIGHTJS_RETURN(protoIt->second);
      }
    }

    if (propName == "constructor") {
      auto ctorIt = promisePtr->properties.find("__constructor__");
      if (ctorIt != promisePtr->properties.end()) {
        LIGHTJS_RETURN(ctorIt->second);
      }
      if (auto intrinsic = env_->get("__intrinsic_Promise__")) {
        LIGHTJS_RETURN(*intrinsic);
      }
      if (auto ctor = env_->get("Promise")) {
        LIGHTJS_RETURN(*ctor);
      }
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    // Add toString method for Promise
    if (propName == "toString") {
      auto toStringFn = std::make_shared<Function>();
      toStringFn->isNative = true;
      toStringFn->nativeFunc = [](const std::vector<Value>&) -> Value {
        return Value("[Promise]");
      };
      LIGHTJS_RETURN(Value(toStringFn));
    }

    // Promise.prototype.then(onFulfilled, onRejected)
    if (propName == "then") {
      auto thenFn = std::make_shared<Function>();
      thenFn->isNative = true;
      thenFn->nativeFunc = [this, promisePtr](const std::vector<Value>& args) -> Value {
        std::function<Value(Value)> onFulfilled = nullptr;
        std::function<Value(Value)> onRejected = nullptr;

        // Get onFulfilled callback if provided
        if (!args.empty() && args[0].isFunction()) {
          auto callback = std::get<std::shared_ptr<Function>>(args[0].data);
          onFulfilled = [this, callback](Value val) -> Value {
            Value out = invokeFunction(callback, {val}, Value(Undefined{}));
            if (hasError()) {
              Value err = getError();
              clearError();
              throw std::runtime_error(err.toString());
            }
            return out;
          };
        }

        // Get onRejected callback if provided
        if (args.size() > 1 && args[1].isFunction()) {
          auto callback = std::get<std::shared_ptr<Function>>(args[1].data);
          onRejected = [this, callback](Value val) -> Value {
            Value out = invokeFunction(callback, {val}, Value(Undefined{}));
            if (hasError()) {
              Value err = getError();
              clearError();
              throw std::runtime_error(err.toString());
            }
            return out;
          };
        }

        auto chainedPromise = promisePtr->then(onFulfilled, onRejected);
        return Value(chainedPromise);
      };
      LIGHTJS_RETURN(Value(thenFn));
    }

    // Promise.prototype.catch(onRejected)
    if (propName == "catch") {
      auto catchFn = std::make_shared<Function>();
      catchFn->isNative = true;
      catchFn->nativeFunc = [this, promisePtr](const std::vector<Value>& args) -> Value {
        std::function<Value(Value)> onRejected = nullptr;

        if (!args.empty() && args[0].isFunction()) {
          auto callback = std::get<std::shared_ptr<Function>>(args[0].data);
          onRejected = [this, callback](Value val) -> Value {
            Value out = invokeFunction(callback, {val}, Value(Undefined{}));
            if (hasError()) {
              Value err = getError();
              clearError();
              throw std::runtime_error(err.toString());
            }
            return out;
          };
        }

        auto chainedPromise = promisePtr->catch_(onRejected);
        return Value(chainedPromise);
      };
      LIGHTJS_RETURN(Value(catchFn));
    }

    // Promise.prototype.finally(onFinally)
    if (propName == "finally") {
      auto finallyFn = std::make_shared<Function>();
      finallyFn->isNative = true;
      finallyFn->nativeFunc = [this, promisePtr](const std::vector<Value>& args) -> Value {
        std::function<Value()> onFinally = nullptr;

        if (!args.empty() && args[0].isFunction()) {
          auto callback = std::get<std::shared_ptr<Function>>(args[0].data);
          onFinally = [this, callback]() -> Value {
            Value out = invokeFunction(callback, {}, Value(Undefined{}));
            if (hasError()) {
              Value err = getError();
              clearError();
              throw std::runtime_error(err.toString());
            }
            return out;
          };
        }

        auto chainedPromise = promisePtr->finally(onFinally);
        return Value(chainedPromise);
      };
      LIGHTJS_RETURN(Value(finallyFn));
    }

    if (promisePtr->state == PromiseState::Fulfilled) {
      Value resolvedValue = promisePtr->result;
      if (resolvedValue.isObject()) {
        auto objPtr = std::get<std::shared_ptr<Object>>(resolvedValue.data);
        auto it = objPtr->properties.find(propName);
        if (it != objPtr->properties.end()) {
          LIGHTJS_RETURN(it->second);
        }
      }
    }
  }

  // ArrayBuffer property access
  if (obj.isArrayBuffer()) {
    auto bufferPtr = std::get<std::shared_ptr<ArrayBuffer>>(obj.data);
    if (propName == "byteLength") {
      LIGHTJS_RETURN(Value(static_cast<double>(bufferPtr->byteLength)));
    }
  }

  // DataView property and method access
  if (obj.isDataView()) {
    auto viewPtr = std::get<std::shared_ptr<DataView>>(obj.data);

    if (propName == "buffer") {
      LIGHTJS_RETURN(Value(viewPtr->buffer));
    }
    if (propName == "byteOffset") {
      LIGHTJS_RETURN(Value(static_cast<double>(viewPtr->byteOffset)));
    }
    if (propName == "byteLength") {
      LIGHTJS_RETURN(Value(static_cast<double>(viewPtr->byteLength)));
    }

    // DataView get methods
    if (propName == "getInt8") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(std::make_shared<Error>(ErrorType::TypeError, "getInt8 requires offset"));
        return Value(static_cast<double>(viewPtr->getInt8(static_cast<size_t>(args[0].toNumber()))));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getUint8") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(std::make_shared<Error>(ErrorType::TypeError, "getUint8 requires offset"));
        return Value(static_cast<double>(viewPtr->getUint8(static_cast<size_t>(args[0].toNumber()))));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getInt16") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(std::make_shared<Error>(ErrorType::TypeError, "getInt16 requires offset"));
        bool littleEndian = args.size() > 1 ? args[1].toBool() : false;
        return Value(static_cast<double>(viewPtr->getInt16(static_cast<size_t>(args[0].toNumber()), littleEndian)));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getUint16") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(std::make_shared<Error>(ErrorType::TypeError, "getUint16 requires offset"));
        bool littleEndian = args.size() > 1 ? args[1].toBool() : false;
        return Value(static_cast<double>(viewPtr->getUint16(static_cast<size_t>(args[0].toNumber()), littleEndian)));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getInt32") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(std::make_shared<Error>(ErrorType::TypeError, "getInt32 requires offset"));
        bool littleEndian = args.size() > 1 ? args[1].toBool() : false;
        return Value(static_cast<double>(viewPtr->getInt32(static_cast<size_t>(args[0].toNumber()), littleEndian)));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getUint32") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(std::make_shared<Error>(ErrorType::TypeError, "getUint32 requires offset"));
        bool littleEndian = args.size() > 1 ? args[1].toBool() : false;
        return Value(static_cast<double>(viewPtr->getUint32(static_cast<size_t>(args[0].toNumber()), littleEndian)));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getFloat32") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(std::make_shared<Error>(ErrorType::TypeError, "getFloat32 requires offset"));
        bool littleEndian = args.size() > 1 ? args[1].toBool() : false;
        return Value(static_cast<double>(viewPtr->getFloat32(static_cast<size_t>(args[0].toNumber()), littleEndian)));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getFloat64") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(std::make_shared<Error>(ErrorType::TypeError, "getFloat64 requires offset"));
        bool littleEndian = args.size() > 1 ? args[1].toBool() : false;
        return Value(viewPtr->getFloat64(static_cast<size_t>(args[0].toNumber()), littleEndian));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getBigInt64") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(std::make_shared<Error>(ErrorType::TypeError, "getBigInt64 requires offset"));
        bool littleEndian = args.size() > 1 ? args[1].toBool() : false;
        return Value(BigInt(viewPtr->getBigInt64(static_cast<size_t>(args[0].toNumber()), littleEndian)));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getBigUint64") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(std::make_shared<Error>(ErrorType::TypeError, "getBigUint64 requires offset"));
        bool littleEndian = args.size() > 1 ? args[1].toBool() : false;
        return Value(BigInt(static_cast<int64_t>(viewPtr->getBigUint64(static_cast<size_t>(args[0].toNumber()), littleEndian))));
      };
      LIGHTJS_RETURN(Value(fn));
    }

    // DataView set methods
    if (propName == "setInt8") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(std::make_shared<Error>(ErrorType::TypeError, "setInt8 requires offset and value"));
        viewPtr->setInt8(static_cast<size_t>(args[0].toNumber()), static_cast<int8_t>(args[1].toNumber()));
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "setUint8") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(std::make_shared<Error>(ErrorType::TypeError, "setUint8 requires offset and value"));
        viewPtr->setUint8(static_cast<size_t>(args[0].toNumber()), static_cast<uint8_t>(args[1].toNumber()));
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "setInt16") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(std::make_shared<Error>(ErrorType::TypeError, "setInt16 requires offset and value"));
        bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
        viewPtr->setInt16(static_cast<size_t>(args[0].toNumber()), static_cast<int16_t>(args[1].toNumber()), littleEndian);
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "setUint16") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(std::make_shared<Error>(ErrorType::TypeError, "setUint16 requires offset and value"));
        bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
        viewPtr->setUint16(static_cast<size_t>(args[0].toNumber()), static_cast<uint16_t>(args[1].toNumber()), littleEndian);
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "setInt32") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(std::make_shared<Error>(ErrorType::TypeError, "setInt32 requires offset and value"));
        bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
        viewPtr->setInt32(static_cast<size_t>(args[0].toNumber()), static_cast<int32_t>(args[1].toNumber()), littleEndian);
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "setUint32") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(std::make_shared<Error>(ErrorType::TypeError, "setUint32 requires offset and value"));
        bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
        viewPtr->setUint32(static_cast<size_t>(args[0].toNumber()), static_cast<uint32_t>(args[1].toNumber()), littleEndian);
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "setFloat32") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(std::make_shared<Error>(ErrorType::TypeError, "setFloat32 requires offset and value"));
        bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
        viewPtr->setFloat32(static_cast<size_t>(args[0].toNumber()), static_cast<float>(args[1].toNumber()), littleEndian);
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "setFloat64") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(std::make_shared<Error>(ErrorType::TypeError, "setFloat64 requires offset and value"));
        bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
        viewPtr->setFloat64(static_cast<size_t>(args[0].toNumber()), args[1].toNumber(), littleEndian);
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "setBigInt64") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(std::make_shared<Error>(ErrorType::TypeError, "setBigInt64 requires offset and value"));
        bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
        viewPtr->setBigInt64(static_cast<size_t>(args[0].toNumber()), args[1].toBigInt(), littleEndian);
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "setBigUint64") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(std::make_shared<Error>(ErrorType::TypeError, "setBigUint64 requires offset and value"));
        bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
        viewPtr->setBigUint64(static_cast<size_t>(args[0].toNumber()), static_cast<uint64_t>(args[1].toBigInt()), littleEndian);
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
  }

  // ReadableStream property and method access
  if (obj.isReadableStream()) {
    auto streamPtr = std::get<std::shared_ptr<ReadableStream>>(obj.data);

    if (propName == "locked") {
      LIGHTJS_RETURN(Value(streamPtr->locked));
    }

    if (propName == "getReader") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [streamPtr](const std::vector<Value>& args) -> Value {
        auto reader = streamPtr->getReader();
        if (!reader) {
          return Value(std::make_shared<Error>(ErrorType::TypeError, "ReadableStream is already locked"));
        }
        // Return reader wrapped in an Object for now
        auto readerObj = std::make_shared<Object>();
        readerObj->properties["__reader__"] = Value(true);

        // Add read method
        auto readFn = std::make_shared<Function>();
        readFn->isNative = true;
        readFn->nativeFunc = [reader](const std::vector<Value>& args) -> Value {
          return Value(reader->read());
        };
        readerObj->properties["read"] = Value(readFn);

        // Add releaseLock method
        auto releaseFn = std::make_shared<Function>();
        releaseFn->isNative = true;
        releaseFn->nativeFunc = [reader](const std::vector<Value>& args) -> Value {
          reader->releaseLock();
          return Value(Undefined{});
        };
        readerObj->properties["releaseLock"] = Value(releaseFn);

        // Add closed property (Promise)
        readerObj->properties["closed"] = Value(reader->closedPromise);

        return Value(readerObj);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "cancel") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [streamPtr](const std::vector<Value>& args) -> Value {
        Value reason = args.empty() ? Value(Undefined{}) : args[0];
        return Value(streamPtr->cancel(reason));
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "pipeTo") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [streamPtr](const std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].isWritableStream()) {
          return Value(std::make_shared<Error>(ErrorType::TypeError, "pipeTo requires a WritableStream"));
        }
        auto dest = std::get<std::shared_ptr<WritableStream>>(args[0].data);
        return Value(streamPtr->pipeTo(dest));
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "pipeThrough") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [streamPtr](const std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].isTransformStream()) {
          return Value(std::make_shared<Error>(ErrorType::TypeError, "pipeThrough requires a TransformStream"));
        }
        auto transform = std::get<std::shared_ptr<TransformStream>>(args[0].data);
        auto result = streamPtr->pipeThrough(transform);
        return result ? Value(result) : Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "tee") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [streamPtr](const std::vector<Value>& args) -> Value {
        auto [branch1, branch2] = streamPtr->tee();
        auto result = std::make_shared<Array>();
        result->elements.push_back(Value(branch1));
        result->elements.push_back(Value(branch2));
        return Value(result);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    // Symbol.asyncIterator - returns async iterator for for-await-of
    const std::string& asyncIteratorKey = WellKnownSymbols::asyncIteratorKey();
    if (propName == asyncIteratorKey) {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [streamPtr](const std::vector<Value>& args) -> Value {
        // Return an async iterator object
        auto iteratorObj = std::make_shared<Object>();

        // Get reader for the stream
        auto reader = streamPtr->getReader();
        if (!reader) {
          return Value(std::make_shared<Error>(ErrorType::TypeError, "ReadableStream is already locked"));
        }

        // next() method returns Promise<{value, done}>
        auto nextFn = std::make_shared<Function>();
        nextFn->isNative = true;
        nextFn->nativeFunc = [reader](const std::vector<Value>& args) -> Value {
          return Value(reader->read());
        };
        iteratorObj->properties["next"] = Value(nextFn);

        // return() method for early termination
        auto returnFn = std::make_shared<Function>();
        returnFn->isNative = true;
        returnFn->nativeFunc = [reader](const std::vector<Value>& args) -> Value {
          reader->releaseLock();
          auto promise = std::make_shared<Promise>();
          auto resultObj = std::make_shared<Object>();
          resultObj->properties["value"] = args.empty() ? Value(Undefined{}) : args[0];
          resultObj->properties["done"] = true;
          promise->resolve(Value(resultObj));
          return Value(promise);
        };
        iteratorObj->properties["return"] = Value(returnFn);

        return Value(iteratorObj);
      };
      LIGHTJS_RETURN(Value(fn));
    }
  }

  // WritableStream property and method access
  if (obj.isWritableStream()) {
    auto streamPtr = std::get<std::shared_ptr<WritableStream>>(obj.data);

    if (propName == "locked") {
      LIGHTJS_RETURN(Value(streamPtr->locked));
    }

    if (propName == "getWriter") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [streamPtr](const std::vector<Value>& args) -> Value {
        auto writer = streamPtr->getWriter();
        if (!writer) {
          return Value(std::make_shared<Error>(ErrorType::TypeError, "WritableStream is already locked"));
        }
        // Return writer wrapped in an Object
        auto writerObj = std::make_shared<Object>();
        writerObj->properties["__writer__"] = Value(true);

        // Add write method
        auto writeFn = std::make_shared<Function>();
        writeFn->isNative = true;
        writeFn->nativeFunc = [writer](const std::vector<Value>& args) -> Value {
          Value chunk = args.empty() ? Value(Undefined{}) : args[0];
          return Value(writer->write(chunk));
        };
        writerObj->properties["write"] = Value(writeFn);

        // Add close method
        auto closeFn = std::make_shared<Function>();
        closeFn->isNative = true;
        closeFn->nativeFunc = [writer](const std::vector<Value>& args) -> Value {
          return Value(writer->close());
        };
        writerObj->properties["close"] = Value(closeFn);

        // Add abort method
        auto abortFn = std::make_shared<Function>();
        abortFn->isNative = true;
        abortFn->nativeFunc = [writer](const std::vector<Value>& args) -> Value {
          Value reason = args.empty() ? Value(Undefined{}) : args[0];
          return Value(writer->abort(reason));
        };
        writerObj->properties["abort"] = Value(abortFn);

        // Add releaseLock method
        auto releaseFn = std::make_shared<Function>();
        releaseFn->isNative = true;
        releaseFn->nativeFunc = [writer](const std::vector<Value>& args) -> Value {
          writer->releaseLock();
          return Value(Undefined{});
        };
        writerObj->properties["releaseLock"] = Value(releaseFn);

        // Add closed property (Promise)
        writerObj->properties["closed"] = Value(writer->closedPromise);

        // Add ready property (Promise)
        writerObj->properties["ready"] = Value(writer->readyPromise);

        // Add desiredSize property
        writerObj->properties["desiredSize"] = Value(writer->desiredSize());

        return Value(writerObj);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "abort") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [streamPtr](const std::vector<Value>& args) -> Value {
        Value reason = args.empty() ? Value(Undefined{}) : args[0];
        return Value(streamPtr->abort(reason));
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "close") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [streamPtr](const std::vector<Value>& args) -> Value {
        return Value(streamPtr->close());
      };
      LIGHTJS_RETURN(Value(fn));
    }
  }

  // TransformStream property access
  if (obj.isTransformStream()) {
    auto streamPtr = std::get<std::shared_ptr<TransformStream>>(obj.data);

    if (propName == "readable") {
      LIGHTJS_RETURN(Value(streamPtr->readable));
    }

    if (propName == "writable") {
      LIGHTJS_RETURN(Value(streamPtr->writable));
    }
  }

  if (obj.isClass()) {
    auto clsPtr = std::get<std::shared_ptr<Class>>(obj.data);
    // Check own properties first (name, length, static methods, etc.)
    auto propIt = clsPtr->properties.find(propName);
    if (propIt != clsPtr->properties.end()) {
      LIGHTJS_RETURN(propIt->second);
    }
    auto methodIt = clsPtr->methods.find(propName);
    if (methodIt != clsPtr->methods.end()) {
      LIGHTJS_RETURN(Value(methodIt->second));
    }
    auto staticIt = clsPtr->staticMethods.find(propName);
    if (staticIt != clsPtr->staticMethods.end()) {
      LIGHTJS_RETURN(Value(staticIt->second));
    }
  }

  if (obj.isObject()) {
    auto objPtr = std::get<std::shared_ptr<Object>>(obj.data);

    // Deferred dynamic import namespace: trigger evaluation on first external property access.
    if (propName.rfind("__", 0) != 0) {
      auto pendingIt = objPtr->properties.find("__deferred_pending__");
      auto evalIt = objPtr->properties.find("__deferred_eval__");
      if (pendingIt != objPtr->properties.end() &&
          pendingIt->second.isBool() &&
          pendingIt->second.toBool() &&
          evalIt != objPtr->properties.end() &&
          evalIt->second.isFunction()) {
        auto deferredEvalFn = std::get<std::shared_ptr<Function>>(evalIt->second.data);
        invokeFunction(deferredEvalFn, {}, obj);
        if (hasError()) {
          LIGHTJS_RETURN(Value(Undefined{}));
        }
        objPtr->properties["__deferred_pending__"] = Value(false);
      }
    }

    // Check for getter first
    std::string getterName = "__get_" + propName;
    auto getterIt = objPtr->properties.find(getterName);
    if (getterIt != objPtr->properties.end() && getterIt->second.isFunction()) {
      auto getter = std::get<std::shared_ptr<Function>>(getterIt->second.data);
      // Call the getter with 'this' bound to the object
      LIGHTJS_RETURN(invokeFunction(getter, {}, obj));
    }

    // Direct property lookup
    auto it = objPtr->properties.find(propName);
    if (it != objPtr->properties.end()) {
      LIGHTJS_RETURN(it->second);
    }

    // Walk prototype chain (__proto__)
    {
      auto proto = objPtr;
      int depth = 0;
      while (depth < 50) {
        auto protoIt = proto->properties.find("__proto__");
        if (protoIt == proto->properties.end() || !protoIt->second.isObject()) break;
        proto = std::get<std::shared_ptr<Object>>(protoIt->second.data);
        depth++;
        // Check getter on prototype
        auto protoGetterIt = proto->properties.find("__get_" + propName);
        if (protoGetterIt != proto->properties.end() && protoGetterIt->second.isFunction()) {
          auto getter = std::get<std::shared_ptr<Function>>(protoGetterIt->second.data);
          LIGHTJS_RETURN(invokeFunction(getter, {}, obj));
        }
        auto propIt = proto->properties.find(propName);
        if (propIt != proto->properties.end()) {
          LIGHTJS_RETURN(propIt->second);
        }
      }
    }

    // Object.prototype methods - hasOwnProperty
    if (propName == "hasOwnProperty") {
      auto hopFn = std::make_shared<Function>();
      hopFn->isNative = true;
      hopFn->nativeFunc = [objPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(false);
        std::string key = args[0].toString();
        if (key.size() >= 4 && key.substr(0, 2) == "__" && key.substr(key.size() - 2) == "__") return Value(false);
        if (key.size() > 6 && (key.substr(0, 6) == "__get_" || key.substr(0, 6) == "__set_")) return Value(false);
        if (key.size() > 10 && key.substr(0, 10) == "__non_enum") return Value(false);
        if (key.size() > 14 && key.substr(0, 14) == "__non_writable") return Value(false);
        if (key.size() > 18 && key.substr(0, 18) == "__non_configurable") return Value(false);
        return Value(objPtr->properties.count(key) > 0);
      };
      LIGHTJS_RETURN(Value(hopFn));
    }

    // Some constructor singletons (notably Array) store prototype metadata
    // separately for runtime compatibility.
    if (propName == "prototype") {
      if (auto arrayValue = env_->get("Array");
          arrayValue && arrayValue->isObject() &&
          std::get<std::shared_ptr<Object>>(arrayValue->data).get() == objPtr.get()) {
        if (auto hiddenArrayProto = env_->get("__array_prototype__")) {
          LIGHTJS_RETURN(*hiddenArrayProto);
        }
      }
    }
  }

  if (obj.isFunction()) {
    auto funcPtr = std::get<std::shared_ptr<Function>>(obj.data);

    if (propName == "call") {
      auto callFn = std::make_shared<Function>();
      callFn->isNative = true;
      callFn->properties["__throw_on_new__"] = Value(true);
      callFn->properties["name"] = Value(std::string("call"));
      callFn->properties["length"] = Value(1.0);
      callFn->nativeFunc = [this, funcPtr](const std::vector<Value>& args) -> Value {
        Value thisArg = args.empty() ? Value(Undefined{}) : args[0];
        std::vector<Value> callArgs;
        if (args.size() > 1) {
          callArgs.insert(callArgs.end(), args.begin() + 1, args.end());
        }
        return callFunction(Value(funcPtr), callArgs, thisArg);
      };
      LIGHTJS_RETURN(Value(callFn));
    }

    if (propName == "apply") {
      auto applyFn = std::make_shared<Function>();
      applyFn->isNative = true;
      applyFn->properties["__throw_on_new__"] = Value(true);
      applyFn->properties["name"] = Value(std::string("apply"));
      applyFn->properties["length"] = Value(2.0);
      applyFn->nativeFunc = [this, funcPtr](const std::vector<Value>& args) -> Value {
        Value thisArg = args.empty() ? Value(Undefined{}) : args[0];
        std::vector<Value> callArgs;
        if (args.size() > 1 && args[1].isArray()) {
          auto arr = std::get<std::shared_ptr<Array>>(args[1].data);
          callArgs = arr->elements;
        }
        return callFunction(Value(funcPtr), callArgs, thisArg);
      };
      LIGHTJS_RETURN(Value(applyFn));
    }

    if (propName == "bind") {
      auto bindFn = std::make_shared<Function>();
      bindFn->isNative = true;
      bindFn->properties["__throw_on_new__"] = Value(true);
      bindFn->properties["name"] = Value(std::string("bind"));
      bindFn->properties["length"] = Value(1.0);
      bindFn->nativeFunc = [this, funcPtr](const std::vector<Value>& args) -> Value {
        Value boundThis = args.empty() ? Value(Undefined{}) : args[0];
        std::vector<Value> boundArgs;
        if (args.size() > 1) {
          boundArgs.insert(boundArgs.end(), args.begin() + 1, args.end());
        }
        auto boundFn = std::make_shared<Function>();
        boundFn->isNative = true;
        boundFn->properties["name"] = Value(std::string("bound " + (funcPtr->properties.count("name") ? funcPtr->properties["name"].toString() : "")));
        auto capturedThis = this;
        auto capturedFuncPtr = funcPtr;
        auto capturedBoundThis = boundThis;
        auto capturedBoundArgs = boundArgs;
        boundFn->nativeFunc = [capturedThis, capturedFuncPtr, capturedBoundThis, capturedBoundArgs](const std::vector<Value>& callArgs) -> Value {
          std::vector<Value> finalArgs = capturedBoundArgs;
          finalArgs.insert(finalArgs.end(), callArgs.begin(), callArgs.end());
          return capturedThis->callFunction(Value(capturedFuncPtr), finalArgs, capturedBoundThis);
        };
        return Value(boundFn);
      };
      LIGHTJS_RETURN(Value(bindFn));
    }

    // hasOwnProperty from Object.prototype
    if (propName == "hasOwnProperty") {
      auto hopFn = std::make_shared<Function>();
      hopFn->isNative = true;
      hopFn->nativeFunc = [funcPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(false);
        std::string key = args[0].toString();
        // Internal properties are not visible
        if (key.size() >= 4 && key.substr(0, 2) == "__" && key.substr(key.size() - 2) == "__") return Value(false);
        if (key.size() > 6 && (key.substr(0, 6) == "__get_" || key.substr(0, 6) == "__set_")) return Value(false);
        if (key.size() > 10 && key.substr(0, 10) == "__non_enum") return Value(false);
        if (key.size() > 14 && key.substr(0, 14) == "__non_writable") return Value(false);
        if (key.size() > 18 && key.substr(0, 18) == "__non_configurable") return Value(false);
        return Value(funcPtr->properties.count(key) > 0);
      };
      LIGHTJS_RETURN(Value(hopFn));
    }

    auto it = funcPtr->properties.find(propName);
    if (it != funcPtr->properties.end()) {
      LIGHTJS_RETURN(it->second);
    }

    // Walk prototype chain (__proto__) for functions
    {
      auto protoIt = funcPtr->properties.find("__proto__");
      if (protoIt != funcPtr->properties.end() && protoIt->second.isObject()) {
        auto proto = std::get<std::shared_ptr<Object>>(protoIt->second.data);
        int depth = 0;
        while (proto && depth < 50) {
          auto protoGetterIt = proto->properties.find("__get_" + propName);
          if (protoGetterIt != proto->properties.end() && protoGetterIt->second.isFunction()) {
            auto getter = std::get<std::shared_ptr<Function>>(protoGetterIt->second.data);
            LIGHTJS_RETURN(invokeFunction(getter, {}, obj));
          }
          auto found = proto->properties.find(propName);
          if (found != proto->properties.end()) {
            LIGHTJS_RETURN(found->second);
          }
          auto nextProto = proto->properties.find("__proto__");
          if (nextProto == proto->properties.end() || !nextProto->second.isObject()) break;
          proto = std::get<std::shared_ptr<Object>>(nextProto->second.data);
          depth++;
        }
      }
    }
  }

  // Generator methods
  if (obj.isGenerator()) {
    auto genPtr = std::get<std::shared_ptr<Generator>>(obj.data);
    bool isAsyncGenerator = genPtr->function && genPtr->function->isAsync;
    const auto& asyncIteratorKey = WellKnownSymbols::asyncIteratorKey();

    if (propName == iteratorKey) {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [genPtr](const std::vector<Value>&) -> Value {
        return Value(genPtr);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == asyncIteratorKey && isAsyncGenerator) {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [genPtr](const std::vector<Value>&) -> Value {
        return Value(genPtr);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "next") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [genPtr, this](const std::vector<Value>& args) -> Value {
        Value resumeValue = args.empty() ? Value(Undefined{}) : args[0];
        auto mode = ControlFlow::ResumeMode::Next;
        if (genPtr->state == GeneratorState::SuspendedStart) {
          resumeValue = Value(Undefined{});
        }
        Value step = this->runGeneratorNext(genPtr, mode, resumeValue);
        if (genPtr->function && genPtr->function->isAsync) {
          auto promise = std::make_shared<Promise>();
          if (this->flow_.type == ControlFlow::Type::Throw) {
            Value rejection = this->flow_.value;
            this->clearError();
            promise->reject(rejection);
          } else {
            promise->resolve(step);
          }
          return Value(promise);
        }
        return step;
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "return") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [genPtr](const std::vector<Value>& args) -> Value {
        Value returnValue = args.empty() ? Value(Undefined{}) : args[0];
        genPtr->state = GeneratorState::Completed;
        genPtr->currentValue = std::make_shared<Value>(returnValue);
        Value step = makeIteratorResult(returnValue, true);
        if (genPtr->function && genPtr->function->isAsync) {
          auto promise = std::make_shared<Promise>();
          promise->resolve(step);
          return Value(promise);
        }
        return step;
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "throw") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [genPtr](const std::vector<Value>& args) -> Value {
        genPtr->state = GeneratorState::Completed;
        std::string msg = args.empty() ? "Generator error" : args[0].toString();
        if (genPtr->function && genPtr->function->isAsync) {
          auto promise = std::make_shared<Promise>();
          promise->reject(Value(std::make_shared<Error>(ErrorType::Error, msg)));
          return Value(promise);
        }
        return Value(std::make_shared<Error>(ErrorType::Error, msg));
      };
      LIGHTJS_RETURN(Value(fn));
    }
  }

  // Array iterator helpers continue below
  if (obj.isArray()) {
    auto arrPtr = std::get<std::shared_ptr<Array>>(obj.data);
    if (propName == "length") {
      LIGHTJS_RETURN(Value(static_cast<double>(arrPtr->elements.size())));
    }

    if (propName == iteratorKey) {
      LIGHTJS_RETURN(createIteratorFactory(arrPtr));
    }

    // Array higher-order methods
    // Create a special native function that captures the array and method name
    if (propName == "map") {
      auto mapFn = std::make_shared<Function>();
      mapFn->isNative = true;
      mapFn->nativeFunc = [this, arrPtr](const std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].isFunction()) {
          return Value(std::make_shared<Error>(ErrorType::TypeError, "map requires a callback function"));
        }
        auto callback = std::get<std::shared_ptr<Function>>(args[0].data);
        auto result = std::make_shared<Array>();
        GarbageCollector::instance().reportAllocation(sizeof(Array));

        // Get thisArg if provided
        Value thisArg = args.size() > 1 ? args[1] : Value(Undefined{});

        for (size_t i = 0; i < arrPtr->elements.size(); ++i) {
          // Call the callback function with (element, index, array)
          std::vector<Value> callArgs = {arrPtr->elements[i], Value(static_cast<double>(i)), Value(arrPtr)};
          Value mapped = invokeFunction(callback, callArgs, thisArg);
          result->elements.push_back(mapped);
        }
        return Value(result);
      };
      LIGHTJS_RETURN(Value(mapFn));
    }

    if (propName == "filter") {
      auto filterFn = std::make_shared<Function>();
      filterFn->isNative = true;
      filterFn->nativeFunc = [this, arrPtr](const std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].isFunction()) {
          return Value(std::make_shared<Error>(ErrorType::TypeError, "filter requires a callback function"));
        }
        auto callback = std::get<std::shared_ptr<Function>>(args[0].data);
        auto result = std::make_shared<Array>();
        GarbageCollector::instance().reportAllocation(sizeof(Array));

        // Get thisArg if provided
        Value thisArg = args.size() > 1 ? args[1] : Value(Undefined{});

        for (size_t i = 0; i < arrPtr->elements.size(); ++i) {
          std::vector<Value> callArgs = {arrPtr->elements[i], Value(static_cast<double>(i)), Value(arrPtr)};
          Value keep = invokeFunction(callback, callArgs, thisArg);
          if (keep.toBool()) {
            result->elements.push_back(arrPtr->elements[i]);
          }
        }
        return Value(result);
      };
      LIGHTJS_RETURN(Value(filterFn));
    }

    if (propName == "forEach") {
      auto forEachFn = std::make_shared<Function>();
      forEachFn->isNative = true;
      forEachFn->nativeFunc = [this, arrPtr](const std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].isFunction()) {
          return Value(std::make_shared<Error>(ErrorType::TypeError, "forEach requires a callback function"));
        }
        auto callback = std::get<std::shared_ptr<Function>>(args[0].data);

        // Get thisArg if provided
        Value thisArg = args.size() > 1 ? args[1] : Value(Undefined{});

        for (size_t i = 0; i < arrPtr->elements.size(); ++i) {
          std::vector<Value> callArgs = {arrPtr->elements[i], Value(static_cast<double>(i)), Value(arrPtr)};
          invokeFunction(callback, callArgs, thisArg);
        }
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(forEachFn));
    }

    if (propName == "reduce") {
      auto reduceFn = std::make_shared<Function>();
      reduceFn->isNative = true;
      reduceFn->nativeFunc = [this, arrPtr](const std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].isFunction()) {
          return Value(std::make_shared<Error>(ErrorType::TypeError, "reduce requires a callback function"));
        }
        auto callback = std::get<std::shared_ptr<Function>>(args[0].data);

        if (arrPtr->elements.empty()) {
          return args.size() > 1 ? args[1] : Value(Undefined{});
        }

        Value accumulator = args.size() > 1 ? args[1] : arrPtr->elements[0];
        size_t start = args.size() > 1 ? 0 : 1;

        for (size_t i = start; i < arrPtr->elements.size(); ++i) {
          // reduce callback: (accumulator, currentValue, currentIndex, array)
          std::vector<Value> callArgs = {accumulator, arrPtr->elements[i], Value(static_cast<double>(i)), Value(arrPtr)};
          accumulator = invokeFunction(callback, callArgs, Value(Undefined{}));
        }
        return accumulator;
      };
      LIGHTJS_RETURN(Value(reduceFn));
    }

    if (propName == "reduceRight") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [this, arrPtr](const std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].isFunction()) {
          return Value(std::make_shared<Error>(ErrorType::TypeError, "reduceRight requires a callback function"));
        }
        auto callback = std::get<std::shared_ptr<Function>>(args[0].data);

        if (arrPtr->elements.empty()) {
          return args.size() > 1 ? args[1] : Value(Undefined{});
        }

        size_t len = arrPtr->elements.size();
        Value accumulator = args.size() > 1 ? args[1] : arrPtr->elements[len - 1];
        int start = args.size() > 1 ? static_cast<int>(len) - 1 : static_cast<int>(len) - 2;

        for (int i = start; i >= 0; --i) {
          std::vector<Value> callArgs = {accumulator, arrPtr->elements[i], Value(static_cast<double>(i)), Value(arrPtr)};
          accumulator = invokeFunction(callback, callArgs, Value(Undefined{}));
        }
        return accumulator;
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "find") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [this, arrPtr](const std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].isFunction()) {
          return Value(std::make_shared<Error>(ErrorType::TypeError, "find requires a callback function"));
        }
        auto callback = std::get<std::shared_ptr<Function>>(args[0].data);
        Value thisArg = args.size() > 1 ? args[1] : Value(Undefined{});

        for (size_t i = 0; i < arrPtr->elements.size(); ++i) {
          std::vector<Value> callArgs = {arrPtr->elements[i], Value(static_cast<double>(i)), Value(arrPtr)};
          if (invokeFunction(callback, callArgs, thisArg).toBool()) {
            return arrPtr->elements[i];
          }
        }
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "findIndex") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [this, arrPtr](const std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].isFunction()) {
          return Value(std::make_shared<Error>(ErrorType::TypeError, "findIndex requires a callback function"));
        }
        auto callback = std::get<std::shared_ptr<Function>>(args[0].data);
        Value thisArg = args.size() > 1 ? args[1] : Value(Undefined{});

        for (size_t i = 0; i < arrPtr->elements.size(); ++i) {
          std::vector<Value> callArgs = {arrPtr->elements[i], Value(static_cast<double>(i)), Value(arrPtr)};
          if (invokeFunction(callback, callArgs, thisArg).toBool()) {
            return Value(static_cast<double>(i));
          }
        }
        return Value(-1.0);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "findLast") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [this, arrPtr](const std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].isFunction()) {
          return Value(std::make_shared<Error>(ErrorType::TypeError, "findLast requires a callback function"));
        }
        auto callback = std::get<std::shared_ptr<Function>>(args[0].data);
        Value thisArg = args.size() > 1 ? args[1] : Value(Undefined{});

        for (size_t i = arrPtr->elements.size(); i > 0; --i) {
          size_t idx = i - 1;
          std::vector<Value> callArgs = {arrPtr->elements[idx], Value(static_cast<double>(idx)), Value(arrPtr)};
          if (invokeFunction(callback, callArgs, thisArg).toBool()) {
            return arrPtr->elements[idx];
          }
        }
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "findLastIndex") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [this, arrPtr](const std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].isFunction()) {
          return Value(std::make_shared<Error>(ErrorType::TypeError, "findLastIndex requires a callback function"));
        }
        auto callback = std::get<std::shared_ptr<Function>>(args[0].data);
        Value thisArg = args.size() > 1 ? args[1] : Value(Undefined{});

        for (size_t i = arrPtr->elements.size(); i > 0; --i) {
          size_t idx = i - 1;
          std::vector<Value> callArgs = {arrPtr->elements[idx], Value(static_cast<double>(idx)), Value(arrPtr)};
          if (invokeFunction(callback, callArgs, thisArg).toBool()) {
            return Value(static_cast<double>(idx));
          }
        }
        return Value(-1.0);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "some") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [this, arrPtr](const std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].isFunction()) {
          return Value(std::make_shared<Error>(ErrorType::TypeError, "some requires a callback function"));
        }
        auto callback = std::get<std::shared_ptr<Function>>(args[0].data);
        Value thisArg = args.size() > 1 ? args[1] : Value(Undefined{});

        for (size_t i = 0; i < arrPtr->elements.size(); ++i) {
          std::vector<Value> callArgs = {arrPtr->elements[i], Value(static_cast<double>(i)), Value(arrPtr)};
          if (invokeFunction(callback, callArgs, thisArg).toBool()) {
            return Value(true);
          }
        }
        return Value(false);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "every") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [this, arrPtr](const std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].isFunction()) {
          return Value(std::make_shared<Error>(ErrorType::TypeError, "every requires a callback function"));
        }
        auto callback = std::get<std::shared_ptr<Function>>(args[0].data);
        Value thisArg = args.size() > 1 ? args[1] : Value(Undefined{});

        for (size_t i = 0; i < arrPtr->elements.size(); ++i) {
          std::vector<Value> callArgs = {arrPtr->elements[i], Value(static_cast<double>(i)), Value(arrPtr)};
          if (!invokeFunction(callback, callArgs, thisArg).toBool()) {
            return Value(false);
          }
        }
        return Value(true);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "push") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        for (const auto& arg : args) {
          arrPtr->elements.push_back(arg);
        }
        return Value(static_cast<double>(arrPtr->elements.size()));
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "pop") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        if (arrPtr->elements.empty()) {
          return Value(Undefined{});
        }
        Value result = arrPtr->elements.back();
        arrPtr->elements.pop_back();
        return result;
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "shift") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        if (arrPtr->elements.empty()) {
          return Value(Undefined{});
        }
        Value result = arrPtr->elements.front();
        arrPtr->elements.erase(arrPtr->elements.begin());
        return result;
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "unshift") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        for (size_t i = 0; i < args.size(); ++i) {
          arrPtr->elements.insert(arrPtr->elements.begin() + i, args[i]);
        }
        return Value(static_cast<double>(arrPtr->elements.size()));
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "slice") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        auto result = std::make_shared<Array>();
        GarbageCollector::instance().reportAllocation(sizeof(Array));

        int len = static_cast<int>(arrPtr->elements.size());
        int start = 0;
        int end = len;

        if (args.size() > 0 && args[0].isNumber()) {
          start = static_cast<int>(std::get<double>(args[0].data));
          if (start < 0) start = std::max(0, len + start);
          if (start > len) start = len;
        }

        if (args.size() > 1 && args[1].isNumber()) {
          end = static_cast<int>(std::get<double>(args[1].data));
          if (end < 0) end = std::max(0, len + end);
          if (end > len) end = len;
        }

        for (int i = start; i < end; ++i) {
          result->elements.push_back(arrPtr->elements[i]);
        }
        return Value(result);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "splice") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        auto removed = std::make_shared<Array>();
        GarbageCollector::instance().reportAllocation(sizeof(Array));

        int len = static_cast<int>(arrPtr->elements.size());
        int start = 0;
        int deleteCount = len;

        if (args.size() > 0 && args[0].isNumber()) {
          start = static_cast<int>(std::get<double>(args[0].data));
          if (start < 0) start = std::max(0, len + start);
          if (start > len) start = len;
        }

        if (args.size() > 1 && args[1].isNumber()) {
          deleteCount = std::max(0, static_cast<int>(std::get<double>(args[1].data)));
          deleteCount = std::min(deleteCount, len - start);
        }

        // Remove elements
        for (int i = 0; i < deleteCount; ++i) {
          removed->elements.push_back(arrPtr->elements[start]);
          arrPtr->elements.erase(arrPtr->elements.begin() + start);
        }

        // Insert new elements
        for (size_t i = 2; i < args.size(); ++i) {
          arrPtr->elements.insert(arrPtr->elements.begin() + start + (i - 2), args[i]);
        }

        return Value(removed);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "toSpliced") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        auto result = std::make_shared<Array>();
        GarbageCollector::instance().reportAllocation(sizeof(Array));

        // Copy all elements to new array
        for (const auto& elem : arrPtr->elements) {
          result->elements.push_back(elem);
        }

        int len = static_cast<int>(result->elements.size());
        int start = 0;
        int deleteCount = 0;

        if (args.size() > 0 && args[0].isNumber()) {
          start = static_cast<int>(std::get<double>(args[0].data));
          if (start < 0) start = std::max(0, len + start);
          if (start > len) start = len;
        }

        if (args.size() > 1 && args[1].isNumber()) {
          deleteCount = std::max(0, static_cast<int>(std::get<double>(args[1].data)));
          deleteCount = std::min(deleteCount, len - start);
        }

        // Remove elements
        for (int i = 0; i < deleteCount; ++i) {
          result->elements.erase(result->elements.begin() + start);
        }

        // Insert new elements
        for (size_t i = 2; i < args.size(); ++i) {
          result->elements.insert(result->elements.begin() + start + (i - 2), args[i]);
        }

        return Value(result);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "join") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        std::string separator = ",";
        if (args.size() > 0) {
          separator = args[0].toString();
        }

        std::string result;
        for (size_t i = 0; i < arrPtr->elements.size(); ++i) {
          if (i > 0) result += separator;
          if (!arrPtr->elements[i].isUndefined() && !arrPtr->elements[i].isNull()) {
            result += arrPtr->elements[i].toString();
          }
        }
        return Value(result);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "indexOf") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(-1.0);

        Value searchElement = args[0];
        int fromIndex = 0;

        if (args.size() > 1 && args[1].isNumber()) {
          fromIndex = static_cast<int>(std::get<double>(args[1].data));
          int len = static_cast<int>(arrPtr->elements.size());
          if (fromIndex < 0) fromIndex = std::max(0, len + fromIndex);
        }

        for (size_t i = fromIndex; i < arrPtr->elements.size(); ++i) {
          if (arrPtr->elements[i].toString() == searchElement.toString()) {
            return Value(static_cast<double>(i));
          }
        }
        return Value(-1.0);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "lastIndexOf") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(-1.0);

        Value searchElement = args[0];
        int len = static_cast<int>(arrPtr->elements.size());
        int fromIndex = len - 1;

        if (args.size() > 1 && args[1].isNumber()) {
          fromIndex = static_cast<int>(std::get<double>(args[1].data));
          if (fromIndex < 0) fromIndex = len + fromIndex;
          if (fromIndex >= len) fromIndex = len - 1;
        }

        for (int i = fromIndex; i >= 0; --i) {
          if (arrPtr->elements[i].toString() == searchElement.toString()) {
            return Value(static_cast<double>(i));
          }
        }
        return Value(-1.0);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "includes") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(false);

        Value searchElement = args[0];
        int fromIndex = 0;

        if (args.size() > 1 && args[1].isNumber()) {
          fromIndex = static_cast<int>(std::get<double>(args[1].data));
          int len = static_cast<int>(arrPtr->elements.size());
          if (fromIndex < 0) fromIndex = std::max(0, len + fromIndex);
        }

        for (size_t i = fromIndex; i < arrPtr->elements.size(); ++i) {
          if (arrPtr->elements[i].toString() == searchElement.toString()) {
            return Value(true);
          }
        }
        return Value(false);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "at") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(Undefined{});
        int index = static_cast<int>(args[0].toNumber());
        int len = static_cast<int>(arrPtr->elements.size());
        if (index < 0) index = len + index;
        if (index < 0 || index >= len) return Value(Undefined{});
        return arrPtr->elements[index];
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "reverse") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        std::reverse(arrPtr->elements.begin(), arrPtr->elements.end());
        return Value(arrPtr);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "sort") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr, this](const std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].isFunction()) {
          // Default sort: convert to strings and compare lexicographically
          std::sort(arrPtr->elements.begin(), arrPtr->elements.end(),
            [](const Value& a, const Value& b) {
              return a.toString() < b.toString();
            });
        } else {
          // Sort with comparator function
          auto compareFn = std::get<std::shared_ptr<Function>>(args[0].data);
          std::sort(arrPtr->elements.begin(), arrPtr->elements.end(),
            [compareFn, this](const Value& a, const Value& b) {
              std::vector<Value> compareArgs = {a, b};
              Value result;
              if (compareFn->isNative) {
                result = compareFn->nativeFunc(compareArgs);
              } else {
                result = this->invokeFunction(compareFn, compareArgs);
              }
              return result.toNumber() < 0;
            });
        }
        return Value(arrPtr);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "toSorted") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr, this](const std::vector<Value>& args) -> Value {
        auto result = std::make_shared<Array>();
        GarbageCollector::instance().reportAllocation(sizeof(Array));
        result->elements = arrPtr->elements;  // Copy

        if (args.empty() || !args[0].isFunction()) {
          std::sort(result->elements.begin(), result->elements.end(),
            [](const Value& a, const Value& b) {
              return a.toString() < b.toString();
            });
        } else {
          auto compareFn = std::get<std::shared_ptr<Function>>(args[0].data);
          std::sort(result->elements.begin(), result->elements.end(),
            [compareFn, this](const Value& a, const Value& b) {
              std::vector<Value> compareArgs = {a, b};
              Value r;
              if (compareFn->isNative) {
                r = compareFn->nativeFunc(compareArgs);
              } else {
                r = this->invokeFunction(compareFn, compareArgs);
              }
              return r.toNumber() < 0;
            });
        }
        return Value(result);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "toReversed") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        auto result = std::make_shared<Array>();
        GarbageCollector::instance().reportAllocation(sizeof(Array));
        result->elements = arrPtr->elements;
        std::reverse(result->elements.begin(), result->elements.end());
        return Value(result);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "at") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(Undefined{});
        int index = static_cast<int>(args[0].toNumber());
        int size = static_cast<int>(arrPtr->elements.size());
        if (index < 0) index = size + index;
        if (index < 0 || index >= size) return Value(Undefined{});
        return arrPtr->elements[index];
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "with") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(arrPtr);
        int index = static_cast<int>(args[0].toNumber());
        int size = static_cast<int>(arrPtr->elements.size());
        if (index < 0) index = size + index;
        if (index < 0 || index >= size) {
          return Value(std::make_shared<Error>(ErrorType::RangeError, "Invalid index"));
        }
        auto result = std::make_shared<Array>();
        GarbageCollector::instance().reportAllocation(sizeof(Array));
        result->elements = arrPtr->elements;
        result->elements[index] = args[1];
        return Value(result);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "concat") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        auto result = std::make_shared<Array>();
        GarbageCollector::instance().reportAllocation(sizeof(Array));

        // Copy original array elements
        result->elements = arrPtr->elements;

        // Add all arguments
        for (const auto& arg : args) {
          if (arg.isArray()) {
            auto otherArr = std::get<std::shared_ptr<Array>>(arg.data);
            result->elements.insert(result->elements.end(),
                                  otherArr->elements.begin(),
                                  otherArr->elements.end());
          } else {
            result->elements.push_back(arg);
          }
        }

        return Value(result);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "flat") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        int depth = 1;
        if (args.size() > 0 && args[0].isNumber()) {
          depth = static_cast<int>(std::get<double>(args[0].data));
        }

        std::function<void(const std::vector<Value>&, int, std::vector<Value>&)> flattenImpl;
        flattenImpl = [&flattenImpl](const std::vector<Value>& src, int d, std::vector<Value>& dest) {
          for (const auto& elem : src) {
            if (d > 0 && elem.isArray()) {
              auto inner = std::get<std::shared_ptr<Array>>(elem.data);
              flattenImpl(inner->elements, d - 1, dest);
            } else {
              dest.push_back(elem);
            }
          }
        };

        auto result = std::make_shared<Array>();
        GarbageCollector::instance().reportAllocation(sizeof(Array));
        flattenImpl(arrPtr->elements, depth, result->elements);
        return Value(result);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "flatMap") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [this, arrPtr](const std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].isFunction()) {
          return Value(std::make_shared<Error>(ErrorType::TypeError, "flatMap requires a callback function"));
        }
        auto callback = std::get<std::shared_ptr<Function>>(args[0].data);
        Value thisArg = args.size() > 1 ? args[1] : Value(Undefined{});

        auto result = std::make_shared<Array>();
        GarbageCollector::instance().reportAllocation(sizeof(Array));

        for (size_t i = 0; i < arrPtr->elements.size(); ++i) {
          std::vector<Value> callArgs = {arrPtr->elements[i], Value(static_cast<double>(i)), Value(arrPtr)};
          Value mapped = invokeFunction(callback, callArgs, thisArg);

          if (mapped.isArray()) {
            auto inner = std::get<std::shared_ptr<Array>>(mapped.data);
            result->elements.insert(result->elements.end(),
                                  inner->elements.begin(),
                                  inner->elements.end());
          } else {
            result->elements.push_back(mapped);
          }
        }
        return Value(result);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "fill") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(arrPtr);

        Value fillValue = args[0];
        int len = static_cast<int>(arrPtr->elements.size());
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

        for (int i = start; i < end; ++i) {
          arrPtr->elements[i] = fillValue;
        }
        return Value(arrPtr);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "copyWithin") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(arrPtr);

        int len = static_cast<int>(arrPtr->elements.size());
        int target = static_cast<int>(std::get<double>(args[0].data));
        if (target < 0) target = std::max(0, len + target);

        int start = 0;
        if (args.size() > 1 && args[1].isNumber()) {
          start = static_cast<int>(std::get<double>(args[1].data));
          if (start < 0) start = std::max(0, len + start);
        }

        int end = len;
        if (args.size() > 2 && args[2].isNumber()) {
          end = static_cast<int>(std::get<double>(args[2].data));
          if (end < 0) end = std::max(0, len + end);
        }

        int count = std::min(end - start, len - target);
        std::vector<Value> temp(arrPtr->elements.begin() + start, arrPtr->elements.begin() + start + count);

        for (int i = 0; i < count && target + i < len; ++i) {
          arrPtr->elements[target + i] = temp[i];
        }
        return Value(arrPtr);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "keys") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        auto iterObj = std::make_shared<Object>();
        auto indexPtr = std::make_shared<size_t>(0);
        auto nextFn = std::make_shared<Function>();
        nextFn->isNative = true;
        nextFn->nativeFunc = [arrPtr, indexPtr](const std::vector<Value>&) -> Value {
          auto result = std::make_shared<Object>();
          if (*indexPtr >= arrPtr->elements.size()) {
            result->properties["value"] = Value(Undefined{});
            result->properties["done"] = Value(true);
          } else {
            result->properties["value"] = Value(static_cast<double>((*indexPtr)++));
            result->properties["done"] = Value(false);
          }
          return Value(result);
        };
        iterObj->properties["next"] = Value(nextFn);
        return Value(iterObj);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "entries") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        auto iterObj = std::make_shared<Object>();
        auto indexPtr = std::make_shared<size_t>(0);
        auto nextFn = std::make_shared<Function>();
        nextFn->isNative = true;
        nextFn->nativeFunc = [arrPtr, indexPtr](const std::vector<Value>&) -> Value {
          auto result = std::make_shared<Object>();
          if (*indexPtr >= arrPtr->elements.size()) {
            result->properties["value"] = Value(Undefined{});
            result->properties["done"] = Value(true);
          } else {
            auto pair = std::make_shared<Array>();
            pair->elements.push_back(Value(static_cast<double>(*indexPtr)));
            pair->elements.push_back(arrPtr->elements[*indexPtr]);
            (*indexPtr)++;
            result->properties["value"] = Value(pair);
            result->properties["done"] = Value(false);
          }
          return Value(result);
        };
        iterObj->properties["next"] = Value(nextFn);
        return Value(iterObj);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "values") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [arrPtr](const std::vector<Value>& args) -> Value {
        auto iterObj = std::make_shared<Object>();
        auto indexPtr = std::make_shared<size_t>(0);
        auto nextFn = std::make_shared<Function>();
        nextFn->isNative = true;
        nextFn->nativeFunc = [arrPtr, indexPtr](const std::vector<Value>&) -> Value {
          auto result = std::make_shared<Object>();
          if (*indexPtr >= arrPtr->elements.size()) {
            result->properties["value"] = Value(Undefined{});
            result->properties["done"] = Value(true);
          } else {
            result->properties["value"] = arrPtr->elements[(*indexPtr)++];
            result->properties["done"] = Value(false);
          }
          return Value(result);
        };
        iterObj->properties["next"] = Value(nextFn);
        return Value(iterObj);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    size_t idx = 0;
    if (parseArrayIndex(propName, idx) && idx < arrPtr->elements.size()) {
      LIGHTJS_RETURN(arrPtr->elements[idx]);
    }
    auto propIt = arrPtr->properties.find(propName);
    if (propIt != arrPtr->properties.end()) {
      LIGHTJS_RETURN(propIt->second);
    }

    // Walk prototype chain (__proto__) for arrays
    {
      auto protoIt = arrPtr->properties.find("__proto__");
      if (protoIt != arrPtr->properties.end() && protoIt->second.isObject()) {
        auto proto = std::get<std::shared_ptr<Object>>(protoIt->second.data);
        int depth = 0;
        while (proto && depth < 50) {
          // Check getter on prototype
          auto protoGetterIt = proto->properties.find("__get_" + propName);
          if (protoGetterIt != proto->properties.end() && protoGetterIt->second.isFunction()) {
            auto getter = std::get<std::shared_ptr<Function>>(protoGetterIt->second.data);
            LIGHTJS_RETURN(invokeFunction(getter, {}, obj));
          }
          auto found = proto->properties.find(propName);
          if (found != proto->properties.end()) {
            LIGHTJS_RETURN(found->second);
          }
          auto nextProto = proto->properties.find("__proto__");
          if (nextProto == proto->properties.end() || !nextProto->second.isObject()) break;
          proto = std::get<std::shared_ptr<Object>>(nextProto->second.data);
          depth++;
        }
      }
    }
  }

  if (obj.isMap()) {
    auto mapPtr = std::get<std::shared_ptr<Map>>(obj.data);
    if (propName == "size") {
      LIGHTJS_RETURN(Value(static_cast<double>(mapPtr->size())));
    }
    if (propName == "set") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [mapPtr](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) {
          return Value(mapPtr);
        }
        mapPtr->set(args[0], args[1]);
        return Value(mapPtr);
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "get") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [mapPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(Undefined{});
        return mapPtr->get(args[0]);
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "has") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [mapPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(false);
        return Value(mapPtr->has(args[0]));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "delete") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [mapPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(false);
        return Value(mapPtr->deleteKey(args[0]));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "clear") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [mapPtr](const std::vector<Value>&) -> Value {
        mapPtr->clear();
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "forEach") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [this, mapPtr](const std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].isFunction()) {
          return Value(std::make_shared<Error>(ErrorType::TypeError, "forEach requires a callback function"));
        }
        auto callback = std::get<std::shared_ptr<Function>>(args[0].data);
        Value thisArg = args.size() > 1 ? args[1] : Value(Undefined{});
        for (size_t i = 0; i < mapPtr->entries.size(); ++i) {
          auto& entry = mapPtr->entries[i];
          invokeFunction(callback, {entry.second, entry.first, Value(mapPtr)}, thisArg);
        }
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "entries" || propName == iteratorKey) {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [mapPtr](const std::vector<Value>&) -> Value {
        auto iterObj = std::make_shared<Object>();
        auto indexPtr = std::make_shared<size_t>(0);
        auto nextFn = std::make_shared<Function>();
        nextFn->isNative = true;
        nextFn->nativeFunc = [mapPtr, indexPtr](const std::vector<Value>&) -> Value {
          if (*indexPtr >= mapPtr->entries.size()) {
            return makeIteratorResult(Value(Undefined{}), true);
          }
          auto& entry = mapPtr->entries[*indexPtr];
          auto pair = std::make_shared<Array>();
          pair->elements.push_back(entry.first);
          pair->elements.push_back(entry.second);
          (*indexPtr)++;
          return makeIteratorResult(Value(pair), false);
        };
        iterObj->properties["next"] = Value(nextFn);
        return Value(iterObj);
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "keys") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [mapPtr](const std::vector<Value>&) -> Value {
        auto iterObj = std::make_shared<Object>();
        auto indexPtr = std::make_shared<size_t>(0);
        auto nextFn = std::make_shared<Function>();
        nextFn->isNative = true;
        nextFn->nativeFunc = [mapPtr, indexPtr](const std::vector<Value>&) -> Value {
          if (*indexPtr >= mapPtr->entries.size()) {
            return makeIteratorResult(Value(Undefined{}), true);
          }
          Value key = mapPtr->entries[*indexPtr].first;
          (*indexPtr)++;
          return makeIteratorResult(key, false);
        };
        iterObj->properties["next"] = Value(nextFn);
        return Value(iterObj);
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "values") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [mapPtr](const std::vector<Value>&) -> Value {
        auto iterObj = std::make_shared<Object>();
        auto indexPtr = std::make_shared<size_t>(0);
        auto nextFn = std::make_shared<Function>();
        nextFn->isNative = true;
        nextFn->nativeFunc = [mapPtr, indexPtr](const std::vector<Value>&) -> Value {
          if (*indexPtr >= mapPtr->entries.size()) {
            return makeIteratorResult(Value(Undefined{}), true);
          }
          Value val = mapPtr->entries[*indexPtr].second;
          (*indexPtr)++;
          return makeIteratorResult(val, false);
        };
        iterObj->properties["next"] = Value(nextFn);
        return Value(iterObj);
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "constructor") {
      if (auto ctor = env_->get("Map")) {
        LIGHTJS_RETURN(*ctor);
      }
      LIGHTJS_RETURN(Value(Undefined{}));
    }
  }

  if (obj.isSet()) {
    auto setPtr = std::get<std::shared_ptr<Set>>(obj.data);
    if (propName == "size") {
      LIGHTJS_RETURN(Value(static_cast<double>(setPtr->size())));
    }
    if (propName == "add") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [setPtr](const std::vector<Value>& args) -> Value {
        if (!args.empty()) {
          setPtr->add(args[0]);
        }
        return Value(setPtr);
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "has") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [setPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(false);
        return Value(setPtr->has(args[0]));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "delete") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [setPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(false);
        return Value(setPtr->deleteValue(args[0]));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "clear") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [setPtr](const std::vector<Value>&) -> Value {
        setPtr->clear();
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "forEach") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [this, setPtr](const std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].isFunction()) {
          return Value(std::make_shared<Error>(ErrorType::TypeError, "forEach requires a callback function"));
        }
        auto callback = std::get<std::shared_ptr<Function>>(args[0].data);
        Value thisArg = args.size() > 1 ? args[1] : Value(Undefined{});
        for (size_t i = 0; i < setPtr->values.size(); ++i) {
          invokeFunction(callback, {setPtr->values[i], setPtr->values[i], Value(setPtr)}, thisArg);
        }
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "values" || propName == "keys" || propName == iteratorKey) {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [setPtr](const std::vector<Value>&) -> Value {
        auto iterObj = std::make_shared<Object>();
        auto indexPtr = std::make_shared<size_t>(0);
        auto nextFn = std::make_shared<Function>();
        nextFn->isNative = true;
        nextFn->nativeFunc = [setPtr, indexPtr](const std::vector<Value>&) -> Value {
          if (*indexPtr >= setPtr->values.size()) {
            return makeIteratorResult(Value(Undefined{}), true);
          }
          Value val = setPtr->values[*indexPtr];
          (*indexPtr)++;
          return makeIteratorResult(val, false);
        };
        iterObj->properties["next"] = Value(nextFn);
        return Value(iterObj);
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "entries") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [setPtr](const std::vector<Value>&) -> Value {
        auto iterObj = std::make_shared<Object>();
        auto indexPtr = std::make_shared<size_t>(0);
        auto nextFn = std::make_shared<Function>();
        nextFn->isNative = true;
        nextFn->nativeFunc = [setPtr, indexPtr](const std::vector<Value>&) -> Value {
          if (*indexPtr >= setPtr->values.size()) {
            return makeIteratorResult(Value(Undefined{}), true);
          }
          Value val = setPtr->values[*indexPtr];
          auto pair = std::make_shared<Array>();
          pair->elements.push_back(val);
          pair->elements.push_back(val);
          (*indexPtr)++;
          return makeIteratorResult(Value(pair), false);
        };
        iterObj->properties["next"] = Value(nextFn);
        return Value(iterObj);
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "constructor") {
      if (auto ctor = env_->get("Set")) {
        LIGHTJS_RETURN(*ctor);
      }
      LIGHTJS_RETURN(Value(Undefined{}));
    }
  }

  if (obj.isTypedArray()) {
    auto taPtr = std::get<std::shared_ptr<TypedArray>>(obj.data);
    if (propName == "length") {
      LIGHTJS_RETURN(Value(static_cast<double>(taPtr->length)));
    }
    if (propName == "byteLength") {
      LIGHTJS_RETURN(Value(static_cast<double>(taPtr->buffer.size())));
    }
    size_t idx = 0;
    if (parseArrayIndex(propName, idx) && idx < taPtr->length) {
      if (taPtr->type == TypedArrayType::BigInt64 || taPtr->type == TypedArrayType::BigUint64) {
        LIGHTJS_RETURN(Value(BigInt(taPtr->getBigIntElement(idx))));
      } else {
        LIGHTJS_RETURN(Value(taPtr->getElement(idx)));
      }
    }
  }

  if (obj.isRegex()) {
    auto regexPtr = std::get<std::shared_ptr<Regex>>(obj.data);

    if (propName == "test") {
      auto testFn = std::make_shared<Function>();
      testFn->isNative = true;
      testFn->nativeFunc = [regexPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(false);
        std::string str = args[0].toString();
#if USE_SIMPLE_REGEX
        return Value(regexPtr->regex->search(str));
#else
        return Value(std::regex_search(str, regexPtr->regex));
#endif
      };
      LIGHTJS_RETURN(Value(testFn));
    }

    if (propName == "exec") {
      auto execFn = std::make_shared<Function>();
      execFn->isNative = true;
      execFn->nativeFunc = [regexPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(Null{});
        std::string str = args[0].toString();
#if USE_SIMPLE_REGEX
        std::vector<simple_regex::Regex::Match> matches;
        if (regexPtr->regex->search(str, matches)) {
          auto arr = std::make_shared<Array>();
          for (const auto& m : matches) {
            arr->elements.push_back(Value(m.str));
          }
          return Value(arr);
        }
#else
        std::smatch match;
        if (std::regex_search(str, match, regexPtr->regex)) {
          auto arr = std::make_shared<Array>();
          for (const auto& m : match) {
            arr->elements.push_back(Value(m.str()));
          }
          return Value(arr);
        }
#endif
        return Value(Null{});
      };
      LIGHTJS_RETURN(Value(execFn));
    }

    if (propName == "source") {
      LIGHTJS_RETURN(Value(regexPtr->pattern));
    }

    if (propName == "flags") {
      LIGHTJS_RETURN(Value(regexPtr->flags));
    }
  }

  if (obj.isError()) {
    auto errorPtr = std::get<std::shared_ptr<Error>>(obj.data);

    if (propName == "toString") {
      auto toStringFn = std::make_shared<Function>();
      toStringFn->isNative = true;
      toStringFn->nativeFunc = [errorPtr](const std::vector<Value>&) -> Value {
        return Value(errorPtr->toString());
      };
      LIGHTJS_RETURN(Value(toStringFn));
    }

    if (propName == "name") {
      LIGHTJS_RETURN(Value(errorPtr->getName()));
    }

    if (propName == "constructor") {
      if (auto ctor = env_->get(errorPtr->getName())) {
        LIGHTJS_RETURN(*ctor);
      }
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    if (propName == "message") {
      LIGHTJS_RETURN(Value(errorPtr->message));
    }
  }

  if (obj.isNumber()) {
    double num = std::get<double>(obj.data);

    if (propName == "toFixed") {
      auto toFixedFn = std::make_shared<Function>();
      toFixedFn->isNative = true;
      toFixedFn->nativeFunc = [num](const std::vector<Value>& args) -> Value {
        int digits = args.empty() ? 0 : static_cast<int>(args[0].toNumber());
        if (digits < 0) digits = 0;
        if (digits > 100) digits = 100;

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(digits) << num;
        return Value(oss.str());
      };
      LIGHTJS_RETURN(Value(toFixedFn));
    }

    if (propName == "toPrecision") {
      auto toPrecisionFn = std::make_shared<Function>();
      toPrecisionFn->isNative = true;
      toPrecisionFn->nativeFunc = [num](const std::vector<Value>& args) -> Value {
        if (args.empty()) {
          return Value(std::to_string(num));
        }
        int precision = static_cast<int>(args[0].toNumber());
        if (precision < 1) precision = 1;
        if (precision > 100) precision = 100;

        std::ostringstream oss;
        oss << std::setprecision(precision) << num;
        return Value(oss.str());
      };
      LIGHTJS_RETURN(Value(toPrecisionFn));
    }

    if (propName == "toExponential") {
      auto toExponentialFn = std::make_shared<Function>();
      toExponentialFn->isNative = true;
      toExponentialFn->nativeFunc = [num](const std::vector<Value>& args) -> Value {
        int digits = args.empty() ? 6 : static_cast<int>(args[0].toNumber());
        if (digits < 0) digits = 0;
        if (digits > 100) digits = 100;

        std::ostringstream oss;
        oss << std::scientific << std::setprecision(digits) << num;
        return Value(oss.str());
      };
      LIGHTJS_RETURN(Value(toExponentialFn));
    }

    if (propName == "toString") {
      auto toStringFn = std::make_shared<Function>();
      toStringFn->isNative = true;
      toStringFn->nativeFunc = [num](const std::vector<Value>& args) -> Value {
        if (args.empty()) {
          return Value(std::to_string(num));
        }
        int radix = static_cast<int>(args[0].toNumber());
        if (radix < 2 || radix > 36) {
          return Value(std::make_shared<Error>(ErrorType::RangeError, "toString() radix must be between 2 and 36"));
        }
        // For simplicity, only implement radix 10, 16, 8, 2
        if (radix == 10) {
          return Value(std::to_string(num));
        } else if (radix == 16) {
          std::ostringstream oss;
          oss << std::hex << static_cast<long long>(num);
          return Value(oss.str());
        } else if (radix == 8) {
          std::ostringstream oss;
          oss << std::oct << static_cast<long long>(num);
          return Value(oss.str());
        } else if (radix == 2) {
          std::string binary;
          long long n = static_cast<long long>(num);
          if (n == 0) return Value("0");
          bool negative = n < 0;
          if (negative) n = -n;
          while (n > 0) {
            binary = (n % 2 == 0 ? "0" : "1") + binary;
            n /= 2;
          }
          return Value(negative ? "-" + binary : binary);
        }
        return Value(std::to_string(num));
      };
      LIGHTJS_RETURN(Value(toStringFn));
    }
  }

  if (obj.isString()) {
    std::string str = std::get<std::string>(obj.data);

    if (propName == "toString" || propName == "valueOf") {
      auto toStringFn = std::make_shared<Function>();
      toStringFn->isNative = true;
      toStringFn->nativeFunc = [str](const std::vector<Value>&) -> Value {
        return Value(str);
      };
      LIGHTJS_RETURN(Value(toStringFn));
    }

    if (propName == "length") {
      // Return Unicode code point length, not byte length
      LIGHTJS_RETURN(Value(static_cast<double>(unicode::utf8Length(str))));
    }

    // Support numeric indexing for strings (e.g., str[0])
    // Check if propName is a valid non-negative integer
    bool isNumericIndex = !propName.empty() && std::all_of(propName.begin(), propName.end(), ::isdigit);
    if (isNumericIndex) {
      size_t index = std::stoul(propName);
      size_t strLen = unicode::utf8Length(str);
      if (index < strLen) {
        // Get character at Unicode code point index
        std::string charAtIndex = unicode::charAt(str, index);
        LIGHTJS_RETURN(Value(charAtIndex));
      }
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    if (propName == iteratorKey) {
      auto charArray = std::make_shared<Array>();
      GarbageCollector::instance().reportAllocation(sizeof(Array));
      for (char c : str) {
        charArray->elements.push_back(Value(std::string(1, c)));
      }
      LIGHTJS_RETURN(createIteratorFactory(charArray));
    }

    if (propName == "charAt") {
      auto charAtFn = std::make_shared<Function>();
      charAtFn->isNative = true;
      charAtFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        std::vector<Value> funcArgs = {Value(str)};
        funcArgs.insert(funcArgs.end(), args.begin(), args.end());
        return String_charAt(funcArgs);
      };
      LIGHTJS_RETURN(Value(charAtFn));
    }

    if (propName == "charCodeAt") {
      auto charCodeAtFn = std::make_shared<Function>();
      charCodeAtFn->isNative = true;
      charCodeAtFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        std::vector<Value> funcArgs = {Value(str)};
        funcArgs.insert(funcArgs.end(), args.begin(), args.end());
        return String_charCodeAt(funcArgs);
      };
      LIGHTJS_RETURN(Value(charCodeAtFn));
    }

    if (propName == "codePointAt") {
      auto codePointAtFn = std::make_shared<Function>();
      codePointAtFn->isNative = true;
      codePointAtFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        std::vector<Value> funcArgs = {Value(str)};
        funcArgs.insert(funcArgs.end(), args.begin(), args.end());
        return String_codePointAt(funcArgs);
      };
      LIGHTJS_RETURN(Value(codePointAtFn));
    }

    if (propName == "includes") {
      auto includesFn = std::make_shared<Function>();
      includesFn->isNative = true;
      includesFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(false);
        std::string searchStr = args[0].toString();
        size_t position = 0;
        if (args.size() > 1) {
          position = static_cast<size_t>(args[1].toNumber());
        }
        return Value(str.find(searchStr, position) != std::string::npos);
      };
      LIGHTJS_RETURN(Value(includesFn));
    }

    if (propName == "repeat") {
      auto repeatFn = std::make_shared<Function>();
      repeatFn->isNative = true;
      repeatFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(std::string(""));
        int count = static_cast<int>(args[0].toNumber());
        if (count < 0 || count == INT_MAX) return Value(std::string(""));
        std::string result;
        for (int i = 0; i < count; ++i) {
          result += str;
        }
        return Value(result);
      };
      LIGHTJS_RETURN(Value(repeatFn));
    }

    if (propName == "padStart") {
      auto padStartFn = std::make_shared<Function>();
      padStartFn->isNative = true;
      padStartFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(str);
        size_t targetLength = static_cast<size_t>(args[0].toNumber());
        if (targetLength <= str.length()) return Value(str);

        std::string padString = args.size() > 1 ? args[1].toString() : " ";
        if (padString.empty()) return Value(str);

        size_t padLength = targetLength - str.length();
        std::string result;
        while (result.length() < padLength) {
          result += padString;
        }
        result = result.substr(0, padLength) + str;
        return Value(result);
      };
      LIGHTJS_RETURN(Value(padStartFn));
    }

    if (propName == "padEnd") {
      auto padEndFn = std::make_shared<Function>();
      padEndFn->isNative = true;
      padEndFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(str);
        size_t targetLength = static_cast<size_t>(args[0].toNumber());
        if (targetLength <= str.length()) return Value(str);

        std::string padString = args.size() > 1 ? args[1].toString() : " ";
        if (padString.empty()) return Value(str);

        size_t padLength = targetLength - str.length();
        std::string result = str;
        while (result.length() < targetLength) {
          result += padString;
        }
        result = result.substr(0, targetLength);
        return Value(result);
      };
      LIGHTJS_RETURN(Value(padEndFn));
    }

    if (propName == "trim") {
      auto trimFn = std::make_shared<Function>();
      trimFn->isNative = true;
      trimFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        size_t start = 0;
        size_t end = str.length();
        while (start < end && std::isspace(static_cast<unsigned char>(str[start]))) {
          ++start;
        }
        while (end > start && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
          --end;
        }
        return Value(str.substr(start, end - start));
      };
      LIGHTJS_RETURN(Value(trimFn));
    }

    if (propName == "trimStart") {
      auto trimStartFn = std::make_shared<Function>();
      trimStartFn->isNative = true;
      trimStartFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        size_t start = 0;
        while (start < str.length() && std::isspace(static_cast<unsigned char>(str[start]))) {
          ++start;
        }
        return Value(str.substr(start));
      };
      LIGHTJS_RETURN(Value(trimStartFn));
    }

    if (propName == "trimEnd") {
      auto trimEndFn = std::make_shared<Function>();
      trimEndFn->isNative = true;
      trimEndFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        size_t end = str.length();
        while (end > 0 && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
          --end;
        }
        return Value(str.substr(0, end));
      };
      LIGHTJS_RETURN(Value(trimEndFn));
    }

    if (propName == "split") {
      auto splitFn = std::make_shared<Function>();
      splitFn->isNative = true;
      splitFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        auto result = std::make_shared<Array>();
        GarbageCollector::instance().reportAllocation(sizeof(Array));

        if (args.empty()) {
          // No separator: return array with entire string
          result->elements.push_back(Value(str));
          return Value(result);
        }

        std::string separator = args[0].toString();
        int limit = args.size() > 1 ? static_cast<int>(args[1].toNumber()) : -1;

        if (separator.empty()) {
          // Split into individual characters
          size_t len = unicode::utf8Length(str);
          for (size_t i = 0; i < len && (limit < 0 || static_cast<int>(i) < limit); ++i) {
            result->elements.push_back(Value(unicode::charAt(str, i)));
          }
          return Value(result);
        }

        size_t start = 0;
        size_t pos;
        int count = 0;
        while ((pos = str.find(separator, start)) != std::string::npos) {
          if (limit >= 0 && count >= limit) break;
          result->elements.push_back(Value(str.substr(start, pos - start)));
          start = pos + separator.length();
          count++;
        }
        if (limit < 0 || count < limit) {
          result->elements.push_back(Value(str.substr(start)));
        }
        return Value(result);
      };
      LIGHTJS_RETURN(Value(splitFn));
    }

    if (propName == "startsWith") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(true);
        std::string searchStr = args[0].toString();
        size_t position = args.size() > 1 ? static_cast<size_t>(args[1].toNumber()) : 0;
        if (position > str.length()) return Value(false);
        return Value(str.compare(position, searchStr.length(), searchStr) == 0);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "endsWith") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(true);
        std::string searchStr = args[0].toString();
        size_t endPos = args.size() > 1 ? static_cast<size_t>(args[1].toNumber()) : str.length();
        if (endPos > str.length()) endPos = str.length();
        if (searchStr.length() > endPos) return Value(false);
        return Value(str.compare(endPos - searchStr.length(), searchStr.length(), searchStr) == 0);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "at") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(Undefined{});
        int index = static_cast<int>(args[0].toNumber());
        int len = static_cast<int>(unicode::utf8Length(str));
        if (index < 0) index = len + index;
        if (index < 0 || index >= len) return Value(Undefined{});
        return Value(unicode::charAt(str, index));
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "normalize") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        // Basic implementation - just returns the string as-is
        // Full Unicode normalization (NFC, NFD, etc.) is complex
        return Value(str);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "localeCompare") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(0.0);
        std::string other = args[0].toString();
        int result = str.compare(other);
        return Value(static_cast<double>(result < 0 ? -1 : (result > 0 ? 1 : 0)));
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "concat") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [this, str](const std::vector<Value>& args) -> Value {
        std::string result = str;
        for (const auto& arg : args) {
          Value primitive = arg;
          if (isObjectLike(primitive)) {
            primitive = toPrimitiveValue(primitive, true);
            if (hasError()) {
              return Value(Undefined{});
            }
          }
          result += primitive.toString();
        }
        return Value(result);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "indexOf") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(-1.0);
        std::string searchStr = args[0].toString();
        size_t fromIndex = 0;
        if (args.size() > 1) {
          double fi = args[1].toNumber();
          if (std::isnan(fi) || fi < 0) fi = 0;
          fromIndex = static_cast<size_t>(fi);
        }
        if (fromIndex >= str.length() && !searchStr.empty()) return Value(-1.0);
        size_t pos = str.find(searchStr, fromIndex);
        if (pos == std::string::npos) return Value(-1.0);
        return Value(static_cast<double>(pos));
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "lastIndexOf") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(-1.0);
        std::string searchStr = args[0].toString();
        size_t fromIndex = str.length();
        if (args.size() > 1 && !args[1].isUndefined()) {
          double fi = args[1].toNumber();
          if (!std::isnan(fi)) {
            if (fi < 0) fi = 0;
            fromIndex = static_cast<size_t>(fi);
          }
        }
        size_t pos = str.rfind(searchStr, fromIndex);
        if (pos == std::string::npos) return Value(-1.0);
        return Value(static_cast<double>(pos));
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "search") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(0.0);
        if (args[0].isRegex()) {
          auto regexPtr = std::get<std::shared_ptr<Regex>>(args[0].data);
#if USE_SIMPLE_REGEX
          std::vector<simple_regex::Regex::Match> matches;
          if (regexPtr->regex->search(str, matches) && !matches.empty()) {
            return Value(static_cast<double>(matches[0].start));
          }
#else
          std::smatch match;
          if (std::regex_search(str, match, regexPtr->regex)) {
            return Value(static_cast<double>(match.position(0)));
          }
#endif
          return Value(-1.0);
        }
        std::string searchStr = args[0].toString();
        size_t pos = str.find(searchStr);
        if (pos == std::string::npos) return Value(-1.0);
        return Value(static_cast<double>(pos));
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "match") {
      auto matchFn = std::make_shared<Function>();
      matchFn->isNative = true;
      matchFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        if (args.empty() || !args[0].isRegex()) return Value(Null{});
        auto regexPtr = std::get<std::shared_ptr<Regex>>(args[0].data);
#if USE_SIMPLE_REGEX
        std::vector<simple_regex::Regex::Match> matches;
        if (regexPtr->regex->search(str, matches)) {
          auto arr = std::make_shared<Array>();
          for (const auto& m : matches) {
            arr->elements.push_back(Value(m.str));
          }
          return Value(arr);
        }
#else
        std::smatch match;
        if (std::regex_search(str, match, regexPtr->regex)) {
          auto arr = std::make_shared<Array>();
          for (const auto& m : match) {
            arr->elements.push_back(Value(m.str()));
          }
          return Value(arr);
        }
#endif
        return Value(Null{});
      };
      LIGHTJS_RETURN(Value(matchFn));
    }

    // String.prototype.matchAll - ES2020
    if (propName == "matchAll") {
      if (auto strCtor = env_->get("String"); strCtor && strCtor->isObject()) {
        auto strObj = std::get<std::shared_ptr<Object>>(strCtor->data);
        auto protoIt = strObj->properties.find("prototype");
        if (protoIt != strObj->properties.end() && protoIt->second.isObject()) {
          auto protoObj = std::get<std::shared_ptr<Object>>(protoIt->second.data);
          auto methodIt = protoObj->properties.find("matchAll");
          if (methodIt != protoObj->properties.end() && methodIt->second.isFunction()) {
            LIGHTJS_RETURN(methodIt->second);
          }
        }
      }
      LIGHTJS_RETURN(Value(Undefined{}));
    }

    if (propName == "replace") {
      auto replaceFn = std::make_shared<Function>();
      replaceFn->isNative = true;
      replaceFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(str);
        if (args[0].isRegex()) {
          auto regexPtr = std::get<std::shared_ptr<Regex>>(args[0].data);
          std::string replacement = args[1].toString();
#if USE_SIMPLE_REGEX
          return Value(regexPtr->regex->replace(str, replacement));
#else
          return Value(std::regex_replace(str, regexPtr->regex, replacement));
#endif
        } else {
          std::string search = args[0].toString();
          std::string replacement = args[1].toString();
          std::string result = str;
          size_t pos = result.find(search);
          if (pos != std::string::npos) {
            result.replace(pos, search.length(), replacement);
          }
          return Value(result);
        }
      };
      LIGHTJS_RETURN(Value(replaceFn));
    }

    if (propName == "replaceAll") {
      auto replaceAllFn = std::make_shared<Function>();
      replaceAllFn->isNative = true;
      replaceAllFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value(str);
        std::string search = args[0].toString();
        std::string replacement = args[1].toString();
        std::string result = str;
        if (search.empty()) return Value(result);
        size_t pos = 0;
        while ((pos = result.find(search, pos)) != std::string::npos) {
          result.replace(pos, search.length(), replacement);
          pos += replacement.length();
        }
        return Value(result);
      };
      LIGHTJS_RETURN(Value(replaceAllFn));
    }

    if (propName == "at") {
      auto atFn = std::make_shared<Function>();
      atFn->isNative = true;
      atFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(Undefined{});
        int index = static_cast<int>(args[0].toNumber());
        int len = static_cast<int>(unicode::utf8Length(str));
        if (index < 0) index = len + index;
        if (index < 0 || index >= len) return Value(Undefined{});
        return Value(unicode::charAt(str, index));
      };
      LIGHTJS_RETURN(Value(atFn));
    }

    if (propName == "repeat") {
      auto repeatFn = std::make_shared<Function>();
      repeatFn->isNative = true;
      repeatFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(std::string(""));
        int count = static_cast<int>(args[0].toNumber());
        if (count < 0) return Value(std::make_shared<Error>(ErrorType::RangeError, "Invalid count value"));
        if (count == 0) return Value(std::string(""));
        std::string result;
        result.reserve(str.length() * count);
        for (int i = 0; i < count; ++i) {
          result += str;
        }
        return Value(result);
      };
      LIGHTJS_RETURN(Value(repeatFn));
    }

    if (propName == "padStart") {
      auto padStartFn = std::make_shared<Function>();
      padStartFn->isNative = true;
      padStartFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(str);
        int targetLength = static_cast<int>(args[0].toNumber());
        int currentLength = static_cast<int>(unicode::utf8Length(str));
        if (targetLength <= currentLength) return Value(str);
        std::string padString = args.size() > 1 ? args[1].toString() : " ";
        if (padString.empty()) return Value(str);
        int padLength = targetLength - currentLength;
        std::string padding;
        while (static_cast<int>(unicode::utf8Length(padding)) < padLength) {
          padding += padString;
        }
        // Trim to exact length
        std::string result;
        int count = 0;
        size_t i = 0;
        while (i < padding.size() && count < padLength) {
          std::string ch = unicode::charAt(padding, count);
          result += ch;
          count++;
        }
        return Value(result + str);
      };
      LIGHTJS_RETURN(Value(padStartFn));
    }

    if (propName == "padEnd") {
      auto padEndFn = std::make_shared<Function>();
      padEndFn->isNative = true;
      padEndFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value(str);
        int targetLength = static_cast<int>(args[0].toNumber());
        int currentLength = static_cast<int>(unicode::utf8Length(str));
        if (targetLength <= currentLength) return Value(str);
        std::string padString = args.size() > 1 ? args[1].toString() : " ";
        if (padString.empty()) return Value(str);
        int padLength = targetLength - currentLength;
        std::string padding;
        while (static_cast<int>(unicode::utf8Length(padding)) < padLength) {
          padding += padString;
        }
        // Trim to exact length
        std::string result;
        int count = 0;
        while (count < padLength) {
          std::string ch = unicode::charAt(padding, count);
          result += ch;
          count++;
        }
        return Value(str + result);
      };
      LIGHTJS_RETURN(Value(padEndFn));
    }

    if (propName == "trim") {
      auto trimFn = std::make_shared<Function>();
      trimFn->isNative = true;
      trimFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        size_t start = 0;
        size_t end = str.length();
        while (start < end && std::isspace(static_cast<unsigned char>(str[start]))) {
          ++start;
        }
        while (end > start && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
          --end;
        }
        return Value(str.substr(start, end - start));
      };
      LIGHTJS_RETURN(Value(trimFn));
    }

    if (propName == "trimStart") {
      auto trimStartFn = std::make_shared<Function>();
      trimStartFn->isNative = true;
      trimStartFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        size_t start = 0;
        while (start < str.length() && std::isspace(static_cast<unsigned char>(str[start]))) {
          ++start;
        }
        return Value(str.substr(start));
      };
      LIGHTJS_RETURN(Value(trimStartFn));
    }

    if (propName == "trimEnd") {
      auto trimEndFn = std::make_shared<Function>();
      trimEndFn->isNative = true;
      trimEndFn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        size_t end = str.length();
        while (end > 0 && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
          --end;
        }
        return Value(str.substr(0, end));
      };
      LIGHTJS_RETURN(Value(trimEndFn));
    }

    if (propName == "substring") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        int len = static_cast<int>(str.length());
        int start = 0, end = len;
        if (!args.empty()) {
          double s = args[0].toNumber();
          if (std::isnan(s) || s < 0) s = 0;
          if (s > len) s = len;
          start = static_cast<int>(s);
        }
        if (args.size() > 1 && !args[1].isUndefined()) {
          double e = args[1].toNumber();
          if (std::isnan(e) || e < 0) e = 0;
          if (e > len) e = len;
          end = static_cast<int>(e);
        }
        if (start > end) std::swap(start, end);
        return Value(str.substr(start, end - start));
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "slice") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        int len = static_cast<int>(str.length());
        int start = 0, end = len;
        if (!args.empty()) {
          int s = static_cast<int>(args[0].toNumber());
          if (s < 0) s = std::max(0, len + s);
          if (s > len) s = len;
          start = s;
        }
        if (args.size() > 1 && !args[1].isUndefined()) {
          int e = static_cast<int>(args[1].toNumber());
          if (e < 0) e = std::max(0, len + e);
          if (e > len) e = len;
          end = e;
        }
        if (start >= end) return Value(std::string(""));
        return Value(str.substr(start, end - start));
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "substr") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        int len = static_cast<int>(str.length());
        int start = 0;
        if (!args.empty()) {
          start = static_cast<int>(args[0].toNumber());
          if (start < 0) start = std::max(0, len + start);
        }
        int length = len - start;
        if (args.size() > 1 && !args[1].isUndefined()) {
          length = static_cast<int>(args[1].toNumber());
          if (length < 0) length = 0;
        }
        if (start >= len || length <= 0) return Value(std::string(""));
        return Value(str.substr(start, length));
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "toUpperCase" || propName == "toLocaleUpperCase") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        std::string result = str;
        std::transform(result.begin(), result.end(), result.begin(), ::toupper);
        return Value(result);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "toLowerCase" || propName == "toLocaleLowerCase") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        std::string result = str;
        std::transform(result.begin(), result.end(), result.begin(), ::tolower);
        return Value(result);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "constructor") {
      if (auto strCtor = env_->get("String")) {
        LIGHTJS_RETURN(*strCtor);
      }
    }
  }

  LIGHTJS_RETURN(Value(Undefined{}));
}

Value Interpreter::makeIteratorResult(const Value& value, bool done) {
  auto resultObj = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  resultObj->properties["value"] = value;
  resultObj->properties["done"] = Value(done);
  return Value(resultObj);
}

Value Interpreter::createIteratorFactory(const std::shared_ptr<Array>& arrPtr) {
  auto iteratorFactory = std::make_shared<Function>();
  iteratorFactory->isNative = true;
  iteratorFactory->nativeFunc = [arrPtr](const std::vector<Value>&) -> Value {
    auto iteratorObj = std::make_shared<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    auto state = std::make_shared<size_t>(0);
    auto nextFn = std::make_shared<Function>();
    nextFn->isNative = true;
    nextFn->nativeFunc = [arrPtr, state](const std::vector<Value>&) -> Value {
      if (!arrPtr || *state >= arrPtr->elements.size()) {
        return Interpreter::makeIteratorResult(Value(Undefined{}), true);
      }
      Value value = arrPtr->elements[(*state)++];
      return Interpreter::makeIteratorResult(value, false);
    };
    iteratorObj->properties["next"] = Value(nextFn);
    return Value(iteratorObj);
  };
  return Value(iteratorFactory);
}
Value Interpreter::runGeneratorNext(const std::shared_ptr<Generator>& genPtr,
                                    ControlFlow::ResumeMode mode,
                                    const Value& resumeValue) {
  if (!genPtr) {
    return makeIteratorResult(Value(Undefined{}), true);
  }

  if (genPtr->state == GeneratorState::Completed) {
    return makeIteratorResult(Value(Undefined{}), true);
  }

  if (genPtr->state == GeneratorState::SuspendedStart ||
      genPtr->state == GeneratorState::SuspendedYield) {
    bool wasSuspendedYield = (genPtr->state == GeneratorState::SuspendedYield);
    genPtr->state = GeneratorState::Executing;

    if (genPtr->function && genPtr->context) {
      auto prevEnv = env_;
      env_ = std::static_pointer_cast<Environment>(genPtr->context);

      auto bodyPtr = std::static_pointer_cast<std::vector<StmtPtr>>(genPtr->function->body);
      Value result = Value(Undefined{});
      bool hadThrow = false;
      Value thrownValue = Value(Undefined{});

      auto prevFlow = flow_;
      flow_.reset();
      if (mode != ControlFlow::ResumeMode::None && wasSuspendedYield) {
        flow_.prepareResume(mode, resumeValue);
      }

      size_t startIndex = genPtr->yieldIndex;
      for (size_t i = startIndex; i < bodyPtr->size(); i++) {
        auto task = evaluate(*(*bodyPtr)[i]);
  LIGHTJS_RUN_TASK(task, result);

        if (flow_.type == ControlFlow::Type::Throw) {
          hadThrow = true;
          thrownValue = flow_.value;
          genPtr->state = GeneratorState::Completed;
          break;
        }

        if (flow_.type == ControlFlow::Type::Yield) {
          genPtr->state = GeneratorState::SuspendedYield;
          genPtr->currentValue = std::make_shared<Value>(flow_.value);
          genPtr->yieldIndex = i;

          flow_ = prevFlow;
          env_ = prevEnv;

          return makeIteratorResult(*genPtr->currentValue, false);
        }

        if (flow_.type == ControlFlow::Type::Return) {
          genPtr->state = GeneratorState::Completed;
          genPtr->currentValue = std::make_shared<Value>(flow_.value);
          result = flow_.value;
          break;
        }
      }

      flow_ = prevFlow;
      env_ = prevEnv;

      if (hadThrow) {
        flow_.type = ControlFlow::Type::Throw;
        flow_.value = thrownValue;
        return Value(Undefined{});
      }

      if (genPtr->state != GeneratorState::Completed &&
          genPtr->state != GeneratorState::SuspendedYield) {
        genPtr->state = GeneratorState::Completed;
        genPtr->currentValue = std::make_shared<Value>(result);
      }

      return makeIteratorResult(*genPtr->currentValue,
                                genPtr->state == GeneratorState::Completed);
    }
  }

  genPtr->state = GeneratorState::Completed;
  return makeIteratorResult(Value(Undefined{}), true);
}

std::optional<Interpreter::IteratorRecord> Interpreter::getIterator(const Value& iterable) {
  auto buildRecord = [&](const Value& value) -> std::optional<IteratorRecord> {
    IteratorRecord record;
    if (value.isGenerator()) {
      record.kind = IteratorRecord::Kind::Generator;
      record.generator = std::get<std::shared_ptr<Generator>>(value.data);
      return record;
    }
    if (value.isArray()) {
      record.kind = IteratorRecord::Kind::Array;
      record.array = std::get<std::shared_ptr<Array>>(value.data);
      record.index = 0;
      return record;
    }
    if (value.isString()) {
      record.kind = IteratorRecord::Kind::String;
      record.stringValue = std::get<std::string>(value.data);
      record.index = 0;
      return record;
    }
    if (value.isTypedArray()) {
      record.kind = IteratorRecord::Kind::TypedArray;
      record.typedArray = std::get<std::shared_ptr<TypedArray>>(value.data);
      record.index = 0;
      return record;
    }
    if (value.isMap()) {
      auto mapPtr = std::get<std::shared_ptr<Map>>(value.data);
      auto iterObj = std::make_shared<Object>();
      auto indexPtr = std::make_shared<size_t>(0);
      auto nextFn = std::make_shared<Function>();
      nextFn->isNative = true;
      nextFn->nativeFunc = [mapPtr, indexPtr](const std::vector<Value>&) -> Value {
        if (*indexPtr >= mapPtr->entries.size()) {
          return makeIteratorResult(Value(Undefined{}), true);
        }
        auto& entry = mapPtr->entries[*indexPtr];
        auto pair = std::make_shared<Array>();
        pair->elements.push_back(entry.first);
        pair->elements.push_back(entry.second);
        (*indexPtr)++;
        return makeIteratorResult(Value(pair), false);
      };
      iterObj->properties["next"] = Value(nextFn);
      record.kind = IteratorRecord::Kind::IteratorObject;
      record.iteratorObject = iterObj;
      record.nextMethod = Value(nextFn);
      return record;
    }
    if (value.isSet()) {
      auto setPtr = std::get<std::shared_ptr<Set>>(value.data);
      auto iterObj = std::make_shared<Object>();
      auto indexPtr = std::make_shared<size_t>(0);
      auto nextFn = std::make_shared<Function>();
      nextFn->isNative = true;
      nextFn->nativeFunc = [setPtr, indexPtr](const std::vector<Value>&) -> Value {
        if (*indexPtr >= setPtr->values.size()) {
          return makeIteratorResult(Value(Undefined{}), true);
        }
        Value val = setPtr->values[*indexPtr];
        (*indexPtr)++;
        return makeIteratorResult(val, false);
      };
      iterObj->properties["next"] = Value(nextFn);
      record.kind = IteratorRecord::Kind::IteratorObject;
      record.iteratorObject = iterObj;
      record.nextMethod = Value(nextFn);
      return record;
    }
    if (value.isObject()) {
      // Only treat as IteratorObject if it has a 'next' method (i.e., it's already an iterator)
      // Otherwise, fall through to check for Symbol.iterator
      auto obj = std::get<std::shared_ptr<Object>>(value.data);
      // Per GetIterator spec (7.4.1): cache next method (supports getters)
      auto getterIt = obj->properties.find("__get_next");
      if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
        // Call the getter to get the next method
        Value nextMethod = callFunction(getterIt->second, {}, value);
        if (nextMethod.isFunction()) {
          record.kind = IteratorRecord::Kind::IteratorObject;
          record.iteratorObject = obj;
          record.nextMethod = nextMethod;
          return record;
        }
      }
      auto nextIt = obj->properties.find("next");
      if (nextIt != obj->properties.end()) {
        record.kind = IteratorRecord::Kind::IteratorObject;
        record.iteratorObject = obj;
        record.nextMethod = nextIt->second;  // Cache next() per GetIterator spec (7.4.1)
        return record;
      }
    }
    return std::nullopt;
  };

  const auto& iteratorKey = WellKnownSymbols::iteratorKey();

  // First, try to get Symbol.iterator method from the iterable
  auto tryObjectIterator = [&](const Value& target) -> std::optional<IteratorRecord> {
    Value method;
    bool hasMethod = false;
    std::shared_ptr<Object> targetObj;

    if (target.isObject()) {
      targetObj = std::get<std::shared_ptr<Object>>(target.data);
      auto it = targetObj->properties.find(iteratorKey);
      if (it != targetObj->properties.end()) {
        method = it->second;
        hasMethod = method.isFunction();
      }
    } else if (target.isProxy()) {
      // Handle Proxy: resolve Symbol.iterator through the proxy's get trap
      auto proxyPtr = std::get<std::shared_ptr<Proxy>>(target.data);
      if (proxyPtr->handler && proxyPtr->handler->isObject()) {
        auto handlerObj = std::get<std::shared_ptr<Object>>(proxyPtr->handler->data);
        auto getTrapIt = handlerObj->properties.find("get");
        if (getTrapIt != handlerObj->properties.end() && getTrapIt->second.isFunction()) {
          Value resolved = callFunction(getTrapIt->second,
            {proxyPtr->target ? *proxyPtr->target : Value(Undefined{}), Value(iteratorKey), target},
            Value(Undefined{}));
          if (resolved.isFunction()) {
            method = resolved;
            hasMethod = true;
          }
        } else if (proxyPtr->target && proxyPtr->target->isObject()) {
          auto targetInner = std::get<std::shared_ptr<Object>>(proxyPtr->target->data);
          auto it = targetInner->properties.find(iteratorKey);
          if (it != targetInner->properties.end()) {
            method = it->second;
            hasMethod = method.isFunction();
          }
        }
      } else if (proxyPtr->target && proxyPtr->target->isObject()) {
        auto targetInner = std::get<std::shared_ptr<Object>>(proxyPtr->target->data);
        auto it = targetInner->properties.find(iteratorKey);
        if (it != targetInner->properties.end()) {
          method = it->second;
          hasMethod = method.isFunction();
        }
      }
    } else if (target.isFunction()) {
      auto funcPtr = std::get<std::shared_ptr<Function>>(target.data);
      auto it = funcPtr->properties.find(iteratorKey);
      if (it != funcPtr->properties.end() && it->second.isFunction()) {
        method = it->second;
        hasMethod = true;
      }
    }

    if (hasMethod) {
      auto iterValue = callFunction(method, {});
      if (iterValue.isGenerator()) {
        IteratorRecord record;
        record.kind = IteratorRecord::Kind::Generator;
        record.generator = std::get<std::shared_ptr<Generator>>(iterValue.data);
        return record;
      }
      if (iterValue.isObject()) {
        auto iterObj = std::get<std::shared_ptr<Object>>(iterValue.data);
        IteratorRecord record;
        record.kind = IteratorRecord::Kind::IteratorObject;
        record.iteratorObject = iterObj;
        // Cache next() method per GetIterator spec (7.4.1) - supports getters
        auto getterIt = iterObj->properties.find("__get_next");
        if (getterIt != iterObj->properties.end() && getterIt->second.isFunction()) {
          record.nextMethod = callFunction(getterIt->second, {}, iterValue);
        } else {
          auto nextIt = iterObj->properties.find("next");
          if (nextIt != iterObj->properties.end()) {
            record.nextMethod = nextIt->second;
          }
        }
        return record;
      }
      // Handle Proxy as iterator result
      if (iterValue.isProxy()) {
        auto proxyPtr = std::get<std::shared_ptr<Proxy>>(iterValue.data);
        // Resolve 'next' through the Proxy get trap
        Value nextMethod;
        if (proxyPtr->handler && proxyPtr->handler->isObject()) {
          auto handlerObj = std::get<std::shared_ptr<Object>>(proxyPtr->handler->data);
          auto getTrapIt = handlerObj->properties.find("get");
          if (getTrapIt != handlerObj->properties.end() && getTrapIt->second.isFunction()) {
            nextMethod = callFunction(getTrapIt->second,
              {proxyPtr->target ? *proxyPtr->target : Value(Undefined{}), Value(std::string("next")), iterValue},
              Value(Undefined{}));
          } else if (proxyPtr->target && proxyPtr->target->isObject()) {
            // No get trap - fall through to target
            auto targetObj = std::get<std::shared_ptr<Object>>(proxyPtr->target->data);
            auto nextIt = targetObj->properties.find("next");
            if (nextIt != targetObj->properties.end()) {
              nextMethod = nextIt->second;
            }
          }
        } else if (proxyPtr->target && proxyPtr->target->isObject()) {
          auto targetObj = std::get<std::shared_ptr<Object>>(proxyPtr->target->data);
          auto nextIt = targetObj->properties.find("next");
          if (nextIt != targetObj->properties.end()) {
            nextMethod = nextIt->second;
          }
        }
        // Create a wrapper Object to act as the iterator
        auto iterObj = std::make_shared<Object>();
        iterObj->properties["__proxy__"] = iterValue;  // Keep proxy alive
        if (nextMethod.isFunction()) {
          // Create a next() wrapper that calls through the proxy
          auto nextFunc = std::make_shared<Function>();
          nextFunc->isNative = true;
          auto proxyCopy = proxyPtr;
          auto handlerCopy = proxyPtr->handler;
          nextFunc->nativeFunc = [this, proxyCopy, nextMethod](const std::vector<Value>& args) -> Value {
            return callFunction(nextMethod, {}, Value(proxyCopy->target ? *proxyCopy->target : Value(Undefined{})));
          };
          iterObj->properties["next"] = Value(nextFunc);
          IteratorRecord record;
          record.kind = IteratorRecord::Kind::IteratorObject;
          record.iteratorObject = iterObj;
          record.nextMethod = Value(nextFunc);
          return record;
        }
      }
      if (auto nested = buildRecord(iterValue)) {
        return nested;
      }
    }

    return std::nullopt;
  };

  // Try Symbol.iterator first
  if (auto custom = tryObjectIterator(iterable)) {
    return custom;
  }

  // Fall back to built-in iterators for arrays, strings, generators
  if (auto direct = buildRecord(iterable)) {
    return direct;
  }

  return std::nullopt;
}

// Remove duplicate tryObjectIterator definition below - it's now inline above
Value Interpreter::iteratorNext(IteratorRecord& record) {
  switch (record.kind) {
    case IteratorRecord::Kind::Generator:
      if (record.generator && record.generator->function && record.generator->function->isAsync) {
        auto promise = std::make_shared<Promise>();
        Value step = runGeneratorNext(
          record.generator, ControlFlow::ResumeMode::Next, Value(Undefined{}));
        if (flow_.type == ControlFlow::Type::Throw) {
          Value rejection = flow_.value;
          clearError();
          promise->reject(rejection);
        } else {
          promise->resolve(step);
        }
        return Value(promise);
      }
      return runGeneratorNext(
        record.generator, ControlFlow::ResumeMode::Next, Value(Undefined{}));
    case IteratorRecord::Kind::Array: {
      if (!record.array || record.index >= record.array->elements.size()) {
        return makeIteratorResult(Value(Undefined{}), true);
      }
      // Check for getter on this index (e.g., Object.defineProperty(arr, '0', {get: ...}))
      std::string idxStr = std::to_string(record.index);
      auto getterIt = record.array->properties.find("__get_" + idxStr);
      if (getterIt != record.array->properties.end() && getterIt->second.isFunction()) {
        record.index++;
        Value value = callFunction(getterIt->second, {}, Value(record.array));
        return makeIteratorResult(value, false);
      }
      Value value = record.array->elements[record.index++];
      return makeIteratorResult(value, false);
    }
    case IteratorRecord::Kind::String: {
      size_t cpLen = unicode::utf8Length(record.stringValue);
      if (record.index >= cpLen) {
        return makeIteratorResult(Value(Undefined{}), true);
      }
      std::string ch = unicode::charAt(record.stringValue, record.index++);
      return makeIteratorResult(Value(ch), false);
    }
    case IteratorRecord::Kind::IteratorObject: {
      if (!record.iteratorObject) {
        return makeIteratorResult(Value(Undefined{}), true);
      }
      // Use cached nextMethod per GetIterator spec (7.4.1)
      if (record.nextMethod.isFunction()) {
        return callFunction(record.nextMethod, {}, Value(record.iteratorObject));
      }
      // Fallback: look up next from iterator object
      auto nextIt = record.iteratorObject->properties.find("next");
      if (nextIt == record.iteratorObject->properties.end() || !nextIt->second.isFunction()) {
        return makeIteratorResult(Value(Undefined{}), true);
      }
      return callFunction(nextIt->second, {}, Value(record.iteratorObject));
    }
    case IteratorRecord::Kind::TypedArray: {
      if (!record.typedArray || record.index >= record.typedArray->length) {
        return makeIteratorResult(Value(Undefined{}), true);
      }
      Value value = record.typedArray->getElement(record.index++);
      return makeIteratorResult(value, false);
    }
  }

  return makeIteratorResult(Value(Undefined{}), true);
}

void Interpreter::iteratorClose(IteratorRecord& record) {
  if (record.kind == IteratorRecord::Kind::IteratorObject) {
    if (!record.iteratorObject) return;
    // Per spec IteratorClose step 4: Let innerResult be GetMethod(iterator, "return")
    // GetMethod checks for getters, null, and undefined
    Value returnMethod;
    bool hasReturn = false;
    // Check for getter first (__get_return)
    auto getterIt = record.iteratorObject->properties.find("__get_return");
    if (getterIt != record.iteratorObject->properties.end()) {
      if (getterIt->second.isFunction()) {
        returnMethod = callFunction(getterIt->second, {}, Value(record.iteratorObject));
        // If the getter threw, propagate the error (step 7)
        if (flow_.type == ControlFlow::Type::Throw) return;
        hasReturn = true;
      }
    } else {
      auto returnIt = record.iteratorObject->properties.find("return");
      if (returnIt != record.iteratorObject->properties.end()) {
        returnMethod = returnIt->second;
        hasReturn = true;
      }
    }
    if (!hasReturn) return;
    // Per GetMethod step 3: If func is undefined or null, return undefined
    if (returnMethod.isNull() || returnMethod.isUndefined()) return;
    // If return is not callable, throw TypeError
    if (!returnMethod.isFunction()) {
      throwError(ErrorType::TypeError, "iterator.return is not a function");
      return;
    }
    Value result = callFunction(returnMethod, {}, Value(record.iteratorObject));
    // If return() threw, propagate that throw
    if (flow_.type == ControlFlow::Type::Throw) return;
    // Per spec 7.4.6 step 9: if return() result is not an Object, throw TypeError
    if (!isObjectLike(result)) {
      throwError(ErrorType::TypeError, "Iterator result is not an object");
    }
  } else if (record.kind == IteratorRecord::Kind::Generator) {
    if (!record.generator) return;
    if (record.generator->state == GeneratorState::Completed) return;
    // Call return on the generator to run finally blocks
    runGeneratorNext(record.generator, ControlFlow::ResumeMode::Return, Value(Undefined{}));
  }
  // For Array, String, TypedArray - no close needed
}

Value Interpreter::callFunction(const Value& callee, const std::vector<Value>& args, const Value& thisValue) {
  if (!callee.isFunction()) {
    return Value(Undefined{});
  }

  auto func = std::get<std::shared_ptr<Function>>(callee.data);
  std::vector<Value> currentArgs = args;
  Value currentThis = thisValue;
  auto namedExprIt = func->properties.find("__named_expression__");
  bool pushNamedExpr = namedExprIt != func->properties.end() &&
                       namedExprIt->second.isBool() &&
                       namedExprIt->second.toBool();
  if (pushNamedExpr) {
    activeNamedExpressionStack_.push_back(func);
  }
  struct NamedExprStackGuard {
    Interpreter* interpreter;
    bool active;
    ~NamedExprStackGuard() {
      if (active && !interpreter->activeNamedExpressionStack_.empty()) {
        interpreter->activeNamedExpressionStack_.pop_back();
      }
    }
  } namedExprGuard{this, pushNamedExpr};

  auto bindParameters = [&](std::shared_ptr<Environment>& targetEnv) {
    Value boundThis = currentThis;
    if (!func->isStrict && (boundThis.isUndefined() || boundThis.isNull())) {
      if (auto globalThisValue = targetEnv->get("globalThis")) {
        boundThis = *globalThisValue;
      }
    }
    if (!boundThis.isUndefined()) {
      targetEnv->define("this", boundThis);
    }
    auto superIt = func->properties.find("__super_class__");
    if (superIt != func->properties.end()) {
      targetEnv->define("__super__", superIt->second);
    }

    auto argumentsArray = std::make_shared<Array>();
    GarbageCollector::instance().reportAllocation(sizeof(Array));
    argumentsArray->elements = currentArgs;
    targetEnv->define("arguments", Value(argumentsArray));

    for (size_t i = 0; i < func->params.size(); ++i) {
      if (i < currentArgs.size()) {
        targetEnv->define(func->params[i].name, currentArgs[i]);
      } else if (func->params[i].defaultValue) {
        auto defaultExpr = std::static_pointer_cast<Expression>(func->params[i].defaultValue);
        auto defaultTask = evaluate(*defaultExpr);
        LIGHTJS_RUN_TASK_VOID(defaultTask);
        targetEnv->define(func->params[i].name, defaultTask.result());
      } else {
        targetEnv->define(func->params[i].name, Value(Undefined{}));
      }
    }

    if (func->restParam.has_value()) {
      auto restArr = std::make_shared<Array>();
      GarbageCollector::instance().reportAllocation(sizeof(Array));
      for (size_t i = func->params.size(); i < currentArgs.size(); ++i) {
        restArr->elements.push_back(currentArgs[i]);
      }
      targetEnv->define(*func->restParam, Value(restArr));
    }
  };

  if (func->isNative) {
    auto itReflectConstruct = func->properties.find("__reflect_construct__");
    if (itReflectConstruct != func->properties.end() &&
        itReflectConstruct->second.isBool() &&
        itReflectConstruct->second.toBool()) {
      if (args.size() < 2) {
        throwError(ErrorType::TypeError, "Reflect.construct target is not a function");
        return Value(Undefined{});
      }

      Value target = args[0];
      if (!target.isFunction() && !target.isClass() && !target.isProxy()) {
        throwError(ErrorType::TypeError, "Reflect.construct target is not a function");
        return Value(Undefined{});
      }

      std::vector<Value> constructArgs;
      if (args[1].isArray()) {
        auto arr = std::get<std::shared_ptr<Array>>(args[1].data);
        constructArgs = arr->elements;
      }

      Value newTarget = (args.size() >= 3) ? args[2] : target;
      auto constructTask = constructValue(target, constructArgs, newTarget);
      Value constructed;
      LIGHTJS_RUN_TASK(constructTask, constructed);
      return constructed;
    }

	    bool isIntrinsicEval = false;
	    auto intrinsicEvalIt = func->properties.find("__is_intrinsic_eval__");
	    if (intrinsicEvalIt != func->properties.end() &&
	        intrinsicEvalIt->second.isBool() &&
	        intrinsicEvalIt->second.toBool()) {
	      isIntrinsicEval = true;
	    }
	    bool prevActiveDirectEvalInvocation = activeDirectEvalInvocation_;
	    if (isIntrinsicEval) {
	      activeDirectEvalInvocation_ = pendingDirectEvalCall_;
	      pendingDirectEvalCall_ = false;
	    }

	    try {
	      auto itUsesThis = func->properties.find("__uses_this_arg__");
	      Value nativeResult = Value(Undefined{});
	      if (itUsesThis != func->properties.end() && itUsesThis->second.isBool() && itUsesThis->second.toBool()) {
	        std::vector<Value> nativeArgs;
	        nativeArgs.reserve(args.size() + 1);
	        nativeArgs.push_back(thisValue);
	        nativeArgs.insert(nativeArgs.end(), args.begin(), args.end());
	        nativeResult = func->nativeFunc(nativeArgs);
	      } else {
	        nativeResult = func->nativeFunc(args);
	      }
	      activeDirectEvalInvocation_ = prevActiveDirectEvalInvocation;
	      return nativeResult;
	    } catch (const std::exception& e) {
	      activeDirectEvalInvocation_ = prevActiveDirectEvalInvocation;
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
        errorType = ErrorType::Error;
      }
      throwError(errorType, message);
      return Value(Undefined{});
    }
  }

  if (func->isGenerator) {
    auto generator = std::make_shared<Generator>(func, func->closure);
    GarbageCollector::instance().reportAllocation(sizeof(Generator));
    auto genEnv = std::static_pointer_cast<Environment>(func->closure);
    genEnv = genEnv->createChild();
    bindParameters(genEnv);
    generator->context = genEnv;
    return Value(generator);
  }

  if (func->isAsync) {
    // Push stack frame for async function calls
    auto stackFrame = pushStackFrame("<async>");

    auto promise = std::make_shared<Promise>();
    auto prevEnv = env_;
    env_ = std::static_pointer_cast<Environment>(func->closure);
    env_ = env_->createChild();
    bindParameters(env_);

    auto bodyPtr = std::static_pointer_cast<std::vector<StmtPtr>>(func->body);
    bool previousStrictMode = strictMode_;
    strictMode_ = func->isStrict;
    Value result = Value(Undefined{});
    bool returned = false;

    // Hoist var and function declarations in async function body
    hoistVarDeclarations(*bodyPtr);
    for (const auto& stmt : *bodyPtr) {
      if (std::holds_alternative<FunctionDeclaration>(stmt->node)) {
        auto task = evaluate(*stmt);
        LIGHTJS_RUN_TASK_VOID(task);
      }
    }

    auto prevFlow = flow_;
    flow_ = ControlFlow{};

    try {
      for (const auto& stmt : *bodyPtr) {
        // Skip function declarations - already hoisted
        if (std::holds_alternative<FunctionDeclaration>(stmt->node)) {
          continue;
        }
        auto stmtTask = evaluate(*stmt);
        Value stmtResult = Value(Undefined{});
        LIGHTJS_RUN_TASK(stmtTask, stmtResult);

        if (flow_.type == ControlFlow::Type::Return) {
          result = flow_.value;
          returned = true;
          break;
        }

        // Preserve throw flow control (errors)
        if (flow_.type == ControlFlow::Type::Throw) {
          promise->reject(flow_.value);
          break;
        }
      }
      if (flow_.type != ControlFlow::Type::Throw) {
        if (!returned) {
          result = Value(Undefined{});
        }
        promise->resolve(result);
      }
    } catch (const std::exception& e) {
      promise->reject(Value(std::string(e.what())));
    }

    // Async functions convert abrupt completion into Promise rejection.
    flow_ = prevFlow;
    strictMode_ = previousStrictMode;
    env_ = prevEnv;
    return Value(promise);
  }

  // Push stack frame for JavaScript function calls
  auto stackFrame = pushStackFrame("<function>");

  auto prevEnv = env_;
  auto bodyPtr = std::static_pointer_cast<std::vector<StmtPtr>>(func->body);
  bool previousStrictMode = strictMode_;
  strictMode_ = func->isStrict;
  Value result = Value(Undefined{});

  auto prevFlow = flow_;
  auto prevActiveFunction = activeFunction_;
  bool prevPendingSelfTailCall = pendingSelfTailCall_;
  auto prevPendingSelfTailArgs = std::move(pendingSelfTailArgs_);
  Value prevPendingSelfTailThis = pendingSelfTailThis_;
  activeFunction_ = func;
  pendingSelfTailCall_ = false;
  pendingSelfTailArgs_.clear();
  pendingSelfTailThis_ = Value(Undefined{});

  while (true) {
    env_ = std::static_pointer_cast<Environment>(func->closure);
    env_ = env_->createChild();
    bindParameters(env_);

    // Hoist var and function declarations in function body
    hoistVarDeclarations(*bodyPtr);
    for (const auto& hoistStmt : *bodyPtr) {
      if (std::holds_alternative<FunctionDeclaration>(hoistStmt->node)) {
        auto hoistTask = evaluate(*hoistStmt);
        LIGHTJS_RUN_TASK_VOID(hoistTask);
      }
    }

    bool returned = false;
    bool tailCallSelf = false;
    flow_ = ControlFlow{};
    pendingSelfTailCall_ = false;
    pendingSelfTailArgs_.clear();
    pendingSelfTailThis_ = Value(Undefined{});

    for (const auto& stmt : *bodyPtr) {
      // Skip function declarations - already hoisted
      if (std::holds_alternative<FunctionDeclaration>(stmt->node)) {
        continue;
      }
      auto stmtTask = evaluate(*stmt);
      Value stmtResult = Value(Undefined{});
      LIGHTJS_RUN_TASK(stmtTask, stmtResult);

      if (flow_.type == ControlFlow::Type::Return) {
        if (strictMode_ && pendingSelfTailCall_) {
          currentArgs = std::move(pendingSelfTailArgs_);
          currentThis = pendingSelfTailThis_;
          pendingSelfTailCall_ = false;
          pendingSelfTailArgs_.clear();
          pendingSelfTailThis_ = Value(Undefined{});
          tailCallSelf = true;
        } else {
          result = flow_.value;
          returned = true;
        }
        break;
      }

      // Preserve throw flow control (errors)
      if (flow_.type == ControlFlow::Type::Throw) {
        break;
      }
    }

    if (tailCallSelf) {
      continue;
    }
    if (!returned && flow_.type != ControlFlow::Type::Throw) {
      result = Value(Undefined{});
    }
    break;
  }

  // Don't restore flow if an error was thrown - preserve the error
  if (flow_.type != ControlFlow::Type::Throw) {
    flow_ = prevFlow;
  }
  pendingSelfTailCall_ = prevPendingSelfTailCall;
  pendingSelfTailArgs_ = std::move(prevPendingSelfTailArgs);
  pendingSelfTailThis_ = prevPendingSelfTailThis;
  activeFunction_ = prevActiveFunction;
  strictMode_ = previousStrictMode;
  env_ = prevEnv;
  return result;
}

Task Interpreter::evaluateConditional(const ConditionalExpr& expr) {
  auto testTask = evaluate(*expr.test);
  LIGHTJS_RUN_TASK_VOID(testTask);

  if (testTask.result().toBool()) {
    auto consTask = evaluate(*expr.consequent);
    LIGHTJS_RUN_TASK_VOID(consTask);
    LIGHTJS_RETURN(consTask.result());
  } else {
    auto altTask = evaluate(*expr.alternate);
    LIGHTJS_RUN_TASK_VOID(altTask);
    LIGHTJS_RETURN(altTask.result());
  }
}

Task Interpreter::evaluateArray(const ArrayExpr& expr) {
  // Check memory limit before allocation
  if (!checkMemoryLimit(sizeof(Array))) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  auto arr = std::make_shared<Array>();
  GarbageCollector::instance().reportAllocation(sizeof(Array));

  // Set __proto__ to Array.prototype for prototype chain resolution
  auto arrCtor = env_->get("Array");
  if (arrCtor) {
    std::unordered_map<std::string, Value>* ctorProps = nullptr;
    if (arrCtor->isFunction()) {
      ctorProps = &std::get<std::shared_ptr<Function>>(arrCtor->data)->properties;
    } else if (arrCtor->isObject()) {
      ctorProps = &std::get<std::shared_ptr<Object>>(arrCtor->data)->properties;
    }
    if (ctorProps) {
      auto protoIt = ctorProps->find("prototype");
      if (protoIt != ctorProps->end() && protoIt->second.isObject()) {
        arr->properties["__proto__"] = protoIt->second;
      }
    }
  }

  for (const auto& elem : expr.elements) {
    // Handle holes (elision) - nullptr elements become undefined
    if (!elem) {
      arr->elements.push_back(Value(Undefined{}));
      continue;
    }
    // Check if this is a spread element
    if (auto* spread = std::get_if<SpreadElement>(&elem->node)) {
      // Evaluate the argument
      auto task = evaluate(*spread->argument);
  Value val;
  LIGHTJS_RUN_TASK(task, val);

      // Spread the value into the array
      if (val.isArray()) {
        auto srcArr = std::get<std::shared_ptr<Array>>(val.data);
        for (const auto& item : srcArr->elements) {
          arr->elements.push_back(item);
        }
      } else if (val.isString()) {
        // String is iterable
        std::string str = std::get<std::string>(val.data);
        for (char ch : str) {
          arr->elements.push_back(Value(std::string(1, ch)));
        }
      } else if (val.isObject()) {
        // Try iterator protocol: object with .next() method
        auto obj = std::get<std::shared_ptr<Object>>(val.data);
        auto nextIt = obj->properties.find("next");
        if (nextIt != obj->properties.end() && nextIt->second.isFunction()) {
          for (size_t iterLimit = 0; iterLimit < 100000; ++iterLimit) {
            Value step = callFunction(nextIt->second, {}, val);
            if (step.isObject()) {
              auto stepObj = std::get<std::shared_ptr<Object>>(step.data);
              auto doneIt = stepObj->properties.find("done");
              if (doneIt != stepObj->properties.end() && doneIt->second.toBool()) break;
              auto valueIt = stepObj->properties.find("value");
              arr->elements.push_back(valueIt != stepObj->properties.end() ? valueIt->second : Value(Undefined{}));
            } else {
              break;
            }
          }
        } else {
          arr->elements.push_back(val);
        }
      } else {
        arr->elements.push_back(val);
      }
    } else {
      auto task = evaluate(*elem);
      LIGHTJS_RUN_TASK_VOID(task);
      arr->elements.push_back(task.result());
    }
  }
  LIGHTJS_RETURN(Value(arr));
}

Task Interpreter::evaluateObject(const ObjectExpr& expr) {
  // Check memory limit before allocation
  if (!checkMemoryLimit(sizeof(Object))) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  auto obj = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // Set __proto__ to Object.prototype for prototype chain resolution
  auto objCtor = env_->get("Object");
  if (objCtor) {
    std::unordered_map<std::string, Value>* ctorProps = nullptr;
    if (objCtor->isFunction()) {
      ctorProps = &std::get<std::shared_ptr<Function>>(objCtor->data)->properties;
    } else if (objCtor->isObject()) {
      ctorProps = &std::get<std::shared_ptr<Object>>(objCtor->data)->properties;
    }
    if (ctorProps) {
      auto protoIt = ctorProps->find("prototype");
      if (protoIt != ctorProps->end() && protoIt->second.isObject()) {
        obj->properties["__proto__"] = protoIt->second;
      }
    }
  }

  for (const auto& prop : expr.properties) {
    if (prop.isSpread) {
      // Handle spread syntax: ...sourceObj
      auto spreadTask = evaluate(*prop.value);
  Value spreadVal;
  LIGHTJS_RUN_TASK(spreadTask, spreadVal);

      // Copy properties from spread object
      if (spreadVal.isObject()) {
        auto sourceObj = std::get<std::shared_ptr<Object>>(spreadVal.data);
        for (const auto& [key, value] : sourceObj->properties) {
          obj->properties[key] = value;
        }
      }
    } else {
      // Regular property
      std::string key;

      // For identifier keys, use the identifier name directly (not its value from environment)
      if (prop.key) {
        if (prop.isComputed) {
          // For computed property names, evaluate the expression
          auto keyTask = evaluate(*prop.key);
          LIGHTJS_RUN_TASK_VOID(keyTask);
          key = keyTask.result().toString();
        } else if (auto* ident = std::get_if<Identifier>(&prop.key->node)) {
          key = ident->name;
        } else if (auto* str = std::get_if<StringLiteral>(&prop.key->node)) {
          key = str->value;
        } else if (auto* num = std::get_if<NumberLiteral>(&prop.key->node)) {
          key = std::to_string(static_cast<int>(num->value));
        } else {
          // Fallback: evaluate as expression
          auto keyTask = evaluate(*prop.key);
          LIGHTJS_RUN_TASK_VOID(keyTask);
          key = keyTask.result().toString();
        }
      }

      auto valTask = evaluate(*prop.value);
      LIGHTJS_RUN_TASK_VOID(valTask);
      obj->properties[key] = valTask.result();
    }
  }
  LIGHTJS_RETURN(Value(obj));
}

Task Interpreter::evaluateFunction(const FunctionExpr& expr) {
  auto func = std::make_shared<Function>();
  func->isNative = false;
  func->isAsync = expr.isAsync;
  func->isGenerator = expr.isGenerator;
  func->isStrict = strictMode_ || hasUseStrictDirective(expr.body);

  for (const auto& param : expr.params) {
    FunctionParam funcParam;
    funcParam.name = param.name.name;
    if (param.defaultValue) {
      funcParam.defaultValue = std::shared_ptr<void>(const_cast<Expression*>(param.defaultValue.get()), [](void*){});
    }
    func->params.push_back(funcParam);
  }

  if (expr.restParam.has_value()) {
    func->restParam = expr.restParam->name;
  }

  func->body = std::shared_ptr<void>(const_cast<std::vector<StmtPtr>*>(&expr.body), [](void*){});
  func->closure = env_;
  // Compute length: number of params before first default parameter
  size_t funcLength = 0;
  for (const auto& param : expr.params) {
    if (param.defaultValue) break;
    funcLength++;
  }
  func->properties["length"] = Value(static_cast<double>(funcLength));
  func->properties["name"] = Value(expr.name);
  if (!expr.name.empty()) {
    func->properties["__named_expression__"] = Value(true);
  }
  // Regular functions (non-arrow, non-method) are constructors
  if (!expr.isArrow) {
    func->isConstructor = true;
    // Create default prototype object with constructor back-reference
    auto proto = std::make_shared<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    proto->properties["constructor"] = Value(func);
    proto->properties["__non_enum_constructor"] = Value(true);
    func->properties["prototype"] = Value(proto);
  }

  // Set __proto__ to Function.prototype for proper prototype chain
  auto funcVal = env_->getRoot()->get("Function");
  if (funcVal.has_value() && funcVal->isFunction()) {
    auto funcCtor = std::get<std::shared_ptr<Function>>(funcVal->data);
    auto protoIt = funcCtor->properties.find("prototype");
    if (protoIt != funcCtor->properties.end()) {
      func->properties["__proto__"] = protoIt->second;
    }
  }

  LIGHTJS_RETURN(Value(func));
}

Task Interpreter::evaluateAwait(const AwaitExpr& expr) {
  auto task = evaluate(*expr.argument);
  Value val;
  LIGHTJS_RUN_TASK(task, val);

  if (!val.isPromise() && isObjectLike(val)) {
    auto [foundThen, thenValue] = getPropertyForPrimitive(val, "then");
    if (hasError()) {
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (foundThen && thenValue.isFunction()) {
      auto promise = std::make_shared<Promise>();

      auto resolveFunc = std::make_shared<Function>();
      resolveFunc->isNative = true;
      resolveFunc->nativeFunc = [promise](const std::vector<Value>& args) -> Value {
        if (!args.empty()) {
          promise->resolve(args[0]);
        } else {
          promise->resolve(Value(Undefined{}));
        }
        return Value(Undefined{});
      };

      auto rejectFunc = std::make_shared<Function>();
      rejectFunc->isNative = true;
      rejectFunc->nativeFunc = [promise](const std::vector<Value>& args) -> Value {
        if (!args.empty()) {
          promise->reject(args[0]);
        } else {
          promise->reject(Value(Undefined{}));
        }
        return Value(Undefined{});
      };

      callFunction(thenValue, {Value(resolveFunc), Value(rejectFunc)}, val);
      if (hasError()) {
        Value err = getError();
        clearError();
        promise->reject(err);
      }

      val = Value(promise);
    }
  }

  if (val.isPromise()) {
    auto promise = std::get<std::shared_ptr<Promise>>(val.data);
    if (promise->state == PromiseState::Pending) {
      // TinyJS models await synchronously; drive pending microtasks until settled.
      auto& loop = EventLoopContext::instance().getLoop();
      constexpr size_t kMaxAwaitTicks = 10000;
      size_t ticks = 0;
      while (promise->state == PromiseState::Pending &&
             loop.hasPendingWork() &&
             ticks < kMaxAwaitTicks) {
        loop.runOnce();
        ticks++;
      }
    }

    if (promise->state == PromiseState::Fulfilled) {
      LIGHTJS_RETURN(promise->result);
    } else if (promise->state == PromiseState::Rejected) {
      flow_.type = ControlFlow::Type::Throw;
      flow_.value = promise->result;
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  // Await on non-Promise values still yields to queued microtasks.
  if (!suppressMicrotasks_) {
    auto& loop = EventLoopContext::instance().getLoop();
    if (loop.pendingMicrotaskCount() > 0) {
      loop.runOnce();
    }
  }

  LIGHTJS_RETURN(val);
}

Task Interpreter::evaluateYield(const YieldExpr& expr) {
  auto resumeMode = flow_.takeResumeMode();
  Value resumeValue = flow_.takeResumeValue();

  if (resumeMode == ControlFlow::ResumeMode::Return) {
    flow_.type = ControlFlow::Type::Return;
    flow_.value = resumeValue;
    LIGHTJS_RETURN(Value(Undefined{}));
  }
  if (resumeMode == ControlFlow::ResumeMode::Throw) {
    flow_.type = ControlFlow::Type::Throw;
    flow_.value = resumeValue;
    LIGHTJS_RETURN(Value(Undefined{}));
  }
  if (resumeMode == ControlFlow::ResumeMode::Next) {
    LIGHTJS_RETURN(resumeValue);
  }

  // Evaluate the yielded value
  Value yieldedValue = Value(Undefined{});
  if (expr.argument) {
    auto task = evaluate(*expr.argument);
  LIGHTJS_RUN_TASK(task, yieldedValue);
  }

  // Set the Yield control flow to suspend execution
  flow_.setYield(yieldedValue);

  LIGHTJS_RETURN(yieldedValue);
}

Task Interpreter::constructValue(Value callee, const std::vector<Value>& args, const Value& newTargetOverride) {
  // Check memory limit before potential allocation
  if (!checkMemoryLimit(sizeof(Object))) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  if (!newTargetOverride.isUndefined()) {
    bool validNewTarget = false;
    if (newTargetOverride.isClass()) {
      validNewTarget = true;
    } else if (newTargetOverride.isFunction()) {
      auto fn = std::get<std::shared_ptr<Function>>(newTargetOverride.data);
      validNewTarget = fn->isConstructor;
    } else if (newTargetOverride.isProxy()) {
      validNewTarget = true;
    }

    if (!validNewTarget) {
      throwError(ErrorType::TypeError, "newTarget is not a constructor");
      LIGHTJS_RETURN(Value(Undefined{}));
    }
  }
  Value effectiveNewTarget = newTargetOverride.isUndefined() ? callee : newTargetOverride;

  // Handle Proxy construct trap
  if (callee.isProxy()) {
    auto proxyPtr = std::get<std::shared_ptr<Proxy>>(callee.data);
    if (proxyPtr->handler && proxyPtr->handler->isObject()) {
      auto handlerObj = std::get<std::shared_ptr<Object>>(proxyPtr->handler->data);
      auto trapIt = handlerObj->properties.find("construct");
      if (trapIt != handlerObj->properties.end() && trapIt->second.isFunction()) {
        auto trap = std::get<std::shared_ptr<Function>>(trapIt->second.data);
        // Create args array
        auto argsArray = std::make_shared<Array>();
        argsArray->elements = args;
        // Call construct trap: handler.construct(target, argumentsList, newTarget)
        std::vector<Value> trapArgs = {*proxyPtr->target, Value(argsArray), effectiveNewTarget};
        Value result;
        if (trap->isNative) {
          result = trap->nativeFunc(trapArgs);
        } else {
          result = invokeFunction(trap, trapArgs, Value(Undefined{}));
        }
        // construct trap must return an object
        if (result.isObject() || result.isArray() || result.isFunction()) {
          LIGHTJS_RETURN(result);
        }
        throwError(ErrorType::TypeError, "'construct' on proxy: trap returned non-object");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
    }
    // No construct trap - pass through to target
    if (proxyPtr->target) {
      callee = *proxyPtr->target;
    }
  }

  auto setConstructorTag = [&](Value& instanceVal) {
    Value constructorTag = newTargetOverride.isUndefined() ? callee : effectiveNewTarget;
    if (instanceVal.isObject()) {
      auto obj = std::get<std::shared_ptr<Object>>(instanceVal.data);
      obj->properties["__constructor__"] = constructorTag;
    } else if (instanceVal.isArray()) {
      auto arr = std::get<std::shared_ptr<Array>>(instanceVal.data);
      arr->properties["__constructor__"] = constructorTag;
    } else if (instanceVal.isFunction()) {
      auto fn = std::get<std::shared_ptr<Function>>(instanceVal.data);
      fn->properties["__constructor__"] = constructorTag;
    } else if (instanceVal.isRegex()) {
      auto regex = std::get<std::shared_ptr<Regex>>(instanceVal.data);
      regex->properties["__constructor__"] = constructorTag;
    } else if (instanceVal.isPromise()) {
      auto promise = std::get<std::shared_ptr<Promise>>(instanceVal.data);
      promise->properties["__constructor__"] = constructorTag;
    }
  };

  auto wrapPrimitiveValue = [&](const Value& primitive) -> Value {
    auto wrapper = std::make_shared<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    wrapper->properties["__primitive_value__"] = primitive;

    auto valueOfFn = std::make_shared<Function>();
    valueOfFn->isNative = true;
    valueOfFn->properties["__uses_this_arg__"] = Value(true);
    valueOfFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
      if (!args.empty()) {
        if (args[0].isNumber() || args[0].isString() || args[0].isBool()) {
          return args[0];
        }
        if (args[0].isObject()) {
          auto obj = std::get<std::shared_ptr<Object>>(args[0].data);
          auto it = obj->properties.find("__primitive_value__");
          if (it != obj->properties.end()) {
            return it->second;
          }
        }
      }
      return Value(Undefined{});
    };
    wrapper->properties["valueOf"] = Value(valueOfFn);

    return Value(wrapper);
  };

  if (callee.isObject()) {
    auto objPtr = std::get<std::shared_ptr<Object>>(callee.data);
    auto callableIt = objPtr->properties.find("__callable_object__");
    bool isCallableWrapper = callableIt != objPtr->properties.end() &&
                             callableIt->second.isBool() &&
                             callableIt->second.toBool();
    if (isCallableWrapper) {
      auto ctorIt = objPtr->properties.find("constructor");
      if (ctorIt != objPtr->properties.end()) {
        callee = ctorIt->second;
      }
    }
  }

  // Handle Class constructor
  if (callee.isClass()) {
    auto cls = std::get<std::shared_ptr<Class>>(callee.data);

    // Create the new instance object
    auto instance = std::make_shared<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));

    // Set up prototype chain - copy methods to instance
    for (const auto& [name, method] : cls->methods) {
      instance->properties[name] = Value(method);
    }

    // If class has a superclass, inherit its methods
    if (cls->superClass) {
      for (const auto& [name, method] : cls->superClass->methods) {
        if (instance->properties.find(name) == instance->properties.end()) {
          instance->properties[name] = Value(method);
        }
      }
    }

    // Execute constructor if it exists
    if (cls->constructor) {
      auto prevEnv = env_;
      env_ = std::static_pointer_cast<Environment>(cls->closure);
      env_ = env_->createChild();

      // Bind 'this' to the new instance
      env_->define("this", Value(instance));
      env_->define("__new_target__", effectiveNewTarget);

      // Bind super class if exists
      if (cls->superClass) {
        env_->define("__super__", Value(cls->superClass));
      } else {
        auto superCtorIt = cls->properties.find("__super_constructor__");
        if (superCtorIt != cls->properties.end()) {
          env_->define("__super__", superCtorIt->second);
        }
      }

      // Bind parameters
      auto func = cls->constructor;
      for (size_t i = 0; i < func->params.size(); ++i) {
        if (i < args.size()) {
          env_->define(func->params[i].name, args[i]);
        } else if (func->params[i].defaultValue) {
          auto defaultExpr = std::static_pointer_cast<Expression>(func->params[i].defaultValue);
          auto defaultTask = evaluate(*defaultExpr);
          LIGHTJS_RUN_TASK_VOID(defaultTask);
          env_->define(func->params[i].name, defaultTask.result());
        } else {
          env_->define(func->params[i].name, Value(Undefined{}));
        }
      }

      // Handle rest parameter
      if (func->restParam.has_value()) {
        auto restArr = std::make_shared<Array>();
        GarbageCollector::instance().reportAllocation(sizeof(Array));
        for (size_t i = func->params.size(); i < args.size(); ++i) {
          restArr->elements.push_back(args[i]);
        }
        env_->define(*func->restParam, Value(restArr));
      }

      // Execute constructor body
      auto bodyPtr = std::static_pointer_cast<std::vector<StmtPtr>>(func->body);
      auto prevFlow = flow_;
      flow_ = ControlFlow{};

      for (const auto& stmt : *bodyPtr) {
        auto stmtTask = evaluate(*stmt);
        LIGHTJS_RUN_TASK_VOID(stmtTask);

        if (flow_.type == ControlFlow::Type::Return) {
          break;
        }
      }

      // Get the final `this` value (super() may have replaced it)
      Value finalThis = Value(instance);
      if (auto currentThis = env_->get("this")) {
        finalThis = *currentThis;
      }

      flow_ = prevFlow;
      env_ = prevEnv;

      setConstructorTag(finalThis);
      // If super() replaced `this` with a different type, set `constructor` property
      // to point to this class (simulates prototype.constructor inheritance)
      if (finalThis.isPromise()) {
        auto p = std::get<std::shared_ptr<Promise>>(finalThis.data);
        p->properties["constructor"] = callee;
      } else if (finalThis.isObject() &&
                 std::get<std::shared_ptr<Object>>(finalThis.data).get() != instance.get()) {
        auto obj = std::get<std::shared_ptr<Object>>(finalThis.data);
        obj->properties["constructor"] = callee;
      } else if (finalThis.isArray()) {
        auto arr = std::get<std::shared_ptr<Array>>(finalThis.data);
        arr->properties["constructor"] = callee;
      }
      LIGHTJS_RETURN(finalThis);
    }

    // No explicit constructor with Function super: implicitly call super(...args)
    // ES2020: default constructor for derived class is constructor(...args) { super(...args); }
    // Only for class-extends-Function (not class-extends-Class, which is handled above)
    auto superCtorIt = cls->properties.find("__super_constructor__");
    if (superCtorIt != cls->properties.end()) {
      Value superVal = superCtorIt->second;
      Value result = LIGHTJS_AWAIT(constructValue(superVal, args, effectiveNewTarget));
      if (flow_.type != ControlFlow::Type::None) {
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      setConstructorTag(result);
      if (result.isPromise()) {
        auto p = std::get<std::shared_ptr<Promise>>(result.data);
        p->properties["constructor"] = callee;
      } else if (result.isObject()) {
        auto obj = std::get<std::shared_ptr<Object>>(result.data);
        obj->properties["constructor"] = callee;
      }
      LIGHTJS_RETURN(result);
    }

    instance->properties["__constructor__"] = callee;
    LIGHTJS_RETURN(Value(instance));
  }

  // Handle Function constructor (regular constructor function)
  if (callee.isFunction()) {
    auto func = std::get<std::shared_ptr<Function>>(callee.data);

    if (func->isNative) {
      if (!func->isConstructor) {
        throwError(ErrorType::TypeError, "Function is not a constructor");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      auto noNewIt = func->properties.find("__throw_on_new__");
      if (noNewIt != func->properties.end() && noNewIt->second.isBool() && noNewIt->second.toBool()) {
        throwError(ErrorType::TypeError, "Function is not a constructor");
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      // Native constructors (e.g., Array, Object, Map, etc.)
      Value constructed = func->nativeFunc(args);
      if (constructed.isNumber() || constructed.isString() || constructed.isBool()) {
        constructed = wrapPrimitiveValue(constructed);
      }
      setConstructorTag(constructed);
      LIGHTJS_RETURN(constructed);
    }

    // Create the new instance object
    auto instance = std::make_shared<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));

    // Set __proto__ to constructor's prototype (OrdinaryCreateFromConstructor)
    auto protoIt = func->properties.find("prototype");
    if (protoIt != func->properties.end() && protoIt->second.isObject()) {
      instance->properties["__proto__"] = protoIt->second;
    }

    // Set up execution environment
    auto prevEnv = env_;
    env_ = std::static_pointer_cast<Environment>(func->closure);
    env_ = env_->createChild();

    // Bind 'this' to the new instance
    env_->define("this", Value(instance));
    env_->define("__new_target__", effectiveNewTarget);

    // Bind parameters
    for (size_t i = 0; i < func->params.size(); ++i) {
      if (i < args.size()) {
        env_->define(func->params[i].name, args[i]);
      } else if (func->params[i].defaultValue) {
        auto defaultExpr = std::static_pointer_cast<Expression>(func->params[i].defaultValue);
        auto defaultTask = evaluate(*defaultExpr);
        LIGHTJS_RUN_TASK_VOID(defaultTask);
        env_->define(func->params[i].name, defaultTask.result());
      } else {
        env_->define(func->params[i].name, Value(Undefined{}));
      }
    }

    // Handle rest parameter
    if (func->restParam.has_value()) {
      auto restArr = std::make_shared<Array>();
      GarbageCollector::instance().reportAllocation(sizeof(Array));
      for (size_t i = func->params.size(); i < args.size(); ++i) {
        restArr->elements.push_back(args[i]);
      }
      env_->define(*func->restParam, Value(restArr));
    }

    // Execute function body
    auto bodyPtr = std::static_pointer_cast<std::vector<StmtPtr>>(func->body);
    Value result = Value(Undefined{});
    auto prevFlow = flow_;
    flow_ = ControlFlow{};

    for (const auto& stmt : *bodyPtr) {
      auto stmtTask = evaluate(*stmt);
  LIGHTJS_RUN_TASK(stmtTask, result);

      if (flow_.type == ControlFlow::Type::Return) {
        // If constructor returns an object-like value, use that; otherwise use the instance
        // ES spec: "If Type(result) is Object, return result."
        if (flow_.value.isObject()) {
          instance = std::get<std::shared_ptr<Object>>(flow_.value.data);
        } else if (isObjectLike(flow_.value)) {
          // Constructor returned a non-plain-Object type (Promise, Array, Function, etc.)
          Value returnedVal = flow_.value;
          flow_ = prevFlow;
          env_ = prevEnv;
          setConstructorTag(returnedVal);
          LIGHTJS_RETURN(returnedVal);
        }
        break;
      }
    }

    flow_ = prevFlow;
    env_ = prevEnv;

    Value instanceVal(instance);
    setConstructorTag(instanceVal);
    LIGHTJS_RETURN(Value(instance));
  }

  // Not a constructor
  flow_.type = ControlFlow::Type::Throw;
  flow_.value = Value(std::make_shared<Error>(
    ErrorType::TypeError,
    "Value is not a constructor"
  ));
  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::evaluateNew(const NewExpr& expr) {
  // Evaluate the callee (constructor)
  auto calleeTask = evaluate(*expr.callee);
  Value callee;
  LIGHTJS_RUN_TASK(calleeTask, callee);

  // Evaluate arguments
  std::vector<Value> args;
  for (const auto& arg : expr.arguments) {
    auto argTask = evaluate(*arg);
    LIGHTJS_RUN_TASK_VOID(argTask);
    args.push_back(argTask.result());
  }

  LIGHTJS_RETURN(LIGHTJS_AWAIT(constructValue(callee, args, Value(Undefined{}))));
}

Task Interpreter::evaluateClass(const ClassExpr& expr) {
  auto cls = std::make_shared<Class>(expr.name);
  GarbageCollector::instance().reportAllocation(sizeof(Class));
  cls->closure = env_;

  // Handle superclass
  if (expr.superClass) {
    auto superTask = evaluate(*expr.superClass);
  Value superVal;
  LIGHTJS_RUN_TASK(superTask, superVal);
    if (superVal.isClass()) {
      cls->superClass = std::get<std::shared_ptr<Class>>(superVal.data);
    } else if (superVal.isFunction()) {
      cls->properties["__super_constructor__"] = superVal;
      auto superFunc = std::get<std::shared_ptr<Function>>(superVal.data);
      for (const auto& [key, val] : superFunc->properties) {
        if (key.size() >= 2 && key[0] == '_' && key[1] == '_') continue;
        if (key == "name" || key == "length" || key == "prototype" ||
            key == "caller" || key == "arguments") continue;
        if (cls->properties.find(key) == cls->properties.end()) {
          cls->properties[key] = val;
        }
      }
    }
  }

  // Process methods
  for (const auto& method : expr.methods) {
    // Create function from method definition
    auto func = std::make_shared<Function>();
    func->isNative = false;
    func->isAsync = method.isAsync;
    func->isStrict = true;  // Class bodies are always strict.
    func->closure = env_;

    for (const auto& param : method.params) {
      FunctionParam funcParam;
      funcParam.name = param.name;
      func->params.push_back(funcParam);
    }

    // Store the body - we need to cast away const for the shared_ptr
    func->body = std::shared_ptr<void>(
      const_cast<std::vector<StmtPtr>*>(&method.body),
      [](void*){} // No-op deleter since we don't own the memory
    );
    if (method.kind == MethodDefinition::Kind::Constructor) {
      func->properties["name"] = Value(std::string("constructor"));
    } else {
      func->properties["name"] = Value(method.key.name);
    }
    if (cls->superClass) {
      func->properties["__super_class__"] = Value(cls->superClass);
    } else if (cls->properties.find("__super_constructor__") != cls->properties.end()) {
      func->properties["__super_class__"] = cls->properties["__super_constructor__"];
    } else if (auto objectCtor = env_->get("Object")) {
      func->properties["__super_class__"] = *objectCtor;
    }

    if (method.kind == MethodDefinition::Kind::Constructor) {
      cls->constructor = func;
    } else if (method.isStatic) {
      cls->staticMethods[method.key.name] = func;
      // Static methods become own properties of the class
      cls->properties[method.key.name] = Value(func);
    } else if (method.kind == MethodDefinition::Kind::Get) {
      cls->getters[method.key.name] = func;
    } else if (method.kind == MethodDefinition::Kind::Set) {
      cls->setters[method.key.name] = func;
    } else {
      cls->methods[method.key.name] = func;
    }
  }

  // Set name as own property (per spec: SetFunctionName)
  // Named classes always get a name property; anonymous classes don't (until named evaluation)
  if (!cls->name.empty()) {
    cls->properties["name"] = Value(cls->name);
    cls->properties["__non_writable_name"] = Value(true);
    cls->properties["__non_enum_name"] = Value(true);
  }

  // Set length property (constructor parameter count)
  int ctorLen = cls->constructor ? (int)cls->constructor->params.size() : 0;
  cls->properties["length"] = Value((double)ctorLen);
  cls->properties["__non_writable_length"] = Value(true);
  cls->properties["__non_enum_length"] = Value(true);

  LIGHTJS_RETURN(Value(cls));
}

// Helper for recursively binding destructuring patterns
void Interpreter::bindDestructuringPattern(const Expression& pattern, const Value& value, bool isConst, bool useSet) {
  if (auto* assign = std::get_if<AssignmentPattern>(&pattern.node)) {
    Value boundValue = value;
    if (boundValue.isUndefined()) {
      auto initTask = evaluate(*assign->right);
      Value initValue = Value(Undefined{});
      LIGHTJS_RUN_TASK(initTask, initValue);
      boundValue = initValue;
      // Named evaluation: set function/class name from binding identifier
      // Only when the initializer is an anonymous function definition (not comma expression)
      if (auto* leftId = std::get_if<Identifier>(&assign->left->node)) {
        bool isAnonymousFnDef = !std::get_if<SequenceExpr>(&assign->right->node);
        if (isAnonymousFnDef && boundValue.isFunction()) {
          auto fn = std::get<std::shared_ptr<Function>>(boundValue.data);
          auto nameIt = fn->properties.find("name");
          if (nameIt != fn->properties.end() && nameIt->second.isString() && nameIt->second.toString().empty()) {
            fn->properties["name"] = Value(leftId->name);
          } else if (nameIt == fn->properties.end()) {
            fn->properties["name"] = Value(leftId->name);
          }
        } else if (isAnonymousFnDef) {
          if (auto* cls = std::get_if<std::shared_ptr<Class>>(&boundValue.data)) {
            // Per spec: HasOwnProperty(v, "name") check before SetFunctionName
            bool hasNameProperty = (*cls)->properties.find("name") != (*cls)->properties.end();
            if (!hasNameProperty) {
              (*cls)->name = leftId->name;
              (*cls)->properties["name"] = Value(leftId->name);
              (*cls)->properties["__non_writable_name"] = Value(true);
              (*cls)->properties["__non_enum_name"] = Value(true);
            }
          }
        }
      }
    }
    bindDestructuringPattern(*assign->left, boundValue, isConst, useSet);
    return;
  }

  if (auto* id = std::get_if<Identifier>(&pattern.node)) {
    // Check TDZ before assignment (e.g., assigning to `let` variable before its declaration)
    if (useSet && env_->isTDZ(id->name)) {
      throwError(ErrorType::ReferenceError,
                 "Cannot access '" + id->name + "' before initialization");
      return;
    }
    // Simple identifier
    if (useSet) {
      if (!env_->set(id->name, value)) {
        // Check if this is a const binding being reassigned
        if (env_->isConst(id->name)) {
          throwError(ErrorType::TypeError, "Assignment to constant variable '" + id->name + "'");
          return;
        }
        if (strictMode_) {
          // In strict mode, assignment to undeclared variable is a ReferenceError
          throwError(ErrorType::ReferenceError, id->name + " is not defined");
          return;
        }
        // In non-strict mode, assignment to undeclared variable creates global
        env_->getRoot()->define(id->name, value);
      }
    } else {
      env_->define(id->name, value, isConst);
    }
  } else if (auto* member = std::get_if<MemberExpr>(&pattern.node)) {
    // MemberExpression target in assignment destructuring (e.g., x.y, obj['key'])
    auto objTask = evaluate(*member->object);
    Value objVal = Value(Undefined{});
    LIGHTJS_RUN_TASK(objTask, objVal);
    if (objVal.isObject()) {
      auto obj = std::get<std::shared_ptr<Object>>(objVal.data);
      std::string prop;
      if (member->computed) {
        auto propTask = evaluate(*member->property);
        Value propVal = Value(Undefined{});
        LIGHTJS_RUN_TASK(propTask, propVal);
        prop = propVal.toString();
      } else if (auto* propId = std::get_if<Identifier>(&member->property->node)) {
        prop = propId->name;
      }
      // Check for setter
      auto setterIt = obj->properties.find("__set_" + prop);
      if (setterIt != obj->properties.end() && setterIt->second.isFunction()) {
        callFunction(setterIt->second, {value}, objVal);
      } else {
        obj->properties[prop] = value;
      }
    } else if (objVal.isArray()) {
      auto arr = std::get<std::shared_ptr<Array>>(objVal.data);
      if (member->computed) {
        auto propTask = evaluate(*member->property);
        Value propVal = Value(Undefined{});
        LIGHTJS_RUN_TASK(propTask, propVal);
        size_t idx = static_cast<size_t>(propVal.toNumber());
        if (idx < arr->elements.size()) {
          arr->elements[idx] = value;
        }
      }
    }
  } else if (auto* arrayPat = std::get_if<ArrayPattern>(&pattern.node)) {
    // Array destructuring - null/undefined are not iterable
    if (value.isNull() || value.isUndefined()) {
      throwError(ErrorType::TypeError, "Cannot destructure " + value.toString() + " as it is not iterable");
      return;
    }
    std::shared_ptr<Array> arr;
    if (value.isArray()) {
      arr = std::get<std::shared_ptr<Array>>(value.data);
    } else if (value.isString()) {
      // Strings are iterable - convert to array of chars
      auto str = std::get<std::string>(value.data);
      arr = std::make_shared<Array>();
      for (size_t i = 0; i < str.size(); ++i) {
        arr->elements.push_back(Value(std::string(1, str[i])));
      }
    } else if (value.isGenerator()) {
      // Generators are iterable - lazily iterate via next()
      auto gen = std::get<std::shared_ptr<Generator>>(value.data);
      arr = std::make_shared<Array>();
      IteratorRecord genRecord;
      genRecord.kind = IteratorRecord::Kind::Generator;
      genRecord.generator = gen;
      size_t needed = arrayPat->elements.size();
      bool hasRest = (arrayPat->rest != nullptr);
      // Only pull as many elements as the pattern requires
      for (size_t i = 0; i < needed || hasRest; ++i) {
        Value stepResult = iteratorNext(genRecord);
        if (!stepResult.isObject()) break;
        auto stepObj = std::get<std::shared_ptr<Object>>(stepResult.data);
        auto doneIt2 = stepObj->properties.find("done");
        if (doneIt2 != stepObj->properties.end() && doneIt2->second.toBool()) break;
        auto valIt = stepObj->properties.find("value");
        arr->elements.push_back(valIt != stepObj->properties.end() ? valIt->second : Value(Undefined{}));
        if (i >= needed && !hasRest) break;
      }
    } else if (value.isObject()) {
      // Check for Symbol.iterator on objects
      auto obj = std::get<std::shared_ptr<Object>>(value.data);
      const auto& iteratorKey = WellKnownSymbols::iteratorKey();
      auto it = obj->properties.find(iteratorKey);
      if (it != obj->properties.end() && it->second.isFunction()) {
        Value iterResult = callFunction(it->second, {}, value);
        if (iterResult.isObject()) {
          // Use iterator protocol to lazily collect elements
          auto iterObj = std::get<std::shared_ptr<Object>>(iterResult.data);
          arr = std::make_shared<Array>();
          auto nextIt = iterObj->properties.find("next");
          if (nextIt != iterObj->properties.end() && nextIt->second.isFunction()) {
            size_t needed = arrayPat->elements.size();
            bool hasRest = (arrayPat->rest != nullptr);
            bool iteratorDone = false;
            for (size_t i = 0; i < needed || hasRest; ++i) {
              Value stepResult = callFunction(nextIt->second, {}, iterResult);
              // If next() throws, spec says iterator is considered done
              if (flow_.type == ControlFlow::Type::Throw) { iteratorDone = true; return; }
              if (!stepResult.isObject()) { iteratorDone = true; break; }
              auto stepObj = std::get<std::shared_ptr<Object>>(stepResult.data);
              // Check for getter on 'done' property
              bool isDone = false;
              auto doneGetterIt = stepObj->properties.find("__get_done");
              if (doneGetterIt != stepObj->properties.end() && doneGetterIt->second.isFunction()) {
                Value doneVal = callFunction(doneGetterIt->second, {}, stepResult);
                if (flow_.type == ControlFlow::Type::Throw) { iteratorDone = true; return; }
                isDone = doneVal.toBool();
              } else {
                auto doneIt2 = stepObj->properties.find("done");
                isDone = (doneIt2 != stepObj->properties.end() && doneIt2->second.toBool());
              }
              if (isDone) { iteratorDone = true; break; }
              // Check for getter on 'value' property
              Value elemVal;
              auto valGetterIt = stepObj->properties.find("__get_value");
              if (valGetterIt != stepObj->properties.end() && valGetterIt->second.isFunction()) {
                elemVal = callFunction(valGetterIt->second, {}, stepResult);
                if (flow_.type == ControlFlow::Type::Throw) { iteratorDone = true; return; }
              } else {
                auto valIt = stepObj->properties.find("value");
                elemVal = (valIt != stepObj->properties.end()) ? valIt->second : Value(Undefined{});
              }
              arr->elements.push_back(elemVal);
              if (i >= needed && !hasRest) break;
            }
            // IteratorClose: if iterator was not exhausted, close it
            if (!iteratorDone) {
              IteratorRecord closeRec;
              closeRec.kind = IteratorRecord::Kind::IteratorObject;
              closeRec.iteratorObject = iterObj;
              iteratorClose(closeRec);
              if (flow_.type == ControlFlow::Type::Throw) return;
            }
          }
        } else {
          arr = std::make_shared<Array>();
        }
      } else {
        throwError(ErrorType::TypeError, value.toString() + " is not iterable");
        return;
      }
    } else {
      // Primitives (bool, number, bigint) are not iterable
      throwError(ErrorType::TypeError, value.toString() + " is not iterable");
      return;
    }

    for (size_t i = 0; i < arrayPat->elements.size(); ++i) {
      if (!arrayPat->elements[i]) continue;  // Skip holes

      Value elemValue = (i < arr->elements.size()) ? arr->elements[i] : Value(Undefined{});

      // Recursively bind (handles nested patterns)
      bindDestructuringPattern(*arrayPat->elements[i], elemValue, isConst, useSet);
      if (flow_.type == ControlFlow::Type::Throw) return;
    }

    // Handle rest element
    if (arrayPat->rest) {
      auto restArr = std::make_shared<Array>();
      for (size_t i = arrayPat->elements.size(); i < arr->elements.size(); ++i) {
        restArr->elements.push_back(arr->elements[i]);
      }
      bindDestructuringPattern(*arrayPat->rest, Value(restArr), isConst, useSet);
    }
  } else if (auto* objPat = std::get_if<ObjectPattern>(&pattern.node)) {
    // Object destructuring - null/undefined cannot be destructured
    if (value.isNull() || value.isUndefined()) {
      throwError(ErrorType::TypeError, "Cannot destructure " + value.toString() + " as it is not an object");
      return;
    }
    std::shared_ptr<Object> obj;
    if (value.isObject()) {
      obj = std::get<std::shared_ptr<Object>>(value.data);
    } else if (value.isArray()) {
      // Convert array to object-like representation for destructuring
      auto arr = std::get<std::shared_ptr<Array>>(value.data);
      obj = std::make_shared<Object>();
      for (size_t i = 0; i < arr->elements.size(); ++i) {
        obj->properties[std::to_string(i)] = arr->elements[i];
      }
      obj->properties["length"] = Value(static_cast<double>(arr->elements.size()));
    } else if (value.isString()) {
      // Convert string to object-like representation for destructuring
      auto str = std::get<std::string>(value.data);
      obj = std::make_shared<Object>();
      for (size_t i = 0; i < str.size(); ++i) {
        obj->properties[std::to_string(i)] = Value(std::string(1, str[i]));
      }
      obj->properties["length"] = Value(static_cast<double>(str.size()));
    } else {
      // Create empty object for other primitive values
      obj = std::make_shared<Object>();
    }

    std::unordered_set<std::string> extractedKeys;

    for (const auto& prop : objPat->properties) {
      std::string keyName;
      if (prop.computed) {
        // Computed property key: {[expr]: pattern}
        auto keyTask = evaluate(*prop.key);
        Value keyVal;
        LIGHTJS_RUN_TASK(keyTask, keyVal);
        keyName = keyVal.toString();
      } else if (auto* keyId = std::get_if<Identifier>(&prop.key->node)) {
        keyName = keyId->name;
      } else if (auto* keyStr = std::get_if<StringLiteral>(&prop.key->node)) {
        keyName = keyStr->value;
      } else if (auto* keyNum = std::get_if<NumberLiteral>(&prop.key->node)) {
        keyName = std::to_string(static_cast<int>(keyNum->value));
      } else {
        continue;
      }

      extractedKeys.insert(keyName);
      Value propValue;
      // Check for getter first
      auto getterIt = obj->properties.find("__get_" + keyName);
      if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
        propValue = callFunction(getterIt->second, {}, value);
        if (flow_.type == ControlFlow::Type::Throw) return;
      } else if (obj->properties.count(keyName)) {
        propValue = obj->properties[keyName];
      }

      // Recursively bind (handles nested patterns)
      bindDestructuringPattern(*prop.value, propValue, isConst, useSet);
      if (flow_.type == ControlFlow::Type::Throw) return;
    }

    // Handle rest properties
    if (objPat->rest) {
      auto restObj = std::make_shared<Object>();
      // First collect getter property names
      std::unordered_set<std::string> getterKeys;
      for (const auto& [key, val] : obj->properties) {
        if (key.size() > 6 && key.substr(0, 6) == "__get_") {
          std::string propName = key.substr(6);
          if (extractedKeys.find(propName) == extractedKeys.end() &&
              !obj->properties.count("__non_enum_" + propName)) {
            getterKeys.insert(propName);
          }
        }
      }
      // Copy regular properties (not extracted, not internal)
      for (const auto& [key, val] : obj->properties) {
        if (extractedKeys.find(key) == extractedKeys.end()) {
          // Skip internal properties
          if (key.size() >= 4 && key.substr(0, 2) == "__" && key.substr(key.size() - 2) == "__") continue;
          if (key.substr(0, 6) == "__get_" || key.substr(0, 6) == "__set_") continue;
          if (key.substr(0, 11) == "__non_enum_") continue;
          if (key.substr(0, 15) == "__non_writable_") continue;
          if (key.substr(0, 19) == "__non_configurable_") continue;
          if (key.substr(0, 7) == "__enum_") continue;
          // Skip non-enumerable properties
          if (obj->properties.count("__non_enum_" + key)) continue;
          // Skip if this key has a getter (handled below)
          if (getterKeys.count(key)) continue;
          restObj->properties[key] = val;
        }
      }
      // Call getters and store results as plain values
      for (const auto& propName : getterKeys) {
        auto getterIt = obj->properties.find("__get_" + propName);
        if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
          restObj->properties[propName] = callFunction(getterIt->second, {}, value);
        }
      }
      bindDestructuringPattern(*objPat->rest, Value(restObj), isConst, useSet);
    }
  }
  // Other node types are ignored (error case in real JS)
}

// Helper to invoke a JavaScript function synchronously (used by native array methods for callbacks)
Value Interpreter::invokeFunction(std::shared_ptr<Function> func, const std::vector<Value>& args, const Value& thisValue) {
  if (func->isNative) {
    auto itUsesThis = func->properties.find("__uses_this_arg__");
    if (itUsesThis != func->properties.end() && itUsesThis->second.isBool() && itUsesThis->second.toBool()) {
      std::vector<Value> nativeArgs;
      nativeArgs.reserve(args.size() + 1);
      nativeArgs.push_back(thisValue);
      nativeArgs.insert(nativeArgs.end(), args.begin(), args.end());
      return func->nativeFunc(nativeArgs);
    }
    return func->nativeFunc(args);
  }

  // Save current environment
  auto prevEnv = env_;
  env_ = std::static_pointer_cast<Environment>(func->closure);
  env_ = env_->createChild();

  Value boundThis = thisValue;
  if (!func->isStrict && (boundThis.isUndefined() || boundThis.isNull())) {
    if (auto globalThisValue = env_->get("globalThis")) {
      boundThis = *globalThisValue;
    }
  }
  if (!boundThis.isUndefined()) {
    env_->define("this", boundThis);
  }
  auto superIt = func->properties.find("__super_class__");
  if (superIt != func->properties.end()) {
    env_->define("__super__", superIt->second);
  }

  auto argumentsArray = std::make_shared<Array>();
  GarbageCollector::instance().reportAllocation(sizeof(Array));
  argumentsArray->elements = args;
  env_->define("arguments", Value(argumentsArray));

  // Bind parameters
  for (size_t i = 0; i < func->params.size(); ++i) {
    if (i < args.size()) {
      env_->define(func->params[i].name, args[i]);
    } else if (func->params[i].defaultValue) {
      auto defaultExpr = std::static_pointer_cast<Expression>(func->params[i].defaultValue);
      auto defaultTask = evaluate(*defaultExpr);
      LIGHTJS_RUN_TASK_VOID(defaultTask);
      env_->define(func->params[i].name, defaultTask.result());
    } else {
      env_->define(func->params[i].name, Value(Undefined{}));
    }
  }

  // Handle rest parameter
  if (func->restParam.has_value()) {
    auto restArr = std::make_shared<Array>();
    GarbageCollector::instance().reportAllocation(sizeof(Array));
    for (size_t i = func->params.size(); i < args.size(); ++i) {
      restArr->elements.push_back(args[i]);
    }
    env_->define(*func->restParam, Value(restArr));
  }

  // Execute function body
  auto bodyPtr = std::static_pointer_cast<std::vector<StmtPtr>>(func->body);
  bool previousStrictMode = strictMode_;
  strictMode_ = func->isStrict;
  Value result = Value(Undefined{});
  bool returned = false;

  auto prevFlow = flow_;
  flow_ = ControlFlow{};

  for (const auto& stmt : *bodyPtr) {
    auto stmtTask = evaluate(*stmt);
    Value stmtResult = Value(Undefined{});
    LIGHTJS_RUN_TASK(stmtTask, stmtResult);

    if (flow_.type == ControlFlow::Type::Return) {
      result = flow_.value;
      returned = true;
      break;
    }
    if (flow_.type == ControlFlow::Type::Throw) {
      break;
    }
  }

  if (!returned && flow_.type != ControlFlow::Type::Throw) {
    result = Value(Undefined{});
  }
  if (flow_.type != ControlFlow::Type::Throw) {
    flow_ = prevFlow;
  }
  strictMode_ = previousStrictMode;
  env_ = prevEnv;
  return result;
}

Task Interpreter::evaluateVarDecl(const VarDeclaration& decl) {
  for (const auto& declarator : decl.declarations) {
    Value value = Value(Undefined{});
    if (declarator.init) {
      auto task = evaluate(*declarator.init);
  LIGHTJS_RUN_TASK(task, value);
    } else if (decl.kind == VarDeclaration::Kind::Var) {
      // var without initializer: don't overwrite existing binding
      if (auto* id = std::get_if<Identifier>(&declarator.pattern->node)) {
        if (env_->has(id->name)) {
          continue;
        }
      }
    }

    // Use the unified destructuring helper
    bool isConst = (decl.kind == VarDeclaration::Kind::Const);
    // For var declarations, use set() to update the hoisted binding in function scope
    // rather than define() which would create a new binding in the current block scope
    bool useSet = (decl.kind == VarDeclaration::Kind::Var);
    bindDestructuringPattern(*declarator.pattern, value, isConst, useSet);
  }
  LIGHTJS_RETURN(Value(Undefined{}));
}

// Recursively collect var declarations from a statement and hoist them
// Helper: collect bound names from a pattern expression for var hoisting
static void collectVarHoistNames(const Expression& expr, std::vector<std::string>& names) {
  if (auto* id = std::get_if<Identifier>(&expr.node)) {
    names.push_back(id->name);
  } else if (auto* assign = std::get_if<AssignmentPattern>(&expr.node)) {
    if (assign->left) collectVarHoistNames(*assign->left, names);
  } else if (auto* arrPat = std::get_if<ArrayPattern>(&expr.node)) {
    for (const auto& elem : arrPat->elements) {
      if (elem) collectVarHoistNames(*elem, names);
    }
    if (arrPat->rest) collectVarHoistNames(*arrPat->rest, names);
  } else if (auto* objPat = std::get_if<ObjectPattern>(&expr.node)) {
    for (const auto& prop : objPat->properties) {
      if (prop.value) collectVarHoistNames(*prop.value, names);
    }
    if (objPat->rest) collectVarHoistNames(*objPat->rest, names);
  }
}

void Interpreter::hoistVarDeclarationsFromStmt(const Statement& stmt) {
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.node)) {
    if (varDecl->kind == VarDeclaration::Kind::Var) {
      for (const auto& declarator : varDecl->declarations) {
        std::vector<std::string> names;
        if (declarator.pattern) {
          collectVarHoistNames(*declarator.pattern, names);
        }
        for (const auto& name : names) {
          if (!env_->has(name)) {
            env_->define(name, Value(Undefined{}));
          }
        }
      }
    }
  } else if (auto* block = std::get_if<BlockStmt>(&stmt.node)) {
    hoistVarDeclarations(block->body);
  } else if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    if (ifStmt->consequent) hoistVarDeclarationsFromStmt(*ifStmt->consequent);
    if (ifStmt->alternate) hoistVarDeclarationsFromStmt(*ifStmt->alternate);
  } else if (auto* whileStmt = std::get_if<WhileStmt>(&stmt.node)) {
    if (whileStmt->body) hoistVarDeclarationsFromStmt(*whileStmt->body);
  } else if (auto* doWhile = std::get_if<DoWhileStmt>(&stmt.node)) {
    if (doWhile->body) hoistVarDeclarationsFromStmt(*doWhile->body);
  } else if (auto* forStmt = std::get_if<ForStmt>(&stmt.node)) {
    if (forStmt->init) hoistVarDeclarationsFromStmt(*forStmt->init);
    if (forStmt->body) hoistVarDeclarationsFromStmt(*forStmt->body);
  } else if (auto* forIn = std::get_if<ForInStmt>(&stmt.node)) {
    if (forIn->left) hoistVarDeclarationsFromStmt(*forIn->left);
    if (forIn->body) hoistVarDeclarationsFromStmt(*forIn->body);
  } else if (auto* forOf = std::get_if<ForOfStmt>(&stmt.node)) {
    if (forOf->left) hoistVarDeclarationsFromStmt(*forOf->left);
    if (forOf->body) hoistVarDeclarationsFromStmt(*forOf->body);
  } else if (auto* switchStmt = std::get_if<SwitchStmt>(&stmt.node)) {
    for (const auto& caseClause : switchStmt->cases) {
      hoistVarDeclarations(caseClause.consequent);
    }
  } else if (auto* tryStmt = std::get_if<TryStmt>(&stmt.node)) {
    hoistVarDeclarations(tryStmt->block);
    if (tryStmt->hasHandler) hoistVarDeclarations(tryStmt->handler.body);
    if (tryStmt->hasFinalizer) hoistVarDeclarations(tryStmt->finalizer);
  } else if (auto* labelled = std::get_if<LabelledStmt>(&stmt.node)) {
    if (labelled->body) hoistVarDeclarationsFromStmt(*labelled->body);
  } else if (auto* withStmt = std::get_if<WithStmt>(&stmt.node)) {
    if (withStmt->body) hoistVarDeclarationsFromStmt(*withStmt->body);
  } else if (auto* exportNamed = std::get_if<ExportNamedDeclaration>(&stmt.node)) {
    // Handle export var declarations: export var x = ...
    if (exportNamed->declaration) {
      hoistVarDeclarationsFromStmt(*exportNamed->declaration);
    }
  }
}

// Hoist var declarations and function declarations from a list of statements
void Interpreter::hoistVarDeclarations(const std::vector<StmtPtr>& body) {
  for (const auto& stmt : body) {
    hoistVarDeclarationsFromStmt(*stmt);
  }
}

Task Interpreter::evaluateFuncDecl(const FunctionDeclaration& decl) {
  auto func = std::make_shared<Function>();
  func->isNative = false;
  func->isAsync = decl.isAsync;
  func->isGenerator = decl.isGenerator;
  func->isStrict = strictMode_ || hasUseStrictDirective(decl.body);

  for (const auto& param : decl.params) {
    FunctionParam funcParam;
    funcParam.name = param.name.name;
    if (param.defaultValue) {
      funcParam.defaultValue = std::shared_ptr<void>(const_cast<Expression*>(param.defaultValue.get()), [](void*){});
    }
    func->params.push_back(funcParam);
  }

  if (decl.restParam.has_value()) {
    func->restParam = decl.restParam->name;
  }

  func->body = std::shared_ptr<void>(const_cast<std::vector<StmtPtr>*>(&decl.body), [](void*){});
  func->closure = env_;
  // Compute length: number of params before first default parameter
  size_t funcDeclLen = 0;
  for (const auto& param : decl.params) {
    if (param.defaultValue) break;
    funcDeclLen++;
  }
  func->properties["length"] = Value(static_cast<double>(funcDeclLen));
  func->properties["name"] = Value(decl.id.name);
  // Function declarations are always constructors
  func->isConstructor = true;

  // Create default prototype object with constructor back-reference
  auto proto = std::make_shared<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  proto->properties["constructor"] = Value(func);
  proto->properties["__non_enum_constructor"] = Value(true);
  func->properties["prototype"] = Value(proto);

  // Set __proto__ to Function.prototype for proper prototype chain
  auto funcVal = env_->getRoot()->get("Function");
  if (funcVal.has_value() && funcVal->isFunction()) {
    auto funcCtor = std::get<std::shared_ptr<Function>>(funcVal->data);
    auto protoIt = funcCtor->properties.find("prototype");
    if (protoIt != funcCtor->properties.end()) {
      func->properties["__proto__"] = protoIt->second;
    }
  }

  env_->define(decl.id.name, Value(func));
  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::evaluateReturn(const ReturnStmt& stmt) {
  Value result = Value(Undefined{});
  if (stmt.argument) {
    bool prevTailPosition = inTailPosition_;
    inTailPosition_ = true;
    auto task = evaluate(*stmt.argument);
    LIGHTJS_RUN_TASK(task, result);
    inTailPosition_ = prevTailPosition;

    // If an error was thrown during argument evaluation, preserve it
    if (flow_.type == ControlFlow::Type::Throw) {
      LIGHTJS_RETURN(result);
    }
  }
  flow_.type = ControlFlow::Type::Return;
  flow_.value = result;
  LIGHTJS_RETURN(result);
}

Task Interpreter::evaluateExprStmt(const ExpressionStmt& stmt) {
  auto task = evaluate(*stmt.expression);
  LIGHTJS_RUN_TASK_VOID(task);
  LIGHTJS_RETURN(task.result());
}

Task Interpreter::evaluateBlock(const BlockStmt& stmt) {
  auto prevEnv = env_;
  env_ = env_->createChild();

  // Initialize TDZ for let/const declarations in this block (non-recursive)
  for (const auto& s : stmt.body) {
    if (auto* varDecl = std::get_if<VarDeclaration>(&s->node)) {
      if (varDecl->kind == VarDeclaration::Kind::Let ||
          varDecl->kind == VarDeclaration::Kind::Const) {
        for (const auto& declarator : varDecl->declarations) {
          std::vector<std::string> names;
          collectVarHoistNames(*declarator.pattern, names);
          for (const auto& name : names) {
            env_->defineTDZ(name);
          }
        }
      }
    }
  }

  Value result = Value(Undefined{});
  for (const auto& s : stmt.body) {
    auto task = evaluate(*s);
    LIGHTJS_RUN_TASK_VOID(task);

    // Per spec UpdateEmpty: update completion value unless the abrupt completion
    // has an "empty" value (represented as Undefined in our implementation).
    // When flow is active, only update result if task returned a non-Undefined value.
    if (flow_.type == ControlFlow::Type::None) {
      result = task.result();
    } else if (!task.result().isUndefined()) {
      // Abrupt completion with a non-empty value (e.g., for-of returning body value)
      result = task.result();
    }

    if (flow_.type != ControlFlow::Type::None) {
      break;
    }
  }

  env_ = prevEnv;
  LIGHTJS_RETURN(result);
}

Task Interpreter::evaluateIf(const IfStmt& stmt) {
  auto testTask = evaluate(*stmt.test);
  LIGHTJS_RUN_TASK_VOID(testTask);

  if (testTask.result().toBool()) {
    auto consTask = evaluate(*stmt.consequent);
    LIGHTJS_RUN_TASK_VOID(consTask);
    LIGHTJS_RETURN(consTask.result());
  } else if (stmt.alternate) {
    auto altTask = evaluate(*stmt.alternate);
    LIGHTJS_RUN_TASK_VOID(altTask);
    LIGHTJS_RETURN(altTask.result());
  }

  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::evaluateWhile(const WhileStmt& stmt) {
  Value result = Value(Undefined{});
  std::string myLabel = pendingIterationLabel_;
  pendingIterationLabel_.clear();

  while (true) {
    auto testTask = evaluate(*stmt.test);
    LIGHTJS_RUN_TASK_VOID(testTask);

    if (!testTask.result().toBool()) {
      break;
    }

    auto bodyTask = evaluate(*stmt.body);
  LIGHTJS_RUN_TASK(bodyTask, result);

    if (flow_.type == ControlFlow::Type::Break) {
      if (flow_.label.empty()) flow_.type = ControlFlow::Type::None;
      break;
    } else if (flow_.type == ControlFlow::Type::Continue) {
      if (flow_.label.empty() || (!myLabel.empty() && flow_.label == myLabel)) {
        flow_.type = ControlFlow::Type::None;
        flow_.label.clear();
        continue;
      }
      break;
    } else if (flow_.type != ControlFlow::Type::None) {
      break;
    }
  }

  LIGHTJS_RETURN(result);
}

Task Interpreter::evaluateWith(const WithStmt& stmt) {
  if (strictMode_) {
    throwError(ErrorType::SyntaxError, "Strict mode code may not include a with statement");
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  Value scopeValue = LIGHTJS_AWAIT(evaluate(*stmt.object));
  if (flow_.type != ControlFlow::Type::None) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  auto prevEnv = env_;
  env_ = env_->createChild();
  if (scopeValue.isObject()) {
    env_->define("__with_scope_object__", scopeValue);
  }
  struct EnvRestoreGuard {
    Interpreter* interpreter;
    std::shared_ptr<Environment> previous;
    ~EnvRestoreGuard() { interpreter->env_ = previous; }
  } restore{this, prevEnv};

  auto isVisibleKey = [](const std::string& key) -> bool {
    if (key.empty()) {
      return false;
    }
    return key.rfind("__", 0) != 0;
  };

  auto defineVisible = [&](const std::string& key, const Value& value) {
    if (isVisibleKey(key)) {
      env_->define(key, value);
    }
  };

  auto bindObjectChain = [&](const std::shared_ptr<Object>& root) {
    std::unordered_set<Object*> visited;
    auto current = root;
    int depth = 0;
    while (current && depth < 32 && visited.insert(current.get()).second) {
      for (const auto& [key, value] : current->properties) {
        defineVisible(key, value);
      }
      auto protoIt = current->properties.find("__proto__");
      if (protoIt == current->properties.end() || !protoIt->second.isObject()) {
        break;
      }
      current = std::get<std::shared_ptr<Object>>(protoIt->second.data);
      depth++;
    }
  };

  auto resolvePromisePrototype = [&](const Value& ctorValue) -> std::shared_ptr<Object> {
    if (!ctorValue.isFunction()) {
      return nullptr;
    }
    auto ctorFn = std::get<std::shared_ptr<Function>>(ctorValue.data);
    auto protoIt = ctorFn->properties.find("prototype");
    if (protoIt == ctorFn->properties.end() || !protoIt->second.isObject()) {
      return nullptr;
    }
    return std::get<std::shared_ptr<Object>>(protoIt->second.data);
  };

  if (scopeValue.isObject()) {
    bindObjectChain(std::get<std::shared_ptr<Object>>(scopeValue.data));
  } else if (scopeValue.isPromise()) {
    auto promisePtr = std::get<std::shared_ptr<Promise>>(scopeValue.data);
    for (const auto& [key, value] : promisePtr->properties) {
      defineVisible(key, value);
    }

    Value ctorValue = Value(Undefined{});
    auto ctorIt = promisePtr->properties.find("__constructor__");
    if (ctorIt != promisePtr->properties.end()) {
      ctorValue = ctorIt->second;
    } else if (auto intrinsicPromise = env_->get("__intrinsic_Promise__")) {
      ctorValue = *intrinsicPromise;
    } else if (auto promiseCtor = env_->get("Promise")) {
      ctorValue = *promiseCtor;
    }
    if (!ctorValue.isUndefined()) {
      env_->define("constructor", ctorValue);
    }

    auto promiseProto = resolvePromisePrototype(ctorValue);
    if (!promiseProto) {
      if (auto intrinsicPromise = env_->get("__intrinsic_Promise__")) {
        promiseProto = resolvePromisePrototype(*intrinsicPromise);
      }
    }
    if (promiseProto) {
      auto thenIt = promiseProto->properties.find("then");
      if (thenIt != promiseProto->properties.end()) {
        env_->define("then", thenIt->second);
      }
      auto catchIt = promiseProto->properties.find("catch");
      if (catchIt != promiseProto->properties.end()) {
        env_->define("catch", catchIt->second);
      }
      auto finallyIt = promiseProto->properties.find("finally");
      if (finallyIt != promiseProto->properties.end()) {
        env_->define("finally", finallyIt->second);
      }
    }
  }

  Value result = LIGHTJS_AWAIT(evaluate(*stmt.body));
  LIGHTJS_RETURN(result);
}

Task Interpreter::evaluateFor(const ForStmt& stmt) {
  auto prevEnv = env_;
  env_ = env_->createChild();
  std::string myLabel = pendingIterationLabel_;
  pendingIterationLabel_.clear();

  if (stmt.init) {
    auto initTask = evaluate(*stmt.init);
    LIGHTJS_RUN_TASK_VOID(initTask);
  }

  Value result = Value(Undefined{});

  while (true) {
    if (stmt.test) {
      auto testTask = evaluate(*stmt.test);
      LIGHTJS_RUN_TASK_VOID(testTask);
      if (!testTask.result().toBool()) {
        break;
      }
    }

    auto bodyTask = evaluate(*stmt.body);
  LIGHTJS_RUN_TASK(bodyTask, result);

    if (flow_.type == ControlFlow::Type::Break) {
      if (flow_.label.empty()) flow_.type = ControlFlow::Type::None;
      break;
    } else if (flow_.type == ControlFlow::Type::Continue) {
      if (flow_.label.empty() || (!myLabel.empty() && flow_.label == myLabel)) {
        flow_.type = ControlFlow::Type::None;
        flow_.label.clear();
      } else {
        break;
      }
    } else if (flow_.type != ControlFlow::Type::None) {
      break;
    }

    if (stmt.update) {
      auto updateTask = evaluate(*stmt.update);
      LIGHTJS_RUN_TASK_VOID(updateTask);
    }
  }

  env_ = prevEnv;
  LIGHTJS_RETURN(result);
}

Task Interpreter::evaluateDoWhile(const DoWhileStmt& stmt) {
  Value result = Value(Undefined{});
  // Consume pending label if this loop is directly labeled
  std::string myLabel = pendingIterationLabel_;
  pendingIterationLabel_.clear();

  do {
    auto bodyTask = evaluate(*stmt.body);
  LIGHTJS_RUN_TASK(bodyTask, result);

    if (flow_.type == ControlFlow::Type::Break) {
      if (flow_.label.empty()) flow_.type = ControlFlow::Type::None;
      break;
    } else if (flow_.type == ControlFlow::Type::Continue) {
      if (flow_.label.empty()) {
        flow_.type = ControlFlow::Type::None;
      } else if (!myLabel.empty() && flow_.label == myLabel) {
        // continue targets this labeled loop - consume and check condition
        flow_.type = ControlFlow::Type::None;
        flow_.label.clear();
      } else {
        break;  // continue targets an outer loop
      }
    } else if (flow_.type != ControlFlow::Type::None) {
      break;
    }

    auto testTask = evaluate(*stmt.test);
    LIGHTJS_RUN_TASK_VOID(testTask);

    if (!testTask.result().toBool()) {
      break;
    }
  } while (true);

  LIGHTJS_RETURN(result);
}

Task Interpreter::evaluateForIn(const ForInStmt& stmt) {
  auto prevEnv = env_;
  env_ = env_->createChild();
  std::string myLabel = pendingIterationLabel_;
  pendingIterationLabel_.clear();

  Value result = Value(Undefined{});

  // Get the variable name from the left side (before evaluating RHS, for TDZ)
  std::string varName;
  bool isVarDecl = false;
  bool isLetOrConst = false;
  bool isConst = false;
  const Expression* memberExpr = nullptr;  // For MemberExpr targets
  const Expression* dstrPattern = nullptr;  // For destructuring patterns
  bool dstrIsDecl = false;  // true if destructuring is from var/let/const declaration

  // Helper to collect bound names from a pattern for TDZ
  std::function<void(const Expression&, std::vector<std::string>&)> collectBoundNames;
  collectBoundNames = [&collectBoundNames](const Expression& expr, std::vector<std::string>& names) {
    if (auto* id = std::get_if<Identifier>(&expr.node)) {
      names.push_back(id->name);
    } else if (auto* arr = std::get_if<ArrayPattern>(&expr.node)) {
      for (const auto& elem : arr->elements) {
        if (elem) collectBoundNames(*elem, names);
      }
      if (arr->rest) collectBoundNames(*arr->rest, names);
    } else if (auto* obj = std::get_if<ObjectPattern>(&expr.node)) {
      for (const auto& prop : obj->properties) {
        if (prop.value) collectBoundNames(*prop.value, names);
      }
      if (obj->rest) collectBoundNames(*obj->rest, names);
    } else if (auto* assign = std::get_if<AssignmentPattern>(&expr.node)) {
      if (assign->left) collectBoundNames(*assign->left, names);
    }
  };

  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.left->node)) {
    isVarDecl = true;
    isLetOrConst = (varDecl->kind == VarDeclaration::Kind::Let || varDecl->kind == VarDeclaration::Kind::Const);
    isConst = (varDecl->kind == VarDeclaration::Kind::Const);
    if (!varDecl->declarations.empty()) {
      if (auto* id = std::get_if<Identifier>(&varDecl->declarations[0].pattern->node)) {
        varName = id->name;
      } else {
        // Destructuring pattern in declaration: for (var [a, b] in ...) or for (let {x} in ...)
        dstrPattern = varDecl->declarations[0].pattern.get();
        dstrIsDecl = true;
      }
      // For var declarations, define in loop scope
      if (!isLetOrConst && !varName.empty()) {
        env_->define(varName, Value(Undefined{}));
      }
    }
  } else if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt.left->node)) {
    if (auto* ident = std::get_if<Identifier>(&exprStmt->expression->node)) {
      varName = ident->name;
    } else if (std::get_if<MemberExpr>(&exprStmt->expression->node)) {
      memberExpr = exprStmt->expression.get();
    } else if (std::get_if<ArrayPattern>(&exprStmt->expression->node) ||
               std::get_if<ObjectPattern>(&exprStmt->expression->node)) {
      // Expression destructuring: for ([a, b] in ...) or for ({x} in ...)
      dstrPattern = exprStmt->expression.get();
      dstrIsDecl = false;
    }
  }

  // Per spec (ForIn/OfHeadEvaluation): create TDZ environment for let/const bound names
  // before evaluating the RHS expression.
  auto envBeforeTDZ = env_;
  if (isLetOrConst) {
    std::vector<std::string> tdzNames;
    if (!varName.empty()) {
      tdzNames.push_back(varName);
    } else if (dstrPattern) {
      collectBoundNames(*dstrPattern, tdzNames);
    }
    if (!tdzNames.empty()) {
      auto tdzEnv = env_->createChild();
      for (const auto& name : tdzNames) {
        tdzEnv->defineTDZ(name);
      }
      env_ = tdzEnv;
    }
  }

  // Evaluate the right-hand side (the object to iterate over)
  auto rightTask = evaluate(*stmt.right);
  Value obj;
  LIGHTJS_RUN_TASK(rightTask, obj);

  // Restore env after RHS evaluation (remove TDZ from execution context)
  env_ = envBeforeTDZ;

  // For-in over null/undefined should not execute the body (spec 13.7.5.11 step 5)
  if (obj.isNull() || obj.isUndefined()) {
    env_ = prevEnv;
    LIGHTJS_RETURN(result);
  }

  auto isInternalProp = [](const std::string& key) -> bool {
    return key.size() >= 4 && key.substr(0, 2) == "__" &&
           key.substr(key.size() - 2) == "__";
  };

  auto isMetaProp = [](const std::string& key) -> bool {
    return key.substr(0, 6) == "__get_" || key.substr(0, 6) == "__set_" ||
           key.substr(0, 11) == "__non_enum_" || key.substr(0, 15) == "__non_writable_" ||
           key.substr(0, 19) == "__non_configurable_" || key.substr(0, 7) == "__enum_";
  };

  // Helper to assign a key to the loop variable
  // For let/const, creates a fresh child environment per iteration
  auto assignKey = [&](const std::string& key) -> void {
    if (dstrPattern) {
      // Destructuring pattern: bind key through destructuring
      if (isLetOrConst) {
        auto iterEnv = env_->createChild();
        env_ = iterEnv;
      }
      bindDestructuringPattern(*dstrPattern, Value(key), isConst, !dstrIsDecl);
      return;
    }
    if (isLetOrConst && !varName.empty()) {
      // Create a fresh scope for each iteration (per-iteration binding)
      auto iterEnv = env_->createChild();
      env_ = iterEnv;
      env_->define(varName, Value(key), isConst);
      return;
    }
    if (memberExpr) {
      if (auto* member = std::get_if<MemberExpr>(&memberExpr->node)) {
        auto objTask = evaluate(*member->object);
        Value objVal;
        LIGHTJS_RUN_TASK_VOID(objTask);
        objVal = objTask.result();
        if (objVal.isObject()) {
          auto mObj = std::get<std::shared_ptr<Object>>(objVal.data);
          std::string prop;
          if (member->computed) {
            auto propTask = evaluate(*member->property);
            LIGHTJS_RUN_TASK_VOID(propTask);
            prop = propTask.result().toString();
          } else if (auto* propId = std::get_if<Identifier>(&member->property->node)) {
            prop = propId->name;
          }
          auto setterIt = mObj->properties.find("__set_" + prop);
          if (setterIt != mObj->properties.end() && setterIt->second.isFunction()) {
            callFunction(setterIt->second, {Value(key)}, objVal);
          } else {
            mObj->properties[prop] = Value(key);
          }
        }
      }
    } else if (!varName.empty()) {
      env_->set(varName, Value(key));
    }
  };

  // Helper to sort keys per spec: integer indices ascending first, then string keys in insertion order
  // Since we use unordered_map (no insertion order), we sort strings alphabetically as approximation
  auto sortKeys = [](std::vector<std::string>& keys) {
    std::stable_sort(keys.begin(), keys.end(), [](const std::string& a, const std::string& b) {
      bool aIsNum = !a.empty() && std::all_of(a.begin(), a.end(), ::isdigit);
      bool bIsNum = !b.empty() && std::all_of(b.begin(), b.end(), ::isdigit);
      if (aIsNum && bIsNum) return std::stoul(a) < std::stoul(b);
      if (aIsNum) return true;  // Numeric keys come first
      if (bIsNum) return false;
      return a < b;  // Alphabetical for string keys
    });
  };

  // Helper to collect enumerable keys from an object (including prototype chain)
  auto collectObjectKeys = [&](const std::shared_ptr<Object>& objPtr) -> std::vector<std::string> {
    std::vector<std::string> keys;
    std::unordered_set<std::string> seen;
    // Walk prototype chain
    auto current = objPtr;
    int depth = 0;
    while (current && depth < 50) {
      // Collect keys at this level, sort them, then add
      std::vector<std::string> levelKeys;
      for (const auto& [key, _] : current->properties) {
        if (isInternalProp(key)) continue;
        if (isMetaProp(key)) continue;
        if (seen.count(key)) continue;
        levelKeys.push_back(key);
      }
      sortKeys(levelKeys);
      for (const auto& key : levelKeys) {
        seen.insert(key);  // Mark as seen BEFORE enum check (shadows proto)
        if (current->properties.count("__non_enum_" + key)) continue;
        keys.push_back(key);
      }
      // Walk up prototype chain
      auto protoIt = current->properties.find("__proto__");
      if (protoIt != current->properties.end() && protoIt->second.isObject()) {
        current = std::get<std::shared_ptr<Object>>(protoIt->second.data);
        depth++;
      } else {
        break;
      }
    }
    return keys;
  };

  // Iterate over object properties (including prototype chain)
  if (auto* objPtr = std::get_if<std::shared_ptr<Object>>(&obj.data)) {
    std::vector<std::string> keys = collectObjectKeys(*objPtr);

    for (const auto& key : keys) {
      // Check if property still exists (may have been deleted during iteration)
      // Only check on the own object for deletions
      bool exists = false;
      auto current = *objPtr;
      int depth = 0;
      while (current && depth < 50) {
        if (current->properties.count(key) && !current->properties.count("__non_enum_" + key)) {
          exists = true;
          break;
        }
        auto protoIt = current->properties.find("__proto__");
        if (protoIt != current->properties.end() && protoIt->second.isObject()) {
          current = std::get<std::shared_ptr<Object>>(protoIt->second.data);
          depth++;
        } else {
          break;
        }
      }
      if (!exists) continue;

      auto loopEnv = env_;  // Save loop environment
      assignKey(key);

      auto bodyTask = evaluate(*stmt.body);
  LIGHTJS_RUN_TASK(bodyTask, result);

      if (isLetOrConst) env_ = loopEnv;  // Restore to loop scope

      if (flow_.type == ControlFlow::Type::Break) {
        if (flow_.label.empty()) flow_.type = ControlFlow::Type::None;
        break;
      } else if (flow_.type == ControlFlow::Type::Continue) {
        if (flow_.label.empty() || (!myLabel.empty() && flow_.label == myLabel)) {
          flow_.type = ControlFlow::Type::None;
          flow_.label.clear();
        } else {
          break;
        }
      } else if (flow_.type != ControlFlow::Type::None) {
        break;
      }
    }
  }

  // Iterate over array indices and properties
  else if (auto* arrPtr = std::get_if<std::shared_ptr<Array>>(&obj.data)) {
    std::vector<std::string> keys;
    // Add numeric indices first
    for (size_t i = 0; i < (*arrPtr)->elements.size(); ++i) {
      keys.push_back(std::to_string(i));
    }
    // Add named properties
    for (const auto& [key, _] : (*arrPtr)->properties) {
      if (isInternalProp(key)) continue;
      if (isMetaProp(key)) continue;
      if ((*arrPtr)->properties.count("__non_enum_" + key)) continue;
      keys.push_back(key);
    }

    for (const auto& key : keys) {
      auto loopEnv = env_;
      assignKey(key);
      auto bodyTask = evaluate(*stmt.body);
      LIGHTJS_RUN_TASK(bodyTask, result);
      if (isLetOrConst) env_ = loopEnv;
      if (flow_.type == ControlFlow::Type::Break) {
        if (flow_.label.empty()) flow_.type = ControlFlow::Type::None;
        break;
      } else if (flow_.type == ControlFlow::Type::Continue) {
        if (flow_.label.empty() || (!myLabel.empty() && flow_.label == myLabel)) {
          flow_.type = ControlFlow::Type::None;
          flow_.label.clear();
        } else {
          break;
        }
      } else if (flow_.type != ControlFlow::Type::None) {
        break;
      }
    }
  }

  // Iterate over function properties
  else if (auto* fnPtr = std::get_if<std::shared_ptr<Function>>(&obj.data)) {
    std::vector<std::string> keys;
    for (const auto& [key, _] : (*fnPtr)->properties) {
      if (isInternalProp(key)) continue;
      if (isMetaProp(key)) continue;
      // name, length, prototype are non-enumerable on functions
      if (key == "name" || key == "length" || key == "prototype") continue;
      // Built-in function properties are non-enumerable unless explicitly marked
      if (!(*fnPtr)->properties.count("__enum_" + key)) continue;
      keys.push_back(key);
    }

    for (const auto& key : keys) {
      auto loopEnv = env_;
      assignKey(key);
      auto bodyTask = evaluate(*stmt.body);
      LIGHTJS_RUN_TASK(bodyTask, result);
      if (isLetOrConst) env_ = loopEnv;
      if (flow_.type == ControlFlow::Type::Break) {
        if (flow_.label.empty()) flow_.type = ControlFlow::Type::None;
        break;
      } else if (flow_.type == ControlFlow::Type::Continue) {
        if (flow_.label.empty()) flow_.type = ControlFlow::Type::None;
        else break;
      } else if (flow_.type != ControlFlow::Type::None) {
        break;
      }
    }
  }

  env_ = prevEnv;
  LIGHTJS_RETURN(result);
}

Task Interpreter::evaluateForOf(const ForOfStmt& stmt) {
  auto prevEnv = env_;
  env_ = env_->createChild();

  Value result = Value(Undefined{});
  std::string myLabel = pendingIterationLabel_;
  pendingIterationLabel_.clear();

  // Determine the LHS binding type (before evaluating RHS, for TDZ)
  enum class ForOfLHS { SimpleVar, DestructuringVar, ExpressionTarget };
  ForOfLHS lhsType = ForOfLHS::SimpleVar;
  std::string varName;
  bool isConst = false;
  bool isLetOrConst = false;
  bool isDeclaration = false;  // true for var/let/const declarations, false for expression targets
  const Expression* lhsPattern = nullptr;
  const Expression* lhsExpr = nullptr;

  // Helper to collect bound names from a pattern for TDZ
  std::function<void(const Expression&, std::vector<std::string>&)> collectBoundNames;
  collectBoundNames = [&collectBoundNames](const Expression& expr, std::vector<std::string>& names) {
    if (auto* id = std::get_if<Identifier>(&expr.node)) {
      names.push_back(id->name);
    } else if (auto* arr = std::get_if<ArrayPattern>(&expr.node)) {
      for (const auto& elem : arr->elements) {
        if (elem) collectBoundNames(*elem, names);
      }
      if (arr->rest) collectBoundNames(*arr->rest, names);
    } else if (auto* obj = std::get_if<ObjectPattern>(&expr.node)) {
      for (const auto& prop : obj->properties) {
        if (prop.value) collectBoundNames(*prop.value, names);
      }
      if (obj->rest) collectBoundNames(*obj->rest, names);
    } else if (auto* assign = std::get_if<AssignmentPattern>(&expr.node)) {
      if (assign->left) collectBoundNames(*assign->left, names);
    }
  };

  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.left->node)) {
    isDeclaration = true;
    isConst = (varDecl->kind == VarDeclaration::Kind::Const);
    isLetOrConst = (varDecl->kind == VarDeclaration::Kind::Let ||
                    varDecl->kind == VarDeclaration::Kind::Const);
    if (!varDecl->declarations.empty()) {
      const auto& pattern = varDecl->declarations[0].pattern;
      if (auto* id = std::get_if<Identifier>(&pattern->node)) {
        varName = id->name;
        lhsType = ForOfLHS::SimpleVar;
        // For var declarations, define in the outer scope
        if (varDecl->kind == VarDeclaration::Kind::Var) {
          env_->define(varName, Value(Undefined{}));
        }
      } else {
        lhsType = ForOfLHS::DestructuringVar;
        lhsPattern = pattern.get();
      }
    }
  } else if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt.left->node)) {
    if (auto* ident = std::get_if<Identifier>(&exprStmt->expression->node)) {
      varName = ident->name;
      lhsType = ForOfLHS::SimpleVar;
    } else {
      lhsType = ForOfLHS::ExpressionTarget;
      lhsExpr = exprStmt->expression.get();
    }
  }

  // Per spec (ForIn/OfHeadEvaluation): create TDZ environment for let/const bound names
  // before evaluating the RHS expression. Closures created during RHS evaluation will
  // capture this TDZ env, so accessing the variable will always throw ReferenceError.
  auto envBeforeTDZ = env_;
  if (isLetOrConst) {
    std::vector<std::string> tdzNames;
    if (!varName.empty()) {
      tdzNames.push_back(varName);
    } else if (lhsPattern) {
      collectBoundNames(*lhsPattern, tdzNames);
    }
    if (!tdzNames.empty()) {
      auto tdzEnv = env_->createChild();
      for (const auto& name : tdzNames) {
        tdzEnv->defineTDZ(name);
      }
      env_ = tdzEnv;
    }
  }

  // Evaluate the right-hand side (the iterable to iterate over)
  auto rightTask = evaluate(*stmt.right);
  Value iterable;
  LIGHTJS_RUN_TASK(rightTask, iterable);
  if (hasError()) {
    env_ = prevEnv;
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  // Restore env after RHS evaluation (spec step 4: set LexicalEnvironment to oldEnv)
  // The TDZ child env persists for any closures that captured it.
  env_ = envBeforeTDZ;

  std::optional<IteratorRecord> iteratorOpt;
  if (stmt.isAwait && iterable.isObject()) {
    const auto& asyncIteratorKey = WellKnownSymbols::asyncIteratorKey();
    auto obj = std::get<std::shared_ptr<Object>>(iterable.data);
    auto asyncIt = obj->properties.find(asyncIteratorKey);
    if (asyncIt != obj->properties.end() && asyncIt->second.isFunction()) {
      Value asyncIterValue = callFunction(asyncIt->second, {}, iterable);
      if (asyncIterValue.isObject()) {
        IteratorRecord record;
        record.kind = IteratorRecord::Kind::IteratorObject;
        record.iteratorObject = std::get<std::shared_ptr<Object>>(asyncIterValue.data);
        iteratorOpt = std::move(record);
      }
    }
  }

  if (!iteratorOpt.has_value()) {
    iteratorOpt = getIterator(iterable);
  }
  if (!iteratorOpt.has_value()) {
    env_ = prevEnv;
    throwError(ErrorType::TypeError, "Value is not iterable");
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  auto iterator = std::move(*iteratorOpt);
  while (true) {
    Value stepResult = iteratorNext(iterator);
    // Per spec: if IteratorNext (calling next()) throws, propagate error WITHOUT calling iteratorClose
    if (flow_.type == ControlFlow::Type::Throw) {
      env_ = prevEnv;
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (stmt.isAwait && stepResult.isPromise()) {
      auto promise = std::get<std::shared_ptr<Promise>>(stepResult.data);
      if (promise->state == PromiseState::Rejected) {
        env_ = prevEnv;
        flow_.type = ControlFlow::Type::Throw;
        flow_.value = promise->result;
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (promise->state == PromiseState::Fulfilled) {
        stepResult = promise->result;
      } else {
        break;
      }
    }
    // Per spec (7.4.2 IteratorNext), result must be an Object type
    // In JS, Object includes plain objects, arrays, functions, regex, etc.
    // Helper to extract properties from any object-like Value (supports getters)
    auto getProperty = [this](const Value& val, const std::string& key) -> std::optional<Value> {
      if (val.isProxy()) {
        // Use Proxy get trap
        auto proxyPtr = std::get<std::shared_ptr<Proxy>>(val.data);
        if (proxyPtr->handler && proxyPtr->handler->isObject()) {
          auto handlerObj = std::get<std::shared_ptr<Object>>(proxyPtr->handler->data);
          auto trapIt = handlerObj->properties.find("get");
          if (trapIt != handlerObj->properties.end() && trapIt->second.isFunction()) {
            return callFunction(trapIt->second, {proxyPtr->target ? *proxyPtr->target : Value(Undefined{}), Value(key), val}, Value(Undefined{}));
          }
        }
        // Fall through to target if no get trap
        if (proxyPtr->target && proxyPtr->target->isObject()) {
          auto obj = std::get<std::shared_ptr<Object>>(proxyPtr->target->data);
          auto it = obj->properties.find(key);
          if (it != obj->properties.end()) return it->second;
        }
        return std::nullopt;
      }
      if (val.isObject()) {
        auto obj = std::get<std::shared_ptr<Object>>(val.data);
        // Check for getter first
        auto getterIt = obj->properties.find("__get_" + key);
        if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
          return callFunction(getterIt->second, {}, val);
        }
        auto it = obj->properties.find(key);
        if (it != obj->properties.end()) return it->second;
      } else if (val.isArray()) {
        auto arr = std::get<std::shared_ptr<Array>>(val.data);
        auto it = arr->properties.find(key);
        if (it != arr->properties.end()) return it->second;
      } else if (val.isFunction()) {
        auto fn = std::get<std::shared_ptr<Function>>(val.data);
        auto it = fn->properties.find(key);
        if (it != fn->properties.end()) return it->second;
      } else if (val.isRegex()) {
        auto rx = std::get<std::shared_ptr<Regex>>(val.data);
        auto it = rx->properties.find(key);
        if (it != rx->properties.end()) return it->second;
      }
      return std::nullopt;
    };

    if (!isObjectLike(stepResult)) {
      if (iterator.kind == IteratorRecord::Kind::IteratorObject) {
        iteratorClose(iterator);
        throwError(ErrorType::TypeError, "Iterator result " + stepResult.toString() + " is not an object");
        env_ = prevEnv;
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      break;
    }

    bool isDone = false;
    if (auto doneOpt = getProperty(stepResult, "done")) {
      isDone = doneOpt->toBool();
    }
    // Per spec: if IteratorStep throws (e.g., getter on 'done'), propagate without closing
    if (flow_.type == ControlFlow::Type::Throw) {
      env_ = prevEnv;
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (isDone) {
      break;
    }

    Value currentValue = Value(Undefined{});
    if (auto valueOpt = getProperty(stepResult, "value")) {
      currentValue = *valueOpt;
    }
    // Per spec: if IteratorValue throws (e.g., getter on 'value'), propagate without closing
    if (flow_.type == ControlFlow::Type::Throw) {
      env_ = prevEnv;
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    if (stmt.isAwait && currentValue.isPromise()) {
      auto valuePromise = std::get<std::shared_ptr<Promise>>(currentValue.data);
      if (valuePromise->state == PromiseState::Rejected) {
        env_ = prevEnv;
        flow_.type = ControlFlow::Type::Throw;
        flow_.value = valuePromise->result;
        LIGHTJS_RETURN(Value(Undefined{}));
      }
      if (valuePromise->state == PromiseState::Fulfilled) {
        currentValue = valuePromise->result;
      } else {
        break;
      }
    }

    // Create per-iteration scope for let/const declarations
    auto iterEnv = env_->createChild();
    auto outerEnv = env_;
    env_ = iterEnv;

    // Assign value to LHS
    if (lhsType == ForOfLHS::SimpleVar) {
      if (isConst) {
        // let/const declaration: define in per-iteration scope
        env_->define(varName, currentValue, true);
      } else if (!varName.empty() && env_->isConst(varName)) {
        // Expression target assigning to const variable
        throwError(ErrorType::TypeError, "Assignment to constant variable '" + varName + "'");
        env_ = prevEnv;
        LIGHTJS_RETURN(Value(Undefined{}));
      } else if (isDeclaration) {
        // var/let declaration: define in scope
        env_->define(varName, currentValue);
      } else {
        // Expression target: update existing variable in parent scope
        if (!env_->set(varName, currentValue)) {
          env_->define(varName, currentValue);
        }
      }
    } else if (lhsType == ForOfLHS::DestructuringVar) {
      bindDestructuringPattern(*lhsPattern, currentValue, isConst);
      if (flow_.type == ControlFlow::Type::Throw) {
        // Per spec IteratorClose step 5: original throw completion always wins
        auto savedFlow = flow_;
        flow_.type = ControlFlow::Type::None;
        iteratorClose(iterator);
        flow_ = savedFlow;  // Always restore original throw (spec step 5)
        env_ = prevEnv;
        LIGHTJS_RETURN(Value(Undefined{}));
      }
    } else if (lhsType == ForOfLHS::ExpressionTarget && lhsExpr) {
      // Bare destructuring assignment or member expression target
      if (std::get_if<ArrayPattern>(&lhsExpr->node) || std::get_if<ObjectPattern>(&lhsExpr->node)) {
        // Destructuring assignment (for ({a, b} of ...) or for ([a, b] of ...))
        bindDestructuringPattern(*lhsExpr, currentValue, false, true);
        if (flow_.type == ControlFlow::Type::Throw) {
          // Per spec IteratorClose step 5: original throw completion always wins
          auto savedFlow = flow_;
          flow_.type = ControlFlow::Type::None;
          iteratorClose(iterator);
          flow_ = savedFlow;  // Always restore original throw (spec step 5)
          env_ = prevEnv;
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      } else if (auto* member = std::get_if<MemberExpr>(& lhsExpr->node)) {
        auto objTask = evaluate(*member->object);
        Value objVal;
        LIGHTJS_RUN_TASK(objTask, objVal);
        if (objVal.isObject()) {
          auto obj = std::get<std::shared_ptr<Object>>(objVal.data);
          std::string prop;
          if (member->computed) {
            auto propTask = evaluate(*member->property);
            Value propVal;
            LIGHTJS_RUN_TASK(propTask, propVal);
            prop = propVal.toString();
          } else if (auto* propId = std::get_if<Identifier>(&member->property->node)) {
            prop = propId->name;
          }
          // Check for setter
          auto setterIt = obj->properties.find("__set_" + prop);
          if (setterIt != obj->properties.end() && setterIt->second.isFunction()) {
            callFunction(setterIt->second, {currentValue}, objVal);
          } else {
            obj->properties[prop] = currentValue;
          }
        }
        // If assignment (via setter) threw, close iterator and propagate
        if (flow_.type == ControlFlow::Type::Throw) {
          auto savedFlow = flow_;
          flow_.type = ControlFlow::Type::None;
          iteratorClose(iterator);
          flow_ = savedFlow;  // Original throw wins (spec step 5)
          env_ = prevEnv;
          LIGHTJS_RETURN(Value(Undefined{}));
        }
      }
    }

    auto bodyTask = evaluate(*stmt.body);
    LIGHTJS_RUN_TASK(bodyTask, result);

    // Restore to outer loop env
    env_ = outerEnv;

    if (flow_.type == ControlFlow::Type::Break) {
      if (flow_.label.empty()) flow_.type = ControlFlow::Type::None;
      iteratorClose(iterator);
      break;
    } else if (flow_.type == ControlFlow::Type::Continue) {
      if (flow_.label.empty() || (!myLabel.empty() && flow_.label == myLabel)) {
        flow_.type = ControlFlow::Type::None;
        flow_.label.clear();
        // continue this loop
      } else {
        iteratorClose(iterator);
        break;
      }
    } else if (flow_.type == ControlFlow::Type::Return) {
      iteratorClose(iterator);
      break;
    } else if (flow_.type == ControlFlow::Type::Throw) {
      // For throw completion, still try to close iterator but preserve the throw
      auto savedFlow = flow_;
      flow_.type = ControlFlow::Type::None;
      iteratorClose(iterator);
      // If iteratorClose didn't throw, restore original throw
      if (flow_.type == ControlFlow::Type::None) {
        flow_ = savedFlow;
      }
      break;
    } else if (flow_.type != ControlFlow::Type::None) {
      iteratorClose(iterator);
      break;
    }
  }

  env_ = prevEnv;
  LIGHTJS_RETURN(result);
}

Task Interpreter::evaluateSwitch(const SwitchStmt& stmt) {
  // Evaluate the discriminant
  auto discriminantTask = evaluate(*stmt.discriminant);
  Value discriminant;
  LIGHTJS_RUN_TASK(discriminantTask, discriminant);

  Value result = Value(Undefined{});
  bool foundMatch = false;
  bool hasDefault = false;
  size_t defaultIndex = 0;

  // Find default case if any
  for (size_t i = 0; i < stmt.cases.size(); i++) {
    if (!stmt.cases[i].test) {
      hasDefault = true;
      defaultIndex = i;
      break;
    }
  }

  // First pass: find matching case
  for (size_t i = 0; i < stmt.cases.size(); i++) {
    const auto& caseClause = stmt.cases[i];

    if (caseClause.test) {
      auto testTask = evaluate(*caseClause.test);
  Value testValue;
  LIGHTJS_RUN_TASK(testTask, testValue);

      // Perform strict equality check
      bool isEqual = false;
      if (discriminant.isBigInt() && testValue.isBigInt()) {
        isEqual = (discriminant.toBigInt() == testValue.toBigInt());
      } else if (discriminant.isNumber() && testValue.isNumber()) {
        isEqual = (discriminant.toNumber() == testValue.toNumber());
      } else if (discriminant.isString() && testValue.isString()) {
        isEqual = (discriminant.toString() == testValue.toString());
      } else if (discriminant.isBool() && testValue.isBool()) {
        isEqual = (discriminant.toBool() == testValue.toBool());
      } else if (discriminant.isNull() && testValue.isNull()) {
        isEqual = true;
      } else if (discriminant.isUndefined() && testValue.isUndefined()) {
        isEqual = true;
      }

      if (isEqual) {
        foundMatch = true;
      }
    }

    // Execute if we found a match or if we're in fall-through mode
    if (foundMatch) {
      for (const auto& consequentStmt : caseClause.consequent) {
        auto stmtTask = evaluate(*consequentStmt);
  LIGHTJS_RUN_TASK(stmtTask, result);

        if (flow_.type == ControlFlow::Type::Break) {
          if (flow_.label.empty()) flow_.type = ControlFlow::Type::None;
          LIGHTJS_RETURN(result);
        } else if (flow_.type != ControlFlow::Type::None) {
          LIGHTJS_RETURN(result);
        }
      }
    }
  }

  // If no match found, execute default case
  if (!foundMatch && hasDefault) {
    const auto& defaultCase = stmt.cases[defaultIndex];
    for (const auto& consequentStmt : defaultCase.consequent) {
      auto stmtTask = evaluate(*consequentStmt);
  LIGHTJS_RUN_TASK(stmtTask, result);

      if (flow_.type == ControlFlow::Type::Break) {
        if (flow_.label.empty()) flow_.type = ControlFlow::Type::None;
        LIGHTJS_RETURN(result);
      } else if (flow_.type != ControlFlow::Type::None) {
        LIGHTJS_RETURN(result);
      }
    }
  }

  LIGHTJS_RETURN(result);
}

Task Interpreter::evaluateTry(const TryStmt& stmt) {
  auto prevFlow = flow_;
  Value result = Value(Undefined{});

  for (const auto& s : stmt.block) {
    auto task = evaluate(*s);
  LIGHTJS_RUN_TASK(task, result);

    if (flow_.type == ControlFlow::Type::Throw && stmt.hasHandler) {
      auto catchEnv = env_->createChild();
      auto prevEnv = env_;
      env_ = catchEnv;

      if (stmt.handler.paramPattern) {
        bindDestructuringPattern(*stmt.handler.paramPattern, flow_.value, false);
      } else if (!stmt.handler.param.name.empty()) {
        env_->define(stmt.handler.param.name, flow_.value);
      }

      flow_.type = ControlFlow::Type::None;

      for (const auto& catchStmt : stmt.handler.body) {
        auto catchTask = evaluate(*catchStmt);
  LIGHTJS_RUN_TASK(catchTask, result);
        if (flow_.type != ControlFlow::Type::None) {
          break;
        }
      }

      env_ = prevEnv;
      break;
    }

    if (flow_.type != ControlFlow::Type::None) {
      break;
    }
  }

  if (stmt.hasFinalizer) {
    // Save current control flow state (from try/catch)
    auto savedFlow = flow_;
    flow_.type = ControlFlow::Type::None;
    flow_.label.clear();

    for (const auto& finalStmt : stmt.finalizer) {
      auto finalTask = evaluate(*finalStmt);
      Value finalResult;
      LIGHTJS_RUN_TASK(finalTask, finalResult);
      if (flow_.type != ControlFlow::Type::None) {
        break;
      }
    }

    // If finally block produced its own control flow, it overrides try/catch's
    // If finally block completed normally, restore try/catch's control flow
    if (flow_.type == ControlFlow::Type::None) {
      flow_ = savedFlow;
    }
  }

  LIGHTJS_RETURN(result);
}

Task Interpreter::evaluateImport(const ImportDeclaration& stmt) {
  auto importFnValue = env_->get("import");
  if (!importFnValue || !importFnValue->isFunction()) {
    throwError(ErrorType::ReferenceError, "import is not defined");
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  Value importResult = callFunction(*importFnValue, {Value(stmt.source)}, Value(Undefined{}));
  if (hasError()) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }
  if (!importResult.isPromise()) {
    throwError(ErrorType::TypeError, "import() did not return a Promise");
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  auto promise = std::get<std::shared_ptr<Promise>>(importResult.data);
  if (promise->state == PromiseState::Rejected) {
    flow_.type = ControlFlow::Type::Throw;
    flow_.value = promise->result;
    LIGHTJS_RETURN(Value(Undefined{}));
  }
  if (promise->state != PromiseState::Fulfilled || !promise->result.isObject()) {
    throwError(ErrorType::Error, "Failed to resolve import '" + stmt.source + "'");
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  Value namespaceValue = promise->result;
  auto namespaceObj = std::get<std::shared_ptr<Object>>(namespaceValue.data);

  auto hasExport = [&](const std::string& name) -> bool {
    if (namespaceObj->isModuleNamespace) {
      return std::find(namespaceObj->moduleExportNames.begin(),
                       namespaceObj->moduleExportNames.end(),
                       name) != namespaceObj->moduleExportNames.end();
    }
    return namespaceObj->properties.find(name) != namespaceObj->properties.end();
  };

  auto readExport = [&](const std::string& name) -> Value {
    if (namespaceObj->isModuleNamespace) {
      auto getterIt = namespaceObj->properties.find("__get_" + name);
      if (getterIt != namespaceObj->properties.end() && getterIt->second.isFunction()) {
        Value v = callFunction(getterIt->second, {}, namespaceValue);
        if (hasError()) {
          return Value(Undefined{});
        }
        return v;
      }
    }
    auto it = namespaceObj->properties.find(name);
    if (it != namespaceObj->properties.end()) {
      return it->second;
    }
    return Value(Undefined{});
  };

  if (stmt.defaultImport) {
    if (!hasExport("default")) {
      throwError(ErrorType::SyntaxError, "Module '" + stmt.source + "' does not export 'default'");
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    env_->define(stmt.defaultImport->name, readExport("default"));
  }

  if (stmt.namespaceImport) {
    env_->define(stmt.namespaceImport->name, namespaceValue);
  }

  for (const auto& spec : stmt.specifiers) {
    const std::string& importedName = spec.imported.name;
    if (!hasExport(importedName)) {
      throwError(ErrorType::SyntaxError, "Module '" + stmt.source + "' does not export '" + importedName + "'");
      LIGHTJS_RETURN(Value(Undefined{}));
    }
    env_->define(spec.local.name, readExport(importedName));
  }

  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::evaluateExportNamed(const ExportNamedDeclaration& stmt) {
  // If there's a declaration, evaluate it
  if (stmt.declaration) {
    LIGHTJS_RETURN(LIGHTJS_AWAIT(evaluate(*stmt.declaration)));
  }

  // Export bindings are handled at the module level
  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::evaluateExportDefault(const ExportDefaultDeclaration& stmt) {
  // Evaluate the expression being exported
  auto task = evaluate(*stmt.declaration);
  LIGHTJS_RUN_TASK_VOID(task);

  // The module system will capture this value
  LIGHTJS_RETURN(task.result());
}

Task Interpreter::evaluateExportAll(const ExportAllDeclaration& stmt) {
  // Re-exports are handled at the module level
  LIGHTJS_RETURN(Value(Undefined{}));
}

}
