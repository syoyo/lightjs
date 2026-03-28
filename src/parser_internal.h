#pragma once

#include "parser.h"
#include "lexer.h"
#include <stdexcept>
#include <cctype>
#include <cstdlib>
#include <cerrno>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <iostream>

namespace lightjs {

inline std::vector<std::set<std::string>>& privateNameScopeStack() {
  static std::vector<std::set<std::string>> stack;
  return stack;
}

namespace {

int digitValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
  return -1;
}

bool parseBigIntLiteral64(const std::string& raw, bigint::BigIntValue& out) {
  return bigint::parseBigIntLiteral(raw, out);
}

bool parseNumberLiteral(const std::string& raw, bool strictMode, double& out) {
  if (raw.empty()) return false;

  std::string normalized;
  normalized.reserve(raw.size());
  for (char c : raw) {
    if (c != '_') normalized.push_back(c);
  }
  if (normalized.empty()) return false;

  if (normalized.size() >= 2 && normalized[0] == '0' &&
      (normalized[1] == 'x' || normalized[1] == 'X' ||
       normalized[1] == 'o' || normalized[1] == 'O' ||
       normalized[1] == 'b' || normalized[1] == 'B')) {
    int base = 10;
    if (normalized[1] == 'x' || normalized[1] == 'X') base = 16;
    if (normalized[1] == 'o' || normalized[1] == 'O') base = 8;
    if (normalized[1] == 'b' || normalized[1] == 'B') base = 2;

    uint64_t value = 0;
    bool sawDigit = false;
    for (size_t i = 2; i < normalized.size(); ++i) {
      int d = digitValue(normalized[i]);
      if (d < 0 || d >= base) return false;
      sawDigit = true;
      value = value * static_cast<uint64_t>(base) + static_cast<uint64_t>(d);
    }
    if (!sawDigit) return false;
    out = static_cast<double>(value);
    return true;
  }

  auto isLegacyLeadingZeroInteger = [&](const std::string& literal) -> bool {
    if (literal.size() <= 1 || literal[0] != '0') return false;
    if (literal[1] == 'x' || literal[1] == 'X' ||
        literal[1] == 'o' || literal[1] == 'O' ||
        literal[1] == 'b' || literal[1] == 'B') {
      return false;
    }
    if (literal.find('.') != std::string::npos ||
        literal.find('e') != std::string::npos ||
        literal.find('E') != std::string::npos) {
      return false;
    }
    return std::all_of(literal.begin() + 1, literal.end(), [](unsigned char c) {
      return std::isdigit(c);
    });
  };

  if (isLegacyLeadingZeroInteger(normalized)) {
    if (strictMode) {
      return false;
    }
    bool allOctalDigits = std::all_of(normalized.begin() + 1, normalized.end(), [](char c) {
      return c >= '0' && c <= '7';
    });
    if (allOctalDigits) {
      uint64_t value = 0;
      for (size_t i = 1; i < normalized.size(); ++i) {
        value = value * 8 + static_cast<uint64_t>(normalized[i] - '0');
      }
      out = static_cast<double>(value);
      return true;
    }
  }

  // Use strtod to accept subnormals (e.g. 4.94e-324) without throwing.
  errno = 0;
  char* end = nullptr;
  out = std::strtod(normalized.c_str(), &end);
  if (!end || *end != '\0') {
    return false;
  }
  return true;
}

bool isWellFormedUnicodeString(const std::string& value) {
  size_t i = 0;
  while (i < value.size()) {
    unsigned char lead = static_cast<unsigned char>(value[i]);
    uint32_t codePoint = 0;
    size_t trailCount = 0;

    if (lead <= 0x7F) {
      codePoint = lead;
      trailCount = 0;
    } else if ((lead & 0xE0) == 0xC0) {
      codePoint = lead & 0x1F;
      trailCount = 1;
    } else if ((lead & 0xF0) == 0xE0) {
      codePoint = lead & 0x0F;
      trailCount = 2;
    } else if ((lead & 0xF8) == 0xF0) {
      codePoint = lead & 0x07;
      trailCount = 3;
    } else {
      return false;
    }

    if (i + trailCount >= value.size()) {
      return false;
    }

    for (size_t j = 1; j <= trailCount; ++j) {
      unsigned char trail = static_cast<unsigned char>(value[i + j]);
      if ((trail & 0xC0) != 0x80) {
        return false;
      }
      codePoint = (codePoint << 6) | static_cast<uint32_t>(trail & 0x3F);
    }

    if (trailCount == 1 && codePoint < 0x80) {
      return false;
    }
    if (trailCount == 2 && codePoint < 0x800) {
      return false;
    }
    if (trailCount == 3 && codePoint < 0x10000) {
      return false;
    }
    if (codePoint > 0x10FFFF) {
      return false;
    }
    if (codePoint >= 0xD800 && codePoint <= 0xDFFF) {
      return false;
    }

    i += trailCount + 1;
  }
  return true;
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
  auto expPos = out.find_first_of("eE");
  if (expPos != std::string::npos) {
    out[expPos] = 'e';
    size_t expStart = expPos + 1;
    if (expStart < out.size() && (out[expStart] == '+' || out[expStart] == '-')) {
      ++expStart;
    }
    size_t firstNonZero = expStart;
    while (firstNonZero + 1 < out.size() && out[firstNonZero] == '0') {
      ++firstNonZero;
    }
    if (firstNonZero > expStart) {
      out.erase(expStart, firstNonZero - expStart);
    }
    return out;
  }
  auto dot = out.find('.');
  if (dot != std::string::npos) {
    while (!out.empty() && out.back() == '0') out.pop_back();
    if (!out.empty() && out.back() == '.') out.pop_back();
  }
  return out;
}

bool isIdentifierNameToken(TokenType type) {
  switch (type) {
    case TokenType::Identifier:
    case TokenType::True:
    case TokenType::False:
    case TokenType::Null:
    case TokenType::Undefined:
    case TokenType::Let:
    case TokenType::Const:
    case TokenType::Var:
    case TokenType::Function:
    case TokenType::Async:
    case TokenType::Await:
    case TokenType::Yield:
    case TokenType::Return:
    case TokenType::If:
    case TokenType::Else:
    case TokenType::While:
    case TokenType::For:
    case TokenType::With:
    case TokenType::In:
    case TokenType::Instanceof:
    case TokenType::Of:
    case TokenType::Do:
    case TokenType::Switch:
    case TokenType::Case:
    case TokenType::Break:
    case TokenType::Continue:
    case TokenType::Debugger:
    case TokenType::Try:
    case TokenType::Catch:
    case TokenType::Finally:
    case TokenType::Throw:
    case TokenType::New:
    case TokenType::This:
    case TokenType::Typeof:
    case TokenType::Void:
    case TokenType::Delete:
    case TokenType::Import:
    case TokenType::Export:
    case TokenType::From:
    case TokenType::As:
    case TokenType::Default:
    case TokenType::Class:
    case TokenType::Extends:
    case TokenType::Static:
    case TokenType::Super:
    case TokenType::Get:
    case TokenType::Set:
    case TokenType::Enum:
      return true;
    default:
      return false;
  }
}

bool isStrictModeRestrictedIdentifier(const std::string& name) {
  if (name == "eval" || name == "arguments") return true;
  if (name == "implements" || name == "interface" || name == "package") return true;
  if (name == "private" || name == "protected" || name == "public") return true;
  if (name == "static" || name == "yield") return true;
  return false;
}

bool isStrictFutureReservedIdentifier(const std::string& name) {
  if (name == "implements" || name == "interface" || name == "package") return true;
  if (name == "private" || name == "protected" || name == "public") return true;
  if (name == "static" || name == "yield" || name == "let") return true;
  return false;
}

bool isAlwaysReservedIdentifierWord(const std::string& name) {
  return name == "break" || name == "case" || name == "catch" || name == "class" ||
         name == "const" || name == "continue" || name == "debugger" ||
         name == "default" || name == "delete" || name == "do" || name == "else" ||
         name == "export" || name == "extends" || name == "finally" || name == "for" ||
         name == "function" || name == "if" || name == "import" || name == "in" ||
         name == "instanceof" || name == "new" || name == "return" || name == "super" ||
         name == "switch" || name == "this" || name == "throw" || name == "try" ||
         name == "typeof" || name == "var" || name == "void" || name == "while" ||
         name == "with" || name == "enum";
}

bool isUpdateTarget(const Expression& expr) {
  if (std::holds_alternative<Identifier>(expr.node)) {
    return true;
  }
  if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
    return !member->optional;
  }
  return false;
}

// Collect all bound names from a destructuring pattern for duplicate checking
void collectBoundNames(const Expression& expr, std::vector<std::string>& names) {
  if (auto* id = std::get_if<Identifier>(&expr.node)) {
    names.push_back(id->name);
  } else if (auto* arrPat = std::get_if<ArrayPattern>(&expr.node)) {
    for (auto& elem : arrPat->elements) {
      if (elem) collectBoundNames(*elem, names);
    }
    if (arrPat->rest) collectBoundNames(*arrPat->rest, names);
  } else if (auto* objPat = std::get_if<ObjectPattern>(&expr.node)) {
    for (auto& prop : objPat->properties) {
      if (prop.value) collectBoundNames(*prop.value, names);
    }
    if (objPat->rest) collectBoundNames(*objPat->rest, names);
  } else if (auto* assign = std::get_if<AssignmentPattern>(&expr.node)) {
    if (assign->left) collectBoundNames(*assign->left, names);
  } else if (auto* spread = std::get_if<SpreadElement>(&expr.node)) {
    if (spread->argument) collectBoundNames(*spread->argument, names);
  }
}

// Collect var-declared names from a statement tree (for redeclaration checks)
void collectVarDeclaredNames(const Statement& stmt,
                             std::vector<std::string>& names,
                             bool includeFunctionDeclarations) {
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.node)) {
    if (varDecl->kind == VarDeclaration::Kind::Var) {
      for (auto& decl : varDecl->declarations) {
        if (decl.pattern) collectBoundNames(*decl.pattern, names);
      }
    }
  } else if (auto* fnDecl = std::get_if<FunctionDeclaration>(&stmt.node)) {
    if (includeFunctionDeclarations) {
      names.push_back(fnDecl->id.name);
    }
  } else if (auto* block = std::get_if<BlockStmt>(&stmt.node)) {
    for (auto& s : block->body) {
      if (s) collectVarDeclaredNames(*s, names, includeFunctionDeclarations);
    }
  } else if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    if (ifStmt->consequent) collectVarDeclaredNames(*ifStmt->consequent, names, includeFunctionDeclarations);
    if (ifStmt->alternate) collectVarDeclaredNames(*ifStmt->alternate, names, includeFunctionDeclarations);
  } else if (auto* forStmt = std::get_if<ForStmt>(&stmt.node)) {
    if (forStmt->init) collectVarDeclaredNames(*forStmt->init, names, includeFunctionDeclarations);
    if (forStmt->body) collectVarDeclaredNames(*forStmt->body, names, includeFunctionDeclarations);
  } else if (auto* whileStmt = std::get_if<WhileStmt>(&stmt.node)) {
    if (whileStmt->body) collectVarDeclaredNames(*whileStmt->body, names, includeFunctionDeclarations);
  } else if (auto* labeled = std::get_if<LabelledStmt>(&stmt.node)) {
    if (labeled->body) collectVarDeclaredNames(*labeled->body, names, includeFunctionDeclarations);
  } else if (auto* tryStmt = std::get_if<TryStmt>(&stmt.node)) {
    for (auto& s : tryStmt->block) {
      if (s) collectVarDeclaredNames(*s, names, includeFunctionDeclarations);
    }
    for (auto& s : tryStmt->handler.body) {
      if (s) collectVarDeclaredNames(*s, names, includeFunctionDeclarations);
    }
    for (auto& s : tryStmt->finalizer) {
      if (s) collectVarDeclaredNames(*s, names, includeFunctionDeclarations);
    }
  } else if (auto* switchStmt = std::get_if<SwitchStmt>(&stmt.node)) {
    for (auto& c : switchStmt->cases) {
      for (auto& s : c.consequent) {
        if (s) collectVarDeclaredNames(*s, names, includeFunctionDeclarations);
      }
    }
  }
}

bool hasDuplicateBoundNames(const Expression& pattern) {
  std::vector<std::string> names;
  collectBoundNames(pattern, names);
  std::set<std::string> seen;
  for (auto& name : names) {
    if (seen.count(name)) return true;
    seen.insert(name);
  }
  return false;
}

bool hasUseStrictDirectiveInBody(const std::vector<StmtPtr>& body, bool* hasLegacyEscapeBeforeStrict = nullptr) {
  bool sawLegacy = false;
  for (const auto& stmt : body) {
    if (!stmt) break;
    auto* exprStmt = std::get_if<ExpressionStmt>(&stmt->node);
    if (!exprStmt || !exprStmt->expression) {
      break;
    }
    if (auto* str = std::get_if<StringLiteral>(&exprStmt->expression->node)) {
      if (str->value == "use strict" && !str->hasEscape) {
        if (hasLegacyEscapeBeforeStrict) *hasLegacyEscapeBeforeStrict = sawLegacy;
        return true;
      }
      if (str->hasLegacyEscape) sawLegacy = true;
      continue;
    }
    break;
  }
  return false;
}

bool expressionContainsSuper(const Expression& expr);
bool statementContainsSuper(const Statement& stmt);
bool expressionContainsYield(const Expression& expr);
bool expressionContainsSuperCall(const Expression& expr);
bool expressionContainsAwaitLike(const Expression& expr);
bool expressionContainsStrictRestrictedWrite(const Expression& expr);
bool statementContainsStrictRestrictedWrite(const Statement& stmt);
bool expressionContainsStrictRestrictedIdentifierReference(const Expression& expr);
bool statementContainsStrictRestrictedIdentifierReference(const Statement& stmt);
bool fieldInitContainsSuperCall(const Expression& expr);
bool fieldInitStmtContainsSuperCall(const Statement& stmt);
bool fieldInitContainsArguments(const Expression& expr);
bool fieldInitStmtContainsArguments(const Statement& stmt);

bool expressionContainsSuper(const Expression& expr) {
  if (std::holds_alternative<SuperExpr>(expr.node)) {
    return true;
  }
  if (auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
    return (binary->left && expressionContainsSuper(*binary->left)) ||
           (binary->right && expressionContainsSuper(*binary->right));
  }
  if (auto* unary = std::get_if<UnaryExpr>(&expr.node)) {
    return unary->argument && expressionContainsSuper(*unary->argument);
  }
  if (auto* assign = std::get_if<AssignmentExpr>(&expr.node)) {
    return (assign->left && expressionContainsSuper(*assign->left)) ||
           (assign->right && expressionContainsSuper(*assign->right));
  }
  if (auto* update = std::get_if<UpdateExpr>(&expr.node)) {
    return update->argument && expressionContainsSuper(*update->argument);
  }
  if (auto* call = std::get_if<CallExpr>(&expr.node)) {
    if (call->callee && expressionContainsSuper(*call->callee)) return true;
    for (const auto& arg : call->arguments) {
      if (arg && expressionContainsSuper(*arg)) return true;
    }
    return false;
  }
  if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
    return (member->object && expressionContainsSuper(*member->object)) ||
           (member->property && expressionContainsSuper(*member->property));
  }
  if (auto* cond = std::get_if<ConditionalExpr>(&expr.node)) {
    return (cond->test && expressionContainsSuper(*cond->test)) ||
           (cond->consequent && expressionContainsSuper(*cond->consequent)) ||
           (cond->alternate && expressionContainsSuper(*cond->alternate));
  }
  if (auto* seq = std::get_if<SequenceExpr>(&expr.node)) {
    for (const auto& e : seq->expressions) {
      if (e && expressionContainsSuper(*e)) return true;
    }
    return false;
  }
  if (auto* arr = std::get_if<ArrayExpr>(&expr.node)) {
    for (const auto& e : arr->elements) {
      if (e && expressionContainsSuper(*e)) return true;
    }
    return false;
  }
  if (auto* obj = std::get_if<ObjectExpr>(&expr.node)) {
    for (const auto& prop : obj->properties) {
      if (prop.key && expressionContainsSuper(*prop.key)) return true;
      if (prop.value && expressionContainsSuper(*prop.value)) return true;
    }
    return false;
  }
  if (auto* awaitExpr = std::get_if<AwaitExpr>(&expr.node)) {
    return awaitExpr->argument && expressionContainsSuper(*awaitExpr->argument);
  }
  if (auto* yieldExpr = std::get_if<YieldExpr>(&expr.node)) {
    return yieldExpr->argument && expressionContainsSuper(*yieldExpr->argument);
  }
  if (auto* spread = std::get_if<SpreadElement>(&expr.node)) {
    return spread->argument && expressionContainsSuper(*spread->argument);
  }
  if (auto* newExpr = std::get_if<NewExpr>(&expr.node)) {
    if (newExpr->callee && expressionContainsSuper(*newExpr->callee)) return true;
    for (const auto& arg : newExpr->arguments) {
      if (arg && expressionContainsSuper(*arg)) return true;
    }
    return false;
  }
  if (auto* arrPat = std::get_if<ArrayPattern>(&expr.node)) {
    for (const auto& e : arrPat->elements) {
      if (e && expressionContainsSuper(*e)) return true;
    }
    return arrPat->rest && expressionContainsSuper(*arrPat->rest);
  }
  if (auto* objPat = std::get_if<ObjectPattern>(&expr.node)) {
    for (const auto& prop : objPat->properties) {
      if (prop.key && expressionContainsSuper(*prop.key)) return true;
      if (prop.value && expressionContainsSuper(*prop.value)) return true;
    }
    return objPat->rest && expressionContainsSuper(*objPat->rest);
  }
  if (auto* assignPat = std::get_if<AssignmentPattern>(&expr.node)) {
    return (assignPat->left && expressionContainsSuper(*assignPat->left)) ||
           (assignPat->right && expressionContainsSuper(*assignPat->right));
  }
  return false;
}

bool statementContainsSuper(const Statement& stmt) {
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.node)) {
    for (const auto& decl : varDecl->declarations) {
      if (decl.pattern && expressionContainsSuper(*decl.pattern)) return true;
      if (decl.init && expressionContainsSuper(*decl.init)) return true;
    }
    return false;
  }
  if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt.node)) {
    return exprStmt->expression && expressionContainsSuper(*exprStmt->expression);
  }
  if (auto* ret = std::get_if<ReturnStmt>(&stmt.node)) {
    return ret->argument && expressionContainsSuper(*ret->argument);
  }
  if (auto* block = std::get_if<BlockStmt>(&stmt.node)) {
    for (const auto& s : block->body) {
      if (s && statementContainsSuper(*s)) return true;
    }
    return false;
  }
  if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    if (ifStmt->test && expressionContainsSuper(*ifStmt->test)) return true;
    if (ifStmt->consequent && statementContainsSuper(*ifStmt->consequent)) return true;
    return ifStmt->alternate && statementContainsSuper(*ifStmt->alternate);
  }
  if (auto* whileStmt = std::get_if<WhileStmt>(&stmt.node)) {
    if (whileStmt->test && expressionContainsSuper(*whileStmt->test)) return true;
    return whileStmt->body && statementContainsSuper(*whileStmt->body);
  }
  if (auto* forStmt = std::get_if<ForStmt>(&stmt.node)) {
    if (forStmt->init && statementContainsSuper(*forStmt->init)) return true;
    if (forStmt->test && expressionContainsSuper(*forStmt->test)) return true;
    if (forStmt->update && expressionContainsSuper(*forStmt->update)) return true;
    return forStmt->body && statementContainsSuper(*forStmt->body);
  }
  if (auto* withStmt = std::get_if<WithStmt>(&stmt.node)) {
    if (withStmt->object && expressionContainsSuper(*withStmt->object)) return true;
    return withStmt->body && statementContainsSuper(*withStmt->body);
  }
  if (auto* forInStmt = std::get_if<ForInStmt>(&stmt.node)) {
    if (forInStmt->left && statementContainsSuper(*forInStmt->left)) return true;
    if (forInStmt->right && expressionContainsSuper(*forInStmt->right)) return true;
    return forInStmt->body && statementContainsSuper(*forInStmt->body);
  }
  if (auto* forOfStmt = std::get_if<ForOfStmt>(&stmt.node)) {
    if (forOfStmt->left && statementContainsSuper(*forOfStmt->left)) return true;
    if (forOfStmt->right && expressionContainsSuper(*forOfStmt->right)) return true;
    return forOfStmt->body && statementContainsSuper(*forOfStmt->body);
  }
  if (auto* doWhileStmt = std::get_if<DoWhileStmt>(&stmt.node)) {
    if (doWhileStmt->body && statementContainsSuper(*doWhileStmt->body)) return true;
    return doWhileStmt->test && expressionContainsSuper(*doWhileStmt->test);
  }
  if (auto* switchStmt = std::get_if<SwitchStmt>(&stmt.node)) {
    if (switchStmt->discriminant && expressionContainsSuper(*switchStmt->discriminant)) return true;
    for (const auto& c : switchStmt->cases) {
      if (c.test && expressionContainsSuper(*c.test)) return true;
      for (const auto& s : c.consequent) {
        if (s && statementContainsSuper(*s)) return true;
      }
    }
    return false;
  }
  if (auto* label = std::get_if<LabelledStmt>(&stmt.node)) {
    return label->body && statementContainsSuper(*label->body);
  }
  if (auto* throwStmt = std::get_if<ThrowStmt>(&stmt.node)) {
    return throwStmt->argument && expressionContainsSuper(*throwStmt->argument);
  }
  if (auto* tryStmt = std::get_if<TryStmt>(&stmt.node)) {
    for (const auto& s : tryStmt->block) {
      if (s && statementContainsSuper(*s)) return true;
    }
    if (tryStmt->handler.paramPattern && expressionContainsSuper(*tryStmt->handler.paramPattern)) return true;
    for (const auto& s : tryStmt->handler.body) {
      if (s && statementContainsSuper(*s)) return true;
    }
    for (const auto& s : tryStmt->finalizer) {
      if (s && statementContainsSuper(*s)) return true;
    }
    return false;
  }
  if (auto* exportDefault = std::get_if<ExportDefaultDeclaration>(&stmt.node)) {
    return exportDefault->declaration && expressionContainsSuper(*exportDefault->declaration);
  }
  return false;
}

bool expressionContainsYield(const Expression& expr) {
  if (std::holds_alternative<YieldExpr>(expr.node)) {
    return true;
  }
  if (auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
    return (binary->left && expressionContainsYield(*binary->left)) ||
           (binary->right && expressionContainsYield(*binary->right));
  }
  if (auto* unary = std::get_if<UnaryExpr>(&expr.node)) {
    return unary->argument && expressionContainsYield(*unary->argument);
  }
  if (auto* assign = std::get_if<AssignmentExpr>(&expr.node)) {
    return (assign->left && expressionContainsYield(*assign->left)) ||
           (assign->right && expressionContainsYield(*assign->right));
  }
  if (auto* update = std::get_if<UpdateExpr>(&expr.node)) {
    return update->argument && expressionContainsYield(*update->argument);
  }
  if (auto* call = std::get_if<CallExpr>(&expr.node)) {
    if (call->callee && expressionContainsYield(*call->callee)) return true;
    for (const auto& arg : call->arguments) {
      if (arg && expressionContainsYield(*arg)) return true;
    }
    return false;
  }
  if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
    return (member->object && expressionContainsYield(*member->object)) ||
           (member->property && expressionContainsYield(*member->property));
  }
  if (auto* cond = std::get_if<ConditionalExpr>(&expr.node)) {
    return (cond->test && expressionContainsYield(*cond->test)) ||
           (cond->consequent && expressionContainsYield(*cond->consequent)) ||
           (cond->alternate && expressionContainsYield(*cond->alternate));
  }
  if (auto* seq = std::get_if<SequenceExpr>(&expr.node)) {
    for (const auto& e : seq->expressions) {
      if (e && expressionContainsYield(*e)) return true;
    }
    return false;
  }
  if (auto* arr = std::get_if<ArrayExpr>(&expr.node)) {
    for (const auto& e : arr->elements) {
      if (e && expressionContainsYield(*e)) return true;
    }
    return false;
  }
  if (auto* obj = std::get_if<ObjectExpr>(&expr.node)) {
    for (const auto& prop : obj->properties) {
      if (prop.key && expressionContainsYield(*prop.key)) return true;
      if (prop.value && expressionContainsYield(*prop.value)) return true;
    }
    return false;
  }
  if (auto* awaitExpr = std::get_if<AwaitExpr>(&expr.node)) {
    return awaitExpr->argument && expressionContainsYield(*awaitExpr->argument);
  }
  if (auto* spread = std::get_if<SpreadElement>(&expr.node)) {
    return spread->argument && expressionContainsYield(*spread->argument);
  }
  if (auto* newExpr = std::get_if<NewExpr>(&expr.node)) {
    if (newExpr->callee && expressionContainsYield(*newExpr->callee)) return true;
    for (const auto& arg : newExpr->arguments) {
      if (arg && expressionContainsYield(*arg)) return true;
    }
    return false;
  }
  if (auto* arrPat = std::get_if<ArrayPattern>(&expr.node)) {
    for (const auto& e : arrPat->elements) {
      if (e && expressionContainsYield(*e)) return true;
    }
    return arrPat->rest && expressionContainsYield(*arrPat->rest);
  }
  if (auto* objPat = std::get_if<ObjectPattern>(&expr.node)) {
    for (const auto& prop : objPat->properties) {
      if (prop.key && expressionContainsYield(*prop.key)) return true;
      if (prop.value && expressionContainsYield(*prop.value)) return true;
    }
    return objPat->rest && expressionContainsYield(*objPat->rest);
  }
  if (auto* assignPat = std::get_if<AssignmentPattern>(&expr.node)) {
    return (assignPat->left && expressionContainsYield(*assignPat->left)) ||
           (assignPat->right && expressionContainsYield(*assignPat->right));
  }
  return false;
}

bool expressionContainsSuperCall(const Expression& expr) {
  if (auto* call = std::get_if<CallExpr>(&expr.node)) {
    if (call->callee && std::holds_alternative<SuperExpr>(call->callee->node)) {
      return true;
    }
    if (call->callee && expressionContainsSuperCall(*call->callee)) return true;
    for (const auto& arg : call->arguments) {
      if (arg && expressionContainsSuperCall(*arg)) return true;
    }
    return false;
  }
  if (auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
    return (binary->left && expressionContainsSuperCall(*binary->left)) ||
           (binary->right && expressionContainsSuperCall(*binary->right));
  }
  if (auto* unary = std::get_if<UnaryExpr>(&expr.node)) {
    return unary->argument && expressionContainsSuperCall(*unary->argument);
  }
  if (auto* assign = std::get_if<AssignmentExpr>(&expr.node)) {
    return (assign->left && expressionContainsSuperCall(*assign->left)) ||
           (assign->right && expressionContainsSuperCall(*assign->right));
  }
  if (auto* update = std::get_if<UpdateExpr>(&expr.node)) {
    return update->argument && expressionContainsSuperCall(*update->argument);
  }
  if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
    return (member->object && expressionContainsSuperCall(*member->object)) ||
           (member->property && expressionContainsSuperCall(*member->property));
  }
  if (auto* cond = std::get_if<ConditionalExpr>(&expr.node)) {
    return (cond->test && expressionContainsSuperCall(*cond->test)) ||
           (cond->consequent && expressionContainsSuperCall(*cond->consequent)) ||
           (cond->alternate && expressionContainsSuperCall(*cond->alternate));
  }
  if (auto* seq = std::get_if<SequenceExpr>(&expr.node)) {
    for (const auto& e : seq->expressions) {
      if (e && expressionContainsSuperCall(*e)) return true;
    }
    return false;
  }
  if (auto* arr = std::get_if<ArrayExpr>(&expr.node)) {
    for (const auto& e : arr->elements) {
      if (e && expressionContainsSuperCall(*e)) return true;
    }
    return false;
  }
  if (auto* obj = std::get_if<ObjectExpr>(&expr.node)) {
    for (const auto& prop : obj->properties) {
      if (prop.key && expressionContainsSuperCall(*prop.key)) return true;
      if (prop.value && expressionContainsSuperCall(*prop.value)) return true;
    }
    return false;
  }
  if (auto* awaitExpr = std::get_if<AwaitExpr>(&expr.node)) {
    return awaitExpr->argument && expressionContainsSuperCall(*awaitExpr->argument);
  }
  if (auto* yieldExpr = std::get_if<YieldExpr>(&expr.node)) {
    return yieldExpr->argument && expressionContainsSuperCall(*yieldExpr->argument);
  }
  if (auto* spread = std::get_if<SpreadElement>(&expr.node)) {
    return spread->argument && expressionContainsSuperCall(*spread->argument);
  }
  if (auto* newExpr = std::get_if<NewExpr>(&expr.node)) {
    if (newExpr->callee && expressionContainsSuperCall(*newExpr->callee)) return true;
    for (const auto& arg : newExpr->arguments) {
      if (arg && expressionContainsSuperCall(*arg)) return true;
    }
    return false;
  }
  if (auto* assignPat = std::get_if<AssignmentPattern>(&expr.node)) {
    return (assignPat->left && expressionContainsSuperCall(*assignPat->left)) ||
           (assignPat->right && expressionContainsSuperCall(*assignPat->right));
  }
  return false;
}

// Field initializer SuperCall check: recurse into arrows but stop at regular functions/classes.
bool fieldInitContainsSuperCall(const Expression& expr) {
  if (auto* func = std::get_if<FunctionExpr>(&expr.node)) {
    if (func->isArrow) {
      for (const auto& s : func->body) {
        if (s && fieldInitStmtContainsSuperCall(*s)) return true;
      }
      for (const auto& p : func->params) {
        if (p.defaultValue && fieldInitContainsSuperCall(*p.defaultValue)) return true;
      }
    }
    return false;  // Regular functions don't propagate
  }
  if (std::holds_alternative<ClassExpr>(expr.node)) return false;
  if (auto* call = std::get_if<CallExpr>(&expr.node)) {
    if (call->callee && std::holds_alternative<SuperExpr>(call->callee->node)) return true;
    if (call->callee && fieldInitContainsSuperCall(*call->callee)) return true;
    for (const auto& arg : call->arguments) {
      if (arg && fieldInitContainsSuperCall(*arg)) return true;
    }
    return false;
  }
  if (auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
    return (binary->left && fieldInitContainsSuperCall(*binary->left)) ||
           (binary->right && fieldInitContainsSuperCall(*binary->right));
  }
  if (auto* unary = std::get_if<UnaryExpr>(&expr.node)) {
    return unary->argument && fieldInitContainsSuperCall(*unary->argument);
  }
  if (auto* assign = std::get_if<AssignmentExpr>(&expr.node)) {
    return (assign->left && fieldInitContainsSuperCall(*assign->left)) ||
           (assign->right && fieldInitContainsSuperCall(*assign->right));
  }
  if (auto* update = std::get_if<UpdateExpr>(&expr.node)) {
    return update->argument && fieldInitContainsSuperCall(*update->argument);
  }
  if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
    return (member->object && fieldInitContainsSuperCall(*member->object)) ||
           (member->property && fieldInitContainsSuperCall(*member->property));
  }
  if (auto* cond = std::get_if<ConditionalExpr>(&expr.node)) {
    return (cond->test && fieldInitContainsSuperCall(*cond->test)) ||
           (cond->consequent && fieldInitContainsSuperCall(*cond->consequent)) ||
           (cond->alternate && fieldInitContainsSuperCall(*cond->alternate));
  }
  if (auto* seq = std::get_if<SequenceExpr>(&expr.node)) {
    for (const auto& e : seq->expressions) {
      if (e && fieldInitContainsSuperCall(*e)) return true;
    }
    return false;
  }
  if (auto* arr = std::get_if<ArrayExpr>(&expr.node)) {
    for (const auto& e : arr->elements) {
      if (e && fieldInitContainsSuperCall(*e)) return true;
    }
    return false;
  }
  if (auto* obj = std::get_if<ObjectExpr>(&expr.node)) {
    for (const auto& prop : obj->properties) {
      if (prop.key && fieldInitContainsSuperCall(*prop.key)) return true;
      if (prop.value && fieldInitContainsSuperCall(*prop.value)) return true;
    }
    return false;
  }
  if (auto* awaitExpr = std::get_if<AwaitExpr>(&expr.node)) {
    return awaitExpr->argument && fieldInitContainsSuperCall(*awaitExpr->argument);
  }
  if (auto* yieldExpr = std::get_if<YieldExpr>(&expr.node)) {
    return yieldExpr->argument && fieldInitContainsSuperCall(*yieldExpr->argument);
  }
  if (auto* spread = std::get_if<SpreadElement>(&expr.node)) {
    return spread->argument && fieldInitContainsSuperCall(*spread->argument);
  }
  if (auto* newExpr = std::get_if<NewExpr>(&expr.node)) {
    if (newExpr->callee && fieldInitContainsSuperCall(*newExpr->callee)) return true;
    for (const auto& arg : newExpr->arguments) {
      if (arg && fieldInitContainsSuperCall(*arg)) return true;
    }
    return false;
  }
  if (auto* tmpl = std::get_if<TemplateLiteral>(&expr.node)) {
    for (const auto& e : tmpl->expressions) {
      if (e && fieldInitContainsSuperCall(*e)) return true;
    }
    return false;
  }
  if (auto* assignPat = std::get_if<AssignmentPattern>(&expr.node)) {
    return (assignPat->left && fieldInitContainsSuperCall(*assignPat->left)) ||
           (assignPat->right && fieldInitContainsSuperCall(*assignPat->right));
  }
  return false;
}

bool fieldInitStmtContainsSuperCall(const Statement& stmt) {
  if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt.node)) {
    return exprStmt->expression && fieldInitContainsSuperCall(*exprStmt->expression);
  }
  if (auto* ret = std::get_if<ReturnStmt>(&stmt.node)) {
    return ret->argument && fieldInitContainsSuperCall(*ret->argument);
  }
  if (auto* block = std::get_if<BlockStmt>(&stmt.node)) {
    for (const auto& s : block->body) {
      if (s && fieldInitStmtContainsSuperCall(*s)) return true;
    }
    return false;
  }
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.node)) {
    for (const auto& d : varDecl->declarations) {
      if (d.init && fieldInitContainsSuperCall(*d.init)) return true;
    }
    return false;
  }
  if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    if (ifStmt->test && fieldInitContainsSuperCall(*ifStmt->test)) return true;
    if (ifStmt->consequent && fieldInitStmtContainsSuperCall(*ifStmt->consequent)) return true;
    return ifStmt->alternate && fieldInitStmtContainsSuperCall(*ifStmt->alternate);
  }
  return false;
}

// Field initializer arguments check: recurse into arrows but stop at regular functions/classes.
bool fieldInitContainsArguments(const Expression& expr) {
  if (auto* id = std::get_if<Identifier>(&expr.node)) {
    return id->name == "arguments";
  }
  if (auto* func = std::get_if<FunctionExpr>(&expr.node)) {
    if (func->isArrow) {
      for (const auto& s : func->body) {
        if (s && fieldInitStmtContainsArguments(*s)) return true;
      }
      for (const auto& p : func->params) {
        if (p.defaultValue && fieldInitContainsArguments(*p.defaultValue)) return true;
      }
    }
    return false;
  }
  if (std::holds_alternative<ClassExpr>(expr.node)) return false;
  if (auto* call = std::get_if<CallExpr>(&expr.node)) {
    if (call->callee && fieldInitContainsArguments(*call->callee)) return true;
    for (const auto& arg : call->arguments) {
      if (arg && fieldInitContainsArguments(*arg)) return true;
    }
    return false;
  }
  if (auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
    return (binary->left && fieldInitContainsArguments(*binary->left)) ||
           (binary->right && fieldInitContainsArguments(*binary->right));
  }
  if (auto* unary = std::get_if<UnaryExpr>(&expr.node)) {
    return unary->argument && fieldInitContainsArguments(*unary->argument);
  }
  if (auto* assign = std::get_if<AssignmentExpr>(&expr.node)) {
    return (assign->left && fieldInitContainsArguments(*assign->left)) ||
           (assign->right && fieldInitContainsArguments(*assign->right));
  }
  if (auto* update = std::get_if<UpdateExpr>(&expr.node)) {
    return update->argument && fieldInitContainsArguments(*update->argument);
  }
  if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
    return (member->object && fieldInitContainsArguments(*member->object)) ||
           (member->property && fieldInitContainsArguments(*member->property));
  }
  if (auto* cond = std::get_if<ConditionalExpr>(&expr.node)) {
    return (cond->test && fieldInitContainsArguments(*cond->test)) ||
           (cond->consequent && fieldInitContainsArguments(*cond->consequent)) ||
           (cond->alternate && fieldInitContainsArguments(*cond->alternate));
  }
  if (auto* seq = std::get_if<SequenceExpr>(&expr.node)) {
    for (const auto& e : seq->expressions) {
      if (e && fieldInitContainsArguments(*e)) return true;
    }
    return false;
  }
  if (auto* arr = std::get_if<ArrayExpr>(&expr.node)) {
    for (const auto& e : arr->elements) {
      if (e && fieldInitContainsArguments(*e)) return true;
    }
    return false;
  }
  if (auto* obj = std::get_if<ObjectExpr>(&expr.node)) {
    for (const auto& prop : obj->properties) {
      if (prop.key && fieldInitContainsArguments(*prop.key)) return true;
      if (prop.value && fieldInitContainsArguments(*prop.value)) return true;
    }
    return false;
  }
  if (auto* awaitExpr = std::get_if<AwaitExpr>(&expr.node)) {
    return awaitExpr->argument && fieldInitContainsArguments(*awaitExpr->argument);
  }
  if (auto* yieldExpr = std::get_if<YieldExpr>(&expr.node)) {
    return yieldExpr->argument && fieldInitContainsArguments(*yieldExpr->argument);
  }
  if (auto* spread = std::get_if<SpreadElement>(&expr.node)) {
    return spread->argument && fieldInitContainsArguments(*spread->argument);
  }
  if (auto* newExpr = std::get_if<NewExpr>(&expr.node)) {
    if (newExpr->callee && fieldInitContainsArguments(*newExpr->callee)) return true;
    for (const auto& arg : newExpr->arguments) {
      if (arg && fieldInitContainsArguments(*arg)) return true;
    }
    return false;
  }
  if (auto* tmpl = std::get_if<TemplateLiteral>(&expr.node)) {
    for (const auto& e : tmpl->expressions) {
      if (e && fieldInitContainsArguments(*e)) return true;
    }
    return false;
  }
  if (auto* assignPat = std::get_if<AssignmentPattern>(&expr.node)) {
    return (assignPat->left && fieldInitContainsArguments(*assignPat->left)) ||
           (assignPat->right && fieldInitContainsArguments(*assignPat->right));
  }
  return false;
}

bool fieldInitStmtContainsArguments(const Statement& stmt) {
  if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt.node)) {
    return exprStmt->expression && fieldInitContainsArguments(*exprStmt->expression);
  }
  if (auto* ret = std::get_if<ReturnStmt>(&stmt.node)) {
    return ret->argument && fieldInitContainsArguments(*ret->argument);
  }
  if (auto* block = std::get_if<BlockStmt>(&stmt.node)) {
    for (const auto& s : block->body) {
      if (s && fieldInitStmtContainsArguments(*s)) return true;
    }
    return false;
  }
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.node)) {
    for (const auto& d : varDecl->declarations) {
      if (d.init && fieldInitContainsArguments(*d.init)) return true;
    }
    return false;
  }
  if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    if (ifStmt->test && fieldInitContainsArguments(*ifStmt->test)) return true;
    if (ifStmt->consequent && fieldInitStmtContainsArguments(*ifStmt->consequent)) return true;
    return ifStmt->alternate && fieldInitStmtContainsArguments(*ifStmt->alternate);
  }
  return false;
}

// Class static block early error checks: walk statement list without crossing
// function or static initialization block boundaries (approximated by treating
// all function bodies and class static blocks as boundaries).
bool staticBlockExprContainsAwait(const Expression& expr);
bool staticBlockStmtContainsAwait(const Statement& stmt);

bool staticBlockExprContainsAwait(const Expression& expr) {
  if (std::holds_alternative<AwaitExpr>(expr.node)) return true;
  if (auto* func = std::get_if<FunctionExpr>(&expr.node)) {
    (void)func;
    return false;  // function boundary
  }
  if (auto* cls = std::get_if<ClassExpr>(&expr.node)) {
    if (cls->superClass && staticBlockExprContainsAwait(*cls->superClass)) return true;
    for (const auto& m : cls->methods) {
      if (m.computedKey && staticBlockExprContainsAwait(*m.computedKey)) return true;
      if (m.initializer && staticBlockExprContainsAwait(*m.initializer)) return true;
    }
    return false;
  }
  if (auto* call = std::get_if<CallExpr>(&expr.node)) {
    if (call->callee && staticBlockExprContainsAwait(*call->callee)) return true;
    for (const auto& arg : call->arguments) {
      if (arg && staticBlockExprContainsAwait(*arg)) return true;
    }
    return false;
  }
  if (auto* newExpr = std::get_if<NewExpr>(&expr.node)) {
    if (newExpr->callee && staticBlockExprContainsAwait(*newExpr->callee)) return true;
    for (const auto& arg : newExpr->arguments) {
      if (arg && staticBlockExprContainsAwait(*arg)) return true;
    }
    return false;
  }
  if (auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
    return (binary->left && staticBlockExprContainsAwait(*binary->left)) ||
           (binary->right && staticBlockExprContainsAwait(*binary->right));
  }
  if (auto* unary = std::get_if<UnaryExpr>(&expr.node)) {
    return unary->argument && staticBlockExprContainsAwait(*unary->argument);
  }
  if (auto* assign = std::get_if<AssignmentExpr>(&expr.node)) {
    return (assign->left && staticBlockExprContainsAwait(*assign->left)) ||
           (assign->right && staticBlockExprContainsAwait(*assign->right));
  }
  if (auto* update = std::get_if<UpdateExpr>(&expr.node)) {
    return update->argument && staticBlockExprContainsAwait(*update->argument);
  }
  if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
    return (member->object && staticBlockExprContainsAwait(*member->object)) ||
           (member->property && staticBlockExprContainsAwait(*member->property));
  }
  if (auto* cond = std::get_if<ConditionalExpr>(&expr.node)) {
    return (cond->test && staticBlockExprContainsAwait(*cond->test)) ||
           (cond->consequent && staticBlockExprContainsAwait(*cond->consequent)) ||
           (cond->alternate && staticBlockExprContainsAwait(*cond->alternate));
  }
  if (auto* seq = std::get_if<SequenceExpr>(&expr.node)) {
    for (const auto& e : seq->expressions) {
      if (e && staticBlockExprContainsAwait(*e)) return true;
    }
    return false;
  }
  if (auto* arr = std::get_if<ArrayExpr>(&expr.node)) {
    for (const auto& e : arr->elements) {
      if (e && staticBlockExprContainsAwait(*e)) return true;
    }
    return false;
  }
  if (auto* obj = std::get_if<ObjectExpr>(&expr.node)) {
    for (const auto& prop : obj->properties) {
      if (prop.key && staticBlockExprContainsAwait(*prop.key)) return true;
      if (prop.value && staticBlockExprContainsAwait(*prop.value)) return true;
    }
    return false;
  }
  if (auto* spread = std::get_if<SpreadElement>(&expr.node)) {
    return spread->argument && staticBlockExprContainsAwait(*spread->argument);
  }
  if (auto* yieldExpr = std::get_if<YieldExpr>(&expr.node)) {
    return yieldExpr->argument && staticBlockExprContainsAwait(*yieldExpr->argument);
  }
  if (auto* tmpl = std::get_if<TemplateLiteral>(&expr.node)) {
    for (const auto& e : tmpl->expressions) {
      if (e && staticBlockExprContainsAwait(*e)) return true;
    }
    return false;
  }
  if (auto* assignPat = std::get_if<AssignmentPattern>(&expr.node)) {
    return (assignPat->left && staticBlockExprContainsAwait(*assignPat->left)) ||
           (assignPat->right && staticBlockExprContainsAwait(*assignPat->right));
  }
  return false;
}

bool staticBlockStmtContainsAwait(const Statement& stmt) {
  if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt.node)) {
    return exprStmt->expression && staticBlockExprContainsAwait(*exprStmt->expression);
  }
  if (auto* block = std::get_if<BlockStmt>(&stmt.node)) {
    for (const auto& s : block->body) {
      if (s && staticBlockStmtContainsAwait(*s)) return true;
    }
    return false;
  }
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.node)) {
    for (const auto& d : varDecl->declarations) {
      if (d.init && staticBlockExprContainsAwait(*d.init)) return true;
    }
    return false;
  }
  if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    if (ifStmt->test && staticBlockExprContainsAwait(*ifStmt->test)) return true;
    if (ifStmt->consequent && staticBlockStmtContainsAwait(*ifStmt->consequent)) return true;
    return ifStmt->alternate && staticBlockStmtContainsAwait(*ifStmt->alternate);
  }
  if (auto* clsDecl = std::get_if<ClassDeclaration>(&stmt.node)) {
    if (clsDecl->superClass && staticBlockExprContainsAwait(*clsDecl->superClass)) return true;
    for (const auto& m : clsDecl->methods) {
      if (m.computedKey && staticBlockExprContainsAwait(*m.computedKey)) return true;
      if (m.initializer && staticBlockExprContainsAwait(*m.initializer)) return true;
    }
    return false;
  }
  // Do not traverse into function bodies (function boundary).
  if (std::holds_alternative<FunctionDeclaration>(stmt.node)) return false;
  if (auto* ret = std::get_if<ReturnStmt>(&stmt.node)) {
    return ret->argument && staticBlockExprContainsAwait(*ret->argument);
  }
  return false;
}

bool staticBlockExprContainsArguments(const Expression& expr);
bool staticBlockStmtContainsArguments(const Statement& stmt);

bool staticBlockExprContainsArguments(const Expression& expr) {
  if (auto* id = std::get_if<Identifier>(&expr.node)) {
    return id->name == "arguments";
  }
  if (auto* func = std::get_if<FunctionExpr>(&expr.node)) {
    (void)func;
    return false;  // function boundary
  }
  if (auto* cls = std::get_if<ClassExpr>(&expr.node)) {
    if (cls->superClass && staticBlockExprContainsArguments(*cls->superClass)) return true;
    for (const auto& m : cls->methods) {
      if (m.computedKey && staticBlockExprContainsArguments(*m.computedKey)) return true;
      if (m.initializer && staticBlockExprContainsArguments(*m.initializer)) return true;
    }
    return false;
  }
  if (auto* call = std::get_if<CallExpr>(&expr.node)) {
    if (call->callee && staticBlockExprContainsArguments(*call->callee)) return true;
    for (const auto& arg : call->arguments) {
      if (arg && staticBlockExprContainsArguments(*arg)) return true;
    }
    return false;
  }
  if (auto* newExpr = std::get_if<NewExpr>(&expr.node)) {
    if (newExpr->callee && staticBlockExprContainsArguments(*newExpr->callee)) return true;
    for (const auto& arg : newExpr->arguments) {
      if (arg && staticBlockExprContainsArguments(*arg)) return true;
    }
    return false;
  }
  if (auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
    return (binary->left && staticBlockExprContainsArguments(*binary->left)) ||
           (binary->right && staticBlockExprContainsArguments(*binary->right));
  }
  if (auto* unary = std::get_if<UnaryExpr>(&expr.node)) {
    return unary->argument && staticBlockExprContainsArguments(*unary->argument);
  }
  if (auto* assign = std::get_if<AssignmentExpr>(&expr.node)) {
    return (assign->left && staticBlockExprContainsArguments(*assign->left)) ||
           (assign->right && staticBlockExprContainsArguments(*assign->right));
  }
  if (auto* update = std::get_if<UpdateExpr>(&expr.node)) {
    return update->argument && staticBlockExprContainsArguments(*update->argument);
  }
  if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
    return (member->object && staticBlockExprContainsArguments(*member->object)) ||
           (member->property && staticBlockExprContainsArguments(*member->property));
  }
  if (auto* cond = std::get_if<ConditionalExpr>(&expr.node)) {
    return (cond->test && staticBlockExprContainsArguments(*cond->test)) ||
           (cond->consequent && staticBlockExprContainsArguments(*cond->consequent)) ||
           (cond->alternate && staticBlockExprContainsArguments(*cond->alternate));
  }
  if (auto* seq = std::get_if<SequenceExpr>(&expr.node)) {
    for (const auto& e : seq->expressions) {
      if (e && staticBlockExprContainsArguments(*e)) return true;
    }
    return false;
  }
  if (auto* arr = std::get_if<ArrayExpr>(&expr.node)) {
    for (const auto& e : arr->elements) {
      if (e && staticBlockExprContainsArguments(*e)) return true;
    }
    return false;
  }
  if (auto* obj = std::get_if<ObjectExpr>(&expr.node)) {
    for (const auto& prop : obj->properties) {
      if (prop.key && staticBlockExprContainsArguments(*prop.key)) return true;
      if (prop.value && staticBlockExprContainsArguments(*prop.value)) return true;
    }
    return false;
  }
  if (auto* awaitExpr = std::get_if<AwaitExpr>(&expr.node)) {
    return awaitExpr->argument && staticBlockExprContainsArguments(*awaitExpr->argument);
  }
  if (auto* yieldExpr = std::get_if<YieldExpr>(&expr.node)) {
    return yieldExpr->argument && staticBlockExprContainsArguments(*yieldExpr->argument);
  }
  if (auto* spread = std::get_if<SpreadElement>(&expr.node)) {
    return spread->argument && staticBlockExprContainsArguments(*spread->argument);
  }
  if (auto* tmpl = std::get_if<TemplateLiteral>(&expr.node)) {
    for (const auto& e : tmpl->expressions) {
      if (e && staticBlockExprContainsArguments(*e)) return true;
    }
    return false;
  }
  if (auto* assignPat = std::get_if<AssignmentPattern>(&expr.node)) {
    return (assignPat->left && staticBlockExprContainsArguments(*assignPat->left)) ||
           (assignPat->right && staticBlockExprContainsArguments(*assignPat->right));
  }
  return false;
}

bool staticBlockStmtContainsArguments(const Statement& stmt) {
  if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt.node)) {
    return exprStmt->expression && staticBlockExprContainsArguments(*exprStmt->expression);
  }
  if (auto* block = std::get_if<BlockStmt>(&stmt.node)) {
    for (const auto& s : block->body) {
      if (s && staticBlockStmtContainsArguments(*s)) return true;
    }
    return false;
  }
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.node)) {
    for (const auto& d : varDecl->declarations) {
      if (d.init && staticBlockExprContainsArguments(*d.init)) return true;
    }
    return false;
  }
  if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    if (ifStmt->test && staticBlockExprContainsArguments(*ifStmt->test)) return true;
    if (ifStmt->consequent && staticBlockStmtContainsArguments(*ifStmt->consequent)) return true;
    return ifStmt->alternate && staticBlockStmtContainsArguments(*ifStmt->alternate);
  }
  if (auto* clsDecl = std::get_if<ClassDeclaration>(&stmt.node)) {
    if (clsDecl->superClass && staticBlockExprContainsArguments(*clsDecl->superClass)) return true;
    for (const auto& m : clsDecl->methods) {
      if (m.computedKey && staticBlockExprContainsArguments(*m.computedKey)) return true;
      if (m.initializer && staticBlockExprContainsArguments(*m.initializer)) return true;
    }
    return false;
  }
  if (std::holds_alternative<FunctionDeclaration>(stmt.node)) return false;
  if (auto* ret = std::get_if<ReturnStmt>(&stmt.node)) {
    return ret->argument && staticBlockExprContainsArguments(*ret->argument);
  }
  return false;
}

bool staticBlockStatementListContainsAwait(const std::vector<StmtPtr>& body) {
  for (const auto& s : body) {
    if (s && staticBlockStmtContainsAwait(*s)) return true;
  }
  return false;
}

bool staticBlockStatementListContainsArguments(const std::vector<StmtPtr>& body) {
  for (const auto& s : body) {
    if (s && staticBlockStmtContainsArguments(*s)) return true;
  }
  return false;
}

bool staticBlockStatementListHasDuplicateLexNames(const std::vector<StmtPtr>& body) {
  std::set<std::string> names;
  for (const auto& s : body) {
    if (!s) continue;
    if (auto* varDecl = std::get_if<VarDeclaration>(&s->node)) {
      if (varDecl->kind == VarDeclaration::Kind::Let ||
          varDecl->kind == VarDeclaration::Kind::Const ||
          varDecl->kind == VarDeclaration::Kind::Using ||
          varDecl->kind == VarDeclaration::Kind::AwaitUsing) {
        for (const auto& d : varDecl->declarations) {
          std::vector<std::string> bound;
          if (d.pattern) collectBoundNames(*d.pattern, bound);
          for (const auto& n : bound) {
            if (!names.insert(n).second) return true;
          }
        }
      }
    } else if (auto* clsDecl = std::get_if<ClassDeclaration>(&s->node)) {
      if (!names.insert(clsDecl->id.name).second) return true;
    }
  }
  return false;
}

bool staticBlockStatementListHasLexVarConflict(const std::vector<StmtPtr>& body) {
  std::set<std::string> lexNames;
  for (const auto& s : body) {
    if (!s) continue;
    if (auto* varDecl = std::get_if<VarDeclaration>(&s->node)) {
      if (varDecl->kind == VarDeclaration::Kind::Let ||
          varDecl->kind == VarDeclaration::Kind::Const ||
          varDecl->kind == VarDeclaration::Kind::Using ||
          varDecl->kind == VarDeclaration::Kind::AwaitUsing) {
        for (const auto& d : varDecl->declarations) {
          std::vector<std::string> bound;
          if (d.pattern) collectBoundNames(*d.pattern, bound);
          for (const auto& n : bound) {
            lexNames.insert(n);
          }
        }
      }
    } else if (auto* clsDecl = std::get_if<ClassDeclaration>(&s->node)) {
      lexNames.insert(clsDecl->id.name);
    }
  }

  std::vector<std::string> varNames;
  for (const auto& s : body) {
    if (s) collectVarDeclaredNames(*s, varNames, /*includeFunctionDeclarations*/true);
  }
  for (const auto& n : varNames) {
    if (lexNames.count(n) != 0) return true;
  }
  return false;
}

bool expressionContainsAwaitLike(const Expression& expr) {
  if (std::holds_alternative<AwaitExpr>(expr.node)) {
    return true;
  }
  if (auto* id = std::get_if<Identifier>(&expr.node)) {
    return id->name == "await";
  }
  if (auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
    return (binary->left && expressionContainsAwaitLike(*binary->left)) ||
           (binary->right && expressionContainsAwaitLike(*binary->right));
  }
  if (auto* unary = std::get_if<UnaryExpr>(&expr.node)) {
    return unary->argument && expressionContainsAwaitLike(*unary->argument);
  }
  if (auto* assign = std::get_if<AssignmentExpr>(&expr.node)) {
    return (assign->left && expressionContainsAwaitLike(*assign->left)) ||
           (assign->right && expressionContainsAwaitLike(*assign->right));
  }
  if (auto* call = std::get_if<CallExpr>(&expr.node)) {
    if (call->callee && expressionContainsAwaitLike(*call->callee)) return true;
    for (const auto& arg : call->arguments) {
      if (arg && expressionContainsAwaitLike(*arg)) return true;
    }
    return false;
  }
  if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
    return (member->object && expressionContainsAwaitLike(*member->object)) ||
           (member->property && expressionContainsAwaitLike(*member->property));
  }
  if (auto* cond = std::get_if<ConditionalExpr>(&expr.node)) {
    return (cond->test && expressionContainsAwaitLike(*cond->test)) ||
           (cond->consequent && expressionContainsAwaitLike(*cond->consequent)) ||
           (cond->alternate && expressionContainsAwaitLike(*cond->alternate));
  }
  if (auto* seq = std::get_if<SequenceExpr>(&expr.node)) {
    for (const auto& e : seq->expressions) {
      if (e && expressionContainsAwaitLike(*e)) return true;
    }
    return false;
  }
  if (auto* arr = std::get_if<ArrayExpr>(&expr.node)) {
    for (const auto& e : arr->elements) {
      if (e && expressionContainsAwaitLike(*e)) return true;
    }
    return false;
  }
  if (auto* obj = std::get_if<ObjectExpr>(&expr.node)) {
    for (const auto& prop : obj->properties) {
      if (prop.key && expressionContainsAwaitLike(*prop.key)) return true;
      if (prop.value && expressionContainsAwaitLike(*prop.value)) return true;
    }
    return false;
  }
  if (auto* yieldExpr = std::get_if<YieldExpr>(&expr.node)) {
    return yieldExpr->argument && expressionContainsAwaitLike(*yieldExpr->argument);
  }
  if (auto* spread = std::get_if<SpreadElement>(&expr.node)) {
    return spread->argument && expressionContainsAwaitLike(*spread->argument);
  }
  if (auto* newExpr = std::get_if<NewExpr>(&expr.node)) {
    if (newExpr->callee && expressionContainsAwaitLike(*newExpr->callee)) return true;
    for (const auto& arg : newExpr->arguments) {
      if (arg && expressionContainsAwaitLike(*arg)) return true;
    }
    return false;
  }
  if (auto* assignPat = std::get_if<AssignmentPattern>(&expr.node)) {
    return (assignPat->left && expressionContainsAwaitLike(*assignPat->left)) ||
           (assignPat->right && expressionContainsAwaitLike(*assignPat->right));
  }
  return false;
}

bool expressionContainsStrictRestrictedWrite(const Expression& expr) {
  if (auto* assign = std::get_if<AssignmentExpr>(&expr.node)) {
    if (assign->left) {
      if (auto* id = std::get_if<Identifier>(&assign->left->node)) {
        if (isStrictModeRestrictedIdentifier(id->name)) {
          return true;
        }
      }
      if (expressionContainsStrictRestrictedWrite(*assign->left)) return true;
    }
    return assign->right && expressionContainsStrictRestrictedWrite(*assign->right);
  }
  if (auto* update = std::get_if<UpdateExpr>(&expr.node)) {
    if (update->argument) {
      if (auto* id = std::get_if<Identifier>(&update->argument->node)) {
        if (isStrictModeRestrictedIdentifier(id->name)) {
          return true;
        }
      }
      return expressionContainsStrictRestrictedWrite(*update->argument);
    }
    return false;
  }
  if (auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
    return (binary->left && expressionContainsStrictRestrictedWrite(*binary->left)) ||
           (binary->right && expressionContainsStrictRestrictedWrite(*binary->right));
  }
  if (auto* unary = std::get_if<UnaryExpr>(&expr.node)) {
    return unary->argument && expressionContainsStrictRestrictedWrite(*unary->argument);
  }
  if (auto* call = std::get_if<CallExpr>(&expr.node)) {
    if (call->callee && expressionContainsStrictRestrictedWrite(*call->callee)) return true;
    for (const auto& arg : call->arguments) {
      if (arg && expressionContainsStrictRestrictedWrite(*arg)) return true;
    }
    return false;
  }
  if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
    return (member->object && expressionContainsStrictRestrictedWrite(*member->object)) ||
           (member->computed && member->property && expressionContainsStrictRestrictedWrite(*member->property));
  }
  if (auto* cond = std::get_if<ConditionalExpr>(&expr.node)) {
    return (cond->test && expressionContainsStrictRestrictedWrite(*cond->test)) ||
           (cond->consequent && expressionContainsStrictRestrictedWrite(*cond->consequent)) ||
           (cond->alternate && expressionContainsStrictRestrictedWrite(*cond->alternate));
  }
  if (auto* seq = std::get_if<SequenceExpr>(&expr.node)) {
    for (const auto& e : seq->expressions) {
      if (e && expressionContainsStrictRestrictedWrite(*e)) return true;
    }
    return false;
  }
  if (auto* arr = std::get_if<ArrayExpr>(&expr.node)) {
    for (const auto& e : arr->elements) {
      if (e && expressionContainsStrictRestrictedWrite(*e)) return true;
    }
    return false;
  }
  if (auto* obj = std::get_if<ObjectExpr>(&expr.node)) {
    for (const auto& prop : obj->properties) {
      if (prop.value && expressionContainsStrictRestrictedWrite(*prop.value)) return true;
    }
    return false;
  }
  return false;
}

bool statementContainsStrictRestrictedWrite(const Statement& stmt) {
  if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt.node)) {
    return exprStmt->expression && expressionContainsStrictRestrictedWrite(*exprStmt->expression);
  }
  if (auto* block = std::get_if<BlockStmt>(&stmt.node)) {
    for (const auto& s : block->body) {
      if (s && statementContainsStrictRestrictedWrite(*s)) return true;
    }
    return false;
  }
  if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    if (ifStmt->test && expressionContainsStrictRestrictedWrite(*ifStmt->test)) return true;
    if (ifStmt->consequent && statementContainsStrictRestrictedWrite(*ifStmt->consequent)) return true;
    return ifStmt->alternate && statementContainsStrictRestrictedWrite(*ifStmt->alternate);
  }
  if (auto* whileStmt = std::get_if<WhileStmt>(&stmt.node)) {
    if (whileStmt->test && expressionContainsStrictRestrictedWrite(*whileStmt->test)) return true;
    return whileStmt->body && statementContainsStrictRestrictedWrite(*whileStmt->body);
  }
  if (auto* forStmt = std::get_if<ForStmt>(&stmt.node)) {
    if (forStmt->init && statementContainsStrictRestrictedWrite(*forStmt->init)) return true;
    if (forStmt->test && expressionContainsStrictRestrictedWrite(*forStmt->test)) return true;
    if (forStmt->update && expressionContainsStrictRestrictedWrite(*forStmt->update)) return true;
    return forStmt->body && statementContainsStrictRestrictedWrite(*forStmt->body);
  }
  return false;
}

bool expressionContainsStrictRestrictedIdentifierReference(const Expression& expr) {
  if (auto* id = std::get_if<Identifier>(&expr.node)) {
    return isStrictFutureReservedIdentifier(id->name);
  }
  if (auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
    return (binary->left && expressionContainsStrictRestrictedIdentifierReference(*binary->left)) ||
           (binary->right && expressionContainsStrictRestrictedIdentifierReference(*binary->right));
  }
  if (auto* unary = std::get_if<UnaryExpr>(&expr.node)) {
    return unary->argument && expressionContainsStrictRestrictedIdentifierReference(*unary->argument);
  }
  if (auto* assign = std::get_if<AssignmentExpr>(&expr.node)) {
    return (assign->left && expressionContainsStrictRestrictedIdentifierReference(*assign->left)) ||
           (assign->right && expressionContainsStrictRestrictedIdentifierReference(*assign->right));
  }
  if (auto* update = std::get_if<UpdateExpr>(&expr.node)) {
    return update->argument && expressionContainsStrictRestrictedIdentifierReference(*update->argument);
  }
  if (auto* call = std::get_if<CallExpr>(&expr.node)) {
    if (call->callee && expressionContainsStrictRestrictedIdentifierReference(*call->callee)) return true;
    for (const auto& arg : call->arguments) {
      if (arg && expressionContainsStrictRestrictedIdentifierReference(*arg)) return true;
    }
    return false;
  }
  if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
    if (member->object && expressionContainsStrictRestrictedIdentifierReference(*member->object)) return true;
    if (member->computed && member->property &&
        expressionContainsStrictRestrictedIdentifierReference(*member->property)) return true;
    return false;
  }
  if (auto* cond = std::get_if<ConditionalExpr>(&expr.node)) {
    return (cond->test && expressionContainsStrictRestrictedIdentifierReference(*cond->test)) ||
           (cond->consequent && expressionContainsStrictRestrictedIdentifierReference(*cond->consequent)) ||
           (cond->alternate && expressionContainsStrictRestrictedIdentifierReference(*cond->alternate));
  }
  if (auto* seq = std::get_if<SequenceExpr>(&expr.node)) {
    for (const auto& e : seq->expressions) {
      if (e && expressionContainsStrictRestrictedIdentifierReference(*e)) return true;
    }
    return false;
  }
  if (auto* arr = std::get_if<ArrayExpr>(&expr.node)) {
    for (const auto& e : arr->elements) {
      if (e && expressionContainsStrictRestrictedIdentifierReference(*e)) return true;
    }
    return false;
  }
  if (auto* obj = std::get_if<ObjectExpr>(&expr.node)) {
    for (const auto& prop : obj->properties) {
      if (prop.isComputed && prop.key &&
          expressionContainsStrictRestrictedIdentifierReference(*prop.key)) return true;
      if (prop.value && expressionContainsStrictRestrictedIdentifierReference(*prop.value)) return true;
    }
    return false;
  }
  if (auto* awaitExpr = std::get_if<AwaitExpr>(&expr.node)) {
    return awaitExpr->argument && expressionContainsStrictRestrictedIdentifierReference(*awaitExpr->argument);
  }
  if (auto* yieldExpr = std::get_if<YieldExpr>(&expr.node)) {
    return yieldExpr->argument && expressionContainsStrictRestrictedIdentifierReference(*yieldExpr->argument);
  }
  if (auto* spread = std::get_if<SpreadElement>(&expr.node)) {
    return spread->argument && expressionContainsStrictRestrictedIdentifierReference(*spread->argument);
  }
  if (auto* newExpr = std::get_if<NewExpr>(&expr.node)) {
    if (newExpr->callee && expressionContainsStrictRestrictedIdentifierReference(*newExpr->callee)) return true;
    for (const auto& arg : newExpr->arguments) {
      if (arg && expressionContainsStrictRestrictedIdentifierReference(*arg)) return true;
    }
    return false;
  }
  return false;
}

bool statementContainsStrictRestrictedIdentifierReference(const Statement& stmt) {
  if (auto* decl = std::get_if<VarDeclaration>(&stmt.node)) {
    for (const auto& d : decl->declarations) {
      std::vector<std::string> names;
      if (d.pattern) collectBoundNames(*d.pattern, names);
      for (const auto& n : names) {
        if (isStrictFutureReservedIdentifier(n)) return true;
      }
      if (d.init && expressionContainsStrictRestrictedIdentifierReference(*d.init)) return true;
    }
    return false;
  }
  if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt.node)) {
    return exprStmt->expression && expressionContainsStrictRestrictedIdentifierReference(*exprStmt->expression);
  }
  if (auto* ret = std::get_if<ReturnStmt>(&stmt.node)) {
    return ret->argument && expressionContainsStrictRestrictedIdentifierReference(*ret->argument);
  }
  if (auto* block = std::get_if<BlockStmt>(&stmt.node)) {
    for (const auto& s : block->body) {
      if (s && statementContainsStrictRestrictedIdentifierReference(*s)) return true;
    }
    return false;
  }
  if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    if (ifStmt->test && expressionContainsStrictRestrictedIdentifierReference(*ifStmt->test)) return true;
    if (ifStmt->consequent && statementContainsStrictRestrictedIdentifierReference(*ifStmt->consequent)) return true;
    return ifStmt->alternate && statementContainsStrictRestrictedIdentifierReference(*ifStmt->alternate);
  }
  if (auto* whileStmt = std::get_if<WhileStmt>(&stmt.node)) {
    if (whileStmt->test && expressionContainsStrictRestrictedIdentifierReference(*whileStmt->test)) return true;
    return whileStmt->body && statementContainsStrictRestrictedIdentifierReference(*whileStmt->body);
  }
  if (auto* forStmt = std::get_if<ForStmt>(&stmt.node)) {
    if (forStmt->init && statementContainsStrictRestrictedIdentifierReference(*forStmt->init)) return true;
    if (forStmt->test && expressionContainsStrictRestrictedIdentifierReference(*forStmt->test)) return true;
    if (forStmt->update && expressionContainsStrictRestrictedIdentifierReference(*forStmt->update)) return true;
    return forStmt->body && statementContainsStrictRestrictedIdentifierReference(*forStmt->body);
  }
  return false;
}

bool expressionHasUndeclaredPrivateName(const Expression& expr, const std::set<std::string>& declared);
bool statementHasUndeclaredPrivateName(const Statement& stmt, const std::set<std::string>& declared);

bool expressionHasUndeclaredPrivateName(const Expression& expr, const std::set<std::string>& declared) {
  if (auto* ident = std::get_if<Identifier>(&expr.node)) {
    if (!ident->name.empty() && ident->name[0] == '#') {
      return declared.count(ident->name) == 0;
    }
    return false;
  }
  if (auto* unary = std::get_if<UnaryExpr>(&expr.node)) {
    return unary->argument && expressionHasUndeclaredPrivateName(*unary->argument, declared);
  }
  if (auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
    return (binary->left && expressionHasUndeclaredPrivateName(*binary->left, declared)) ||
           (binary->right && expressionHasUndeclaredPrivateName(*binary->right, declared));
  }
  if (auto* assign = std::get_if<AssignmentExpr>(&expr.node)) {
    return (assign->left && expressionHasUndeclaredPrivateName(*assign->left, declared)) ||
           (assign->right && expressionHasUndeclaredPrivateName(*assign->right, declared));
  }
  if (auto* update = std::get_if<UpdateExpr>(&expr.node)) {
    return update->argument && expressionHasUndeclaredPrivateName(*update->argument, declared);
  }
  if (auto* call = std::get_if<CallExpr>(&expr.node)) {
    if (call->callee && expressionHasUndeclaredPrivateName(*call->callee, declared)) return true;
    for (const auto& arg : call->arguments) {
      if (arg && expressionHasUndeclaredPrivateName(*arg, declared)) return true;
    }
    return false;
  }
  if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
    // AllPrivateNamesValid: obj.#x / call().#x requires #x to be declared in the class body.
    if (!member->computed && member->property &&
        std::holds_alternative<Identifier>(member->property->node)) {
      const auto& id = std::get<Identifier>(member->property->node);
      if (!id.name.empty() && id.name[0] == '#') {
        if (declared.count(id.name) == 0) {
          return true;
        }
      }
    }
    return (member->object && expressionHasUndeclaredPrivateName(*member->object, declared)) ||
           (member->property && expressionHasUndeclaredPrivateName(*member->property, declared));
  }
  if (auto* cond = std::get_if<ConditionalExpr>(&expr.node)) {
    return (cond->test && expressionHasUndeclaredPrivateName(*cond->test, declared)) ||
           (cond->consequent && expressionHasUndeclaredPrivateName(*cond->consequent, declared)) ||
           (cond->alternate && expressionHasUndeclaredPrivateName(*cond->alternate, declared));
  }
  if (auto* seq = std::get_if<SequenceExpr>(&expr.node)) {
    for (const auto& e : seq->expressions) {
      if (e && expressionHasUndeclaredPrivateName(*e, declared)) return true;
    }
    return false;
  }
  if (auto* arr = std::get_if<ArrayExpr>(&expr.node)) {
    for (const auto& e : arr->elements) {
      if (e && expressionHasUndeclaredPrivateName(*e, declared)) return true;
    }
    return false;
  }
  if (auto* obj = std::get_if<ObjectExpr>(&expr.node)) {
    for (const auto& prop : obj->properties) {
      if (prop.key && expressionHasUndeclaredPrivateName(*prop.key, declared)) return true;
      if (prop.value && expressionHasUndeclaredPrivateName(*prop.value, declared)) return true;
    }
    return false;
  }
  if (auto* fn = std::get_if<FunctionExpr>(&expr.node)) {
    for (const auto& param : fn->params) {
      if (param.defaultValue && expressionHasUndeclaredPrivateName(*param.defaultValue, declared)) return true;
    }
    for (const auto& stmt : fn->body) {
      if (stmt && statementHasUndeclaredPrivateName(*stmt, declared)) return true;
    }
    return false;
  }
  // Don't descend into nested classes: they'll validate their own private environment.
  if (std::holds_alternative<ClassExpr>(expr.node)) {
    return false;
  }
  if (auto* awaitExpr = std::get_if<AwaitExpr>(&expr.node)) {
    return awaitExpr->argument && expressionHasUndeclaredPrivateName(*awaitExpr->argument, declared);
  }
  if (auto* yieldExpr = std::get_if<YieldExpr>(&expr.node)) {
    return yieldExpr->argument && expressionHasUndeclaredPrivateName(*yieldExpr->argument, declared);
  }
  if (auto* spread = std::get_if<SpreadElement>(&expr.node)) {
    return spread->argument && expressionHasUndeclaredPrivateName(*spread->argument, declared);
  }
  if (auto* newExpr = std::get_if<NewExpr>(&expr.node)) {
    if (newExpr->callee && expressionHasUndeclaredPrivateName(*newExpr->callee, declared)) return true;
    for (const auto& arg : newExpr->arguments) {
      if (arg && expressionHasUndeclaredPrivateName(*arg, declared)) return true;
    }
    return false;
  }
  if (auto* arrPat = std::get_if<ArrayPattern>(&expr.node)) {
    for (const auto& e : arrPat->elements) {
      if (e && expressionHasUndeclaredPrivateName(*e, declared)) return true;
    }
    return arrPat->rest && expressionHasUndeclaredPrivateName(*arrPat->rest, declared);
  }
  if (auto* objPat = std::get_if<ObjectPattern>(&expr.node)) {
    for (const auto& prop : objPat->properties) {
      if (prop.key && expressionHasUndeclaredPrivateName(*prop.key, declared)) return true;
      if (prop.value && expressionHasUndeclaredPrivateName(*prop.value, declared)) return true;
    }
    return objPat->rest && expressionHasUndeclaredPrivateName(*objPat->rest, declared);
  }
  if (auto* assignPat = std::get_if<AssignmentPattern>(&expr.node)) {
    return (assignPat->left && expressionHasUndeclaredPrivateName(*assignPat->left, declared)) ||
           (assignPat->right && expressionHasUndeclaredPrivateName(*assignPat->right, declared));
  }
  return false;
}

bool statementHasUndeclaredPrivateName(const Statement& stmt, const std::set<std::string>& declared) {
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.node)) {
    for (const auto& decl : varDecl->declarations) {
      if (decl.pattern && expressionHasUndeclaredPrivateName(*decl.pattern, declared)) return true;
      if (decl.init && expressionHasUndeclaredPrivateName(*decl.init, declared)) return true;
    }
    return false;
  }
  if (auto* fnDecl = std::get_if<FunctionDeclaration>(&stmt.node)) {
    for (const auto& param : fnDecl->params) {
      if (param.defaultValue && expressionHasUndeclaredPrivateName(*param.defaultValue, declared)) return true;
    }
    for (const auto& s : fnDecl->body) {
      if (s && statementHasUndeclaredPrivateName(*s, declared)) return true;
    }
    return false;
  }
  // Don't descend into nested classes: they'll validate their own private environment.
  if (std::holds_alternative<ClassDeclaration>(stmt.node)) {
    return false;
  }
  if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt.node)) {
    return exprStmt->expression && expressionHasUndeclaredPrivateName(*exprStmt->expression, declared);
  }
  if (auto* ret = std::get_if<ReturnStmt>(&stmt.node)) {
    return ret->argument && expressionHasUndeclaredPrivateName(*ret->argument, declared);
  }
  if (auto* block = std::get_if<BlockStmt>(&stmt.node)) {
    for (const auto& s : block->body) {
      if (s && statementHasUndeclaredPrivateName(*s, declared)) return true;
    }
    return false;
  }
  if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    if (ifStmt->test && expressionHasUndeclaredPrivateName(*ifStmt->test, declared)) return true;
    if (ifStmt->consequent && statementHasUndeclaredPrivateName(*ifStmt->consequent, declared)) return true;
    return ifStmt->alternate && statementHasUndeclaredPrivateName(*ifStmt->alternate, declared);
  }
  if (auto* whileStmt = std::get_if<WhileStmt>(&stmt.node)) {
    if (whileStmt->test && expressionHasUndeclaredPrivateName(*whileStmt->test, declared)) return true;
    return whileStmt->body && statementHasUndeclaredPrivateName(*whileStmt->body, declared);
  }
  if (auto* forStmt = std::get_if<ForStmt>(&stmt.node)) {
    if (forStmt->init && statementHasUndeclaredPrivateName(*forStmt->init, declared)) return true;
    if (forStmt->test && expressionHasUndeclaredPrivateName(*forStmt->test, declared)) return true;
    if (forStmt->update && expressionHasUndeclaredPrivateName(*forStmt->update, declared)) return true;
    return forStmt->body && statementHasUndeclaredPrivateName(*forStmt->body, declared);
  }
  if (auto* withStmt = std::get_if<WithStmt>(&stmt.node)) {
    if (withStmt->object && expressionHasUndeclaredPrivateName(*withStmt->object, declared)) return true;
    return withStmt->body && statementHasUndeclaredPrivateName(*withStmt->body, declared);
  }
  if (auto* forInStmt = std::get_if<ForInStmt>(&stmt.node)) {
    if (forInStmt->left && statementHasUndeclaredPrivateName(*forInStmt->left, declared)) return true;
    if (forInStmt->right && expressionHasUndeclaredPrivateName(*forInStmt->right, declared)) return true;
    return forInStmt->body && statementHasUndeclaredPrivateName(*forInStmt->body, declared);
  }
  if (auto* forOfStmt = std::get_if<ForOfStmt>(&stmt.node)) {
    if (forOfStmt->left && statementHasUndeclaredPrivateName(*forOfStmt->left, declared)) return true;
    if (forOfStmt->right && expressionHasUndeclaredPrivateName(*forOfStmt->right, declared)) return true;
    return forOfStmt->body && statementHasUndeclaredPrivateName(*forOfStmt->body, declared);
  }
  if (auto* doWhileStmt = std::get_if<DoWhileStmt>(&stmt.node)) {
    if (doWhileStmt->body && statementHasUndeclaredPrivateName(*doWhileStmt->body, declared)) return true;
    return doWhileStmt->test && expressionHasUndeclaredPrivateName(*doWhileStmt->test, declared);
  }
  if (auto* switchStmt = std::get_if<SwitchStmt>(&stmt.node)) {
    if (switchStmt->discriminant && expressionHasUndeclaredPrivateName(*switchStmt->discriminant, declared)) return true;
    for (const auto& c : switchStmt->cases) {
      if (c.test && expressionHasUndeclaredPrivateName(*c.test, declared)) return true;
      for (const auto& s : c.consequent) {
        if (s && statementHasUndeclaredPrivateName(*s, declared)) return true;
      }
    }
    return false;
  }
  if (auto* label = std::get_if<LabelledStmt>(&stmt.node)) {
    return label->body && statementHasUndeclaredPrivateName(*label->body, declared);
  }
  if (auto* throwStmt = std::get_if<ThrowStmt>(&stmt.node)) {
    return throwStmt->argument && expressionHasUndeclaredPrivateName(*throwStmt->argument, declared);
  }
  if (auto* tryStmt = std::get_if<TryStmt>(&stmt.node)) {
    for (const auto& s : tryStmt->block) {
      if (s && statementHasUndeclaredPrivateName(*s, declared)) return true;
    }
    if (tryStmt->handler.paramPattern && expressionHasUndeclaredPrivateName(*tryStmt->handler.paramPattern, declared)) return true;
    for (const auto& s : tryStmt->handler.body) {
      if (s && statementHasUndeclaredPrivateName(*s, declared)) return true;
    }
    for (const auto& s : tryStmt->finalizer) {
      if (s && statementHasUndeclaredPrivateName(*s, declared)) return true;
    }
    return false;
  }
  if (auto* exportDefault = std::get_if<ExportDefaultDeclaration>(&stmt.node)) {
    return exportDefault->declaration && expressionHasUndeclaredPrivateName(*exportDefault->declaration, declared);
  }
  return false;
}

void collectTopLevelLexicallyDeclaredNames(const std::vector<StmtPtr>& body,
                                           std::vector<std::string>& names) {
  for (const auto& stmt : body) {
    if (!stmt) continue;
    if (auto* varDecl = std::get_if<VarDeclaration>(&stmt->node)) {
      if (varDecl->kind == VarDeclaration::Kind::Let ||
          varDecl->kind == VarDeclaration::Kind::Const ||
          varDecl->kind == VarDeclaration::Kind::Using ||
          varDecl->kind == VarDeclaration::Kind::AwaitUsing) {
        for (const auto& decl : varDecl->declarations) {
          if (decl.pattern) collectBoundNames(*decl.pattern, names);
        }
      }
      continue;
    }
    if (auto* fnDecl = std::get_if<FunctionDeclaration>(&stmt->node)) {
      names.push_back(fnDecl->id.name);
      continue;
    }
    if (auto* clsDecl = std::get_if<ClassDeclaration>(&stmt->node)) {
      names.push_back(clsDecl->id.name);
      continue;
    }
  }
}

bool addNameIfUnique(const std::string& name, std::vector<std::string>& names) {
  for (const auto& existing : names) {
    if (existing == name) {
      return false;
    }
  }
  names.push_back(name);
  return true;
}

bool collectStatementListLexicalNames(const std::vector<StmtPtr>& body,
                                      std::vector<std::string>& names,
                                      bool includeFunctionDeclarations) {
  for (const auto& stmt : body) {
    if (!stmt) continue;
    if (auto* varDecl = std::get_if<VarDeclaration>(&stmt->node)) {
      if (varDecl->kind == VarDeclaration::Kind::Let ||
          varDecl->kind == VarDeclaration::Kind::Const ||
          varDecl->kind == VarDeclaration::Kind::Using ||
          varDecl->kind == VarDeclaration::Kind::AwaitUsing) {
        std::vector<std::string> boundNames;
        for (const auto& decl : varDecl->declarations) {
          if (decl.pattern) {
            collectBoundNames(*decl.pattern, boundNames);
          }
        }
        for (const auto& name : boundNames) {
          if (!addNameIfUnique(name, names)) {
            return false;
          }
        }
      }
      continue;
    }
    // Block-level function declarations participate in lexical name checks.
    // At the ScriptBody level, function declarations are treated as var-declared.
    if (includeFunctionDeclarations) {
      if (auto* fnDecl = std::get_if<FunctionDeclaration>(&stmt->node)) {
        if (!addNameIfUnique(fnDecl->id.name, names)) {
          return false;
        }
        continue;
      }
    }
    if (auto* clsDecl = std::get_if<ClassDeclaration>(&stmt->node)) {
      if (!addNameIfUnique(clsDecl->id.name, names)) {
        return false;
      }
    }
  }
  return true;
}

void collectStatementListVarNames(const std::vector<StmtPtr>& body,
                                  std::vector<std::string>& names,
                                  bool includeFunctionDeclarations) {
  for (const auto& stmt : body) {
    if (stmt) {
      collectVarDeclaredNames(*stmt, names, includeFunctionDeclarations);
    }
  }
}

bool hasNameCollision(const std::vector<std::string>& first,
                      const std::vector<std::string>& second) {
  for (const auto& a : first) {
    for (const auto& b : second) {
      if (a == b) {
        return true;
      }
    }
  }
  return false;
}

bool isOptionalChain(const Expression& expr) {
  if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
    return member->optional;
  }
  if (auto* call = std::get_if<CallExpr>(&expr.node)) {
    return call->optional;
  }
  return false;
}

bool hasUnparenthesizedLogicalOp(const ExprPtr& expr) {
  if (!expr || expr->parenthesized) {
    return false;
  }
  if (auto* binary = std::get_if<BinaryExpr>(&expr->node)) {
    return binary->op == BinaryExpr::Op::LogicalOr ||
           binary->op == BinaryExpr::Op::LogicalAnd;
  }
  return false;
}

bool isAssignmentTarget(const Expression& expr) {
  if (std::holds_alternative<MetaProperty>(expr.node)) {
    return false;
  }

  // Parenthesized AssignmentTargets are only valid for simple targets.
  if (expr.parenthesized &&
      !std::holds_alternative<Identifier>(expr.node) &&
      !std::holds_alternative<MemberExpr>(expr.node)) {
    return false;
  }

  if (std::holds_alternative<Identifier>(expr.node)) {
    return true;
  }
  if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
    return !member->optional;
  }
  if (auto* assign = std::get_if<AssignmentPattern>(&expr.node)) {
    return assign->left && isAssignmentTarget(*assign->left);
  }
  if (auto* assignExpr = std::get_if<AssignmentExpr>(&expr.node)) {
    return assignExpr->op == AssignmentExpr::Op::Assign &&
           assignExpr->left && isAssignmentTarget(*assignExpr->left);
  }

  if (auto* arr = std::get_if<ArrayExpr>(&expr.node)) {
    for (size_t i = 0; i < arr->elements.size(); ++i) {
      const auto& elem = arr->elements[i];
      if (!elem) continue;
      if (auto* spread = std::get_if<SpreadElement>(&elem->node)) {
        if (i + 1 != arr->elements.size()) {
          return false;
        }
        if (!spread->argument || !isAssignmentTarget(*spread->argument)) {
          return false;
        }
      } else if (!isAssignmentTarget(*elem)) {
        return false;
      }
    }
    return true;
  }

  if (auto* obj = std::get_if<ObjectExpr>(&expr.node)) {
    for (size_t i = 0; i < obj->properties.size(); ++i) {
      const auto& prop = obj->properties[i];
      if (prop.isSpread && i + 1 != obj->properties.size()) {
        return false;
      }
      if (!prop.value || !isAssignmentTarget(*prop.value)) {
        return false;
      }
    }
    return true;
  }

  return false;
}

bool hasStrictInvalidSimpleAssignmentTarget(const Expression& expr) {
  if (auto* id = std::get_if<Identifier>(&expr.node)) {
    return id->name == "eval" || id->name == "arguments";
  }
  if (std::get_if<MemberExpr>(&expr.node)) {
    return false;
  }
  if (auto* assign = std::get_if<AssignmentPattern>(&expr.node)) {
    return assign->left && hasStrictInvalidSimpleAssignmentTarget(*assign->left);
  }
  if (auto* assignExpr = std::get_if<AssignmentExpr>(&expr.node)) {
    return assignExpr->left && hasStrictInvalidSimpleAssignmentTarget(*assignExpr->left);
  }
  if (auto* arr = std::get_if<ArrayExpr>(&expr.node)) {
    for (const auto& elem : arr->elements) {
      if (!elem) continue;
      if (auto* spread = std::get_if<SpreadElement>(&elem->node)) {
        if (spread->argument && hasStrictInvalidSimpleAssignmentTarget(*spread->argument)) {
          return true;
        }
      } else if (hasStrictInvalidSimpleAssignmentTarget(*elem)) {
        return true;
      }
    }
    return false;
  }
  if (auto* arrPat = std::get_if<ArrayPattern>(&expr.node)) {
    for (const auto& elem : arrPat->elements) {
      if (elem && hasStrictInvalidSimpleAssignmentTarget(*elem)) return true;
    }
    return arrPat->rest && hasStrictInvalidSimpleAssignmentTarget(*arrPat->rest);
  }
  if (auto* obj = std::get_if<ObjectExpr>(&expr.node)) {
    for (const auto& prop : obj->properties) {
      if (prop.value && hasStrictInvalidSimpleAssignmentTarget(*prop.value)) return true;
    }
    return false;
  }
  if (auto* objPat = std::get_if<ObjectPattern>(&expr.node)) {
    for (const auto& prop : objPat->properties) {
      if (prop.value && hasStrictInvalidSimpleAssignmentTarget(*prop.value)) return true;
    }
    return objPat->rest && hasStrictInvalidSimpleAssignmentTarget(*objPat->rest);
  }
  return false;
}

bool convertAssignmentTargetToPattern(Expression& expr,
                                      bool allowYieldIdentifier,
                                      bool allowAwaitIdentifier,
                                      bool strictMode) {
  if (std::get_if<Identifier>(&expr.node) || std::get_if<MemberExpr>(&expr.node)) {
    return true;
  }
  if (auto* assignPat = std::get_if<AssignmentPattern>(&expr.node)) {
    return assignPat->left && convertAssignmentTargetToPattern(*assignPat->left, allowYieldIdentifier,
                                                               allowAwaitIdentifier, strictMode);
  }
  if (auto* assignExpr = std::get_if<AssignmentExpr>(&expr.node)) {
    if (assignExpr->op != AssignmentExpr::Op::Assign || !assignExpr->left || !assignExpr->right) {
      return false;
    }
    if (!convertAssignmentTargetToPattern(*assignExpr->left, allowYieldIdentifier,
                                          allowAwaitIdentifier, strictMode)) {
      return false;
    }
    expr.node = AssignmentPattern{std::move(assignExpr->left), std::move(assignExpr->right)};
    return true;
  }
  if (auto* arr = std::get_if<ArrayExpr>(&expr.node)) {
    ArrayPattern pat;
    for (size_t i = 0; i < arr->elements.size(); ++i) {
      auto& elem = arr->elements[i];
      if (!elem) {
        pat.elements.push_back(nullptr);
        continue;
      }
      if (auto* spread = std::get_if<SpreadElement>(&elem->node)) {
        if (i + 1 != arr->elements.size() || !spread->argument || pat.rest) {
          return false;
        }
        if (!convertAssignmentTargetToPattern(*spread->argument, allowYieldIdentifier,
                                              allowAwaitIdentifier, strictMode)) {
          return false;
        }
        if (std::get_if<AssignmentPattern>(&spread->argument->node)) {
          return false;
        }
        pat.rest = std::move(spread->argument);
        continue;
      }
      if (!convertAssignmentTargetToPattern(*elem, allowYieldIdentifier,
                                            allowAwaitIdentifier, strictMode)) {
        return false;
      }
      pat.elements.push_back(std::move(elem));
    }
    expr.node = std::move(pat);
    return true;
  }
  if (auto* obj = std::get_if<ObjectExpr>(&expr.node)) {
    ObjectPattern pat;
    for (auto& prop : obj->properties) {
      if (prop.isSpread) {
        if (!prop.value || pat.rest) {
          return false;
        }
        if (!convertAssignmentTargetToPattern(*prop.value, allowYieldIdentifier,
                                              allowAwaitIdentifier, strictMode)) {
          return false;
        }
        if (std::get_if<AssignmentPattern>(&prop.value->node)) {
          return false;
        }
        pat.rest = std::move(prop.value);
        continue;
      }
      if (!prop.key || !prop.value) {
        return false;
      }
      if (!prop.isComputed) {
        auto* keyId = std::get_if<Identifier>(&prop.key->node);
        auto* valueId = std::get_if<Identifier>(&prop.value->node);
        if (keyId && valueId && keyId->name == valueId->name) {
          if ((valueId->name == "yield" && !allowYieldIdentifier) ||
              (valueId->name == "await" && !allowAwaitIdentifier)) {
            return false;
          }
          if (strictMode &&
              (isStrictFutureReservedIdentifier(valueId->name) ||
               valueId->name == "eval" || valueId->name == "arguments")) {
            return false;
          }
          if (isAlwaysReservedIdentifierWord(valueId->name)) {
            return false;
          }
        }
      }
      if (!convertAssignmentTargetToPattern(*prop.value, allowYieldIdentifier,
                                            allowAwaitIdentifier, strictMode)) {
        return false;
      }
      ObjectPattern::Property out;
      out.key = std::move(prop.key);
      out.value = std::move(prop.value);
      out.computed = prop.isComputed;
      pat.properties.push_back(std::move(out));
    }
    expr.node = std::move(pat);
    return true;
  }
  return false;
}

// Check if an assignment pattern contains eval/arguments as simple targets (invalid in strict mode)
bool hasStrictModeInvalidTargets(const Expression& expr) {
  if (auto* ident = std::get_if<Identifier>(&expr.node)) {
    return ident->name == "eval" || ident->name == "arguments";
  }
  if (auto* arr = std::get_if<ArrayExpr>(&expr.node)) {
    for (const auto& elem : arr->elements) {
      if (elem && hasStrictModeInvalidTargets(*elem)) return true;
    }
  }
  if (auto* obj = std::get_if<ObjectExpr>(&expr.node)) {
    for (const auto& prop : obj->properties) {
      if (prop.value && hasStrictModeInvalidTargets(*prop.value)) return true;
    }
  }
  if (auto* arrPat = std::get_if<ArrayPattern>(&expr.node)) {
    for (const auto& elem : arrPat->elements) {
      if (elem && hasStrictModeInvalidTargets(*elem)) return true;
    }
    if (arrPat->rest && hasStrictModeInvalidTargets(*arrPat->rest)) return true;
  }
  if (auto* objPat = std::get_if<ObjectPattern>(&expr.node)) {
    for (const auto& prop : objPat->properties) {
      if (prop.value && hasStrictModeInvalidTargets(*prop.value)) return true;
    }
    if (objPat->rest && hasStrictModeInvalidTargets(*objPat->rest)) return true;
  }
  if (auto* assign = std::get_if<AssignmentPattern>(&expr.node)) {
    if (assign->left && hasStrictModeInvalidTargets(*assign->left)) return true;
  }
  if (auto* spread = std::get_if<SpreadElement>(&expr.node)) {
    if (spread->argument && hasStrictModeInvalidTargets(*spread->argument)) return true;
  }
  return false;
}

bool isDirectDynamicImportCall(const ExprPtr& expr) {
  if (!expr || expr->parenthesized) {
    return false;
  }
  auto* call = std::get_if<CallExpr>(&expr->node);
  if (!call || !call->callee) {
    return false;
  }
  auto* id = std::get_if<Identifier>(&call->callee->node);
  return id && id->name == "import";
}

constexpr const char* kImportPhaseSourceSentinel = "__lightjs_import_phase_source__";
constexpr const char* kImportPhaseDeferSentinel = "__lightjs_import_phase_defer__";

}  // namespace

}  // namespace lightjs
