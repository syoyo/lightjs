#include "interpreter.h"
#include <iostream>
#include <cmath>

namespace tinyjs {

Interpreter::Interpreter(std::shared_ptr<Environment> env) : env_(env) {}

Task Interpreter::evaluate(const Program& program) {
  Value result = Value(Undefined{});
  for (const auto& stmt : program.body) {
    auto task = evaluate(*stmt);
    while (!task.done()) {
      std::coroutine_handle<>::from_address(task.handle.address()).resume();
    }
    result = task.result();

    if (flow_.type != ControlFlow::Type::None) {
      break;
    }
  }
  co_return result;
}

Task Interpreter::evaluate(const Statement& stmt) {
  if (auto* node = std::get_if<VarDeclaration>(&stmt.node)) {
    co_return co_await evaluateVarDecl(*node);
  } else if (auto* node = std::get_if<FunctionDeclaration>(&stmt.node)) {
    co_return co_await evaluateFuncDecl(*node);
  } else if (auto* node = std::get_if<ReturnStmt>(&stmt.node)) {
    co_return co_await evaluateReturn(*node);
  } else if (auto* node = std::get_if<ExpressionStmt>(&stmt.node)) {
    co_return co_await evaluateExprStmt(*node);
  } else if (auto* node = std::get_if<BlockStmt>(&stmt.node)) {
    co_return co_await evaluateBlock(*node);
  } else if (auto* node = std::get_if<IfStmt>(&stmt.node)) {
    co_return co_await evaluateIf(*node);
  } else if (auto* node = std::get_if<WhileStmt>(&stmt.node)) {
    co_return co_await evaluateWhile(*node);
  } else if (auto* node = std::get_if<ForStmt>(&stmt.node)) {
    co_return co_await evaluateFor(*node);
  } else if (std::holds_alternative<BreakStmt>(stmt.node)) {
    flow_.type = ControlFlow::Type::Break;
    co_return Value(Undefined{});
  } else if (std::holds_alternative<ContinueStmt>(stmt.node)) {
    flow_.type = ControlFlow::Type::Continue;
    co_return Value(Undefined{});
  } else if (auto* node = std::get_if<ThrowStmt>(&stmt.node)) {
    auto task = evaluate(*node->argument);
    while (!task.done()) {
      std::coroutine_handle<>::from_address(task.handle.address()).resume();
    }
    flow_.type = ControlFlow::Type::Throw;
    flow_.value = task.result();
    co_return Value(Undefined{});
  } else if (auto* node = std::get_if<TryStmt>(&stmt.node)) {
    co_return co_await evaluateTry(*node);
  }
  co_return Value(Undefined{});
}

Task Interpreter::evaluate(const Expression& expr) {
  if (auto* node = std::get_if<Identifier>(&expr.node)) {
    if (auto val = env_->get(node->name)) {
      co_return *val;
    }
    co_return Value(Undefined{});
  } else if (auto* node = std::get_if<NumberLiteral>(&expr.node)) {
    co_return Value(node->value);
  } else if (auto* node = std::get_if<BigIntLiteral>(&expr.node)) {
    co_return Value(BigInt(node->value));
  } else if (auto* node = std::get_if<StringLiteral>(&expr.node)) {
    co_return Value(node->value);
  } else if (auto* node = std::get_if<RegexLiteral>(&expr.node)) {
    auto regex = std::make_shared<Regex>(node->pattern, node->flags);
    co_return Value(regex);
  } else if (auto* node = std::get_if<BoolLiteral>(&expr.node)) {
    co_return Value(node->value);
  } else if (std::holds_alternative<NullLiteral>(expr.node)) {
    co_return Value(Null{});
  } else if (auto* node = std::get_if<BinaryExpr>(&expr.node)) {
    co_return co_await evaluateBinary(*node);
  } else if (auto* node = std::get_if<UnaryExpr>(&expr.node)) {
    co_return co_await evaluateUnary(*node);
  } else if (auto* node = std::get_if<AssignmentExpr>(&expr.node)) {
    co_return co_await evaluateAssignment(*node);
  } else if (auto* node = std::get_if<UpdateExpr>(&expr.node)) {
    co_return co_await evaluateUpdate(*node);
  } else if (auto* node = std::get_if<CallExpr>(&expr.node)) {
    co_return co_await evaluateCall(*node);
  } else if (auto* node = std::get_if<MemberExpr>(&expr.node)) {
    co_return co_await evaluateMember(*node);
  } else if (auto* node = std::get_if<ConditionalExpr>(&expr.node)) {
    co_return co_await evaluateConditional(*node);
  } else if (auto* node = std::get_if<ArrayExpr>(&expr.node)) {
    co_return co_await evaluateArray(*node);
  } else if (auto* node = std::get_if<ObjectExpr>(&expr.node)) {
    co_return co_await evaluateObject(*node);
  } else if (auto* node = std::get_if<FunctionExpr>(&expr.node)) {
    co_return co_await evaluateFunction(*node);
  } else if (auto* node = std::get_if<AwaitExpr>(&expr.node)) {
    co_return co_await evaluateAwait(*node);
  }
  co_return Value(Undefined{});
}

Task Interpreter::evaluateBinary(const BinaryExpr& expr) {
  auto leftTask = evaluate(*expr.left);
  while (!leftTask.done()) {
    std::coroutine_handle<>::from_address(leftTask.handle.address()).resume();
  }
  Value left = leftTask.result();

  auto rightTask = evaluate(*expr.right);
  while (!rightTask.done()) {
    std::coroutine_handle<>::from_address(rightTask.handle.address()).resume();
  }
  Value right = rightTask.result();

  switch (expr.op) {
    case BinaryExpr::Op::Add:
      if (left.isString() || right.isString()) {
        co_return Value(left.toString() + right.toString());
      }
      if (left.isBigInt() && right.isBigInt()) {
        co_return Value(BigInt(left.toBigInt() + right.toBigInt()));
      }
      co_return Value(left.toNumber() + right.toNumber());
    case BinaryExpr::Op::Sub:
      if (left.isBigInt() && right.isBigInt()) {
        co_return Value(BigInt(left.toBigInt() - right.toBigInt()));
      }
      co_return Value(left.toNumber() - right.toNumber());
    case BinaryExpr::Op::Mul:
      if (left.isBigInt() && right.isBigInt()) {
        co_return Value(BigInt(left.toBigInt() * right.toBigInt()));
      }
      co_return Value(left.toNumber() * right.toNumber());
    case BinaryExpr::Op::Div:
      if (left.isBigInt() && right.isBigInt()) {
        co_return Value(BigInt(left.toBigInt() / right.toBigInt()));
      }
      co_return Value(left.toNumber() / right.toNumber());
    case BinaryExpr::Op::Mod:
      if (left.isBigInt() && right.isBigInt()) {
        co_return Value(BigInt(left.toBigInt() % right.toBigInt()));
      }
      co_return Value(std::fmod(left.toNumber(), right.toNumber()));
    case BinaryExpr::Op::Less:
      if (left.isBigInt() && right.isBigInt()) {
        co_return Value(left.toBigInt() < right.toBigInt());
      }
      co_return Value(left.toNumber() < right.toNumber());
    case BinaryExpr::Op::Greater:
      if (left.isBigInt() && right.isBigInt()) {
        co_return Value(left.toBigInt() > right.toBigInt());
      }
      co_return Value(left.toNumber() > right.toNumber());
    case BinaryExpr::Op::LessEqual:
      if (left.isBigInt() && right.isBigInt()) {
        co_return Value(left.toBigInt() <= right.toBigInt());
      }
      co_return Value(left.toNumber() <= right.toNumber());
    case BinaryExpr::Op::GreaterEqual:
      if (left.isBigInt() && right.isBigInt()) {
        co_return Value(left.toBigInt() >= right.toBigInt());
      }
      co_return Value(left.toNumber() >= right.toNumber());
    case BinaryExpr::Op::Equal:
      if (left.isBigInt() && right.isBigInt()) {
        co_return Value(left.toBigInt() == right.toBigInt());
      }
      co_return Value(left.toNumber() == right.toNumber());
    case BinaryExpr::Op::NotEqual:
      if (left.isBigInt() && right.isBigInt()) {
        co_return Value(left.toBigInt() != right.toBigInt());
      }
      co_return Value(left.toNumber() != right.toNumber());
    case BinaryExpr::Op::StrictEqual:
      if (left.isBigInt() && right.isBigInt()) {
        co_return Value(left.toBigInt() == right.toBigInt());
      }
      co_return Value(left.toNumber() == right.toNumber());
    case BinaryExpr::Op::StrictNotEqual:
      if (left.isBigInt() && right.isBigInt()) {
        co_return Value(left.toBigInt() != right.toBigInt());
      }
      co_return Value(left.toNumber() != right.toNumber());
    case BinaryExpr::Op::LogicalAnd:
      co_return left.toBool() ? right : left;
    case BinaryExpr::Op::LogicalOr:
      co_return left.toBool() ? left : right;
  }

  co_return Value(Undefined{});
}

Task Interpreter::evaluateUnary(const UnaryExpr& expr) {
  auto argTask = evaluate(*expr.argument);
  while (!argTask.done()) {
    std::coroutine_handle<>::from_address(argTask.handle.address()).resume();
  }
  Value arg = argTask.result();

  switch (expr.op) {
    case UnaryExpr::Op::Not:
      co_return Value(!arg.toBool());
    case UnaryExpr::Op::Minus:
      if (arg.isBigInt()) {
        co_return Value(BigInt(-arg.toBigInt()));
      }
      co_return Value(-arg.toNumber());
    case UnaryExpr::Op::Plus:
      co_return Value(arg.toNumber());
    case UnaryExpr::Op::Typeof: {
      if (arg.isUndefined()) co_return Value("undefined");
      if (arg.isNull()) co_return Value("object");
      if (arg.isBool()) co_return Value("boolean");
      if (arg.isNumber()) co_return Value("number");
      if (arg.isBigInt()) co_return Value("bigint");
      if (arg.isString()) co_return Value("string");
      if (arg.isFunction()) co_return Value("function");
      co_return Value("object");
    }
  }

  co_return Value(Undefined{});
}

Task Interpreter::evaluateAssignment(const AssignmentExpr& expr) {
  auto rightTask = evaluate(*expr.right);
  while (!rightTask.done()) {
    std::coroutine_handle<>::from_address(rightTask.handle.address()).resume();
  }
  Value right = rightTask.result();

  if (auto* id = std::get_if<Identifier>(&expr.left->node)) {
    if (expr.op == AssignmentExpr::Op::Assign) {
      env_->set(id->name, right);
      co_return right;
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
      co_return result;
    }
  }

  if (auto* member = std::get_if<MemberExpr>(&expr.left->node)) {
    auto objTask = evaluate(*member->object);
    while (!objTask.done()) {
      std::coroutine_handle<>::from_address(objTask.handle.address()).resume();
    }
    Value obj = objTask.result();

    std::string propName;
    if (member->computed) {
      auto propTask = evaluate(*member->property);
      while (!propTask.done()) {
        std::coroutine_handle<>::from_address(propTask.handle.address()).resume();
      }
      propName = propTask.result().toString();
    } else {
      if (auto* id = std::get_if<Identifier>(&member->property->node)) {
        propName = id->name;
      }
    }

    if (obj.isObject()) {
      auto objPtr = std::get<std::shared_ptr<Object>>(obj.data);
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
      co_return right;
    }

    if (obj.isArray()) {
      auto arrPtr = std::get<std::shared_ptr<Array>>(obj.data);
      try {
        size_t idx = std::stoul(propName);
        if (idx >= arrPtr->elements.size()) {
          arrPtr->elements.resize(idx + 1, Value(Undefined{}));
        }
        arrPtr->elements[idx] = right;
        co_return right;
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
        co_return right;
      } catch (...) {}
    }
  }

  co_return right;
}

Task Interpreter::evaluateUpdate(const UpdateExpr& expr) {
  if (auto* id = std::get_if<Identifier>(&expr.argument->node)) {
    if (auto current = env_->get(id->name)) {
      double num = current->toNumber();
      double newVal = (expr.op == UpdateExpr::Op::Increment) ? num + 1 : num - 1;
      env_->set(id->name, Value(newVal));
      co_return expr.prefix ? Value(newVal) : Value(num);
    }
  }

  co_return Value(Undefined{});
}

Task Interpreter::evaluateCall(const CallExpr& expr) {
  auto calleeTask = evaluate(*expr.callee);
  while (!calleeTask.done()) {
    std::coroutine_handle<>::from_address(calleeTask.handle.address()).resume();
  }
  Value callee = calleeTask.result();

  std::vector<Value> args;
  for (const auto& arg : expr.arguments) {
    auto argTask = evaluate(*arg);
    while (!argTask.done()) {
      std::coroutine_handle<>::from_address(argTask.handle.address()).resume();
    }
    args.push_back(argTask.result());
  }

  if (callee.isFunction()) {
    auto func = std::get<std::shared_ptr<Function>>(callee.data);
    if (func->isNative) {
      co_return func->nativeFunc(args);
    }

    // If it's an async function, wrap the result in a Promise
    if (func->isAsync) {
      auto promise = std::make_shared<Promise>();

      // Execute the function body
      auto prevEnv = env_;
      env_ = std::static_pointer_cast<Environment>(func->closure);
      env_ = env_->createChild();

      for (size_t i = 0; i < func->params.size() && i < args.size(); ++i) {
        env_->define(func->params[i], args[i]);
      }

      auto bodyPtr = std::static_pointer_cast<std::vector<StmtPtr>>(func->body);
      Value result = Value(Undefined{});

      auto prevFlow = flow_;
      flow_ = ControlFlow{};

      try {
        for (const auto& stmt : *bodyPtr) {
          auto stmtTask = evaluate(*stmt);
          while (!stmtTask.done()) {
            std::coroutine_handle<>::from_address(stmtTask.handle.address()).resume();
          }
          result = stmtTask.result();

          if (flow_.type == ControlFlow::Type::Return) {
            result = flow_.value;
            break;
          }
        }

        // Resolve the promise with the result
        promise->resolve(result);
      } catch (const std::exception& e) {
        // Reject the promise if there's an error
        promise->reject(Value(std::string(e.what())));
      }

      flow_ = prevFlow;
      env_ = prevEnv;

      co_return Value(promise);
    } else {
      // Regular synchronous function
      auto prevEnv = env_;
      env_ = std::static_pointer_cast<Environment>(func->closure);
      env_ = env_->createChild();

      for (size_t i = 0; i < func->params.size() && i < args.size(); ++i) {
        env_->define(func->params[i], args[i]);
      }

      auto bodyPtr = std::static_pointer_cast<std::vector<StmtPtr>>(func->body);
      Value result = Value(Undefined{});

      auto prevFlow = flow_;
      flow_ = ControlFlow{};

      for (const auto& stmt : *bodyPtr) {
        auto stmtTask = evaluate(*stmt);
        while (!stmtTask.done()) {
          std::coroutine_handle<>::from_address(stmtTask.handle.address()).resume();
        }
        result = stmtTask.result();

        if (flow_.type == ControlFlow::Type::Return) {
          result = flow_.value;
          break;
        }
      }

      flow_ = prevFlow;
      env_ = prevEnv;
      co_return result;
    }
  }

  co_return Value(Undefined{});
}

Task Interpreter::evaluateMember(const MemberExpr& expr) {
  auto objTask = evaluate(*expr.object);
  while (!objTask.done()) {
    std::coroutine_handle<>::from_address(objTask.handle.address()).resume();
  }
  Value obj = objTask.result();

  std::string propName;
  if (expr.computed) {
    auto propTask = evaluate(*expr.property);
    while (!propTask.done()) {
      std::coroutine_handle<>::from_address(propTask.handle.address()).resume();
    }
    propName = propTask.result().toString();
  } else {
    if (auto* id = std::get_if<Identifier>(&expr.property->node)) {
      propName = id->name;
    }
  }

  if (obj.isPromise()) {
    auto promisePtr = std::get<std::shared_ptr<Promise>>(obj.data);
    if (promisePtr->state == PromiseState::Fulfilled) {
      Value resolvedValue = promisePtr->result;
      if (resolvedValue.isObject()) {
        auto objPtr = std::get<std::shared_ptr<Object>>(resolvedValue.data);
        auto it = objPtr->properties.find(propName);
        if (it != objPtr->properties.end()) {
          co_return it->second;
        }
      }
    }
  }

  if (obj.isObject()) {
    auto objPtr = std::get<std::shared_ptr<Object>>(obj.data);
    auto it = objPtr->properties.find(propName);
    if (it != objPtr->properties.end()) {
      co_return it->second;
    }
  }

  if (obj.isArray()) {
    auto arrPtr = std::get<std::shared_ptr<Array>>(obj.data);
    if (propName == "length") {
      co_return Value(static_cast<double>(arrPtr->elements.size()));
    }
    try {
      size_t idx = std::stoul(propName);
      if (idx < arrPtr->elements.size()) {
        co_return arrPtr->elements[idx];
      }
    } catch (...) {}
  }

  if (obj.isTypedArray()) {
    auto taPtr = std::get<std::shared_ptr<TypedArray>>(obj.data);
    if (propName == "length") {
      co_return Value(static_cast<double>(taPtr->length));
    }
    if (propName == "byteLength") {
      co_return Value(static_cast<double>(taPtr->buffer.size()));
    }
    try {
      size_t idx = std::stoul(propName);
      if (idx < taPtr->length) {
        if (taPtr->type == TypedArrayType::BigInt64 || taPtr->type == TypedArrayType::BigUint64) {
          co_return Value(BigInt(taPtr->getBigIntElement(idx)));
        } else {
          co_return Value(taPtr->getElement(idx));
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
      co_return Value(testFn);
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
      co_return Value(execFn);
    }

    if (propName == "source") {
      co_return Value(regexPtr->pattern);
    }

    if (propName == "flags") {
      co_return Value(regexPtr->flags);
    }
  }

  if (obj.isString()) {
    std::string str = std::get<std::string>(obj.data);

    if (propName == "length") {
      co_return Value(static_cast<double>(str.length()));
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
      co_return Value(matchFn);
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
      co_return Value(replaceFn);
    }
  }

  co_return Value(Undefined{});
}

Task Interpreter::evaluateConditional(const ConditionalExpr& expr) {
  auto testTask = evaluate(*expr.test);
  while (!testTask.done()) {
    std::coroutine_handle<>::from_address(testTask.handle.address()).resume();
  }

  if (testTask.result().toBool()) {
    auto consTask = evaluate(*expr.consequent);
    while (!consTask.done()) {
      std::coroutine_handle<>::from_address(consTask.handle.address()).resume();
    }
    co_return consTask.result();
  } else {
    auto altTask = evaluate(*expr.alternate);
    while (!altTask.done()) {
      std::coroutine_handle<>::from_address(altTask.handle.address()).resume();
    }
    co_return altTask.result();
  }
}

Task Interpreter::evaluateArray(const ArrayExpr& expr) {
  auto arr = std::make_shared<Array>();
  for (const auto& elem : expr.elements) {
    auto task = evaluate(*elem);
    while (!task.done()) {
      std::coroutine_handle<>::from_address(task.handle.address()).resume();
    }
    arr->elements.push_back(task.result());
  }
  co_return Value(arr);
}

Task Interpreter::evaluateObject(const ObjectExpr& expr) {
  auto obj = std::make_shared<Object>();
  for (const auto& prop : expr.properties) {
    auto keyTask = evaluate(*prop.key);
    while (!keyTask.done()) {
      std::coroutine_handle<>::from_address(keyTask.handle.address()).resume();
    }
    std::string key = keyTask.result().toString();

    auto valTask = evaluate(*prop.value);
    while (!valTask.done()) {
      std::coroutine_handle<>::from_address(valTask.handle.address()).resume();
    }
    obj->properties[key] = valTask.result();
  }
  co_return Value(obj);
}

Task Interpreter::evaluateFunction(const FunctionExpr& expr) {
  auto func = std::make_shared<Function>();
  func->isNative = false;
  func->isAsync = expr.isAsync;

  for (const auto& param : expr.params) {
    func->params.push_back(param.name);
  }

  func->body = std::shared_ptr<void>(const_cast<std::vector<StmtPtr>*>(&expr.body), [](void*){});
  func->closure = env_;

  co_return Value(func);
}

Task Interpreter::evaluateAwait(const AwaitExpr& expr) {
  auto task = evaluate(*expr.argument);
  while (!task.done()) {
    std::coroutine_handle<>::from_address(task.handle.address()).resume();
  }
  Value val = task.result();

  if (val.isPromise()) {
    auto promise = std::get<std::shared_ptr<Promise>>(val.data);
    if (promise->state == PromiseState::Fulfilled) {
      co_return promise->result;
    } else if (promise->state == PromiseState::Rejected) {
      co_return promise->result;
    }
    co_return Value(Undefined{});
  }

  co_return val;
}

Task Interpreter::evaluateVarDecl(const VarDeclaration& decl) {
  for (const auto& declarator : decl.declarations) {
    Value value = Value(Undefined{});
    if (declarator.init) {
      auto task = evaluate(*declarator.init);
      while (!task.done()) {
        std::coroutine_handle<>::from_address(task.handle.address()).resume();
      }
      value = task.result();
    }
    env_->define(declarator.id.name, value, decl.kind == VarDeclaration::Kind::Const);
  }
  co_return Value(Undefined{});
}

Task Interpreter::evaluateFuncDecl(const FunctionDeclaration& decl) {
  auto func = std::make_shared<Function>();
  func->isNative = false;
  func->isAsync = decl.isAsync;

  for (const auto& param : decl.params) {
    func->params.push_back(param.name);
  }

  func->body = std::shared_ptr<void>(const_cast<std::vector<StmtPtr>*>(&decl.body), [](void*){});
  func->closure = env_;

  env_->define(decl.id.name, Value(func));
  co_return Value(Undefined{});
}

Task Interpreter::evaluateReturn(const ReturnStmt& stmt) {
  Value result = Value(Undefined{});
  if (stmt.argument) {
    auto task = evaluate(*stmt.argument);
    while (!task.done()) {
      std::coroutine_handle<>::from_address(task.handle.address()).resume();
    }
    result = task.result();
  }
  flow_.type = ControlFlow::Type::Return;
  flow_.value = result;
  co_return result;
}

Task Interpreter::evaluateExprStmt(const ExpressionStmt& stmt) {
  auto task = evaluate(*stmt.expression);
  while (!task.done()) {
    std::coroutine_handle<>::from_address(task.handle.address()).resume();
  }
  co_return task.result();
}

Task Interpreter::evaluateBlock(const BlockStmt& stmt) {
  auto prevEnv = env_;
  env_ = env_->createChild();

  Value result = Value(Undefined{});
  for (const auto& s : stmt.body) {
    auto task = evaluate(*s);
    while (!task.done()) {
      std::coroutine_handle<>::from_address(task.handle.address()).resume();
    }
    result = task.result();

    if (flow_.type != ControlFlow::Type::None) {
      break;
    }
  }

  env_ = prevEnv;
  co_return result;
}

Task Interpreter::evaluateIf(const IfStmt& stmt) {
  auto testTask = evaluate(*stmt.test);
  while (!testTask.done()) {
    std::coroutine_handle<>::from_address(testTask.handle.address()).resume();
  }

  if (testTask.result().toBool()) {
    auto consTask = evaluate(*stmt.consequent);
    while (!consTask.done()) {
      std::coroutine_handle<>::from_address(consTask.handle.address()).resume();
    }
    co_return consTask.result();
  } else if (stmt.alternate) {
    auto altTask = evaluate(*stmt.alternate);
    while (!altTask.done()) {
      std::coroutine_handle<>::from_address(altTask.handle.address()).resume();
    }
    co_return altTask.result();
  }

  co_return Value(Undefined{});
}

Task Interpreter::evaluateWhile(const WhileStmt& stmt) {
  Value result = Value(Undefined{});

  while (true) {
    auto testTask = evaluate(*stmt.test);
    while (!testTask.done()) {
      std::coroutine_handle<>::from_address(testTask.handle.address()).resume();
    }

    if (!testTask.result().toBool()) {
      break;
    }

    auto bodyTask = evaluate(*stmt.body);
    while (!bodyTask.done()) {
      std::coroutine_handle<>::from_address(bodyTask.handle.address()).resume();
    }
    result = bodyTask.result();

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

  co_return result;
}

Task Interpreter::evaluateFor(const ForStmt& stmt) {
  auto prevEnv = env_;
  env_ = env_->createChild();

  if (stmt.init) {
    auto initTask = evaluate(*stmt.init);
    while (!initTask.done()) {
      std::coroutine_handle<>::from_address(initTask.handle.address()).resume();
    }
  }

  Value result = Value(Undefined{});

  while (true) {
    if (stmt.test) {
      auto testTask = evaluate(*stmt.test);
      while (!testTask.done()) {
        std::coroutine_handle<>::from_address(testTask.handle.address()).resume();
      }
      if (!testTask.result().toBool()) {
        break;
      }
    }

    auto bodyTask = evaluate(*stmt.body);
    while (!bodyTask.done()) {
      std::coroutine_handle<>::from_address(bodyTask.handle.address()).resume();
    }
    result = bodyTask.result();

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
      while (!updateTask.done()) {
        std::coroutine_handle<>::from_address(updateTask.handle.address()).resume();
      }
    }
  }

  env_ = prevEnv;
  co_return result;
}

Task Interpreter::evaluateTry(const TryStmt& stmt) {
  auto prevFlow = flow_;
  Value result = Value(Undefined{});

  for (const auto& s : stmt.block) {
    auto task = evaluate(*s);
    while (!task.done()) {
      std::coroutine_handle<>::from_address(task.handle.address()).resume();
    }
    result = task.result();

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
        while (!catchTask.done()) {
          std::coroutine_handle<>::from_address(catchTask.handle.address()).resume();
        }
        result = catchTask.result();
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
      while (!finalTask.done()) {
        std::coroutine_handle<>::from_address(finalTask.handle.address()).resume();
      }
    }
  }

  co_return result;
}

}