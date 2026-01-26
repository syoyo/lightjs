#pragma once

#include "ast.h"
#include "value.h"
#include <string>
#include <vector>
#include <memory>

namespace lightjs {

/**
 * Stack frame for error reporting
 */
struct StackFrame {
  std::string functionName;     // Function name or "<anonymous>"
  std::string filename;         // Source filename or "<eval>"
  uint32_t line = 0;           // Line number (1-indexed)
  uint32_t column = 0;         // Column number (1-indexed)

  StackFrame() = default;
  StackFrame(std::string fn, std::string file, uint32_t l, uint32_t c)
    : functionName(std::move(fn)), filename(std::move(file)), line(l), column(c) {}

  // Format as "at functionName (filename:line:column)"
  std::string toString() const;
};

/**
 * Source code context for error display
 */
struct SourceContext {
  std::string filename;
  std::vector<std::string> lines;  // All source lines

  SourceContext() = default;
  SourceContext(std::string file, std::string source);

  // Get specific line (1-indexed), returns empty string if out of bounds
  std::string getLine(uint32_t lineNum) const;

  // Get context around a line (before and after)
  std::vector<std::string> getContext(uint32_t lineNum, uint32_t contextLines = 2) const;
};

/**
 * Error formatter for JavaScript-quality error messages
 */
class ErrorFormatter {
public:
  /**
   * Format an error with stack trace and source context
   *
   * Example output:
   * ```
   * ReferenceError: foo is not defined
   *   at myFunction (script.js:15:5)
   *   at <module> (script.js:20:1)
   *
   *   13 | function myFunction() {
   *   14 |   let x = 10;
   * > 15 |   return foo + x;
   *      |          ^^^
   *   16 | }
   * ```
   */
  static std::string formatError(
    const std::string& errorType,      // e.g., "ReferenceError", "TypeError"
    const std::string& message,        // Error message
    const std::vector<StackFrame>& stackTrace,
    const SourceContext* context = nullptr,  // Optional source context
    uint32_t errorLine = 0,           // Line where error occurred
    uint32_t errorColumn = 0          // Column where error occurred
  );

  /**
   * Format source context with line numbers and error marker
   */
  static std::string formatSourceContext(
    const SourceContext& context,
    uint32_t errorLine,
    uint32_t errorColumn,
    uint32_t contextLines = 2
  );

  /**
   * Create column marker (^^^) under error location
   */
  static std::string createColumnMarker(uint32_t column, uint32_t length = 3);

private:
  // Get line number width for formatting
  static size_t getLineNumberWidth(uint32_t maxLine);

  // Pad line number for alignment
  static std::string formatLineNumber(uint32_t lineNum, size_t width);
};

/**
 * Stack trace manager for the interpreter
 */
class StackTraceManager {
public:
  StackTraceManager() = default;

  // Push a new stack frame
  void pushFrame(const StackFrame& frame);
  void pushFrame(std::string functionName, std::string filename, uint32_t line, uint32_t column);

  // Pop the top stack frame
  void popFrame();

  // Get the current stack trace
  const std::vector<StackFrame>& getStackTrace() const { return frames_; }

  // Clear the stack trace
  void clear() { frames_.clear(); }

  // Get current depth
  size_t depth() const { return frames_.size(); }

  // Get top frame (current function)
  const StackFrame* top() const {
    return frames_.empty() ? nullptr : &frames_.back();
  }

private:
  std::vector<StackFrame> frames_;
};

/**
 * RAII helper for automatic stack frame management
 */
class StackFrameGuard {
public:
  StackFrameGuard(StackTraceManager& manager, const StackFrame& frame)
    : manager_(manager) {
    manager_.pushFrame(frame);
  }

  StackFrameGuard(StackTraceManager& manager, std::string functionName,
                  std::string filename, uint32_t line, uint32_t column)
    : manager_(manager) {
    manager_.pushFrame(std::move(functionName), std::move(filename), line, column);
  }

  ~StackFrameGuard() {
    manager_.popFrame();
  }

  // Non-copyable
  StackFrameGuard(const StackFrameGuard&) = delete;
  StackFrameGuard& operator=(const StackFrameGuard&) = delete;

private:
  StackTraceManager& manager_;
};

} // namespace lightjs
