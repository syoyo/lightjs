#pragma once

#include "ast.h"
#include "value.h"
#include "environment.h"
#include <coroutine>
#include <optional>
#include <memory>

namespace lightjs {

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

  bool await_ready() const noexcept {
    return handle.done();
  }

  void await_suspend(std::coroutine_handle<> awaiting) noexcept {
    // Run this task to completion, then resume the awaiting coroutine
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

  // Environment management for modules
  std::shared_ptr<Environment> getEnvironment() const { return env_; }
  void setEnvironment(std::shared_ptr<Environment> env) { env_ = env; }

  // Stack depth limit for recursion protection
  // Keep this well below what would cause a C stack overflow (~3000-4000 on typical systems)
  static constexpr size_t MAX_STACK_DEPTH = 2000;

  // Check if there's a pending thrown error
  bool hasError() const;
  Value getError() const;
  void clearError();

private:
  std::shared_ptr<Environment> env_;
  size_t stackDepth_ = 0;

  // RAII helper for stack depth tracking
  struct StackGuard {
    size_t& depth_;
    bool overflowed_ = false;

    StackGuard(size_t& depth, size_t maxDepth) : depth_(depth) {
      if (++depth_ > maxDepth) {
        overflowed_ = true;
      }
    }
    ~StackGuard() { --depth_; }
    bool overflowed() const { return overflowed_; }
  };

  struct ControlFlow {
    enum class Type { None, Return, Break, Continue, Throw, Yield };
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
  Task evaluateYield(const YieldExpr& expr);
  Task evaluateNew(const NewExpr& expr);
  Task evaluateClass(const ClassExpr& expr);

  Task evaluateVarDecl(const VarDeclaration& decl);
  Task evaluateFuncDecl(const FunctionDeclaration& decl);
  Task evaluateReturn(const ReturnStmt& stmt);
  Task evaluateExprStmt(const ExpressionStmt& stmt);
  Task evaluateBlock(const BlockStmt& stmt);
  Task evaluateIf(const IfStmt& stmt);
  Task evaluateWhile(const WhileStmt& stmt);
  Task evaluateDoWhile(const DoWhileStmt& stmt);
  Task evaluateFor(const ForStmt& stmt);
  Task evaluateForIn(const ForInStmt& stmt);
  Task evaluateForOf(const ForOfStmt& stmt);
  Task evaluateSwitch(const SwitchStmt& stmt);
  Task evaluateTry(const TryStmt& stmt);
  Task evaluateImport(const ImportDeclaration& stmt);
  Task evaluateExportNamed(const ExportNamedDeclaration& stmt);
  Task evaluateExportDefault(const ExportDefaultDeclaration& stmt);
  Task evaluateExportAll(const ExportAllDeclaration& stmt);

  // Helper for destructuring bindings
  void bindDestructuringPattern(const Expression& pattern, const Value& value, bool isConst);

  // Helper to invoke a JavaScript function (used by native functions to call JS callbacks)
  Value invokeFunction(std::shared_ptr<Function> func, const std::vector<Value>& args, const Value& thisValue = Value(Undefined{}));

  // Helper to format error message with line number
  static std::string formatError(const std::string& msg, const SourceLocation& loc) {
    if (loc.line > 0) {
      return msg + " at line " + std::to_string(loc.line) + ", column " + std::to_string(loc.column);
    }
    return msg;
  }
};

}