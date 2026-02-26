#pragma once

#include "ast.h"
#include "value.h"
#include "environment.h"
#include "error_formatter.h"
#include "compat.h"
#include <optional>
#include <memory>
#include <iostream>
#include <exception>
#include <utility>

#if LIGHTJS_HAS_COROUTINES
#include <coroutine>
#endif

namespace lightjs {

#if LIGHTJS_HAS_COROUTINES

// ============================================================================
// C++20 Coroutine-based Task implementation
// ============================================================================

struct Task {
  struct promise_type {
    Value result = Value(Undefined{});
    std::exception_ptr exception;

    Task get_return_object() {
      return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    std::suspend_never initial_suspend() { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }

    void return_value(Value v) {
      result = v;
    }

    void unhandled_exception() {
      exception = std::current_exception();
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
      if (handle.promise().exception) {
        std::rethrow_exception(handle.promise().exception);
      }
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

// C++20 macros for coroutine operations
#define LIGHTJS_RETURN(val) co_return (val)
#define LIGHTJS_AWAIT(task) co_await (task)

// Run a task to completion and store result
#define LIGHTJS_RUN_TASK(taskvar, outvar) \
  do { \
    while (!(taskvar).done()) { \
      std::coroutine_handle<>::from_address((taskvar).handle.address()).resume(); \
    } \
    (outvar) = (taskvar).result(); \
  } while (0)

// Run a task to completion without storing result
#define LIGHTJS_RUN_TASK_VOID(task) \
  do { \
    while (!(task).done()) { \
      std::coroutine_handle<>::from_address((task).handle.address()).resume(); \
    } \
  } while (0)

#else

// ============================================================================
// C++17 Simple Task implementation (synchronous execution)
// ============================================================================

/**
 * C++17 Task - A simple Value wrapper for synchronous execution.
 *
 * LightJS uses synchronous execution with manual coroutine resumption.
 * All tasks run to completion immediately, so in C++17 mode we can
 * simply wrap the Value directly without actual coroutines.
 */
class Task {
  Value result_;
  bool done_ = true;

public:
  Task() : result_(Value(Undefined{})), done_(true) {}
  explicit Task(Value v) : result_(std::move(v)), done_(true) {}

  // Move semantics
  Task(Task&& other) noexcept : result_(std::move(other.result_)), done_(other.done_) {
    other.done_ = true;
  }

  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      result_ = std::move(other.result_);
      done_ = other.done_;
      other.done_ = true;
    }
    return *this;
  }

  // No copy
  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  Value result() const { return result_; }
  bool done() const { return done_; }
};

// Helper to create a completed Task
inline Task makeTask(Value v) {
  return Task(std::move(v));
}

// Helper to extract value from a Task (no-op in C++17 mode since tasks are always done)
inline Value runTask(Task&& t) {
  return t.result();
}

// C++17 macros - direct returns since execution is synchronous
#define LIGHTJS_RETURN(val) return makeTask(val)
#define LIGHTJS_AWAIT(task) runTask(task)

// In C++17 mode, tasks are always immediately done
#define LIGHTJS_RUN_TASK(taskvar, outvar) (outvar) = (taskvar).result()
#define LIGHTJS_RUN_TASK_VOID(taskvar) ((void)(taskvar))

#endif // LIGHTJS_HAS_COROUTINES

class Interpreter {
  friend class Module;
public:
  explicit Interpreter(std::shared_ptr<Environment> env);

  Task evaluate(const Expression& expr);
  Task evaluate(const Statement& stmt);
  Task evaluate(const Program& program);

  // Environment management for modules
  std::shared_ptr<Environment> getEnvironment() const { return env_; }
  void setEnvironment(std::shared_ptr<Environment> env) { env_ = env; }
  void setSuppressMicrotasks(bool value) { suppressMicrotasks_ = value; }
  bool suppressMicrotasks() const { return suppressMicrotasks_; }
  bool inDirectEvalInvocation() const { return activeDirectEvalInvocation_; }
  bool isStrictMode() const { return strictMode_; }
  void setStrictMode(bool strict) { strictMode_ = strict; }
  void setSourceKeepAlive(std::shared_ptr<void> keep) { sourceKeepAlive_ = std::move(keep); }

  // Stack depth limit for recursion protection
  // Keep this well below what would cause a C stack overflow (~3000-4000 on typical systems)
#if defined(__SANITIZE_ADDRESS__)
  static constexpr size_t MAX_STACK_DEPTH = 256;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
  static constexpr size_t MAX_STACK_DEPTH = 256;
#else
  static constexpr size_t MAX_STACK_DEPTH = 2000;
#endif
#else
  static constexpr size_t MAX_STACK_DEPTH = 2000;
#endif

  // Check if there's a pending thrown error
  bool hasError() const;
  Value getError() const;
  void clearError();
  Value callForHarness(const Value& callee,
                       const std::vector<Value>& args,
                       const Value& thisValue = Value(Undefined{}));
  Value constructFromNative(const Value& constructor,
                            const std::vector<Value>& args);

private:
  std::shared_ptr<Environment> env_;
  size_t stackDepth_ = 0;
  StackTraceManager stackTrace_;
  bool suppressMicrotasks_ = false;

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
    enum class ResumeMode { None, Next, Return, Throw };

    Type type = Type::None;
    Value value;
    std::string label;
    ResumeMode resumeMode = ResumeMode::None;
    Value resumeValue = Value(Undefined{});
    // Completion value from try/finally override for break/continue.
    // Set when finally block produces break/continue to carry the UpdateEmpty'd value.
    std::optional<Value> breakCompletionValue;

    void reset() {
      type = Type::None;
      value = Value(Undefined{});
      label.clear();
      resumeMode = ResumeMode::None;
      resumeValue = Value(Undefined{});
      breakCompletionValue = std::nullopt;
    }

    void setYield(const Value& v) {
      type = Type::Yield;
      value = v;
    }

    void prepareResume(ResumeMode mode, const Value& v) {
      resumeMode = mode;
      resumeValue = v;
    }

    ResumeMode takeResumeMode() {
      auto mode = resumeMode;
      resumeMode = ResumeMode::None;
      return mode;
    }

    Value takeResumeValue() {
      Value v = resumeValue;
      resumeValue = Value(Undefined{});
      return v;
    }
  };

  ControlFlow flow_;
  Value lastMemberBase_ = Value(Undefined{});
  bool hasLastMemberBase_ = false;
  bool strictMode_ = false;
  bool varDeclBypassWith_ = false;  // Set during var declaration eval to bypass with scopes
  bool inTailPosition_ = false;
  std::shared_ptr<Function> activeFunction_ = nullptr;
  bool pendingSelfTailCall_ = false;
  std::vector<Value> pendingSelfTailArgs_;
  Value pendingSelfTailThis_ = Value(Undefined{});
  bool pendingDirectEvalCall_ = false;
  bool activeDirectEvalInvocation_ = false;
  std::shared_ptr<void> sourceKeepAlive_;  // Keeps eval AST alive for function bodies
  std::vector<std::shared_ptr<Function>> activeNamedExpressionStack_;
  std::string pendingIterationLabel_;  // Label for next iteration statement (consumed once)

  struct IteratorRecord {
    enum class Kind { Generator, Array, String, IteratorObject, TypedArray };
    Kind kind = Kind::Array;
    std::shared_ptr<Generator> generator;
    std::shared_ptr<Array> array;
    std::shared_ptr<Object> iteratorObject;
    std::shared_ptr<TypedArray> typedArray;
    std::string stringValue;
    size_t index = 0;
    Value nextMethod;  // Cached next() method per GetIterator spec (7.4.1)
  };

  static Value makeIteratorResult(const Value& value, bool done);
  static Value createIteratorFactory(const std::shared_ptr<Array>& arrPtr);
  Value runGeneratorNext(const std::shared_ptr<Generator>& generator,
                         ControlFlow::ResumeMode mode = ControlFlow::ResumeMode::None,
                         const Value& resumeValue = Value(Undefined{}));
  std::optional<IteratorRecord> getIterator(const Value& iterable);
  Value iteratorNext(IteratorRecord& record);
  void iteratorClose(IteratorRecord& record);
  Value callFunction(const Value& callee, const std::vector<Value>& args, const Value& thisValue = Value(Undefined{}));
  bool isObjectLike(const Value& value) const;
  std::pair<bool, Value> getPropertyForPrimitive(const Value& receiver, const std::string& key);
  Value toPrimitiveValue(const Value& input, bool preferString);

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
  Task constructValue(Value callee, const std::vector<Value>& args,
                      const Value& newTargetOverride = Value(Undefined{}));
  Task evaluateNew(const NewExpr& expr);
  Task evaluateClass(const ClassExpr& expr);

  Task evaluateVarDecl(const VarDeclaration& decl);
  Task evaluateFuncDecl(const FunctionDeclaration& decl);
  void hoistVarDeclarations(const std::vector<StmtPtr>& body);
  void hoistVarDeclarationsFromStmt(const Statement& stmt);
  Task evaluateReturn(const ReturnStmt& stmt);
  Task evaluateExprStmt(const ExpressionStmt& stmt);
  Task evaluateBlock(const BlockStmt& stmt);
  Task evaluateIf(const IfStmt& stmt);
  Task evaluateWhile(const WhileStmt& stmt);
  Task evaluateWith(const WithStmt& stmt);
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
  void bindDestructuringPattern(const Expression& pattern, const Value& value, bool isConst, bool useSet = false);

  // Helper to invoke a JavaScript function (used by native functions to call JS callbacks)
  Value invokeFunction(std::shared_ptr<Function> func, const std::vector<Value>& args, const Value& thisValue = Value(Undefined{}));

  // Helper to format error message with line number
  static std::string formatError(const std::string& msg, const SourceLocation& loc) {
    if (loc.line > 0) {
      return msg + " at line " + std::to_string(loc.line) + ", column " + std::to_string(loc.column);
    }
    return msg;
  }

  // Check heap memory limit and throw error if exceeded
  // Returns true if allocation is safe, sets error and returns false otherwise
  bool checkMemoryLimit(size_t additionalBytes = 0);

  // Helper to throw error with stack trace
  void throwError(ErrorType type, const std::string& message) {
    auto error = std::make_shared<Error>(type, message);
    // Format stack trace using ErrorFormatter
    error->stack = ErrorFormatter::formatError(
      error->getName(),
      message,
      stackTrace_.getStackTrace()
    );
    flow_.type = ControlFlow::Type::Throw;
    flow_.value = Value(error);
  }

  // Helper to push stack frame (RAII)
  StackFrameGuard pushStackFrame(const std::string& functionName,
                                  const std::string& filename = "<script>",
                                  uint32_t line = 0,
                                  uint32_t column = 0) {
    StackFrame frame;
    frame.functionName = functionName;
    frame.filename = filename;
    frame.line = line;
    frame.column = column;
    return StackFrameGuard(stackTrace_, frame);
  }
};

}
