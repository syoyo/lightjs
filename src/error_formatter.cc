#include "error_formatter.h"
#include <sstream>
#include <algorithm>
#include <iomanip>

namespace lightjs {

// StackFrame implementation
std::string StackFrame::toString() const {
  std::ostringstream oss;
  oss << "  at ";

  if (!functionName.empty() && functionName != "<anonymous>") {
    oss << functionName << " ";
  }

  oss << "(" << filename << ":" << line << ":" << column << ")";
  return oss.str();
}

// SourceContext implementation
SourceContext::SourceContext(std::string file, std::string source)
  : filename(std::move(file)) {

  // Split source into lines
  std::istringstream stream(source);
  std::string line;
  while (std::getline(stream, line)) {
    lines.push_back(line);
  }
}

std::string SourceContext::getLine(uint32_t lineNum) const {
  if (lineNum == 0 || lineNum > lines.size()) {
    return "";
  }
  return lines[lineNum - 1];
}

std::vector<std::string> SourceContext::getContext(uint32_t lineNum, uint32_t contextLines) const {
  std::vector<std::string> result;

  if (lineNum == 0 || lineNum > lines.size()) {
    return result;
  }

  // Calculate range
  uint32_t start = lineNum > contextLines ? lineNum - contextLines : 1;
  uint32_t end = std::min(static_cast<uint32_t>(lines.size()), lineNum + contextLines);

  for (uint32_t i = start; i <= end; i++) {
    result.push_back(lines[i - 1]);
  }

  return result;
}

// ErrorFormatter implementation
size_t ErrorFormatter::getLineNumberWidth(uint32_t maxLine) {
  if (maxLine == 0) return 1;
  size_t width = 0;
  while (maxLine > 0) {
    width++;
    maxLine /= 10;
  }
  return width;
}

std::string ErrorFormatter::formatLineNumber(uint32_t lineNum, size_t width) {
  std::ostringstream oss;
  oss << std::setw(width) << std::right << lineNum;
  return oss.str();
}

std::string ErrorFormatter::createColumnMarker(uint32_t column, uint32_t length) {
  if (column == 0) return "";

  std::string marker;
  // Add spaces before the marker
  for (uint32_t i = 1; i < column; i++) {
    marker += ' ';
  }
  // Add marker characters
  for (uint32_t i = 0; i < length; i++) {
    marker += '^';
  }
  return marker;
}

std::string ErrorFormatter::formatSourceContext(
  const SourceContext& context,
  uint32_t errorLine,
  uint32_t errorColumn,
  uint32_t contextLines
) {
  std::ostringstream oss;

  if (errorLine == 0 || errorLine > context.lines.size()) {
    return "";
  }

  // Get context lines
  uint32_t start = errorLine > contextLines ? errorLine - contextLines : 1;
  uint32_t end = std::min(static_cast<uint32_t>(context.lines.size()), errorLine + contextLines);

  // Calculate width for line numbers
  size_t lineWidth = getLineNumberWidth(end);

  oss << "\n";

  for (uint32_t i = start; i <= end; i++) {
    std::string lineNumStr = formatLineNumber(i, lineWidth);

    // Add marker for error line
    if (i == errorLine) {
      oss << "> " << lineNumStr << " | " << context.getLine(i) << "\n";

      // Add column marker
      if (errorColumn > 0) {
        std::string padding(lineWidth + 4, ' ');  // Space for "> XX | "
        oss << padding << createColumnMarker(errorColumn) << "\n";
      }
    } else {
      oss << "  " << lineNumStr << " | " << context.getLine(i) << "\n";
    }
  }

  return oss.str();
}

std::string ErrorFormatter::formatError(
  const std::string& errorType,
  const std::string& message,
  const std::vector<StackFrame>& stackTrace,
  const SourceContext* context,
  uint32_t errorLine,
  uint32_t errorColumn
) {
  std::ostringstream oss;

  // Error header: "ErrorType: message"
  oss << errorType << ": " << message << "\n";

  // Stack trace
  for (const auto& frame : stackTrace) {
    oss << frame.toString() << "\n";
  }

  // Source context (if available)
  if (context && errorLine > 0) {
    oss << formatSourceContext(*context, errorLine, errorColumn);
  }

  return oss.str();
}

// StackTraceManager implementation
void StackTraceManager::pushFrame(const StackFrame& frame) {
  frames_.push_back(frame);
}

void StackTraceManager::pushFrame(std::string functionName, std::string filename,
                                  uint32_t line, uint32_t column) {
  frames_.emplace_back(std::move(functionName), std::move(filename), line, column);
}

void StackTraceManager::popFrame() {
  if (!frames_.empty()) {
    frames_.pop_back();
  }
}

} // namespace lightjs
