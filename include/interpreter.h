#pragma once

#include "ast.h"
#include "value.h"
#include "environment.h"
#include <coroutine>
#include <optional>
#include <memory>

namespace tinyjs {

struct Task {
  struct promise_type {
    Value result = Value(Undefined{});

    Task get_return_object() {
      return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    std::suspend_never initial_suspend() { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }

    void return_value(Value v) {
      result = v;
    }

    void unhandled_exception() {
      std::terminate();
    }
  };

  std::coroutine_handle<promise_type> handle;

  Task(std::coroutine_handle<promise_type> h) : handle(h) {}

  ~Task() {
    if (handle) handle.destroy();
  }

  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  Task(Task&& other) noexcept : handle(other.handle) {
    other.handle = nullptr;
  }

  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      if (handle) handle.destroy();
      handle = other.handle;
      other.handle = nullptr;
    }
    return *this;
  }

  Value result() {
    if (handle && handle.done()) {
      return handle.promise().result;
    }
    return Value(Undefined{});
  }

  bool done() const {
    return handle && handle.done();
  }

  bool await_ready() const {
    return handle.done();
  }

  void await_suspend(std::coroutine_handle<> awaiting) {
    while (!handle.done()) {
      handle.resume();
    }
    awaiting.resume();
  }

  Value await_resume() {
    return result();
  }
};

class Interpreter {
public:
  explicit Interpreter(std::shared_ptr<Environment> env);

  Task evaluate(const Expression& expr);
  Task evaluate(const Statement& stmt);
  Task evaluate(const Program& program);

private:
  std::shared_ptr<Environment> env_;

  struct ControlFlow {
    enum class Type { None, Return, Break, Continue, Throw };
    Type type = Type::None;
    Value value;
  };

  ControlFlow flow_;

  Task evaluateBinary(const BinaryExpr& expr);
  Task evaluateUnary(const UnaryExpr& expr);
  Task evaluateAssignment(const AssignmentExpr& expr);
  Task evaluateUpdate(const UpdateExpr& expr);
  Task evaluateCall(const CallExpr& expr);
  Task evaluateMember(const MemberExpr& expr);
  Task evaluateConditional(const ConditionalExpr& expr);
  Task evaluateArray(const ArrayExpr& expr);
  Task evaluateObject(const ObjectExpr& expr);
  Task evaluateFunction(const FunctionExpr& expr);
  Task evaluateAwait(const AwaitExpr& expr);

  Task evaluateVarDecl(const VarDeclaration& decl);
  Task evaluateFuncDecl(const FunctionDeclaration& decl);
  Task evaluateReturn(const ReturnStmt& stmt);
  Task evaluateExprStmt(const ExpressionStmt& stmt);
  Task evaluateBlock(const BlockStmt& stmt);
  Task evaluateIf(const IfStmt& stmt);
  Task evaluateWhile(const WhileStmt& stmt);
  Task evaluateFor(const ForStmt& stmt);
  Task evaluateTry(const TryStmt& stmt);
};

}