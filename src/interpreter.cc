#include "interpreter.h"
#include "array_methods.h"
#include "string_methods.h"
#include "unicode.h"
#include "gc.h"
#include "symbols.h"
#include <iostream>
#include <cmath>
#include <climits>
#include <sstream>
#include <iomanip>
#include <unordered_set>

namespace lightjs {

Interpreter::Interpreter(std::shared_ptr<Environment> env) : env_(env) {}

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
  Value result = Value(Undefined{});
  for (const auto& stmt : program.body) {
    auto task = evaluate(*stmt);
  LIGHTJS_RUN_TASK(task, result);

    if (flow_.type != ControlFlow::Type::None) {
      break;
    }
  }
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
      }
    }

    // Process methods
    for (const auto& method : node->methods) {
      auto func = std::make_shared<Function>();
      func->isNative = false;
      func->isAsync = method.isAsync;
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
  } else if (std::holds_alternative<BreakStmt>(stmt.node)) {
    flow_.type = ControlFlow::Type::Break;
    LIGHTJS_RETURN(Value(Undefined{}));
  } else if (std::holds_alternative<ContinueStmt>(stmt.node)) {
    flow_.type = ControlFlow::Type::Continue;
    LIGHTJS_RETURN(Value(Undefined{}));
  } else if (auto* node = std::get_if<ThrowStmt>(&stmt.node)) {
    auto task = evaluate(*node->argument);
    LIGHTJS_RUN_TASK_VOID(task);
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
    if (auto val = env_->get(node->name)) {
      LIGHTJS_RETURN(*val);
    }
    // Throw ReferenceError for undefined variables with line info
    throwError(ErrorType::ReferenceError, formatError("'" + node->name + "' is not defined", expr.loc));
    LIGHTJS_RETURN(Value(Undefined{}));
  } else if (auto* node = std::get_if<NumberLiteral>(&expr.node)) {
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
        result += exprTask.result().toString();
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
  }
  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::evaluateBinary(const BinaryExpr& expr) {
  auto leftTask = evaluate(*expr.left);
  Value left;
  LIGHTJS_RUN_TASK(leftTask, left);

  auto rightTask = evaluate(*expr.right);
  Value right;
  LIGHTJS_RUN_TASK(rightTask, right);

  switch (expr.op) {
    case BinaryExpr::Op::Add:
      if (left.isString() || right.isString()) {
        LIGHTJS_RETURN(Value(left.toString() + right.toString()));
      }
      if (left.isBigInt() && right.isBigInt()) {
        LIGHTJS_RETURN(Value(BigInt(left.toBigInt() + right.toBigInt())));
      }
      LIGHTJS_RETURN(Value(left.toNumber() + right.toNumber()));
    case BinaryExpr::Op::Sub:
      if (left.isBigInt() && right.isBigInt()) {
        LIGHTJS_RETURN(Value(BigInt(left.toBigInt() - right.toBigInt())));
      }
      LIGHTJS_RETURN(Value(left.toNumber() - right.toNumber()));
    case BinaryExpr::Op::Mul:
      if (left.isBigInt() && right.isBigInt()) {
        LIGHTJS_RETURN(Value(BigInt(left.toBigInt() * right.toBigInt())));
      }
      LIGHTJS_RETURN(Value(left.toNumber() * right.toNumber()));
    case BinaryExpr::Op::Div:
      if (left.isBigInt() && right.isBigInt()) {
        LIGHTJS_RETURN(Value(BigInt(left.toBigInt() / right.toBigInt())));
      }
      LIGHTJS_RETURN(Value(left.toNumber() / right.toNumber()));
    case BinaryExpr::Op::Mod:
      if (left.isBigInt() && right.isBigInt()) {
        LIGHTJS_RETURN(Value(BigInt(left.toBigInt() % right.toBigInt())));
      }
      LIGHTJS_RETURN(Value(std::fmod(left.toNumber(), right.toNumber())));
    case BinaryExpr::Op::Exp:
      if (left.isBigInt() && right.isBigInt()) {
        // BigInt exponentiation
        int64_t base = left.toBigInt();
        int64_t exp = right.toBigInt();
        if (exp < 0) {
          LIGHTJS_RETURN(Value(0.0));  // Negative exponents for BigInt return 0
        }
        int64_t result = 1;
        for (int64_t i = 0; i < exp; ++i) {
          result *= base;
        }
        LIGHTJS_RETURN(Value(BigInt(result)));
      }
      LIGHTJS_RETURN(Value(std::pow(left.toNumber(), right.toNumber())));
    case BinaryExpr::Op::Less:
      if (left.isBigInt() && right.isBigInt()) {
        LIGHTJS_RETURN(Value(left.toBigInt() < right.toBigInt()));
      }
      LIGHTJS_RETURN(Value(left.toNumber() < right.toNumber()));
    case BinaryExpr::Op::Greater:
      if (left.isBigInt() && right.isBigInt()) {
        LIGHTJS_RETURN(Value(left.toBigInt() > right.toBigInt()));
      }
      LIGHTJS_RETURN(Value(left.toNumber() > right.toNumber()));
    case BinaryExpr::Op::LessEqual:
      if (left.isBigInt() && right.isBigInt()) {
        LIGHTJS_RETURN(Value(left.toBigInt() <= right.toBigInt()));
      }
      LIGHTJS_RETURN(Value(left.toNumber() <= right.toNumber()));
    case BinaryExpr::Op::GreaterEqual:
      if (left.isBigInt() && right.isBigInt()) {
        LIGHTJS_RETURN(Value(left.toBigInt() >= right.toBigInt()));
      }
      LIGHTJS_RETURN(Value(left.toNumber() >= right.toNumber()));
    case BinaryExpr::Op::Equal:
      if (left.isBigInt() && right.isBigInt()) {
        LIGHTJS_RETURN(Value(left.toBigInt() == right.toBigInt()));
      }
      LIGHTJS_RETURN(Value(left.toNumber() == right.toNumber()));
    case BinaryExpr::Op::NotEqual:
      if (left.isBigInt() && right.isBigInt()) {
        LIGHTJS_RETURN(Value(left.toBigInt() != right.toBigInt()));
      }
      LIGHTJS_RETURN(Value(left.toNumber() != right.toNumber()));
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

      // For objects, arrays, functions - compare by reference
      // We already checked the types are the same
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

      // For objects, arrays, functions - compare by reference
      // We already checked the types are the same
      LIGHTJS_RETURN(Value(true));
    }
    case BinaryExpr::Op::LogicalAnd:
      LIGHTJS_RETURN(left.toBool() ? right : left);
    case BinaryExpr::Op::LogicalOr:
      LIGHTJS_RETURN(left.toBool() ? left : right);
    case BinaryExpr::Op::NullishCoalescing:
      // Return right if left is null or undefined, otherwise return left
      LIGHTJS_RETURN((left.isNull() || left.isUndefined()) ? right : left);
  }

  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::evaluateUnary(const UnaryExpr& expr) {
  auto argTask = evaluate(*expr.argument);
  Value arg;
  LIGHTJS_RUN_TASK(argTask, arg);

  switch (expr.op) {
    case UnaryExpr::Op::Not:
      LIGHTJS_RETURN(Value(!arg.toBool()));
    case UnaryExpr::Op::Minus:
      if (arg.isBigInt()) {
        LIGHTJS_RETURN(Value(BigInt(-arg.toBigInt())));
      }
      LIGHTJS_RETURN(Value(-arg.toNumber()));
    case UnaryExpr::Op::Plus:
      LIGHTJS_RETURN(Value(arg.toNumber()));
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
          env_->set(id->name, right);
          LIGHTJS_RETURN(right);
        } else {
          LIGHTJS_RETURN(*current);
        }
      }
    }
  }

  auto rightTask = evaluate(*expr.right);
  Value right;
  LIGHTJS_RUN_TASK(rightTask, right);

  if (auto* id = std::get_if<Identifier>(&expr.left->node)) {
    if (expr.op == AssignmentExpr::Op::Assign) {
      env_->set(id->name, right);
      LIGHTJS_RETURN(right);
    }

    if (auto current = env_->get(id->name)) {
      Value result;
      switch (expr.op) {
        case AssignmentExpr::Op::AddAssign:
          result = Value(current->toNumber() + right.toNumber());
          break;
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
      propName = propTask.result().toString();
    } else {
      if (auto* id = std::get_if<Identifier>(&member->property->node)) {
        propName = id->name;
      }
    }

    if (obj.isObject()) {
      auto objPtr = std::get<std::shared_ptr<Object>>(obj.data);

      // Check if object is frozen (can't modify any properties)
      if (objPtr->frozen) {
        // In strict mode this would throw, but we'll silently fail
        LIGHTJS_RETURN(right);
      }

      // Check if object is sealed (can't add new properties)
      bool isNewProperty = objPtr->properties.find(propName) == objPtr->properties.end();
      if (objPtr->sealed && isNewProperty) {
        // Can't add new properties to sealed object
        LIGHTJS_RETURN(right);
      }

      if (expr.op == AssignmentExpr::Op::Assign) {
        objPtr->properties[propName] = right;
      } else {
        Value current = objPtr->properties[propName];
        switch (expr.op) {
          case AssignmentExpr::Op::AddAssign:
            objPtr->properties[propName] = Value(current.toNumber() + right.toNumber());
            break;
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

    if (obj.isArray()) {
      auto arrPtr = std::get<std::shared_ptr<Array>>(obj.data);
      try {
        size_t idx = std::stoul(propName);
        if (idx >= arrPtr->elements.size()) {
          arrPtr->elements.resize(idx + 1, Value(Undefined{}));
        }
        arrPtr->elements[idx] = right;
        LIGHTJS_RETURN(right);
      } catch (...) {}
    }

    if (obj.isTypedArray()) {
      auto taPtr = std::get<std::shared_ptr<TypedArray>>(obj.data);
      try {
        size_t idx = std::stoul(propName);
        if (taPtr->type == TypedArrayType::BigInt64 || taPtr->type == TypedArrayType::BigUint64) {
          taPtr->setBigIntElement(idx, right.toBigInt());
        } else {
          taPtr->setElement(idx, right.toNumber());
        }
        LIGHTJS_RETURN(right);
      } catch (...) {}
    }
  }

  LIGHTJS_RETURN(right);
}

Task Interpreter::evaluateUpdate(const UpdateExpr& expr) {
  if (auto* id = std::get_if<Identifier>(&expr.argument->node)) {
    if (auto current = env_->get(id->name)) {
      double num = current->toNumber();
      double newVal = (expr.op == UpdateExpr::Op::Increment) ? num + 1 : num - 1;
      env_->set(id->name, Value(newVal));
      LIGHTJS_RETURN(expr.prefix ? Value(newVal) : Value(num));
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
  if (auto* memberExpr = std::get_if<MemberExpr>(&expr.callee->node)) {
    // Evaluate the object (receiver)
    thisValue = LIGHTJS_AWAIT(evaluate(*memberExpr->object));

    // Now evaluate the full member expression to get the method
    callee = LIGHTJS_AWAIT(evaluate(*expr.callee));
  } else {
    callee = LIGHTJS_AWAIT(evaluate(*expr.callee));
  }

  std::vector<Value> args;
  for (const auto& arg : expr.arguments) {
    // Check if this is a spread element
    if (auto* spread = std::get_if<SpreadElement>(&arg->node)) {
      // Evaluate the argument
      Value val = LIGHTJS_AWAIT(evaluate(*spread->argument));

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
      args.push_back(argVal);
    }
  }

  if (callee.isFunction()) {
    LIGHTJS_RETURN(callFunction(callee, args, thisValue));
  }

  // Throw TypeError if trying to call a non-function
  throwError(ErrorType::TypeError, callee.toString() + " is not a function");
  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::evaluateMember(const MemberExpr& expr) {
  auto objTask = evaluate(*expr.object);
  Value obj;
  LIGHTJS_RUN_TASK(objTask, obj);
  std::string propName;
  if (expr.computed) {
    auto propTask = evaluate(*expr.property);
    LIGHTJS_RUN_TASK_VOID(propTask);
    propName = propTask.result().toString();
  } else {
    if (auto* id = std::get_if<Identifier>(&expr.property->node)) {
      propName = id->name;
    }
  }

  // Optional chaining: if object is null or undefined, return undefined
  if (expr.optional && (obj.isNull() || obj.isUndefined())) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  // Proxy trap handling - intercept get operations
  if (obj.isProxy()) {
    auto proxyPtr = std::get<std::shared_ptr<Proxy>>(obj.data);

    // Compute property name
    std::string propName;
    if (expr.computed) {
      Value propVal = LIGHTJS_AWAIT(evaluate(*expr.property));
      propName = propVal.toString();
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
        if (getTrap->isNative) {
          std::vector<Value> trapArgs = {
            *proxyPtr->target,
            Value(propName),
            obj  // receiver is the proxy itself
          };
          LIGHTJS_RETURN(getTrap->nativeFunc(trapArgs));
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
            return invokeFunction(callback, {val}, Value(Undefined{}));
          };
        }

        // Get onRejected callback if provided
        if (args.size() > 1 && args[1].isFunction()) {
          auto callback = std::get<std::shared_ptr<Function>>(args[1].data);
          onRejected = [this, callback](Value val) -> Value {
            return invokeFunction(callback, {val}, Value(Undefined{}));
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
            return invokeFunction(callback, {val}, Value(Undefined{}));
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
            return invokeFunction(callback, {}, Value(Undefined{}));
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
        if (args.empty()) throw std::runtime_error("getInt8 requires offset");
        return Value(static_cast<double>(viewPtr->getInt8(static_cast<size_t>(args[0].toNumber()))));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getUint8") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) throw std::runtime_error("getUint8 requires offset");
        return Value(static_cast<double>(viewPtr->getUint8(static_cast<size_t>(args[0].toNumber()))));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getInt16") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) throw std::runtime_error("getInt16 requires offset");
        bool littleEndian = args.size() > 1 ? args[1].toBool() : false;
        return Value(static_cast<double>(viewPtr->getInt16(static_cast<size_t>(args[0].toNumber()), littleEndian)));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getUint16") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) throw std::runtime_error("getUint16 requires offset");
        bool littleEndian = args.size() > 1 ? args[1].toBool() : false;
        return Value(static_cast<double>(viewPtr->getUint16(static_cast<size_t>(args[0].toNumber()), littleEndian)));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getInt32") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) throw std::runtime_error("getInt32 requires offset");
        bool littleEndian = args.size() > 1 ? args[1].toBool() : false;
        return Value(static_cast<double>(viewPtr->getInt32(static_cast<size_t>(args[0].toNumber()), littleEndian)));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getUint32") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) throw std::runtime_error("getUint32 requires offset");
        bool littleEndian = args.size() > 1 ? args[1].toBool() : false;
        return Value(static_cast<double>(viewPtr->getUint32(static_cast<size_t>(args[0].toNumber()), littleEndian)));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getFloat32") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) throw std::runtime_error("getFloat32 requires offset");
        bool littleEndian = args.size() > 1 ? args[1].toBool() : false;
        return Value(static_cast<double>(viewPtr->getFloat32(static_cast<size_t>(args[0].toNumber()), littleEndian)));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getFloat64") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) throw std::runtime_error("getFloat64 requires offset");
        bool littleEndian = args.size() > 1 ? args[1].toBool() : false;
        return Value(viewPtr->getFloat64(static_cast<size_t>(args[0].toNumber()), littleEndian));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getBigInt64") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) throw std::runtime_error("getBigInt64 requires offset");
        bool littleEndian = args.size() > 1 ? args[1].toBool() : false;
        return Value(BigInt(viewPtr->getBigInt64(static_cast<size_t>(args[0].toNumber()), littleEndian)));
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "getBigUint64") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.empty()) throw std::runtime_error("getBigUint64 requires offset");
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
        if (args.size() < 2) throw std::runtime_error("setInt8 requires offset and value");
        viewPtr->setInt8(static_cast<size_t>(args[0].toNumber()), static_cast<int8_t>(args[1].toNumber()));
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "setUint8") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) throw std::runtime_error("setUint8 requires offset and value");
        viewPtr->setUint8(static_cast<size_t>(args[0].toNumber()), static_cast<uint8_t>(args[1].toNumber()));
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
    if (propName == "setInt16") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [viewPtr](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) throw std::runtime_error("setInt16 requires offset and value");
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
        if (args.size() < 2) throw std::runtime_error("setUint16 requires offset and value");
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
        if (args.size() < 2) throw std::runtime_error("setInt32 requires offset and value");
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
        if (args.size() < 2) throw std::runtime_error("setUint32 requires offset and value");
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
        if (args.size() < 2) throw std::runtime_error("setFloat32 requires offset and value");
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
        if (args.size() < 2) throw std::runtime_error("setFloat64 requires offset and value");
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
        if (args.size() < 2) throw std::runtime_error("setBigInt64 requires offset and value");
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
        if (args.size() < 2) throw std::runtime_error("setBigUint64 requires offset and value");
        bool littleEndian = args.size() > 2 ? args[2].toBool() : false;
        viewPtr->setBigUint64(static_cast<size_t>(args[0].toNumber()), static_cast<uint64_t>(args[1].toBigInt()), littleEndian);
        return Value(Undefined{});
      };
      LIGHTJS_RETURN(Value(fn));
    }
  }

  if (obj.isObject()) {
    auto objPtr = std::get<std::shared_ptr<Object>>(obj.data);
    auto it = objPtr->properties.find(propName);
    if (it != objPtr->properties.end()) {
      LIGHTJS_RETURN(it->second);
    }
  }

  if (obj.isFunction()) {
    auto funcPtr = std::get<std::shared_ptr<Function>>(obj.data);
    auto it = funcPtr->properties.find(propName);
    if (it != funcPtr->properties.end()) {
      LIGHTJS_RETURN(it->second);
    }
  }

  // Generator methods
  if (obj.isGenerator()) {
    auto genPtr = std::get<std::shared_ptr<Generator>>(obj.data);

    if (propName == iteratorKey) {
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
        return this->runGeneratorNext(genPtr, mode, resumeValue);
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
        return makeIteratorResult(returnValue, true);
      };
      LIGHTJS_RETURN(Value(fn));
    }

    if (propName == "throw") {
      auto fn = std::make_shared<Function>();
      fn->isNative = true;
      fn->nativeFunc = [genPtr](const std::vector<Value>& args) -> Value {
        genPtr->state = GeneratorState::Completed;
        throw std::runtime_error(args.empty() ? "Generator error" : args[0].toString());
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
          throw std::runtime_error("map requires a callback function");
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
          throw std::runtime_error("filter requires a callback function");
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
          throw std::runtime_error("forEach requires a callback function");
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
          throw std::runtime_error("reduce requires a callback function");
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
          throw std::runtime_error("reduceRight requires a callback function");
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
          throw std::runtime_error("find requires a callback function");
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
          throw std::runtime_error("findIndex requires a callback function");
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
          throw std::runtime_error("findLast requires a callback function");
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
          throw std::runtime_error("findLastIndex requires a callback function");
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
          throw std::runtime_error("some requires a callback function");
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
          throw std::runtime_error("every requires a callback function");
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
          throw std::runtime_error("Invalid index");
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
          throw std::runtime_error("flatMap requires a callback function");
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

    try {
      size_t idx = std::stoul(propName);
      if (idx < arrPtr->elements.size()) {
        LIGHTJS_RETURN(arrPtr->elements[idx]);
      }
    } catch (...) {}
  }

  if (obj.isTypedArray()) {
    auto taPtr = std::get<std::shared_ptr<TypedArray>>(obj.data);
    if (propName == "length") {
      LIGHTJS_RETURN(Value(static_cast<double>(taPtr->length)));
    }
    if (propName == "byteLength") {
      LIGHTJS_RETURN(Value(static_cast<double>(taPtr->buffer.size())));
    }
    try {
      size_t idx = std::stoul(propName);
      if (idx < taPtr->length) {
        if (taPtr->type == TypedArrayType::BigInt64 || taPtr->type == TypedArrayType::BigUint64) {
          LIGHTJS_RETURN(Value(BigInt(taPtr->getBigIntElement(idx))));
        } else {
          LIGHTJS_RETURN(Value(taPtr->getElement(idx)));
        }
      }
    } catch (...) {}
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
          throw std::runtime_error("toString() radix must be between 2 and 36");
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

    if (propName == "length") {
      // Return Unicode code point length, not byte length
      LIGHTJS_RETURN(Value(static_cast<double>(unicode::utf8Length(str))));
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
      fn->nativeFunc = [str](const std::vector<Value>& args) -> Value {
        std::string result = str;
        for (const auto& arg : args) {
          result += arg.toString();
        }
        return Value(result);
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
        if (count < 0) throw std::runtime_error("RangeError: Invalid count value");
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
    genPtr->state = GeneratorState::Executing;

    if (genPtr->function && genPtr->context) {
      auto prevEnv = env_;
      env_ = std::static_pointer_cast<Environment>(genPtr->context);

      auto bodyPtr = std::static_pointer_cast<std::vector<StmtPtr>>(genPtr->function->body);
      Value result = Value(Undefined{});

      auto prevFlow = flow_;
      flow_.reset();
      if (mode != ControlFlow::ResumeMode::None) {
        flow_.prepareResume(mode, resumeValue);
      }

      size_t startIndex = genPtr->yieldIndex;
      for (size_t i = startIndex; i < bodyPtr->size(); i++) {
        auto task = evaluate(*(*bodyPtr)[i]);
  LIGHTJS_RUN_TASK(task, result);

        if (flow_.type == ControlFlow::Type::Yield) {
          genPtr->state = GeneratorState::SuspendedYield;
          genPtr->currentValue = std::make_shared<Value>(flow_.value);
          genPtr->yieldIndex = i + 1;

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
    if (value.isObject()) {
      // Only treat as IteratorObject if it has a 'next' method (i.e., it's already an iterator)
      // Otherwise, fall through to check for Symbol.iterator
      auto obj = std::get<std::shared_ptr<Object>>(value.data);
      if (obj->properties.find("next") != obj->properties.end()) {
        record.kind = IteratorRecord::Kind::IteratorObject;
        record.iteratorObject = obj;
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
        return record;
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
      return runGeneratorNext(record.generator);
    case IteratorRecord::Kind::Array: {
      if (!record.array || record.index >= record.array->elements.size()) {
        return makeIteratorResult(Value(Undefined{}), true);
      }
      Value value = record.array->elements[record.index++];
      return makeIteratorResult(value, false);
    }
    case IteratorRecord::Kind::String: {
      if (record.index >= record.stringValue.size()) {
        return makeIteratorResult(Value(Undefined{}), true);
      }
      char c = record.stringValue[record.index++];
      return makeIteratorResult(Value(std::string(1, c)), false);
    }
    case IteratorRecord::Kind::IteratorObject: {
      if (!record.iteratorObject) {
        return makeIteratorResult(Value(Undefined{}), true);
      }
      auto nextIt = record.iteratorObject->properties.find("next");
      if (nextIt == record.iteratorObject->properties.end() || !nextIt->second.isFunction()) {
        return makeIteratorResult(Value(Undefined{}), true);
      }
      return callFunction(nextIt->second, {});
    }
  }

  return makeIteratorResult(Value(Undefined{}), true);
}

Value Interpreter::callFunction(const Value& callee, const std::vector<Value>& args, const Value& thisValue) {
  if (!callee.isFunction()) {
    return Value(Undefined{});
  }

  auto func = std::get<std::shared_ptr<Function>>(callee.data);

  auto bindParameters = [&](std::shared_ptr<Environment>& targetEnv) {
    // Bind 'this' for method calls
    if (!thisValue.isUndefined()) {
      targetEnv->define("this", thisValue);
    }

    for (size_t i = 0; i < func->params.size(); ++i) {
      if (i < args.size()) {
        targetEnv->define(func->params[i].name, args[i]);
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
      for (size_t i = func->params.size(); i < args.size(); ++i) {
        restArr->elements.push_back(args[i]);
      }
      targetEnv->define(*func->restParam, Value(restArr));
    }
  };

  if (func->isNative) {
    return func->nativeFunc(args);
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
    Value result = Value(Undefined{});

    auto prevFlow = flow_;
    flow_ = ControlFlow{};

    try {
      for (const auto& stmt : *bodyPtr) {
        auto stmtTask = evaluate(*stmt);
  LIGHTJS_RUN_TASK(stmtTask, result);

        if (flow_.type == ControlFlow::Type::Return) {
          result = flow_.value;
          break;
        }

        // Preserve throw flow control (errors)
        if (flow_.type == ControlFlow::Type::Throw) {
          promise->reject(flow_.value);
          break;
        }
      }
      if (flow_.type != ControlFlow::Type::Throw) {
        promise->resolve(result);
      }
    } catch (const std::exception& e) {
      promise->reject(Value(std::string(e.what())));
    }

    // Don't restore flow if an error was thrown - preserve the error
    if (flow_.type != ControlFlow::Type::Throw) {
      flow_ = prevFlow;
    }
    env_ = prevEnv;
    return Value(promise);
  }

  // Push stack frame for JavaScript function calls
  auto stackFrame = pushStackFrame("<function>");

  auto prevEnv = env_;
  env_ = std::static_pointer_cast<Environment>(func->closure);
  env_ = env_->createChild();
  bindParameters(env_);

  auto bodyPtr = std::static_pointer_cast<std::vector<StmtPtr>>(func->body);
  Value result = Value(Undefined{});

  auto prevFlow = flow_;
  flow_ = ControlFlow{};

  for (const auto& stmt : *bodyPtr) {
    auto stmtTask = evaluate(*stmt);
  LIGHTJS_RUN_TASK(stmtTask, result);

    if (flow_.type == ControlFlow::Type::Return) {
      result = flow_.value;
      break;
    }

    // Preserve throw flow control (errors)
    if (flow_.type == ControlFlow::Type::Throw) {
      break;
    }
  }

  // Don't restore flow if an error was thrown - preserve the error
  if (flow_.type != ControlFlow::Type::Throw) {
    flow_ = prevFlow;
  }
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
  for (const auto& elem : expr.elements) {
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
      } else {
        // Other iterables could be supported here
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

  LIGHTJS_RETURN(Value(func));
}

Task Interpreter::evaluateAwait(const AwaitExpr& expr) {
  auto task = evaluate(*expr.argument);
  Value val;
  LIGHTJS_RUN_TASK(task, val);

  if (val.isPromise()) {
    auto promise = std::get<std::shared_ptr<Promise>>(val.data);
    if (promise->state == PromiseState::Fulfilled) {
      LIGHTJS_RETURN(promise->result);
    } else if (promise->state == PromiseState::Rejected) {
      LIGHTJS_RETURN(promise->result);
    }
    LIGHTJS_RETURN(Value(Undefined{}));
  }

  LIGHTJS_RETURN(val);
}

Task Interpreter::evaluateYield(const YieldExpr& expr) {
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

Task Interpreter::evaluateNew(const NewExpr& expr) {
  // Check memory limit before potential allocation
  if (!checkMemoryLimit(sizeof(Object))) {
    LIGHTJS_RETURN(Value(Undefined{}));
  }

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

      // Bind super class if exists
      if (cls->superClass) {
        env_->define("__super__", Value(cls->superClass));
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

      flow_ = prevFlow;
      env_ = prevEnv;
    }

    LIGHTJS_RETURN(Value(instance));
  }

  // Handle Function constructor (regular constructor function)
  if (callee.isFunction()) {
    auto func = std::get<std::shared_ptr<Function>>(callee.data);

    if (func->isNative) {
      // Native constructors (e.g., Array, Object, Map, etc.)
      LIGHTJS_RETURN(func->nativeFunc(args));
    }

    // Create the new instance object
    auto instance = std::make_shared<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));

    // Set up execution environment
    auto prevEnv = env_;
    env_ = std::static_pointer_cast<Environment>(func->closure);
    env_ = env_->createChild();

    // Bind 'this' to the new instance
    env_->define("this", Value(instance));

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
        // If constructor returns an object, use that; otherwise use the instance
        if (flow_.value.isObject()) {
          instance = std::get<std::shared_ptr<Object>>(flow_.value.data);
        }
        break;
      }
    }

    flow_ = prevFlow;
    env_ = prevEnv;

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
    }
  }

  // Process methods
  for (const auto& method : expr.methods) {
    // Create function from method definition
    auto func = std::make_shared<Function>();
    func->isNative = false;
    func->isAsync = method.isAsync;
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

  LIGHTJS_RETURN(Value(cls));
}

// Helper for recursively binding destructuring patterns
void Interpreter::bindDestructuringPattern(const Expression& pattern, const Value& value, bool isConst) {
  if (auto* id = std::get_if<Identifier>(&pattern.node)) {
    // Simple identifier
    env_->define(id->name, value, isConst);
  } else if (auto* arrayPat = std::get_if<ArrayPattern>(&pattern.node)) {
    // Array destructuring
    std::shared_ptr<Array> arr;
    if (value.isArray()) {
      arr = std::get<std::shared_ptr<Array>>(value.data);
    } else {
      // Create empty array for non-array values
      arr = std::make_shared<Array>();
    }

    for (size_t i = 0; i < arrayPat->elements.size(); ++i) {
      if (!arrayPat->elements[i]) continue;  // Skip holes

      Value elemValue = (i < arr->elements.size()) ? arr->elements[i] : Value(Undefined{});

      // Recursively bind (handles nested patterns)
      bindDestructuringPattern(*arrayPat->elements[i], elemValue, isConst);
    }

    // Handle rest element
    if (arrayPat->rest) {
      auto restArr = std::make_shared<Array>();
      for (size_t i = arrayPat->elements.size(); i < arr->elements.size(); ++i) {
        restArr->elements.push_back(arr->elements[i]);
      }
      bindDestructuringPattern(*arrayPat->rest, Value(restArr), isConst);
    }
  } else if (auto* objPat = std::get_if<ObjectPattern>(&pattern.node)) {
    // Object destructuring
    std::shared_ptr<Object> obj;
    if (value.isObject()) {
      obj = std::get<std::shared_ptr<Object>>(value.data);
    } else {
      // Create empty object for non-object values
      obj = std::make_shared<Object>();
    }

    std::unordered_set<std::string> extractedKeys;

    for (const auto& prop : objPat->properties) {
      std::string keyName;
      if (auto* keyId = std::get_if<Identifier>(&prop.key->node)) {
        keyName = keyId->name;
      } else if (auto* keyStr = std::get_if<StringLiteral>(&prop.key->node)) {
        keyName = keyStr->value;
      } else {
        continue;
      }

      extractedKeys.insert(keyName);
      Value propValue = obj->properties.count(keyName) ? obj->properties[keyName] : Value(Undefined{});

      // Recursively bind (handles nested patterns)
      bindDestructuringPattern(*prop.value, propValue, isConst);
    }

    // Handle rest properties
    if (objPat->rest) {
      auto restObj = std::make_shared<Object>();
      for (const auto& [key, val] : obj->properties) {
        if (extractedKeys.find(key) == extractedKeys.end()) {
          restObj->properties[key] = val;
        }
      }
      bindDestructuringPattern(*objPat->rest, Value(restObj), isConst);
    }
  }
  // Other node types are ignored (error case in real JS)
}

// Helper to invoke a JavaScript function synchronously (used by native array methods for callbacks)
Value Interpreter::invokeFunction(std::shared_ptr<Function> func, const std::vector<Value>& args, const Value& thisValue) {
  if (func->isNative) {
    return func->nativeFunc(args);
  }

  // Save current environment
  auto prevEnv = env_;
  env_ = std::static_pointer_cast<Environment>(func->closure);
  env_ = env_->createChild();

  // Bind 'this' if provided
  if (!thisValue.isUndefined()) {
    env_->define("this", thisValue);
  }

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
      result = flow_.value;
      break;
    }
  }

  flow_ = prevFlow;
  env_ = prevEnv;
  return result;
}

Task Interpreter::evaluateVarDecl(const VarDeclaration& decl) {
  for (const auto& declarator : decl.declarations) {
    Value value = Value(Undefined{});
    if (declarator.init) {
      auto task = evaluate(*declarator.init);
  LIGHTJS_RUN_TASK(task, value);
    }

    // Use the unified destructuring helper
    bool isConst = (decl.kind == VarDeclaration::Kind::Const);
    bindDestructuringPattern(*declarator.pattern, value, isConst);
  }
  LIGHTJS_RETURN(Value(Undefined{}));
}


Task Interpreter::evaluateFuncDecl(const FunctionDeclaration& decl) {
  auto func = std::make_shared<Function>();
  func->isNative = false;
  func->isAsync = decl.isAsync;
  func->isGenerator = decl.isGenerator;

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

  env_->define(decl.id.name, Value(func));
  LIGHTJS_RETURN(Value(Undefined{}));
}

Task Interpreter::evaluateReturn(const ReturnStmt& stmt) {
  Value result = Value(Undefined{});
  if (stmt.argument) {
    auto task = evaluate(*stmt.argument);
  LIGHTJS_RUN_TASK(task, result);

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

  Value result = Value(Undefined{});
  for (const auto& s : stmt.body) {
    auto task = evaluate(*s);
  LIGHTJS_RUN_TASK(task, result);

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

  while (true) {
    auto testTask = evaluate(*stmt.test);
    LIGHTJS_RUN_TASK_VOID(testTask);

    if (!testTask.result().toBool()) {
      break;
    }

    auto bodyTask = evaluate(*stmt.body);
  LIGHTJS_RUN_TASK(bodyTask, result);

    if (flow_.type == ControlFlow::Type::Break) {
      flow_.type = ControlFlow::Type::None;
      break;
    } else if (flow_.type == ControlFlow::Type::Continue) {
      flow_.type = ControlFlow::Type::None;
      continue;
    } else if (flow_.type != ControlFlow::Type::None) {
      break;
    }
  }

  LIGHTJS_RETURN(result);
}

Task Interpreter::evaluateFor(const ForStmt& stmt) {
  auto prevEnv = env_;
  env_ = env_->createChild();

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
      flow_.type = ControlFlow::Type::None;
      break;
    } else if (flow_.type == ControlFlow::Type::Continue) {
      flow_.type = ControlFlow::Type::None;
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

  do {
    auto bodyTask = evaluate(*stmt.body);
  LIGHTJS_RUN_TASK(bodyTask, result);

    if (flow_.type == ControlFlow::Type::Break) {
      flow_.type = ControlFlow::Type::None;
      break;
    } else if (flow_.type == ControlFlow::Type::Continue) {
      flow_.type = ControlFlow::Type::None;
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

  // Evaluate the right-hand side (the object to iterate over)
  auto rightTask = evaluate(*stmt.right);
  Value obj;
  LIGHTJS_RUN_TASK(rightTask, obj);

  Value result = Value(Undefined{});

  // Get the variable name from the left side
  std::string varName;
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.left->node)) {
    if (!varDecl->declarations.empty()) {
      // For now, only support simple identifier patterns in for-in/for-of
      if (auto* id = std::get_if<Identifier>(&varDecl->declarations[0].pattern->node)) {
        varName = id->name;
      }
      // Define the variable in the loop scope
      env_->define(varName, Value(Undefined{}));
    }
  } else if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt.left->node)) {
    if (auto* ident = std::get_if<Identifier>(&exprStmt->expression->node)) {
      varName = ident->name;
    }
  }

  // Iterate over object properties
  if (auto* objPtr = std::get_if<std::shared_ptr<Object>>(&obj.data)) {
    for (const auto& [key, value] : (*objPtr)->properties) {
      // Assign the key to the loop variable
      env_->set(varName, Value(key));

      auto bodyTask = evaluate(*stmt.body);
  LIGHTJS_RUN_TASK(bodyTask, result);

      if (flow_.type == ControlFlow::Type::Break) {
        flow_.type = ControlFlow::Type::None;
        break;
      } else if (flow_.type == ControlFlow::Type::Continue) {
        flow_.type = ControlFlow::Type::None;
        continue;
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

  // Evaluate the right-hand side (the iterable to iterate over)
  auto rightTask = evaluate(*stmt.right);
  Value iterable;
  LIGHTJS_RUN_TASK(rightTask, iterable);

  Value result = Value(Undefined{});

  // Get the variable name from the left side
  std::string varName;
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.left->node)) {
    if (!varDecl->declarations.empty()) {
      // For now, only support simple identifier patterns in for-in/for-of
      if (auto* id = std::get_if<Identifier>(&varDecl->declarations[0].pattern->node)) {
        varName = id->name;
      }
      // Define the variable in the loop scope
      env_->define(varName, Value(Undefined{}));
    }
  } else if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt.left->node)) {
    if (auto* ident = std::get_if<Identifier>(&exprStmt->expression->node)) {
      varName = ident->name;
    }
  }

  auto iteratorOpt = getIterator(iterable);
  if (iteratorOpt.has_value()) {
    auto iterator = std::move(*iteratorOpt);
    while (true) {
      Value stepResult = iteratorNext(iterator);
      if (!stepResult.isObject()) {
        break;
      }

      auto resultObj = std::get<std::shared_ptr<Object>>(stepResult.data);
      bool isDone = false;
      if (auto doneIt = resultObj->properties.find("done"); doneIt != resultObj->properties.end()) {
        isDone = doneIt->second.toBool();
      }
      if (isDone) {
        break;
      }

      Value currentValue = Value(Undefined{});
      if (auto valueIt = resultObj->properties.find("value"); valueIt != resultObj->properties.end()) {
        currentValue = valueIt->second;
      }

      env_->set(varName, currentValue);

      auto bodyTask = evaluate(*stmt.body);
  LIGHTJS_RUN_TASK(bodyTask, result);

      if (flow_.type == ControlFlow::Type::Break) {
        flow_.type = ControlFlow::Type::None;
        break;
      } else if (flow_.type == ControlFlow::Type::Continue) {
        flow_.type = ControlFlow::Type::None;
        continue;
      } else if (flow_.type != ControlFlow::Type::None) {
        break;
      }
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
          flow_.type = ControlFlow::Type::None;
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
        flow_.type = ControlFlow::Type::None;
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

      if (!stmt.handler.param.name.empty()) {
        env_->define(stmt.handler.param.name, flow_.value);
      }

      flow_.type = ControlFlow::Type::None;

      for (const auto& catchStmt : stmt.handler.body) {
        auto catchTask = evaluate(*catchStmt);
  LIGHTJS_RUN_TASK(catchTask, result);
      }

      env_ = prevEnv;
      break;
    }

    if (flow_.type != ControlFlow::Type::None) {
      break;
    }
  }

  if (stmt.hasFinalizer) {
    for (const auto& finalStmt : stmt.finalizer) {
      auto finalTask = evaluate(*finalStmt);
      LIGHTJS_RUN_TASK_VOID(finalTask);
    }
  }

  LIGHTJS_RETURN(result);
}

Task Interpreter::evaluateImport(const ImportDeclaration& stmt) {
  // Import evaluation is handled at the module level during instantiation
  // This is just a placeholder as imports are resolved before execution
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
