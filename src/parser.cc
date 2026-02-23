#include "parser.h"
#include "lexer.h"
#include <stdexcept>
#include <cctype>
#include <set>

namespace lightjs {

namespace {

int digitValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
  return -1;
}

bool parseBigIntLiteral64(const std::string& raw, int64_t& out) {
  if (raw.empty()) return false;

  size_t i = 0;
  int base = 10;
  if (raw.size() >= 2 && raw[0] == '0') {
    if (raw[1] == 'x' || raw[1] == 'X') {
      base = 16;
      i = 2;
    } else if (raw[1] == 'o' || raw[1] == 'O') {
      base = 8;
      i = 2;
    } else if (raw[1] == 'b' || raw[1] == 'B') {
      base = 2;
      i = 2;
    }
  }

  bool sawDigit = false;
  uint64_t value = 0;
  for (; i < raw.size(); ++i) {
    char c = raw[i];
    if (c == '_') {
      continue;
    }
    int d = digitValue(c);
    if (d < 0 || d >= base) {
      return false;
    }
    sawDigit = true;
    value = value * static_cast<uint64_t>(base) + static_cast<uint64_t>(d);
  }

  if (!sawDigit) return false;
  out = static_cast<int64_t>(value);
  return true;
}

bool parseNumberLiteral(const std::string& raw, double& out) {
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

  try {
    out = std::stod(normalized);
    return true;
  } catch (...) {
    return false;
  }
}

bool isIdentifierNameToken(TokenType type) {
  switch (type) {
    case TokenType::Identifier:
    case TokenType::PrivateIdentifier:
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
      return true;
    default:
      return false;
  }
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
void collectVarDeclaredNames(const Statement& stmt, std::vector<std::string>& names) {
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.node)) {
    if (varDecl->kind == VarDeclaration::Kind::Var) {
      for (auto& decl : varDecl->declarations) {
        if (decl.pattern) collectBoundNames(*decl.pattern, names);
      }
    }
  } else if (auto* block = std::get_if<BlockStmt>(&stmt.node)) {
    for (auto& s : block->body) {
      if (s) collectVarDeclaredNames(*s, names);
    }
  } else if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    if (ifStmt->consequent) collectVarDeclaredNames(*ifStmt->consequent, names);
    if (ifStmt->alternate) collectVarDeclaredNames(*ifStmt->alternate, names);
  } else if (auto* forStmt = std::get_if<ForStmt>(&stmt.node)) {
    if (forStmt->init) collectVarDeclaredNames(*forStmt->init, names);
    if (forStmt->body) collectVarDeclaredNames(*forStmt->body, names);
  } else if (auto* whileStmt = std::get_if<WhileStmt>(&stmt.node)) {
    if (whileStmt->body) collectVarDeclaredNames(*whileStmt->body, names);
  } else if (auto* labeled = std::get_if<LabelledStmt>(&stmt.node)) {
    if (labeled->body) collectVarDeclaredNames(*labeled->body, names);
  } else if (auto* tryStmt = std::get_if<TryStmt>(&stmt.node)) {
    for (auto& s : tryStmt->block) {
      if (s) collectVarDeclaredNames(*s, names);
    }
    for (auto& s : tryStmt->handler.body) {
      if (s) collectVarDeclaredNames(*s, names);
    }
    for (auto& s : tryStmt->finalizer) {
      if (s) collectVarDeclaredNames(*s, names);
    }
  } else if (auto* switchStmt = std::get_if<SwitchStmt>(&stmt.node)) {
    for (auto& c : switchStmt->cases) {
      for (auto& s : c.consequent) {
        if (s) collectVarDeclaredNames(*s, names);
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

  if (std::holds_alternative<Identifier>(expr.node)) {
    return true;
  }
  if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
    return !member->optional;
  }

  if (auto* arr = std::get_if<ArrayExpr>(&expr.node)) {
    for (const auto& elem : arr->elements) {
      if (!elem) continue;
      if (auto* spread = std::get_if<SpreadElement>(&elem->node)) {
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
    for (const auto& prop : obj->properties) {
      if (!prop.value || !isAssignmentTarget(*prop.value)) {
        return false;
      }
    }
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

Parser::Parser(std::vector<Token> tokens, bool isModule)
  : tokens_(std::move(tokens)), isModule_(isModule), strictMode_(isModule) {}

const Token& Parser::current() const {
  if (pos_ >= tokens_.size()) {
    return tokens_.back();
  }
  return tokens_[pos_];
}

const Token& Parser::peek(size_t offset) const {
  if (pos_ + offset >= tokens_.size()) {
    return tokens_.back();
  }
  return tokens_[pos_ + offset];
}

const Token& Parser::advance() {
  if (pos_ < tokens_.size()) {
    return tokens_[pos_++];
  }
  return tokens_.back();
}

bool Parser::match(TokenType type) const {
  return current().type == type;
}

bool Parser::expect(TokenType type) {
  if (!match(type)) {
    return false;
  }
  advance();
  return true;
}

void Parser::consumeSemicolon() {
  if (match(TokenType::Semicolon)) {
    advance();
  }
}

bool Parser::canUseAwaitAsIdentifier() const {
  return asyncFunctionDepth_ == 0 && !isModule_;
}

bool Parser::canParseAwaitExpression() const {
  if (asyncFunctionDepth_ > 0) {
    return true;
  }
  return isModule_ && functionDepth_ == 0;
}

bool Parser::canUseYieldAsIdentifier() const {
  return generatorFunctionDepth_ == 0 && !strictMode_;
}

bool Parser::isIdentifierLikeToken(TokenType type) const {
  if (type == TokenType::Identifier) {
    return true;
  }
  if (type == TokenType::Await) {
    return canUseAwaitAsIdentifier();
  }
  if (type == TokenType::Yield) {
    return canUseYieldAsIdentifier();
  }
  // In non-strict mode, 'let' can be used as an identifier
  if (type == TokenType::Let && !strictMode_) {
    return true;
  }
  // 'async' is a contextual keyword, not a reserved word - can be used as identifier
  if (type == TokenType::Async) {
    return true;
  }
  // 'get', 'set', 'from', 'as', 'of', 'static' are contextual keywords
  if (type == TokenType::Get || type == TokenType::Set ||
      type == TokenType::From || type == TokenType::As ||
      type == TokenType::Of || type == TokenType::Static) {
    return true;
  }
  return false;
}

std::optional<Program> Parser::parse() {
  Program program;
  program.isModule = isModule_;
  bool inDirectivePrologue = true;
  while (!match(TokenType::EndOfFile)) {
    if (auto stmt = parseStatement()) {
      if (inDirectivePrologue) {
        bool isDirectiveString = false;
        if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt->node)) {
          if (exprStmt->expression) {
            if (auto* str = std::get_if<StringLiteral>(&exprStmt->expression->node)) {
              isDirectiveString = true;
              if (str->value == "use strict") {
                strictMode_ = true;
              }
            }
          }
        }
        if (!isDirectiveString) {
          inDirectivePrologue = false;
        }
      }
      program.body.push_back(std::move(stmt));
    } else {
      return std::nullopt;
    }
  }
  return program;
}

StmtPtr Parser::parseStatement() {
  // Parse labeled statements
  if (match(TokenType::Identifier) && peek().type == TokenType::Colon) {
    const Token& tok = current();
    std::string label = tok.value;
    advance();  // label identifier
    advance();  // :
    // let/const/class/async-function are not allowed as the body of a labeled statement
    if (match(TokenType::Let) || match(TokenType::Const) || match(TokenType::Class)) {
      error_ = true;
      return nullptr;
    }
    if (match(TokenType::Async) && peek().type == TokenType::Function) {
      error_ = true;
      return nullptr;
    }
    // In strict mode, function declarations are also not allowed
    if (strictMode_ && match(TokenType::Function)) {
      error_ = true;
      return nullptr;
    }
    auto body = parseStatement();
    if (!body) return nullptr;
    return makeStmt(LabelledStmt{label, std::move(body)}, tok);
  }

  switch (current().type) {
    case TokenType::Semicolon: {
      const Token& tok = current();
      advance();
      return makeStmt(ExpressionStmt{makeExpr(NullLiteral{}, tok)}, tok);
    }
    case TokenType::Let:
      // In non-strict mode, 'let' can be an identifier expression
      if (!strictMode_) {
        auto nextType = peek().type;
        bool nextOnSameLine = (peek().line == current().line);
        // 'let' followed by binding pattern on same line → declaration
        // 'let' followed by newline or non-pattern → identifier expression
        if (!nextOnSameLine ||
            (nextType != TokenType::Identifier && nextType != TokenType::LeftBracket &&
             nextType != TokenType::LeftBrace && nextType != TokenType::Await &&
             nextType != TokenType::Yield && nextType != TokenType::Async &&
             nextType != TokenType::Get && nextType != TokenType::Set &&
             nextType != TokenType::From && nextType != TokenType::As &&
             nextType != TokenType::Of && nextType != TokenType::Static)) {
          return parseExpressionStatement();
        }
      }
      [[fallthrough]];
    case TokenType::Const:
    case TokenType::Var:
      return parseVarDeclaration();
    case TokenType::Async:
      if (peek().type == TokenType::Function) {
        return parseFunctionDeclaration();
      }
      return parseExpressionStatement();
    case TokenType::Function:
      return parseFunctionDeclaration();
    case TokenType::Class:
      return parseClassDeclaration();
    case TokenType::Return:
      return parseReturnStatement();
    case TokenType::If:
      return parseIfStatement();
    case TokenType::While:
      return parseWhileStatement();
    case TokenType::With:
      return parseWithStatement();
    case TokenType::Do:
      return parseDoWhileStatement();
    case TokenType::For:
      return parseForStatement();
    case TokenType::Switch:
      return parseSwitchStatement();
    case TokenType::Break: {
      const Token& tok = current();
      advance();
      std::string label;
      if (match(TokenType::Identifier) && current().line == tok.line) {
        label = current().value;
        advance();
      }
      consumeSemicolon();
      return makeStmt(BreakStmt{label}, tok);
    }
    case TokenType::Continue: {
      const Token& tok = current();
      advance();
      std::string label;
      if (match(TokenType::Identifier) && current().line == tok.line) {
        label = current().value;
        advance();
      }
      consumeSemicolon();
      return makeStmt(ContinueStmt{label}, tok);
    }
    case TokenType::Throw: {
      const Token& tok = current();
      advance();
      auto argument = parseExpression();
      if (!argument) {
        return nullptr;
      }
      return makeStmt(ThrowStmt{std::move(argument)}, tok);
    }
    case TokenType::Try:
      return parseTryStatement();
    case TokenType::Import:
      if (current().escaped) {
        return nullptr;
      }
      if (peek().type == TokenType::Dot || peek().type == TokenType::LeftParen) {
        return parseExpressionStatement();
      }
      if (!isModule_) {
        return nullptr;
      }
      return parseImportDeclaration();
    case TokenType::Export:
      return parseExportDeclaration();
    case TokenType::LeftBrace:
      return parseBlockStatement();
    default:
      return parseExpressionStatement();
  }
}

StmtPtr Parser::parseVarDeclaration() {
  const Token& startTok = current();
  VarDeclaration::Kind kind;
  switch (current().type) {
    case TokenType::Let: kind = VarDeclaration::Kind::Let; break;
    case TokenType::Const: kind = VarDeclaration::Kind::Const; break;
    case TokenType::Var: kind = VarDeclaration::Kind::Var; break;
    default: return nullptr;
  }
  advance();

  VarDeclaration decl;
  decl.kind = kind;

  do {
    if (!decl.declarations.empty()) {
      if (!expect(TokenType::Comma)) {
        return nullptr;
      }
    }

    // Parse pattern (identifier, array pattern, or object pattern)
    ExprPtr pattern = parsePattern();
    if (!pattern) {
      return nullptr;
    }

    // Strict mode: reject 'eval' and 'arguments' as binding names
    if (strictMode_) {
      std::vector<std::string> boundNames;
      collectBoundNames(*pattern, boundNames);
      for (auto& name : boundNames) {
        if (name == "eval" || name == "arguments") {
          return nullptr;
        }
      }
    }

    ExprPtr init = nullptr;
    // If parsePattern() consumed the '=' and created an AssignmentPattern,
    // unwrap it: the left side is the pattern, right side is the initializer
    if (auto* assignPat = std::get_if<AssignmentPattern>(&pattern->node)) {
      init = std::move(assignPat->right);
      pattern = std::move(assignPat->left);
    } else if (match(TokenType::Equal)) {
      advance();
      init = parseExpression();
      if (!init) {
        return nullptr;
      }
    }

    // const declarations must have initializers
    if (kind == VarDeclaration::Kind::Const && !init) {
      // Check if pattern is a simple identifier (not destructuring - destructuring gets init from AssignmentPattern)
      if (std::get_if<Identifier>(&pattern->node)) {
        error_ = true;
        return nullptr;  // SyntaxError: Missing initializer in const declaration
      }
    }

    decl.declarations.push_back({std::move(pattern), std::move(init)});
  } while (match(TokenType::Comma));

  consumeSemicolon();
  return makeStmt(std::move(decl), startTok);
}

StmtPtr Parser::parseFunctionDeclaration() {
  const Token& startTok = current();
  bool isAsync = false;
  if (match(TokenType::Async)) {
    isAsync = true;
    advance();
  }

  expect(TokenType::Function);

  // Check for generator function (function*)
  bool isGenerator = false;
  if (match(TokenType::Star)) {
    isGenerator = true;
    advance();
  }

  if (!isIdentifierLikeToken(current().type)) {
    return nullptr;
  }
  std::string name = current().value;
  advance();

  ++functionDepth_;
  if (isAsync) {
    ++asyncFunctionDepth_;
  }

  expect(TokenType::LeftParen);

  std::vector<Parameter> params;
  std::optional<Identifier> restParam;

  while (!match(TokenType::RightParen)) {
    if (!params.empty()) {
      expect(TokenType::Comma);
    }

    // Check for rest parameter
    if (match(TokenType::DotDotDot)) {
      advance();
      if (isIdentifierLikeToken(current().type)) {
        restParam = Identifier{current().value};
        advance();
      }
      // Rest parameter must be last
      break;
    } else if (isIdentifierLikeToken(current().type)) {
      Parameter param;
      param.name = Identifier{current().value};
      advance();

      // Check for default value
      if (match(TokenType::Equal)) {
        advance();
        param.defaultValue = parseAssignment();
      }

      params.push_back(std::move(param));
    } else {
      // Unexpected token in parameter list
      break;
    }
  }
  expect(TokenType::RightParen);

  if (isGenerator) {
    ++generatorFunctionDepth_;
  }
  auto block = parseBlockStatement();
  if (isGenerator) {
    --generatorFunctionDepth_;
  }
  if (isAsync) {
    --asyncFunctionDepth_;
  }
  --functionDepth_;
  if (!block) {
    return nullptr;
  }

  auto blockStmt = std::get_if<BlockStmt>(&block->node);
  if (!blockStmt) {
    return nullptr;
  }

  FunctionDeclaration funcDecl;
  funcDecl.id = {name};
  funcDecl.params = std::move(params);
  funcDecl.restParam = restParam;
  funcDecl.body = std::move(blockStmt->body);
  funcDecl.isAsync = isAsync;
  funcDecl.isGenerator = isGenerator;

  return makeStmt(std::move(funcDecl), startTok);
}

StmtPtr Parser::parseClassDeclaration() {
  expect(TokenType::Class);

  if (!isIdentifierLikeToken(current().type)) {
    return nullptr;
  }
  std::string className = current().value;
  advance();

  ExprPtr superClass;
  if (match(TokenType::Extends)) {
    advance();
    superClass = parseCall();
  }

  expect(TokenType::LeftBrace);

  std::vector<MethodDefinition> methods;
  while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
    // Skip semicolons
    if (match(TokenType::Semicolon)) {
      advance();
      continue;
    }

    MethodDefinition method;

    // Check for static
    if (match(TokenType::Static)) {
      method.isStatic = true;
      advance();
    }

    // Check for async (only if followed by a method name + '(')
    if (match(TokenType::Async) && peek().type != TokenType::Semicolon &&
        peek().type != TokenType::Equal && peek().type != TokenType::RightBrace) {
      method.isAsync = true;
      advance();
    }

    // Check for getter/setter - only if followed by name + '('
    if ((match(TokenType::Get) || match(TokenType::Set)) &&
        (isIdentifierNameToken(peek().type) || peek().type == TokenType::PrivateIdentifier)) {
      // Peek ahead: if the token after the name is '(', it's a getter/setter
      // Otherwise treat get/set as a field name
      size_t saved = pos_;
      TokenType savedType = current().type;
      advance(); // consume get/set
      bool isGetterSetter = false;
      if (isIdentifierNameToken(current().type) || match(TokenType::PrivateIdentifier)) {
        advance(); // consume name
        if (match(TokenType::LeftParen)) {
          isGetterSetter = true;
        }
      }
      // Restore position
      pos_ = saved;

      if (isGetterSetter) {
        if (savedType == TokenType::Get) {
          method.kind = MethodDefinition::Kind::Get;
        } else {
          method.kind = MethodDefinition::Kind::Set;
        }
        advance(); // consume get/set
      }
    }

    // Member name (identifier or private identifier)
    if (match(TokenType::PrivateIdentifier)) {
      method.isPrivate = true;
      method.key.name = current().value; // includes '#' prefix
      advance();
    } else if (isIdentifierNameToken(current().type)) {
      std::string memberName = current().value;
      if (memberName == "constructor" && method.kind == MethodDefinition::Kind::Method) {
        method.kind = MethodDefinition::Kind::Constructor;
      }
      method.key.name = memberName;
      advance();
    } else {
      return nullptr;
    }

    // Distinguish field vs method: if '(' follows, it's a method; otherwise a field
    if (match(TokenType::LeftParen)) {
      // Method/constructor/getter/setter
      ++functionDepth_;
      if (method.isAsync) {
        ++asyncFunctionDepth_;
      }
      advance(); // consume '('
      while (!match(TokenType::RightParen)) {
        if (isIdentifierLikeToken(current().type)) {
          method.params.push_back({current().value});
          advance();
          if (match(TokenType::Comma)) {
            advance();
          }
        } else {
          break;
        }
      }
      expect(TokenType::RightParen);

      // Method body
      bool disallowSuperCall = !superClass && !method.isStatic &&
                               method.kind == MethodDefinition::Kind::Constructor;
      if (disallowSuperCall) {
        ++superCallDisallowDepth_;
      }
      expect(TokenType::LeftBrace);
      while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
        auto stmt = parseStatement();
        if (!stmt) {
          if (method.isAsync) {
            --asyncFunctionDepth_;
          }
          --functionDepth_;
          if (disallowSuperCall) {
            --superCallDisallowDepth_;
          }
          return nullptr;
        }
        method.body.push_back(std::move(stmt));
      }
      expect(TokenType::RightBrace);
      if (method.isAsync) {
        --asyncFunctionDepth_;
      }
      --functionDepth_;
      if (disallowSuperCall) {
        --superCallDisallowDepth_;
      }
    } else {
      // Field declaration
      method.kind = MethodDefinition::Kind::Field;
      if (match(TokenType::Equal)) {
        advance(); // consume '='
        method.initializer = parseAssignment();
      }
      // Consume optional semicolon
      if (match(TokenType::Semicolon)) {
        advance();
      }
    }

    methods.push_back(std::move(method));
  }
  expect(TokenType::RightBrace);

  auto decl = std::make_unique<Statement>(ClassDeclaration{
    {className},
    std::move(superClass),
    std::move(methods)
  });

  return decl;
}

StmtPtr Parser::parseReturnStatement() {
  const Token& startTok = current();
  expect(TokenType::Return);

  ExprPtr argument = nullptr;
  if (!match(TokenType::Semicolon) && !match(TokenType::EndOfFile)) {
    argument = parseAssignment();
    if (!argument) {
      return nullptr;
    }
    if (match(TokenType::Comma)) {
      std::vector<ExprPtr> sequence;
      sequence.push_back(std::move(argument));
      while (match(TokenType::Comma)) {
        advance();
        auto nextExpr = parseAssignment();
        if (!nextExpr) {
          return nullptr;
        }
        sequence.push_back(std::move(nextExpr));
      }
      argument = std::make_unique<Expression>(SequenceExpr{std::move(sequence)});
    }
  }

  consumeSemicolon();
  return makeStmt(ReturnStmt{std::move(argument)}, startTok);
}

StmtPtr Parser::parseIfStatement() {
  const Token& startTok = current();
  expect(TokenType::If);
  if (!expect(TokenType::LeftParen)) {
    return nullptr;
  }
  auto test = parseExpression();
  if (!test || !expect(TokenType::RightParen)) {
    return nullptr;
  }

  // let/const are not allowed as the body of an if statement
  if (match(TokenType::Let) || match(TokenType::Const)) {
    error_ = true;
    return nullptr;
  }
  auto consequent = parseStatement();
  if (!consequent) {
    return nullptr;
  }
  StmtPtr alternate = nullptr;

  if (match(TokenType::Else)) {
    advance();
    // let/const are not allowed as the body of an else clause
    if (match(TokenType::Let) || match(TokenType::Const)) {
      error_ = true;
      return nullptr;
    }
    alternate = parseStatement();
    if (!alternate) {
      return nullptr;
    }
  }

  return makeStmt(IfStmt{
    std::move(test),
    std::move(consequent),
    std::move(alternate)
  }, startTok);
}

StmtPtr Parser::parseWhileStatement() {
  const Token& startTok = current();
  expect(TokenType::While);
  if (!expect(TokenType::LeftParen)) {
    return nullptr;
  }
  auto test = parseExpression();
  if (!test || !expect(TokenType::RightParen)) {
    return nullptr;
  }
  // let/const/class/async-function are not allowed as the body of a while statement
  if (match(TokenType::Let) || match(TokenType::Const) || match(TokenType::Class)) {
    error_ = true;
    return nullptr;
  }
  // async function declarations not allowed in single-statement position
  if (match(TokenType::Async) && peek().type == TokenType::Function) {
    error_ = true;
    return nullptr;
  }
  auto body = parseStatement();
  if (!body) {
    return nullptr;
  }
  // Check for labeled function declarations in body (also forbidden)
  {
    const Statement* check = body.get();
    while (auto* lab = std::get_if<LabelledStmt>(&check->node)) {
      check = lab->body.get();
    }
    if (std::get_if<FunctionDeclaration>(&check->node)) {
      error_ = true;
      return nullptr;
    }
  }

  return makeStmt(WhileStmt{std::move(test), std::move(body)}, startTok);
}

StmtPtr Parser::parseWithStatement() {
  const Token& startTok = current();
  if (strictMode_) {
    return nullptr;
  }

  expect(TokenType::With);
  if (!expect(TokenType::LeftParen)) {
    return nullptr;
  }

  auto object = parseExpression();
  if (!object || !expect(TokenType::RightParen)) {
    return nullptr;
  }

  auto body = parseStatement();
  if (!body) {
    return nullptr;
  }

  return makeStmt(WithStmt{std::move(object), std::move(body)}, startTok);
}

StmtPtr Parser::parseForStatement() {
  expect(TokenType::For);
  bool isAwait = false;
  if (match(TokenType::Await)) {
    isAwait = true;
    advance();
  }
  if (!expect(TokenType::LeftParen)) {
    return nullptr;
  }

  // Check if it's a for...in or for...of loop
  size_t savedPos = pos_;
  bool isForInOrOf = false;
  bool isForOf = false;

  // Try to detect for...in or for...of pattern
  if (match(TokenType::Let) || match(TokenType::Const) || match(TokenType::Var)) {
    auto declType = current().type;
    advance();
    // In non-strict mode, 'let' directly followed by 'in' means 'let' is an identifier
    if (!strictMode_ && declType == TokenType::Let && match(TokenType::In)) {
      isForInOrOf = true;
      isForOf = false;
    } else if (!strictMode_ && declType == TokenType::Let && match(TokenType::Of)) {
      // SPEC: for (let of ...) is always a SyntaxError
      // The lookahead restriction [lookahead != let] applies to for-of LHS
      pos_ = savedPos;
      return nullptr;
    // Allow 'let' as identifier in non-strict mode (e.g., 'var let of [23]')
    } else if (match(TokenType::Identifier) || (!strictMode_ && match(TokenType::Let))) {
      advance();
      if (match(TokenType::In)) {
        isForInOrOf = true;
        isForOf = false;
      } else if (match(TokenType::Of)) {
        isForInOrOf = true;
        isForOf = true;
      }
    } else if (match(TokenType::LeftBracket) || match(TokenType::LeftBrace)) {
      // Destructuring pattern: scan ahead past balanced brackets to find 'of' or 'in'
      int depth = 1;
      advance();
      while (depth > 0 && !match(TokenType::EndOfFile)) {
        if (match(TokenType::LeftBracket) || match(TokenType::LeftBrace) || match(TokenType::LeftParen)) {
          depth++;
        } else if (match(TokenType::RightBracket) || match(TokenType::RightBrace) || match(TokenType::RightParen)) {
          depth--;
        }
        advance();
      }
      if (match(TokenType::In)) {
        isForInOrOf = true;
        isForOf = false;
      } else if (match(TokenType::Of)) {
        isForInOrOf = true;
        isForOf = true;
      }
    }
  } else if (match(TokenType::Import) &&
             peek().type == TokenType::Dot &&
             peek(2).type == TokenType::Identifier &&
             peek(2).value == "meta" &&
             peek(3).type == TokenType::In) {
    isForInOrOf = true;
    isForOf = false;
  } else if (match(TokenType::Import) &&
             peek().type == TokenType::Dot &&
             peek(2).type == TokenType::Identifier &&
             peek(2).value == "meta" &&
             peek(3).type == TokenType::Of) {
    isForInOrOf = true;
    isForOf = true;
  } else if (match(TokenType::LeftBracket) || match(TokenType::LeftBrace)) {
    // Bare destructuring assignment: for ([a, b] of ...) or for ({a, b} of ...)
    // Also handles MemberExpression: for ([expr][idx] in ...) or for ({}.prop in ...)
    int depth = 1;
    advance();
    while (depth > 0 && !match(TokenType::EndOfFile)) {
      if (match(TokenType::LeftBracket) || match(TokenType::LeftBrace) || match(TokenType::LeftParen)) {
        depth++;
      } else if (match(TokenType::RightBracket) || match(TokenType::RightBrace) || match(TokenType::RightParen)) {
        depth--;
      }
      advance();
    }
    // Continue past member expression chains (e.g., [let][1] or {}.prop)
    while (match(TokenType::Dot) || match(TokenType::LeftBracket)) {
      if (match(TokenType::Dot)) {
        advance();
        if (match(TokenType::Identifier) || isIdentifierLikeToken(current().type)) {
          advance();
        }
      } else {
        // Computed member access [expr]
        advance();
        int bracketDepth = 1;
        while (bracketDepth > 0 && !match(TokenType::EndOfFile)) {
          if (match(TokenType::LeftBracket)) bracketDepth++;
          else if (match(TokenType::RightBracket)) bracketDepth--;
          advance();
        }
      }
    }
    if (match(TokenType::In)) {
      isForInOrOf = true;
      isForOf = false;
    } else if (match(TokenType::Of)) {
      isForInOrOf = true;
      isForOf = true;
    }
  } else if (match(TokenType::LeftParen)) {
    // Parenthesized LHS: for ((x) of ...) or for ((x.y) in ...)
    int parenDepth = 1;
    advance();
    while (parenDepth > 0 && !match(TokenType::EndOfFile)) {
      if (match(TokenType::LeftParen)) parenDepth++;
      else if (match(TokenType::RightParen)) parenDepth--;
      advance();
    }
    // After closing paren, check for member expressions
    while (match(TokenType::Dot) || match(TokenType::LeftBracket)) {
      if (match(TokenType::Dot)) {
        advance();
        if (match(TokenType::Identifier) || isIdentifierLikeToken(current().type)) {
          advance();
        }
      } else {
        advance();
        int bracketDepth = 1;
        while (bracketDepth > 0 && !match(TokenType::EndOfFile)) {
          if (match(TokenType::LeftBracket)) bracketDepth++;
          else if (match(TokenType::RightBracket)) bracketDepth--;
          advance();
        }
      }
    }
    if (match(TokenType::In)) {
      isForInOrOf = true;
      isForOf = false;
    } else if (match(TokenType::Of)) {
      isForInOrOf = true;
      isForOf = true;
    }
  } else if (match(TokenType::Identifier) || match(TokenType::Async) || match(TokenType::Of)) {
    advance();
    // Skip member expressions (x.y, x[y], etc.)
    while (match(TokenType::Dot) || match(TokenType::LeftBracket)) {
      if (match(TokenType::Dot)) {
        advance();
        if (match(TokenType::Identifier) || isIdentifierLikeToken(current().type)) {
          advance();
        }
      } else {
        // Skip bracket content
        advance();
        int bracketDepth = 1;
        while (bracketDepth > 0 && !match(TokenType::EndOfFile)) {
          if (match(TokenType::LeftBracket)) bracketDepth++;
          else if (match(TokenType::RightBracket)) bracketDepth--;
          advance();
        }
      }
    }
    if (match(TokenType::In)) {
      isForInOrOf = true;
      isForOf = false;
    } else if (match(TokenType::Of)) {
      isForInOrOf = true;
      isForOf = true;
    }
  }

  // Reset position
  pos_ = savedPos;

  // `for await` is only valid with `of` iteration.
  if (isAwait && (!isForInOrOf || !isForOf)) {
    return nullptr;
  }

  if (isForInOrOf) {
    // Parse for...in or for...of
    StmtPtr left = nullptr;
    // In non-strict mode, 'let' directly followed by 'in' is an identifier, not a declaration
    // Note: 'let' followed by 'of' was already rejected above (spec lookahead restriction)
    bool letAsIdentifier = !strictMode_ && match(TokenType::Let) &&
      peek().type == TokenType::In;
    if (!letAsIdentifier && (match(TokenType::Let) || match(TokenType::Const) || match(TokenType::Var))) {
      VarDeclaration::Kind kind;
      switch (current().type) {
        case TokenType::Let: kind = VarDeclaration::Kind::Let; break;
        case TokenType::Const: kind = VarDeclaration::Kind::Const; break;
        case TokenType::Var: kind = VarDeclaration::Kind::Var; break;
        default: return nullptr;
      }
      advance();

      // Parse pattern (identifier, array pattern, or object pattern)
      ExprPtr pattern = parsePattern();
      if (!pattern) {
        return nullptr;
      }

      // Strict mode: reject 'eval' and 'arguments' as binding names
      if (strictMode_) {
        std::vector<std::string> boundNames;
        collectBoundNames(*pattern, boundNames);
        for (auto& name : boundNames) {
          if (name == "eval" || name == "arguments") {
            return nullptr;
          }
        }
      }

      // Check for duplicate bound names in let/const declarations
      if (kind != VarDeclaration::Kind::Var && hasDuplicateBoundNames(*pattern)) {
        return nullptr;
      }

      // BoundNames of ForDeclaration must not contain "let"
      if (kind != VarDeclaration::Kind::Var) {
        std::vector<std::string> boundNames;
        collectBoundNames(*pattern, boundNames);
        for (auto& name : boundNames) {
          if (name == "let") return nullptr;
        }
      }

      VarDeclaration decl;
      decl.kind = kind;
      decl.declarations.push_back({std::move(pattern), nullptr});
      left = std::make_unique<Statement>(std::move(decl));
    } else if (match(TokenType::LeftBracket) || match(TokenType::LeftBrace)) {
      // Check if this is a MemberExpression (e.g., [let][1]) rather than destructuring
      // by peeking past the balanced brackets to see if member access follows
      size_t peekPos = pos_;
      int peekDepth = 1;
      peekPos++; // skip opening bracket/brace
      while (peekDepth > 0 && peekPos < tokens_.size()) {
        auto t = tokens_[peekPos].type;
        if (t == TokenType::LeftBracket || t == TokenType::LeftBrace || t == TokenType::LeftParen)
          peekDepth++;
        else if (t == TokenType::RightBracket || t == TokenType::RightBrace || t == TokenType::RightParen)
          peekDepth--;
        peekPos++;
      }
      bool isMemberExpr = peekPos < tokens_.size() &&
        (tokens_[peekPos].type == TokenType::LeftBracket ||
         tokens_[peekPos].type == TokenType::Dot);

      if (isMemberExpr) {
        // Parse as MemberExpression (e.g., [let][1].prop)
        auto expr = parseMember();
        if (!expr) return nullptr;
        left = std::make_unique<Statement>(ExpressionStmt{std::move(expr)});
      } else {
        // Bare destructuring assignment pattern
        auto pattern = parsePattern();
        if (!pattern) {
          return nullptr;
        }
        // Strict mode: reject eval/arguments as assignment targets in destructuring
        if (strictMode_ && hasStrictModeInvalidTargets(*pattern)) {
          return nullptr;
        }
        left = std::make_unique<Statement>(ExpressionStmt{std::move(pattern)});
      }
    } else {
      auto expr = parseMember();
      if (!expr) {
        return nullptr;
      }
      if (!isAssignmentTarget(*expr)) {
        return nullptr;
      }
      // Strict mode: reject eval/arguments as simple assignment targets
      if (strictMode_ && hasStrictModeInvalidTargets(*expr)) {
        return nullptr;
      }
      left = std::make_unique<Statement>(ExpressionStmt{std::move(expr)});
    }

    bool isOf = match(TokenType::Of);
    if (isOf) {
      // The 'of' keyword must not contain escape sequences
      if (current().escaped) {
        return nullptr;
      }
      // SPEC: for (async of ...) is a SyntaxError (lookahead restriction)
      // But for (\u0061sync of ...) and for ((async) of ...) are allowed
      if (left) {
        auto* exprStmt = std::get_if<ExpressionStmt>(&left->node);
        if (exprStmt && exprStmt->expression) {
          auto* ident = std::get_if<Identifier>(&exprStmt->expression->node);
          if (ident && ident->name == "async") {
            // Check if the 'async' token was NOT escaped
            // We need to check if the original token was literally 'async'
            // (not an escape sequence like \u0061sync)
            // Since parseMember already consumed it, check if the token before 'of' was escaped
            if (pos_ >= 2 && tokens_[pos_ - 1].type == TokenType::Async && !tokens_[pos_ - 1].escaped) {
              return nullptr;
            }
          }
        }
      }
      advance();
    } else {
      if (!expect(TokenType::In)) {
        return nullptr;
      }
    }

    // Parse RHS expression
    // For-of only allows AssignmentExpression; for-in allows Expression (comma sequences)
    auto right = parseAssignment();
    if (!right) return nullptr;
    if (match(TokenType::Comma)) {
      if (isOf) {
        // SyntaxError: comma expression not allowed after 'of'
        return nullptr;
      }
      std::vector<ExprPtr> sequence;
      sequence.push_back(std::move(right));
      while (match(TokenType::Comma)) {
        advance();
        auto next = parseAssignment();
        if (!next) return nullptr;
        sequence.push_back(std::move(next));
      }
      right = std::make_unique<Expression>(SequenceExpr{std::move(sequence)});
    }
    if (!expect(TokenType::RightParen)) {
      return nullptr;
    }

    // Declarations are not allowed as for-in/for-of body
    if (match(TokenType::Function) || match(TokenType::Class)) {
      return nullptr;
    }
    // `let` or `const` declarations not allowed as body
    if (match(TokenType::Const)) {
      return nullptr;
    }
    if (match(TokenType::Let)) {
      // ExpressionStatement lookahead restriction: 'let [' is always forbidden
      if (peek().type == TokenType::LeftBracket) {
        return nullptr;
      }
      if (strictMode_) {
        // In strict mode, 'let' is always a declaration keyword - reject as body
        return nullptr;
      }
      // In non-strict mode, only reject 'let' followed by declaration pattern on same line
      if (peek().line == current().line &&
          (peek().type == TokenType::Identifier || peek().type == TokenType::LeftBrace)) {
        return nullptr;
      }
      // Otherwise: let as identifier expression (handled by parseStatement)
    }
    // `async function` not allowed as body
    if (match(TokenType::Async) && peek().type == TokenType::Function) {
      return nullptr;
    }

    auto body = parseStatement();
    if (!body) {
      return nullptr;
    }

    // Check for labeled function declarations in body (also forbidden)
    {
      const Statement* check = body.get();
      while (auto* lab = std::get_if<LabelledStmt>(&check->node)) {
        check = lab->body.get();
      }
      if (std::get_if<FunctionDeclaration>(&check->node)) {
        return nullptr;
      }
    }

    // Check that var declarations in body don't redeclare let/const head bindings
    if (left) {
      if (auto* varDecl = std::get_if<VarDeclaration>(&left->node)) {
        if (varDecl->kind == VarDeclaration::Kind::Let ||
            varDecl->kind == VarDeclaration::Kind::Const) {
          std::vector<std::string> headNames;
          for (auto& decl : varDecl->declarations) {
            if (decl.pattern) collectBoundNames(*decl.pattern, headNames);
          }
          std::vector<std::string> bodyVarNames;
          collectVarDeclaredNames(*body, bodyVarNames);
          std::set<std::string> headSet(headNames.begin(), headNames.end());
          for (auto& name : bodyVarNames) {
            if (headSet.count(name)) {
              return nullptr;  // var redeclares head let/const binding
            }
          }
        }
      }
    }

    if (isOf) {
      ForOfStmt forOf;
      forOf.left = std::move(left);
      forOf.right = std::move(right);
      forOf.body = std::move(body);
      forOf.isAwait = isAwait;
      return std::make_unique<Statement>(std::move(forOf));
    } else {
      return std::make_unique<Statement>(ForInStmt{
        std::move(left),
        std::move(right),
        std::move(body)
      });
    }
  }

  // Regular for loop
  StmtPtr init = nullptr;
  if (!match(TokenType::Semicolon)) {
    if (match(TokenType::Let) || match(TokenType::Const) || match(TokenType::Var)) {
      init = parseVarDeclaration();
      if (!init) {
        return nullptr;
      }
    } else {
      auto expr = parseExpression();
      if (!expr) {
        return nullptr;
      }
      if (!expect(TokenType::Semicolon)) {
        return nullptr;
      }
      init = std::make_unique<Statement>(ExpressionStmt{std::move(expr)});
    }
  } else {
    advance();
  }

  ExprPtr test = nullptr;
  if (!match(TokenType::Semicolon)) {
    test = parseExpression();
    if (!test) {
      return nullptr;
    }
  }
  if (!expect(TokenType::Semicolon)) {
    return nullptr;
  }

  ExprPtr update = nullptr;
  if (!match(TokenType::RightParen)) {
    update = parseExpression();
    if (!update) {
      return nullptr;
    }
  }
  if (!expect(TokenType::RightParen)) {
    return nullptr;
  }

  // let/const are not allowed as the body of a for statement
  if (match(TokenType::Let) || match(TokenType::Const)) {
    error_ = true;
    return nullptr;
  }
  auto body = parseStatement();
  if (!body) {
    return nullptr;
  }

  return std::make_unique<Statement>(ForStmt{
    std::move(init),
    std::move(test),
    std::move(update),
    std::move(body)
  });
}

StmtPtr Parser::parseDoWhileStatement() {
  expect(TokenType::Do);
  // let/const/class/async-function are not allowed as the body of a do-while statement
  if (match(TokenType::Let) || match(TokenType::Const) || match(TokenType::Class)) {
    error_ = true;
    return nullptr;
  }
  if (match(TokenType::Async) && peek().type == TokenType::Function) {
    error_ = true;
    return nullptr;
  }
  auto body = parseStatement();
  if (!body) {
    return nullptr;
  }
  // Check for labeled function declarations in body (also forbidden)
  {
    const Statement* check = body.get();
    while (auto* lab = std::get_if<LabelledStmt>(&check->node)) {
      check = lab->body.get();
    }
    if (std::get_if<FunctionDeclaration>(&check->node)) {
      error_ = true;
      return nullptr;
    }
  }
  if (!expect(TokenType::While) || !expect(TokenType::LeftParen)) {
    return nullptr;
  }
  auto test = parseExpression();
  if (!test || !expect(TokenType::RightParen)) {
    return nullptr;
  }
  consumeSemicolon();

  return std::make_unique<Statement>(DoWhileStmt{std::move(body), std::move(test)});
}

StmtPtr Parser::parseSwitchStatement() {
  expect(TokenType::Switch);
  expect(TokenType::LeftParen);
  auto discriminant = parseExpression();
  expect(TokenType::RightParen);
  expect(TokenType::LeftBrace);

  std::vector<SwitchCase> cases;
  while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
    if (match(TokenType::Case)) {
      advance();
      auto test = parseExpression();
      expect(TokenType::Colon);

      std::vector<StmtPtr> consequent;
      while (!match(TokenType::Case) && !match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
        // Check for 'default' keyword
        if (match(TokenType::Default)) {
          break;
        }
        if (error_) return nullptr;
        if (auto stmt = parseStatement()) {
          consequent.push_back(std::move(stmt));
        } else if (error_) {
          return nullptr;
        }
      }

      cases.push_back(SwitchCase{std::move(test), std::move(consequent)});
    } else if (match(TokenType::Default)) {
      advance();
      expect(TokenType::Colon);

      std::vector<StmtPtr> consequent;
      while (!match(TokenType::Case) && !match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
        if (error_) return nullptr;
        if (auto stmt = parseStatement()) {
          consequent.push_back(std::move(stmt));
        } else if (error_) {
          return nullptr;
        }
      }

      cases.push_back(SwitchCase{nullptr, std::move(consequent)});
    } else {
      break;
    }
  }

  expect(TokenType::RightBrace);
  return std::make_unique<Statement>(SwitchStmt{std::move(discriminant), std::move(cases)});
}

StmtPtr Parser::parseBlockStatement() {
  expect(TokenType::LeftBrace);

  std::vector<StmtPtr> body;
  while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
    auto stmt = parseStatement();
    if (!stmt) {
      return nullptr;
    }
    body.push_back(std::move(stmt));
  }

  expect(TokenType::RightBrace);
  return std::make_unique<Statement>(BlockStmt{std::move(body)});
}

StmtPtr Parser::parseExpressionStatement() {
  auto expr = parseAssignment();
  if (!expr) {
    return nullptr;
  }
  if (auto* id = std::get_if<Identifier>(&expr->node)) {
    if (id->name == "await" && pos_ > 0) {
      const Token& lastTok = tokens_[pos_ - 1];
      if (current().line == lastTok.line) {
        switch (current().type) {
          case TokenType::Number:
          case TokenType::BigInt:
          case TokenType::String:
          case TokenType::TemplateLiteral:
          case TokenType::Regex:
          case TokenType::True:
          case TokenType::False:
          case TokenType::Null:
          case TokenType::Undefined:
          case TokenType::Identifier:
          case TokenType::Await:
          case TokenType::Yield:
          case TokenType::This:
          case TokenType::Function:
          case TokenType::Async:
          case TokenType::Class:
          case TokenType::New:
          case TokenType::LeftBrace:
          case TokenType::LeftBracket:
            return nullptr;
          default:
            break;
        }
      }
    }
  }
  if (match(TokenType::Comma)) {
    std::vector<ExprPtr> sequence;
    sequence.push_back(std::move(expr));
    while (match(TokenType::Comma)) {
      advance();
      auto nextExpr = parseAssignment();
      if (!nextExpr) {
        return nullptr;
      }
      sequence.push_back(std::move(nextExpr));
    }
    expr = std::make_unique<Expression>(SequenceExpr{std::move(sequence)});
  }
  consumeSemicolon();
  return std::make_unique<Statement>(ExpressionStmt{std::move(expr)});
}

StmtPtr Parser::parseTryStatement() {
  expect(TokenType::Try);
  auto tryBlock = parseBlockStatement();
  if (!tryBlock) {
    return nullptr;
  }

  auto tryBlockStmt = std::get_if<BlockStmt>(&tryBlock->node);
  if (!tryBlockStmt) {
    return nullptr;
  }

  CatchClause handler;
  bool hasHandler = false;
  std::vector<StmtPtr> finalizer;
  bool hasFinalizer = false;

  if (match(TokenType::Catch)) {
    advance();
    hasHandler = true;

    if (match(TokenType::LeftParen)) {
      advance();
      // catch() with empty parens is a SyntaxError (distinct from catch {})
      if (match(TokenType::RightParen)) {
        error_ = true;
        return nullptr;
      }
      auto pattern = parsePattern();
      if (!pattern) {
        return nullptr;
      }
      if (auto* id = std::get_if<Identifier>(&pattern->node)) {
        handler.param = {id->name};
      } else {
        handler.paramPattern = std::move(pattern);
      }
      expect(TokenType::RightParen);
    }

    auto catchBlock = parseBlockStatement();
    if (!catchBlock) {
      return nullptr;
    }
    auto catchBlockStmt = std::get_if<BlockStmt>(&catchBlock->node);
    if (!catchBlockStmt) {
      return nullptr;
    }
    handler.body = std::move(catchBlockStmt->body);
  }

  if (match(TokenType::Finally)) {
    advance();
    // finally must NOT have parameters: finally(e){} is a SyntaxError
    if (match(TokenType::LeftParen)) {
      error_ = true;
      return nullptr;
    }
    hasFinalizer = true;
    auto finallyBlock = parseBlockStatement();
    if (!finallyBlock) {
      return nullptr;
    }
    auto finallyBlockStmt = std::get_if<BlockStmt>(&finallyBlock->node);
    if (!finallyBlockStmt) {
      return nullptr;
    }
    finalizer = std::move(finallyBlockStmt->body);
  }

  if (!hasHandler && !hasFinalizer) {
    return nullptr;
  }

  return std::make_unique<Statement>(TryStmt{
    std::move(tryBlockStmt->body),
    std::move(handler),
    std::move(finalizer),
    hasHandler,
    hasFinalizer
  });
}

StmtPtr Parser::parseImportDeclaration() {
  expect(TokenType::Import);

  ImportDeclaration import;

  // Check for default import: import foo from "module"
  if (match(TokenType::Identifier)) {
    import.defaultImport = Identifier{current().value};
    advance();

    // Check for additional named imports: import foo, { bar } from "module"
    if (match(TokenType::Comma)) {
      advance();
    }
  }

  // Check for namespace import: import * as name from "module"
  if (match(TokenType::Star)) {
    advance();
    if (!expect(TokenType::As)) {
      return nullptr;
    }
    if (!isIdentifierLikeToken(current().type)) {
      return nullptr;
    }
    import.namespaceImport = Identifier{current().value};
    advance();
  }
  // Check for named imports: import { foo, bar as baz } from "module"
  else if (match(TokenType::LeftBrace)) {
    advance();

    while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
      if (!import.specifiers.empty()) {
        if (!expect(TokenType::Comma)) {
          return nullptr;
        }
      }

      if (!isIdentifierNameToken(current().type)) {
        return nullptr;
      }

      ImportSpecifier spec;
      spec.imported = Identifier{current().value};
      spec.local = spec.imported;
      advance();

      // Check for renaming: foo as bar
      if (match(TokenType::As)) {
        advance();
        if (!isIdentifierLikeToken(current().type)) {
          return nullptr;
        }
        spec.local = Identifier{current().value};
        advance();
      }

      import.specifiers.push_back(spec);
    }

    expect(TokenType::RightBrace);
  }

  // Expect 'from' keyword
  expect(TokenType::From);

  // Expect module source string
  if (match(TokenType::String)) {
    import.source = current().value;
    advance();
  }

  consumeSemicolon();

  return std::make_unique<Statement>(std::move(import));
}

StmtPtr Parser::parseExportDeclaration() {
  expect(TokenType::Export);

  // Export default declaration
  if (match(TokenType::Default)) {
    advance();

    ExportDefaultDeclaration exportDefault;

    // Can be expression or function/class declaration
    if (match(TokenType::Function) || match(TokenType::Async)) {
      exportDefault.declaration = parseFunctionExpression();
    } else {
      exportDefault.declaration = parseAssignment();
    }

    consumeSemicolon();
    return std::make_unique<Statement>(std::move(exportDefault));
  }

  // Export all declaration: export * from "module" or export * as name from "module"
  if (match(TokenType::Star)) {
    advance();

    ExportAllDeclaration exportAll;

    if (match(TokenType::As)) {
      advance();
      if (!isIdentifierNameToken(current().type)) {
        return nullptr;
      }
      exportAll.exported = Identifier{current().value};
      advance();
    }

    expect(TokenType::From);

    if (match(TokenType::String)) {
      exportAll.source = current().value;
      advance();
    }

    consumeSemicolon();
    return std::make_unique<Statement>(std::move(exportAll));
  }

  // Export named declaration
  ExportNamedDeclaration exportNamed;

  // Export variable/function declaration
  if (match(TokenType::Const) || match(TokenType::Let) ||
      match(TokenType::Var) || match(TokenType::Function) ||
      match(TokenType::Async) || match(TokenType::Class)) {
    exportNamed.declaration = parseStatement();
    return std::make_unique<Statement>(std::move(exportNamed));
  }

  // Export list: export { foo, bar as baz }
  if (match(TokenType::LeftBrace)) {
    advance();

    while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
      if (!exportNamed.specifiers.empty()) {
        if (!expect(TokenType::Comma)) {
          return nullptr;
        }
      }

      if (!isIdentifierNameToken(current().type)) {
        return nullptr;
      }

      ExportSpecifier spec;
      spec.local = Identifier{current().value};
      spec.exported = spec.local;
      advance();

      // Check for renaming: foo as bar
      if (match(TokenType::As)) {
        advance();
        if (!isIdentifierNameToken(current().type)) {
          return nullptr;
        }
        spec.exported = Identifier{current().value};
        advance();
      }

      exportNamed.specifiers.push_back(spec);
    }

    expect(TokenType::RightBrace);

    // Check for re-export: export { foo } from "module"
    if (match(TokenType::From)) {
      advance();
      if (match(TokenType::String)) {
        exportNamed.source = current().value;
        advance();
      }
    }

    consumeSemicolon();
  }

  return std::make_unique<Statement>(std::move(exportNamed));
}

ExprPtr Parser::parseExpression() {
  return parseAssignment();
}

ExprPtr Parser::parseAssignment() {
  auto parseArrowBodyInto = [&](FunctionExpr& func) -> bool {
    if (match(TokenType::LeftBrace)) {
      auto blockStmt = parseBlockStatement();
      if (!blockStmt) {
        return false;
      }
      if (auto* block = std::get_if<BlockStmt>(&blockStmt->node)) {
        func.body = std::move(block->body);
        return true;
      }
      return false;
    }

    auto expr = parseAssignment();
    if (!expr) {
      return false;
    }
    auto returnStmt = std::make_unique<Statement>(ReturnStmt{std::move(expr)});
    func.body.push_back(std::move(returnStmt));
    return true;
  };

  auto prependDestructurePrologue = [&](FunctionExpr& func, std::vector<StmtPtr>& prologue) {
    if (prologue.empty()) {
      return;
    }
    std::vector<StmtPtr> newBody;
    newBody.reserve(prologue.size() + func.body.size());
    for (auto& stmt : prologue) {
      newBody.push_back(std::move(stmt));
    }
    for (auto& stmt : func.body) {
      newBody.push_back(std::move(stmt));
    }
    func.body = std::move(newBody);
  };

  // Case 0: async arrow functions
  if (match(TokenType::Async)) {
    size_t savedPos = pos_;
    advance();  // async

    // async x => ...
    if (match(TokenType::Identifier)) {
      auto paramName = current().value;
      advance();
      if (match(TokenType::Arrow)) {
        advance();
        FunctionExpr func;
        func.isAsync = true;
        func.isArrow = true;
        Parameter param;
        param.name = Identifier{paramName};
        func.params.push_back(std::move(param));
        ++functionDepth_;
        ++asyncFunctionDepth_;
        if (!parseArrowBodyInto(func)) {
          --asyncFunctionDepth_;
          --functionDepth_;
          return nullptr;
        }
        --asyncFunctionDepth_;
        --functionDepth_;
        return std::make_unique<Expression>(std::move(func));
      }
      pos_ = savedPos;
    } else if (match(TokenType::LeftParen)) {
      // async (...) => ...
      int prevFunctionDepth = functionDepth_;
      int prevAsyncDepth = asyncFunctionDepth_;
      ++functionDepth_;
      ++asyncFunctionDepth_;
      advance();
      std::vector<Parameter> params;
      std::optional<Identifier> restParam;
      std::vector<StmtPtr> destructurePrologue;
      bool validParams = true;

      if (!match(TokenType::RightParen)) {
        do {
          if (match(TokenType::DotDotDot)) {
            advance();
            if (!isIdentifierLikeToken(current().type)) {
              validParams = false;
              break;
            }
            restParam = Identifier{current().value};
            advance();
            break;
          }

          Parameter param;
          bool hasDestructurePattern = false;
          if (isIdentifierLikeToken(current().type)) {
            param.name = Identifier{current().value};
            advance();
          } else if (match(TokenType::LeftBracket) || match(TokenType::LeftBrace)) {
            auto pattern = parsePattern();
            if (!pattern) {
              validParams = false;
              break;
            }
            std::string tempName = "__arrow_param_" + std::to_string(arrowDestructureTempCounter_++);
            param.name = Identifier{tempName};
            hasDestructurePattern = true;

            VarDeclaration destructDecl;
            destructDecl.kind = VarDeclaration::Kind::Let;
            VarDeclarator destructBinding;
            destructBinding.pattern = std::move(pattern);
            destructBinding.init = std::make_unique<Expression>(Identifier{tempName});
            destructDecl.declarations.push_back(std::move(destructBinding));
            destructurePrologue.push_back(std::make_unique<Statement>(std::move(destructDecl)));
          } else {
            validParams = false;
            break;
          }

          if (match(TokenType::Equal)) {
            if (hasDestructurePattern) {
              validParams = false;
              break;
            }
            advance();
            param.defaultValue = parseAssignment();
            if (!param.defaultValue) {
              --asyncFunctionDepth_;
              --functionDepth_;
              return nullptr;
            }
          }

          params.push_back(std::move(param));

          if (match(TokenType::Comma)) {
            advance();
          } else {
            break;
          }
        } while (true);
      }

      if (validParams && match(TokenType::RightParen)) {
        advance();
        if (match(TokenType::Arrow)) {
          advance();
          FunctionExpr func;
          func.isAsync = true;
          func.isArrow = true;
          func.params = std::move(params);
          func.restParam = restParam;
          if (!parseArrowBodyInto(func)) {
            --asyncFunctionDepth_;
            --functionDepth_;
            return nullptr;
          }
          prependDestructurePrologue(func, destructurePrologue);
          --asyncFunctionDepth_;
          --functionDepth_;
          return std::make_unique<Expression>(std::move(func));
        }
      }
      functionDepth_ = prevFunctionDepth;
      asyncFunctionDepth_ = prevAsyncDepth;
      pos_ = savedPos;
    } else {
      pos_ = savedPos;
    }
  }

  // Check for arrow functions
  // Case 1: Single parameter without parentheses (e.g., x => x * 2)
  if (match(TokenType::Identifier)) {
    size_t savedPos = pos_;
    auto paramName = current().value;
    advance();

    if (match(TokenType::Arrow)) {
      advance(); // consume =>

      FunctionExpr func;
      func.isArrow = true;
      Parameter param;
      param.name = Identifier{paramName};
      func.params.push_back(std::move(param));

      ++functionDepth_;
      if (!parseArrowBodyInto(func)) {
        --functionDepth_;
        return nullptr;
      }
      --functionDepth_;

      return std::make_unique<Expression>(std::move(func));
    }

    // Not an arrow function, restore position
    pos_ = savedPos;
  }

  // Case 2: Parenthesized parameters (e.g., (x, y) => x + y) or (x) => x * 2
  if (match(TokenType::LeftParen)) {
    size_t savedPos = pos_;
    advance();

    // Try to parse as arrow function parameters
    std::vector<Parameter> params;
    std::optional<Identifier> restParam;
    std::vector<StmtPtr> destructurePrologue;
    bool isArrowFunc = false;

    if (!match(TokenType::RightParen)) {
      // Parse parameter list
      do {
        if (match(TokenType::DotDotDot)) {
          // Rest parameter
          advance();
          if (!match(TokenType::Identifier)) {
            // Not a valid arrow function, restore
            pos_ = savedPos;
            goto normal_parse;
          }
          restParam = Identifier{current().value};
          advance();
          break;
        }

        Parameter param;
        bool hasDestructurePattern = false;
        if (match(TokenType::Identifier)) {
          param.name = Identifier{current().value};
          advance();
        } else if (match(TokenType::LeftBracket) || match(TokenType::LeftBrace)) {
          auto pattern = parsePattern();
          if (!pattern) {
            pos_ = savedPos;
            goto normal_parse;
          }
          std::string tempName = "__arrow_param_" + std::to_string(arrowDestructureTempCounter_++);
          param.name = Identifier{tempName};
          hasDestructurePattern = true;

          VarDeclaration destructDecl;
          destructDecl.kind = VarDeclaration::Kind::Let;
          VarDeclarator destructBinding;
          destructBinding.pattern = std::move(pattern);
          destructBinding.init = std::make_unique<Expression>(Identifier{tempName});
          destructDecl.declarations.push_back(std::move(destructBinding));
          destructurePrologue.push_back(std::make_unique<Statement>(std::move(destructDecl)));
        } else {
          // Not a valid arrow function, restore
          pos_ = savedPos;
          goto normal_parse;
        }

        // Check for default value (arrow functions support defaults)
        if (match(TokenType::Equal)) {
          if (hasDestructurePattern) {
            pos_ = savedPos;
            goto normal_parse;
          }
          advance();
          param.defaultValue = parseAssignment();
          if (!param.defaultValue) {
            return nullptr;
          }
        }

        params.push_back(std::move(param));

        if (match(TokenType::Comma)) {
          advance();
        } else {
          break;
        }
      } while (true);
    }

    if (match(TokenType::RightParen)) {
      advance();
      if (match(TokenType::Arrow)) {
        isArrowFunc = true;
      }
    }

    if (isArrowFunc) {
      advance(); // consume =>

      FunctionExpr func;
      func.isArrow = true;
      func.params = std::move(params);
      func.restParam = restParam;

      ++functionDepth_;
      if (!parseArrowBodyInto(func)) {
        --functionDepth_;
        return nullptr;
      }
      prependDestructurePrologue(func, destructurePrologue);
      --functionDepth_;

      return std::make_unique<Expression>(std::move(func));
    }

    // Not an arrow function, restore
    pos_ = savedPos;
  }

normal_parse:
  auto left = parseConditional();
  if (!left) {
    return nullptr;
  }

  if (match(TokenType::Equal) || match(TokenType::PlusEqual) ||
      match(TokenType::MinusEqual) || match(TokenType::StarEqual) ||
      match(TokenType::SlashEqual) || match(TokenType::PercentEqual) ||
      match(TokenType::StarStarEqual) || match(TokenType::AmpEqual) ||
      match(TokenType::PipeEqual) || match(TokenType::CaretEqual) ||
      match(TokenType::LeftShiftEqual) || match(TokenType::RightShiftEqual) ||
      match(TokenType::UnsignedRightShiftEqual) ||
      match(TokenType::AmpAmpEqual) ||
      match(TokenType::PipePipeEqual) || match(TokenType::QuestionQuestionEqual)) {
    if (!left || !isAssignmentTarget(*left)) {
      return nullptr;
    }

    // In strict mode, 'arguments' and 'eval' cannot be assignment targets
    if (strictMode_) {
      if (auto* ident = std::get_if<Identifier>(&left->node)) {
        if (ident->name == "arguments" || ident->name == "eval") {
          return nullptr;  // SyntaxError
        }
      }
    }

    AssignmentExpr::Op op;
    switch (current().type) {
      case TokenType::Equal: op = AssignmentExpr::Op::Assign; break;
      case TokenType::PlusEqual: op = AssignmentExpr::Op::AddAssign; break;
      case TokenType::MinusEqual: op = AssignmentExpr::Op::SubAssign; break;
      case TokenType::StarEqual: op = AssignmentExpr::Op::MulAssign; break;
      case TokenType::SlashEqual: op = AssignmentExpr::Op::DivAssign; break;
      case TokenType::PercentEqual: op = AssignmentExpr::Op::ModAssign; break;
      case TokenType::StarStarEqual: op = AssignmentExpr::Op::ExpAssign; break;
      case TokenType::AmpEqual: op = AssignmentExpr::Op::BitwiseAndAssign; break;
      case TokenType::PipeEqual: op = AssignmentExpr::Op::BitwiseOrAssign; break;
      case TokenType::CaretEqual: op = AssignmentExpr::Op::BitwiseXorAssign; break;
      case TokenType::LeftShiftEqual: op = AssignmentExpr::Op::LeftShiftAssign; break;
      case TokenType::RightShiftEqual: op = AssignmentExpr::Op::RightShiftAssign; break;
      case TokenType::UnsignedRightShiftEqual: op = AssignmentExpr::Op::UnsignedRightShiftAssign; break;
      case TokenType::AmpAmpEqual: op = AssignmentExpr::Op::AndAssign; break;
      case TokenType::PipePipeEqual: op = AssignmentExpr::Op::OrAssign; break;
      case TokenType::QuestionQuestionEqual: op = AssignmentExpr::Op::NullishAssign; break;
      default: op = AssignmentExpr::Op::Assign; break;
    }
    advance();

    auto right = parseAssignment();
    if (!right) {
      return nullptr;
    }
    return std::make_unique<Expression>(AssignmentExpr{op, std::move(left), std::move(right)});
  }

  return left;
}

ExprPtr Parser::parseConditional() {
  auto expr = parseNullishCoalescing();
  if (!expr) {
    return nullptr;
  }

  if (match(TokenType::Question)) {
    advance();
    auto consequent = parseExpression();
    if (!consequent || !expect(TokenType::Colon)) {
      return nullptr;
    }
    auto alternate = parseExpression();
    if (!alternate) {
      return nullptr;
    }
    return std::make_unique<Expression>(ConditionalExpr{
      std::move(expr),
      std::move(consequent),
      std::move(alternate)
    });
  }

  return expr;
}

ExprPtr Parser::parseNullishCoalescing() {
  auto left = parseLogicalOr();
  if (!left) {
    return nullptr;
  }

  while (match(TokenType::QuestionQuestion)) {
    if (hasUnparenthesizedLogicalOp(left)) {
      return nullptr;
    }
    advance();
    auto right = parseLogicalOr();
    if (!right) {
      return nullptr;
    }
    if (hasUnparenthesizedLogicalOp(right)) {
      return nullptr;
    }
    left = std::make_unique<Expression>(BinaryExpr{
      BinaryExpr::Op::NullishCoalescing,
      std::move(left),
      std::move(right)
    });
  }

  return left;
}

ExprPtr Parser::parseLogicalOr() {
  auto left = parseLogicalAnd();
  if (!left) {
    return nullptr;
  }

  while (match(TokenType::PipePipe)) {
    advance();
    auto right = parseLogicalAnd();
    if (!right) {
      return nullptr;
    }
    left = std::make_unique<Expression>(BinaryExpr{
      BinaryExpr::Op::LogicalOr,
      std::move(left),
      std::move(right)
    });
  }

  return left;
}

ExprPtr Parser::parseLogicalAnd() {
  auto left = parseBitwiseOr();
  if (!left) {
    return nullptr;
  }

  while (match(TokenType::AmpAmp)) {
    advance();
    auto right = parseBitwiseOr();
    if (!right) {
      return nullptr;
    }
    left = std::make_unique<Expression>(BinaryExpr{
      BinaryExpr::Op::LogicalAnd,
      std::move(left),
      std::move(right)
    });
  }

  return left;
}

ExprPtr Parser::parseBitwiseOr() {
  auto left = parseBitwiseXor();
  if (!left) {
    return nullptr;
  }

  while (match(TokenType::Pipe)) {
    advance();
    auto right = parseBitwiseXor();
    if (!right) {
      return nullptr;
    }
    left = std::make_unique<Expression>(BinaryExpr{
      BinaryExpr::Op::BitwiseOr,
      std::move(left),
      std::move(right)
    });
  }

  return left;
}

ExprPtr Parser::parseBitwiseXor() {
  auto left = parseBitwiseAnd();
  if (!left) {
    return nullptr;
  }

  while (match(TokenType::Caret)) {
    advance();
    auto right = parseBitwiseAnd();
    if (!right) {
      return nullptr;
    }
    left = std::make_unique<Expression>(BinaryExpr{
      BinaryExpr::Op::BitwiseXor,
      std::move(left),
      std::move(right)
    });
  }

  return left;
}

ExprPtr Parser::parseBitwiseAnd() {
  auto left = parseEquality();
  if (!left) {
    return nullptr;
  }

  while (match(TokenType::Amp)) {
    advance();
    auto right = parseEquality();
    if (!right) {
      return nullptr;
    }
    left = std::make_unique<Expression>(BinaryExpr{
      BinaryExpr::Op::BitwiseAnd,
      std::move(left),
      std::move(right)
    });
  }

  return left;
}

ExprPtr Parser::parseEquality() {
  auto left = parseRelational();
  if (!left) {
    return nullptr;
  }

  while (match(TokenType::EqualEqual) || match(TokenType::EqualEqualEqual) ||
         match(TokenType::BangEqual) || match(TokenType::BangEqualEqual)) {

    BinaryExpr::Op op;
    switch (current().type) {
      case TokenType::EqualEqual: op = BinaryExpr::Op::Equal; break;
      case TokenType::EqualEqualEqual: op = BinaryExpr::Op::StrictEqual; break;
      case TokenType::BangEqual: op = BinaryExpr::Op::NotEqual; break;
      case TokenType::BangEqualEqual: op = BinaryExpr::Op::StrictNotEqual; break;
      default: op = BinaryExpr::Op::Equal; break;
    }
    advance();

    auto right = parseRelational();
    if (!right) {
      return nullptr;
    }
    left = std::make_unique<Expression>(BinaryExpr{op, std::move(left), std::move(right)});
  }

  return left;
}

ExprPtr Parser::parseRelational() {
  auto left = parseShift();
  if (!left) {
    return nullptr;
  }

  while (match(TokenType::Less) || match(TokenType::Greater) ||
         match(TokenType::LessEqual) || match(TokenType::GreaterEqual) ||
         match(TokenType::In) || match(TokenType::Instanceof)) {

    BinaryExpr::Op op;
    switch (current().type) {
      case TokenType::Less: op = BinaryExpr::Op::Less; break;
      case TokenType::Greater: op = BinaryExpr::Op::Greater; break;
      case TokenType::LessEqual: op = BinaryExpr::Op::LessEqual; break;
      case TokenType::GreaterEqual: op = BinaryExpr::Op::GreaterEqual; break;
      case TokenType::In: op = BinaryExpr::Op::In; break;
      case TokenType::Instanceof: op = BinaryExpr::Op::Instanceof; break;
      default: op = BinaryExpr::Op::Less; break;
    }
    advance();

    auto right = parseShift();
    if (!right) {
      return nullptr;
    }
    left = std::make_unique<Expression>(BinaryExpr{op, std::move(left), std::move(right)});
  }

  return left;
}

ExprPtr Parser::parseShift() {
  auto left = parseAdditive();
  if (!left) {
    return nullptr;
  }

  while (match(TokenType::LeftShift) || match(TokenType::RightShift) ||
         match(TokenType::UnsignedRightShift)) {
    BinaryExpr::Op op;
    switch (current().type) {
      case TokenType::LeftShift: op = BinaryExpr::Op::LeftShift; break;
      case TokenType::RightShift: op = BinaryExpr::Op::RightShift; break;
      case TokenType::UnsignedRightShift: op = BinaryExpr::Op::UnsignedRightShift; break;
      default: op = BinaryExpr::Op::LeftShift; break;
    }
    advance();

    auto right = parseAdditive();
    if (!right) {
      return nullptr;
    }
    left = std::make_unique<Expression>(BinaryExpr{op, std::move(left), std::move(right)});
  }

  return left;
}

ExprPtr Parser::parseAdditive() {
  auto left = parseMultiplicative();
  if (!left) {
    return nullptr;
  }

  while (match(TokenType::Plus) || match(TokenType::Minus)) {
    BinaryExpr::Op op = match(TokenType::Plus) ? BinaryExpr::Op::Add : BinaryExpr::Op::Sub;
    advance();
    auto right = parseMultiplicative();
    if (!right) {
      return nullptr;
    }
    left = std::make_unique<Expression>(BinaryExpr{op, std::move(left), std::move(right)});
  }

  return left;
}

ExprPtr Parser::parseMultiplicative() {
  auto left = parseExponentiation();
  if (!left) {
    return nullptr;
  }

  while (match(TokenType::Star) || match(TokenType::Slash) || match(TokenType::Percent)) {
    BinaryExpr::Op op;
    switch (current().type) {
      case TokenType::Star: op = BinaryExpr::Op::Mul; break;
      case TokenType::Slash: op = BinaryExpr::Op::Div; break;
      case TokenType::Percent: op = BinaryExpr::Op::Mod; break;
      default: op = BinaryExpr::Op::Mul; break;
    }
    advance();
    auto right = parseExponentiation();
    if (!right) {
      return nullptr;
    }
    left = std::make_unique<Expression>(BinaryExpr{op, std::move(left), std::move(right)});
  }

  return left;
}

ExprPtr Parser::parseExponentiation() {
  auto left = parseUnary();
  if (!left) {
    return nullptr;
  }

  // Right-to-left associativity for **
  if (match(TokenType::StarStar)) {
    advance();
    auto right = parseExponentiation();  // Right associative
    if (!right) {
      return nullptr;
    }
    return std::make_unique<Expression>(BinaryExpr{BinaryExpr::Op::Exp, std::move(left), std::move(right)});
  }

  return left;
}

ExprPtr Parser::parseUnary() {
  if (match(TokenType::Await) && canParseAwaitExpression()) {
    if (current().escaped) {
      return nullptr;
    }
    advance();
    auto argument = parseUnary();
    if (!argument) {
      return nullptr;
    }
    return std::make_unique<Expression>(AwaitExpr{std::move(argument)});
  }

  if (match(TokenType::Yield) && !canUseYieldAsIdentifier()) {
    // `yield` is only an expression keyword inside generator function bodies.
    if (generatorFunctionDepth_ == 0) {
      return nullptr;
    }
    advance();
    bool delegate = false;

    // Check for yield* (delegate to another iterator)
    if (match(TokenType::Star)) {
      delegate = true;
      advance();
    }

    // yield can be used without an argument
    ExprPtr argument = nullptr;
    if (!match(TokenType::Semicolon) && !match(TokenType::RightBrace) &&
        !match(TokenType::RightParen) && !match(TokenType::Comma)) {
      argument = parseAssignment();
    }

    return std::make_unique<Expression>(YieldExpr{std::move(argument), delegate});
  }

  if (match(TokenType::Bang) || match(TokenType::Minus) ||
      match(TokenType::Plus) || match(TokenType::Typeof) ||
      match(TokenType::Void) || match(TokenType::Tilde) ||
      match(TokenType::Delete)) {

    UnaryExpr::Op op;
    switch (current().type) {
      case TokenType::Bang: op = UnaryExpr::Op::Not; break;
      case TokenType::Minus: op = UnaryExpr::Op::Minus; break;
      case TokenType::Plus: op = UnaryExpr::Op::Plus; break;
      case TokenType::Typeof: op = UnaryExpr::Op::Typeof; break;
      case TokenType::Void: op = UnaryExpr::Op::Void; break;
      case TokenType::Tilde: op = UnaryExpr::Op::BitNot; break;
      case TokenType::Delete: op = UnaryExpr::Op::Delete; break;
      default: op = UnaryExpr::Op::Not; break;
    }
    advance();

    auto argument = parseUnary();
    if (!argument) {
      return nullptr;
    }
    return std::make_unique<Expression>(UnaryExpr{op, std::move(argument)});
  }

  if (match(TokenType::PlusPlus) || match(TokenType::MinusMinus)) {
    UpdateExpr::Op op = match(TokenType::PlusPlus) ?
      UpdateExpr::Op::Increment : UpdateExpr::Op::Decrement;
    advance();
    auto argument = parseUnary();
    if (!argument || !isUpdateTarget(*argument)) {
      return nullptr;
    }
    return std::make_unique<Expression>(UpdateExpr{op, std::move(argument), true});
  }

  return parsePostfix();
}

ExprPtr Parser::parsePostfix() {
  auto expr = parseCall();

  if (match(TokenType::PlusPlus) || match(TokenType::MinusMinus)) {
    UpdateExpr::Op op = match(TokenType::PlusPlus) ?
      UpdateExpr::Op::Increment : UpdateExpr::Op::Decrement;
    advance();
    if (!expr || !isUpdateTarget(*expr)) {
      return nullptr;
    }
    return std::make_unique<Expression>(UpdateExpr{op, std::move(expr), false});
  }

  return expr;
}

ExprPtr Parser::parseCall() {
  auto expr = parsePrimary();
  if (!expr) {
    return nullptr;
  }
  expr = parseMemberSuffix(std::move(expr));
  if (!expr) {
    return nullptr;
  }

  // Track whether we're inside an optional chain for short-circuit propagation
  auto isExprInOptionalChain = [](const Expression& e) -> bool {
    if (auto* m = std::get_if<MemberExpr>(&e.node)) {
      return m->optional || m->inOptionalChain;
    }
    if (auto* c = std::get_if<CallExpr>(&e.node)) {
      return c->optional || c->inOptionalChain;
    }
    return false;
  };

  // Loop to handle both calls and member access after calls
  // This enables chaining like: arr.slice(1).length or obj.method().prop.method2()
  while (true) {
    bool optChain = expr && isExprInOptionalChain(*expr);

    if (match(TokenType::QuestionDot) && peek().type == TokenType::LeftParen) {
      optChain = true;
      advance();  // consume ?.
      advance();  // consume (
      std::vector<ExprPtr> args;

      while (!match(TokenType::RightParen)) {
        if (!args.empty()) {
          if (!expect(TokenType::Comma)) {
            return nullptr;
          }
          if (match(TokenType::RightParen)) {
            break;
          }
        }

        // Check for spread element in arguments
        if (match(TokenType::DotDotDot)) {
          advance();
          auto arg = parseExpression();
          if (!arg) {
            return nullptr;
          }
          args.push_back(std::make_unique<Expression>(SpreadElement{std::move(arg)}));
        } else {
          auto arg = parseExpression();
          if (!arg) {
            return nullptr;
          }
          args.push_back(std::move(arg));
        }
      }

      if (!expect(TokenType::RightParen)) {
        return nullptr;
      }
      CallExpr call;
      call.callee = std::move(expr);
      call.arguments = std::move(args);
      call.optional = true;
      call.inOptionalChain = true;
      expr = std::make_unique<Expression>(std::move(call));
      expr = parseMemberSuffix(std::move(expr), true);
      if (!expr) {
        return nullptr;
      }
    } else if (match(TokenType::LeftParen)) {
      if (superCallDisallowDepth_ > 0 &&
          expr && std::holds_alternative<SuperExpr>(expr->node)) {
        return nullptr;
      }
      advance();
      std::vector<ExprPtr> args;

      while (!match(TokenType::RightParen)) {
        if (!args.empty()) {
          if (!expect(TokenType::Comma)) {
            return nullptr;
          }
          if (match(TokenType::RightParen)) {
            break;
          }
        }

        // Check for spread element in arguments
        if (match(TokenType::DotDotDot)) {
          advance();
          auto arg = parseExpression();
          if (!arg) {
            return nullptr;
          }
          args.push_back(std::make_unique<Expression>(SpreadElement{std::move(arg)}));
        } else {
          auto arg = parseExpression();
          if (!arg) {
            return nullptr;
          }
          args.push_back(std::move(arg));
        }
      }

      if (!expect(TokenType::RightParen)) {
        return nullptr;
      }
      CallExpr call;
      call.callee = std::move(expr);
      call.arguments = std::move(args);
      if (auto* member = std::get_if<MemberExpr>(&call.callee->node)) {
        call.optional = member->optional;
      }
      call.inOptionalChain = optChain;
      expr = std::make_unique<Expression>(std::move(call));
      // Handle member access after a call (e.g., arr.slice(1).length)
      expr = parseMemberSuffix(std::move(expr), optChain);
      if (!expr) {
        return nullptr;
      }
    } else if (match(TokenType::TemplateLiteral)) {
      if (isOptionalChain(*expr)) {
        return nullptr;
      }
      // Tagged template call: tag`...`
      std::vector<ExprPtr> args;
      auto templateArg = parsePrimary();
      if (!templateArg) {
        return nullptr;
      }
      if (auto* templateLiteral = std::get_if<TemplateLiteral>(&templateArg->node)) {
        ArrayExpr templateParts;
        for (const auto& quasi : templateLiteral->quasis) {
          templateParts.elements.push_back(
            std::make_unique<Expression>(StringLiteral{quasi}));
        }
        args.push_back(std::make_unique<Expression>(std::move(templateParts)));
        for (auto& subst : templateLiteral->expressions) {
          args.push_back(std::move(subst));
        }
      } else {
        args.push_back(std::move(templateArg));
      }

      CallExpr call;
      call.callee = std::move(expr);
      call.arguments = std::move(args);
      if (auto* member = std::get_if<MemberExpr>(&call.callee->node)) {
        call.optional = member->optional;
      }
      call.inOptionalChain = optChain;
      expr = std::make_unique<Expression>(std::move(call));
      expr = parseMemberSuffix(std::move(expr), optChain);
      if (!expr) {
        return nullptr;
      }
    } else {
      break;
    }
  }

  return expr;
}

ExprPtr Parser::parseMember() {
  auto expr = parsePrimary();
  return parseMemberSuffix(std::move(expr));
}

ExprPtr Parser::parseMemberSuffix(ExprPtr expr, bool inOptionalChain) {
  if (!expr) {
    return nullptr;
  }
  while (true) {
    if (match(TokenType::QuestionDot) && peek().type == TokenType::LeftParen) {
      break;
    }

    if (match(TokenType::QuestionDot)) {
      inOptionalChain = true;
      advance();
      if (match(TokenType::LeftBracket)) {
        advance();
        auto prop = parseAssignment();
        if (!prop) {
          return nullptr;
        }
        while (match(TokenType::Comma)) {
          advance();
          prop = parseAssignment();
          if (!prop) {
            return nullptr;
          }
        }
        if (!expect(TokenType::RightBracket)) {
          return nullptr;
        }
        MemberExpr member;
        member.object = std::move(expr);
        member.property = std::move(prop);
        member.computed = true;
        member.optional = true;
        member.inOptionalChain = true;
        expr = std::make_unique<Expression>(std::move(member));
        continue;
      }

      // Accept identifiers OR keywords as property names
      // Keywords like catch, finally, class, etc. can be property names in JS
      if (isIdentifierNameToken(current().type)) {
        auto prop = std::make_unique<Expression>(Identifier{current().value});
        advance();
        MemberExpr member;
        member.object = std::move(expr);
        member.property = std::move(prop);
        member.computed = false;
        member.optional = true;
        member.inOptionalChain = true;
        expr = std::make_unique<Expression>(std::move(member));
        continue;
      }
      return nullptr;
    } else if (match(TokenType::Dot)) {
      advance();
      // Accept identifiers, keywords, or private identifiers as property names
      if (match(TokenType::PrivateIdentifier)) {
        // Private field access: obj.#field → MemberExpr with #-prefixed name
        auto prop = std::make_unique<Expression>(Identifier{current().value});
        advance();
        MemberExpr member;
        member.object = std::move(expr);
        member.property = std::move(prop);
        member.computed = false;
        member.optional = false;
        member.inOptionalChain = inOptionalChain;
        expr = std::make_unique<Expression>(std::move(member));
        continue;
      }
      if (isIdentifierNameToken(current().type)) {
        auto prop = std::make_unique<Expression>(Identifier{current().value});
        advance();
        MemberExpr member;
        member.object = std::move(expr);
        member.property = std::move(prop);
        member.computed = false;
        member.optional = false;
        member.inOptionalChain = inOptionalChain;
        expr = std::make_unique<Expression>(std::move(member));
        continue;
      }
      break;
    } else if (match(TokenType::LeftBracket)) {
      advance();
      auto prop = parseAssignment();
      if (!prop) {
        return nullptr;
      }
      while (match(TokenType::Comma)) {
        advance();
        prop = parseAssignment();
        if (!prop) {
          return nullptr;
        }
      }
      if (!expect(TokenType::RightBracket)) {
        return nullptr;
      }
      MemberExpr member;
      member.object = std::move(expr);
      member.property = std::move(prop);
      member.computed = true;
      member.optional = false;
      member.inOptionalChain = inOptionalChain;
      expr = std::make_unique<Expression>(std::move(member));
    } else {
      break;
    }
  }
  return expr;
}

ExprPtr Parser::parsePrimary() {
  if (match(TokenType::Number)) {
    const Token& tok = current();
    double value = 0.0;
    if (!parseNumberLiteral(tok.value, value)) {
      return nullptr;
    }
    advance();
    return makeExpr(NumberLiteral{value}, tok);
  }

  if (match(TokenType::BigInt)) {
    const Token& tok = current();
    int64_t value = 0;
    if (!parseBigIntLiteral64(tok.value, value)) {
      return nullptr;
    }
    advance();
    return makeExpr(BigIntLiteral{value}, tok);
  }

  if (match(TokenType::String)) {
    const Token& tok = current();
    std::string value = tok.value;
    advance();
    return makeExpr(StringLiteral{value}, tok);
  }

  if (match(TokenType::TemplateLiteral)) {
    const Token& tok = current();
    std::string content = tok.value;
    advance();

    // Parse the template literal content to extract quasis and expressions
    std::vector<std::string> quasis;
    std::vector<ExprPtr> expressions;

    std::string currentQuasi;
    size_t i = 0;
    while (i < content.length()) {
      if (i + 1 < content.length() && content[i] == '$' && content[i+1] == '{') {
        // Found interpolation start
        quasis.push_back(currentQuasi);
        currentQuasi.clear();

        // Find the matching closing brace
        i += 2;
        int braceCount = 1;
        std::string exprStr;
        while (i < content.length() && braceCount > 0) {
          if (content[i] == '{') braceCount++;
          else if (content[i] == '}') braceCount--;

          if (braceCount > 0) {
            exprStr += content[i];
          }
          i++;
        }

        // Parse the expression
        Lexer exprLexer(exprStr);
        auto exprTokens = exprLexer.tokenize();
        Parser exprParser(exprTokens, isModule_);
        if (auto expr = exprParser.parseExpression()) {
          expressions.push_back(std::move(expr));
        }
      } else {
        currentQuasi += content[i];
        i++;
      }
    }
    quasis.push_back(currentQuasi);

    return makeExpr(TemplateLiteral{std::move(quasis), std::move(expressions)}, tok);
  }

  if (match(TokenType::Regex)) {
    const Token& tok = current();
    std::string value = tok.value;
    advance();
    size_t sep = value.find("||");
    std::string pattern = value.substr(0, sep);
    std::string flags = (sep != std::string::npos) ? value.substr(sep + 2) : "";
    return makeExpr(RegexLiteral{pattern, flags}, tok);
  }

  if (match(TokenType::True)) {
    const Token& tok = current();
    advance();
    return makeExpr(BoolLiteral{true}, tok);
  }

  if (match(TokenType::False)) {
    const Token& tok = current();
    advance();
    return makeExpr(BoolLiteral{false}, tok);
  }

  if (match(TokenType::Null)) {
    const Token& tok = current();
    advance();
    return makeExpr(NullLiteral{}, tok);
  }

  // Handle 'undefined' keyword as identifier (it's defined in global scope)
  if (match(TokenType::Undefined)) {
    const Token& tok = current();
    advance();
    return makeExpr(Identifier{"undefined"}, tok);
  }

  if (match(TokenType::Identifier)) {
    const Token& tok = current();
    std::string name = tok.value;
    advance();
    return makeExpr(Identifier{name}, tok);
  }

  // In non-strict mode, 'let' can be used as an identifier
  if (match(TokenType::Let) && !strictMode_) {
    const Token& tok = current();
    advance();
    return makeExpr(Identifier{"let"}, tok);
  }

  // Contextual keywords can be used as identifiers
  if (match(TokenType::Get) || match(TokenType::Set) ||
      match(TokenType::From) || match(TokenType::As) ||
      match(TokenType::Of) || match(TokenType::Static)) {
    const Token& tok = current();
    advance();
    return makeExpr(Identifier{tok.value}, tok);
  }

  if (match(TokenType::Await) && canUseAwaitAsIdentifier()) {
    const Token& tok = current();
    std::string name = tok.value;
    advance();
    return makeExpr(Identifier{name}, tok);
  }

  if (match(TokenType::Yield) && canUseYieldAsIdentifier()) {
    const Token& tok = current();
    std::string name = tok.value;
    advance();
    return makeExpr(Identifier{name}, tok);
  }

  // In non-strict mode, 'let' can be used as an identifier
  if (match(TokenType::Let) && !strictMode_) {
    const Token& tok = current();
    advance();
    return makeExpr(Identifier{"let"}, tok);
  }

  // Dynamic import: import(specifier), import.meta, import.source(specifier), import.defer(specifier)
  if (match(TokenType::Import)) {
    Token importTok = current();
    bool importEscaped = importTok.escaped;
    advance();
    if (importEscaped) {
      return nullptr;
    }
    if (match(TokenType::LeftParen)) {
      auto importId = std::make_unique<Expression>(Identifier{"import"});

      advance();
      if (match(TokenType::RightParen)) {
        return nullptr;
      }

      std::vector<ExprPtr> args;
      auto specifier = parseAssignment();
      if (!specifier) {
        return nullptr;
      }
      args.push_back(std::move(specifier));

      if (match(TokenType::Comma)) {
        advance();
        if (!match(TokenType::RightParen)) {
          auto options = parseAssignment();
          if (!options) {
            return nullptr;
          }
          args.push_back(std::move(options));
          if (match(TokenType::Comma)) {
            advance();
          }
        }
      }
      if (!expect(TokenType::RightParen)) {
        return nullptr;
      }
      return std::make_unique<Expression>(CallExpr{std::move(importId), std::move(args)});
    }
    // import.meta - ES2020
    if (match(TokenType::Dot)) {
      advance(); // consume '.'
      if (match(TokenType::Identifier) && !current().escaped) {
        if (current().value == "meta") {
          if (!isModule_) {
            return nullptr;
          }
          advance(); // consume 'meta'
          return makeExpr(MetaProperty{"meta", ""}, importTok);
        }
        if (current().value == "source" || current().value == "defer") {
          const bool isSourcePhase = (current().value == "source");
          advance();  // consume 'source' or 'defer'

          if (!match(TokenType::LeftParen)) {
            return nullptr;
          }
          advance();  // consume '('
          if (match(TokenType::RightParen)) {
            return nullptr;
          }

          auto importId = std::make_unique<Expression>(Identifier{"import"});
          std::vector<ExprPtr> args;
          auto specifier = parseAssignment();
          if (!specifier) {
            return nullptr;
          }
          args.push_back(std::move(specifier));
          args.push_back(std::make_unique<Expression>(StringLiteral{
            isSourcePhase ? kImportPhaseSourceSentinel : kImportPhaseDeferSentinel
          }));

          // Keep grammar strict: import.source/defer currently accepts exactly one argument.
          if (match(TokenType::Comma)) {
            return nullptr;
          }
          if (!expect(TokenType::RightParen)) {
            return nullptr;
          }
          return std::make_unique<Expression>(CallExpr{std::move(importId), std::move(args)});
        }
      }
    }
    // If not followed by '(' or '.meta', it's a static import statement (error here)
    return nullptr;
  }

  if (match(TokenType::Async)) {
    if (!current().escaped && peek().type == TokenType::Function) {
      return parseFunctionExpression();
    }
    // 'async' used as identifier (not followed by function, or escaped)
    const Token& tok = current();
    advance();
    return makeExpr(Identifier{"async"}, tok);
  }

  if (match(TokenType::Function)) {
    return parseFunctionExpression();
  }

  if (match(TokenType::Class)) {
    return parseClassExpression();
  }

  if (match(TokenType::New)) {
    return parseNewExpression();
  }

  if (match(TokenType::This)) {
    advance();
    return std::make_unique<Expression>(ThisExpr{});
  }

  if (match(TokenType::Super)) {
    advance();
    return std::make_unique<Expression>(SuperExpr{});
  }

  if (match(TokenType::LeftParen)) {
    advance();
    auto expr = parseAssignment();
    if (!expr) {
      return nullptr;
    }
    if (match(TokenType::Comma)) {
      std::vector<ExprPtr> sequence;
      sequence.push_back(std::move(expr));
      while (match(TokenType::Comma)) {
        advance();
        auto next = parseAssignment();
        if (!next) {
          return nullptr;
        }
        sequence.push_back(std::move(next));
      }
      expr = std::make_unique<Expression>(SequenceExpr{std::move(sequence)});
    }
    if (!expect(TokenType::RightParen)) {
      return nullptr;
    }
    if (expr) {
      expr->parenthesized = true;
    }
    return expr;
  }

  if (match(TokenType::LeftBracket)) {
    return parseArrayExpression();
  }

  if (match(TokenType::LeftBrace)) {
    return parseObjectExpression();
  }

  return nullptr;
}

ExprPtr Parser::parseArrayExpression() {
  expect(TokenType::LeftBracket);

  std::vector<ExprPtr> elements;
  while (!match(TokenType::RightBracket)) {
    if (!elements.empty()) {
      expect(TokenType::Comma);
      if (match(TokenType::RightBracket)) break;
    }

    // Handle holes (elision): consecutive commas like [,] or [1,,2]
    if (match(TokenType::Comma)) {
      // Push nullptr for the hole (will become undefined)
      elements.push_back(nullptr);
      continue;
    }

    // Check for spread element
    if (match(TokenType::DotDotDot)) {
      advance();
      auto arg = parseExpression();
      elements.push_back(std::make_unique<Expression>(SpreadElement{std::move(arg)}));
    } else {
      elements.push_back(parseExpression());
    }
  }

  expect(TokenType::RightBracket);
  return std::make_unique<Expression>(ArrayExpr{std::move(elements)});
}

ExprPtr Parser::parseObjectExpression() {
  expect(TokenType::LeftBrace);

  std::vector<ObjectProperty> properties;
  while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
    if (!properties.empty()) {
      if (!expect(TokenType::Comma)) {
        return nullptr;
      }
      if (match(TokenType::RightBrace)) break;
    }

    // Check for spread syntax
    if (match(TokenType::DotDotDot)) {
      advance();
      auto spreadExpr = parseExpression();
      if (!spreadExpr) {
        return nullptr;
      }
      ObjectProperty prop;
      prop.isSpread = true;
      prop.value = std::move(spreadExpr);
      // key is nullptr for spread properties
      properties.push_back(std::move(prop));
    } else {
      ExprPtr key;
      bool isComputed = false;
      bool isGenerator = false;
      bool isAsync = false;

      // Check for async method
      if (match(TokenType::Async)) {
        isAsync = true;
        advance();
      }

      // Check for generator method: *methodName() or *[computed]()
      if (match(TokenType::Star)) {
        isGenerator = true;
        advance();
      }

      // Check for computed property name [expr]
      if (match(TokenType::LeftBracket)) {
        advance();
        key = parseExpression();
        expect(TokenType::RightBracket);
        isComputed = true;
      } else if (isIdentifierNameToken(current().type) &&
                 !match(TokenType::Get) &&
                 !match(TokenType::Set)) {
        std::string identName = current().value;
        TokenType identType = current().type;
        key = std::make_unique<Expression>(Identifier{identName});
        advance();

        // Check for shorthand property notation (only if not generator/async)
        if (!isGenerator && !isAsync && identType == TokenType::Identifier &&
            (match(TokenType::Comma) || match(TokenType::RightBrace))) {
          // Shorthand: {x} means {x: x}
          ObjectProperty prop;
          prop.key = std::move(key);
          prop.value = std::make_unique<Expression>(Identifier{identName});
          prop.isSpread = false;
          properties.push_back(std::move(prop));
          continue;
        }
      } else if (match(TokenType::Get) || match(TokenType::Set)) {
        // Handle 'get' and 'set' as property keys (contextual keywords)
        // If followed by ':', it's a regular property: {get: 42}
        // If followed by identifier and '(', it's a getter/setter: {get foo() {}}
        bool isGetter = current().type == TokenType::Get;
        std::string keyName = current().value;
        advance();

        if (match(TokenType::Colon)) {
          // Regular property with 'get' or 'set' as key name
          key = std::make_unique<Expression>(Identifier{keyName});
        } else if (match(TokenType::LeftBracket)) {
          // Computed accessor key: get [expr]() {} / set [expr](v) {}
          advance();
          key = parseExpression();
          if (!key) {
            return nullptr;
          }
          if (!expect(TokenType::RightBracket)) {
            return nullptr;
          }
          isComputed = true;

          if (!expect(TokenType::LeftParen)) {
            return nullptr;
          }
          std::vector<Parameter> params;
          if (!isGetter && match(TokenType::Identifier)) {
            Parameter param;
            param.name = Identifier{current().value};
            advance();
            params.push_back(std::move(param));
          }
          if (!expect(TokenType::RightParen)) {
            return nullptr;
          }

          if (!expect(TokenType::LeftBrace)) {
            return nullptr;
          }
          std::vector<StmtPtr> body;
          while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
            auto stmt = parseStatement();
            if (!stmt) {
              return nullptr;
            }
            body.push_back(std::move(stmt));
          }
          if (!expect(TokenType::RightBrace)) {
            return nullptr;
          }

          FunctionExpr funcExpr;
          funcExpr.params = std::move(params);
          funcExpr.body = std::move(body);
          funcExpr.isAsync = false;
          funcExpr.isGenerator = false;
          funcExpr.isArrow = false;

          ObjectProperty prop;
          prop.key = std::move(key);
          prop.value = std::make_unique<Expression>(std::move(funcExpr));
          prop.isSpread = false;
          prop.isComputed = true;
          properties.push_back(std::move(prop));
          continue;
        } else if (isIdentifierNameToken(current().type) ||
                   match(TokenType::String) ||
                   match(TokenType::Number)) {
          // Getter/setter syntax: get propName() or set propName(value)
          std::string propName = current().value;
          key = std::make_unique<Expression>(Identifier{propName});
          advance();

          // Expect '(' for the parameter list
          expect(TokenType::LeftParen);
          std::vector<Parameter> params;

          // Setters have one parameter, getters have none
          if (!isGetter && match(TokenType::Identifier)) {
            Parameter param;
            param.name = Identifier{current().value};
            advance();
            params.push_back(std::move(param));
          }
          if (!expect(TokenType::RightParen)) {
            return nullptr;
          }

          // Parse function body
          if (!expect(TokenType::LeftBrace)) {
            return nullptr;
          }
          std::vector<StmtPtr> body;
          while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
            auto stmt = parseStatement();
            if (!stmt) {
              return nullptr;
            }
            body.push_back(std::move(stmt));
          }
          if (!expect(TokenType::RightBrace)) {
            return nullptr;
          }

          // Create FunctionExpr for the getter/setter
          FunctionExpr funcExpr;
          funcExpr.params = std::move(params);
          funcExpr.body = std::move(body);
          funcExpr.isAsync = false;
          funcExpr.isGenerator = false;
          funcExpr.isArrow = false;

          // Mark the property with a special naming convention for getters/setters
          // The interpreter will need to handle this specially
          ObjectProperty prop;
          prop.key = std::move(key);
          prop.value = std::make_unique<Expression>(std::move(funcExpr));
          prop.isSpread = false;
          prop.isComputed = false;

          // Store getter/setter info in a wrapper object for interpreter
          // We'll use a special internal property name convention
          auto actualKey = std::make_unique<Expression>(Identifier{(isGetter ? "__get_" : "__set_") + propName});
          prop.key = std::move(actualKey);

          properties.push_back(std::move(prop));
          continue;
        } else if (match(TokenType::LeftParen)) {
          // Method shorthand with 'get' or 'set' as method name: {get() {...}}
          key = std::make_unique<Expression>(Identifier{keyName});
          // Don't advance - let the method parsing handle it below
        } else {
          // Just a property named 'get' or 'set' without colon (error or shorthand)
          key = std::make_unique<Expression>(Identifier{keyName});
        }
      } else if (match(TokenType::String)) {
        key = std::make_unique<Expression>(StringLiteral{current().value});
        advance();
      } else if (match(TokenType::Number)) {
        key = std::make_unique<Expression>(NumberLiteral{std::stod(current().value)});
        advance();
      }

      if (!key) {
        return nullptr;
      }

      // Check for shorthand method syntax: key() { ... } or key(params) { ... }
      if (match(TokenType::LeftParen)) {
        // This is a method shorthand
        advance();
        std::vector<Parameter> params;

        while (!match(TokenType::RightParen) && !match(TokenType::EndOfFile)) {
          if (!params.empty()) {
            expect(TokenType::Comma);
          }
          if (match(TokenType::Identifier)) {
            Parameter param;
            param.name = Identifier{current().value};
            advance();
            // Check for default value
            if (match(TokenType::Equal)) {
              advance();
              param.defaultValue = parseExpression();
            }
            params.push_back(std::move(param));
          } else if (!match(TokenType::RightParen)) {
            // Unknown token in parameter list, skip it
            advance();
          }
        }
        if (!expect(TokenType::RightParen)) {
          return nullptr;
        }

        // Parse function body
        if (isAsync) {
          ++asyncFunctionDepth_;
        }
        if (isGenerator) {
          ++generatorFunctionDepth_;
        }
        if (!expect(TokenType::LeftBrace)) {
          if (isGenerator) {
            --generatorFunctionDepth_;
          }
          if (isAsync) {
            --asyncFunctionDepth_;
          }
          return nullptr;
        }
        std::vector<StmtPtr> body;
        while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
          auto stmt = parseStatement();
          if (!stmt) {
            if (isGenerator) {
              --generatorFunctionDepth_;
            }
            if (isAsync) {
              --asyncFunctionDepth_;
            }
            return nullptr;
          }
          body.push_back(std::move(stmt));
        }
        if (!expect(TokenType::RightBrace)) {
          if (isGenerator) {
            --generatorFunctionDepth_;
          }
          if (isAsync) {
            --asyncFunctionDepth_;
          }
          return nullptr;
        }
        if (isGenerator) {
          --generatorFunctionDepth_;
        }
        if (isAsync) {
          --asyncFunctionDepth_;
        }

        // Create FunctionExpr for the method
        FunctionExpr funcExpr;
        funcExpr.params = std::move(params);
        funcExpr.body = std::move(body);
        funcExpr.isGenerator = isGenerator;
        funcExpr.isAsync = isAsync;

        ObjectProperty prop;
        prop.key = std::move(key);
        prop.value = std::make_unique<Expression>(std::move(funcExpr));
        prop.isSpread = false;
        prop.isComputed = isComputed;
        properties.push_back(std::move(prop));
      } else {
        // Regular property with colon
        if (!expect(TokenType::Colon)) {
          return nullptr;
        }
        auto value = parseExpression();
        if (!value) {
          return nullptr;
        }

        ObjectProperty prop;
        prop.key = std::move(key);
        prop.value = std::move(value);
        prop.isSpread = false;
        prop.isComputed = isComputed;
        properties.push_back(std::move(prop));
      }
    }
  }

  expect(TokenType::RightBrace);
  return std::make_unique<Expression>(ObjectExpr{std::move(properties)});
}

ExprPtr Parser::parseFunctionExpression() {
  bool isAsync = false;
  if (match(TokenType::Async)) {
    isAsync = true;
    advance();
  }

  expect(TokenType::Function);

  // Check for generator function (function*)
  bool isGenerator = false;
  if (match(TokenType::Star)) {
    isGenerator = true;
    advance();
  }

  std::string name;
  if (isIdentifierLikeToken(current().type)) {
    name = current().value;
    advance();
  }

  ++functionDepth_;
  if (isAsync) {
    ++asyncFunctionDepth_;
  }

  expect(TokenType::LeftParen);

  std::vector<Parameter> params;
  std::optional<Identifier> restParam;

  while (!match(TokenType::RightParen)) {
    if (!params.empty()) {
      expect(TokenType::Comma);
    }

    // Check for rest parameter
    if (match(TokenType::DotDotDot)) {
      advance();
      if (isIdentifierLikeToken(current().type)) {
        restParam = Identifier{current().value};
        advance();
      }
      // Rest parameter must be last
      break;
    } else if (isIdentifierLikeToken(current().type)) {
      Parameter param;
      param.name = Identifier{current().value};
      advance();

      // Check for default value
      if (match(TokenType::Equal)) {
        advance();
        param.defaultValue = parseAssignment();
      }

      params.push_back(std::move(param));
    } else {
      // Unexpected token in parameter list
      break;
    }
  }

  expect(TokenType::RightParen);

  if (isGenerator) {
    ++generatorFunctionDepth_;
  }
  auto block = parseBlockStatement();
  if (isGenerator) {
    --generatorFunctionDepth_;
  }
  if (isAsync) {
    --asyncFunctionDepth_;
  }
  --functionDepth_;
  if (!block) {
    return nullptr;
  }
  auto blockStmt = std::get_if<BlockStmt>(&block->node);
  if (!blockStmt) {
    return nullptr;
  }

  FunctionExpr funcExpr;
  funcExpr.params = std::move(params);
  funcExpr.restParam = restParam;
  funcExpr.name = name;
  funcExpr.isAsync = isAsync;
  funcExpr.isGenerator = isGenerator;
  funcExpr.body = std::move(blockStmt->body);

  return std::make_unique<Expression>(std::move(funcExpr));
}

ExprPtr Parser::parseClassExpression() {
  expect(TokenType::Class);

  std::string className;
  if (isIdentifierLikeToken(current().type)) {
    className = current().value;
    advance();
  }

  ExprPtr superClass;
  if (match(TokenType::Extends)) {
    advance();
    superClass = parseCall();
  }

  expect(TokenType::LeftBrace);

  std::vector<MethodDefinition> methods;
  while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
    // Skip semicolons
    if (match(TokenType::Semicolon)) {
      advance();
      continue;
    }

    MethodDefinition method;

    // Check for static
    if (match(TokenType::Static)) {
      method.isStatic = true;
      advance();
    }

    // Check for async (only if followed by a method name + '(')
    if (match(TokenType::Async) && peek().type != TokenType::Semicolon &&
        peek().type != TokenType::Equal && peek().type != TokenType::RightBrace) {
      method.isAsync = true;
      advance();
    }

    // Check for getter/setter - only if followed by name + '('
    if ((match(TokenType::Get) || match(TokenType::Set)) &&
        (isIdentifierNameToken(peek().type) || peek().type == TokenType::PrivateIdentifier)) {
      size_t saved = pos_;
      TokenType savedType = current().type;
      advance();
      bool isGetterSetter = false;
      if (isIdentifierNameToken(current().type) || match(TokenType::PrivateIdentifier)) {
        advance();
        if (match(TokenType::LeftParen)) {
          isGetterSetter = true;
        }
      }
      pos_ = saved;

      if (isGetterSetter) {
        if (savedType == TokenType::Get) {
          method.kind = MethodDefinition::Kind::Get;
        } else {
          method.kind = MethodDefinition::Kind::Set;
        }
        advance();
      }
    }

    // Member name (identifier or private identifier)
    if (match(TokenType::PrivateIdentifier)) {
      method.isPrivate = true;
      method.key.name = current().value;
      advance();
    } else if (isIdentifierNameToken(current().type)) {
      std::string memberName = current().value;
      if (memberName == "constructor" && method.kind == MethodDefinition::Kind::Method) {
        method.kind = MethodDefinition::Kind::Constructor;
      }
      method.key.name = memberName;
      advance();
    } else {
      return nullptr;
    }

    // Distinguish field vs method
    if (match(TokenType::LeftParen)) {
      ++functionDepth_;
      if (method.isAsync) {
        ++asyncFunctionDepth_;
      }
      advance();
      while (!match(TokenType::RightParen)) {
        if (isIdentifierLikeToken(current().type)) {
          method.params.push_back({current().value});
          advance();
          if (match(TokenType::Comma)) {
            advance();
          }
        } else {
          break;
        }
      }
      expect(TokenType::RightParen);

      bool disallowSuperCall = !superClass && !method.isStatic &&
                               method.kind == MethodDefinition::Kind::Constructor;
      if (disallowSuperCall) {
        ++superCallDisallowDepth_;
      }
      expect(TokenType::LeftBrace);
      while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
        auto stmt = parseStatement();
        if (!stmt) {
          if (method.isAsync) {
            --asyncFunctionDepth_;
          }
          --functionDepth_;
          if (disallowSuperCall) {
            --superCallDisallowDepth_;
          }
          return nullptr;
        }
        method.body.push_back(std::move(stmt));
      }
      expect(TokenType::RightBrace);
      if (method.isAsync) {
        --asyncFunctionDepth_;
      }
      --functionDepth_;
      if (disallowSuperCall) {
        --superCallDisallowDepth_;
      }
    } else {
      // Field declaration
      method.kind = MethodDefinition::Kind::Field;
      if (match(TokenType::Equal)) {
        advance();
        method.initializer = parseAssignment();
      }
      if (match(TokenType::Semicolon)) {
        advance();
      }
    }

    methods.push_back(std::move(method));
  }
  expect(TokenType::RightBrace);

  ClassExpr classExpr;
  classExpr.name = className;
  classExpr.superClass = std::move(superClass);
  classExpr.methods = std::move(methods);

  return std::make_unique<Expression>(std::move(classExpr));
}

ExprPtr Parser::parseNewExpression() {
  Token newTok = current();
  expect(TokenType::New);

  // MetaProperty: new.target
  if (match(TokenType::Dot) &&
      peek().type == TokenType::Identifier &&
      peek().value == "target") {
    advance();  // .
    advance();  // target
    return makeExpr(MetaProperty{"new", "target"}, newTok);
  }

  auto callee = parseMember();
  if (!callee) {
    return nullptr;
  }
  if (isDirectDynamicImportCall(callee)) {
    return nullptr;
  }

  std::vector<ExprPtr> args;
  if (match(TokenType::LeftParen)) {
    advance();
    while (!match(TokenType::RightParen)) {
      auto arg = parseAssignment();
      if (!arg) {
        return nullptr;
      }
      args.push_back(std::move(arg));
      if (!match(TokenType::RightParen)) {
        if (!expect(TokenType::Comma)) {
          return nullptr;
        }
      }
    }
    if (!expect(TokenType::RightParen)) {
      return nullptr;
    }
  }

  NewExpr newExpr;
  newExpr.callee = std::move(callee);
  newExpr.arguments = std::move(args);

  return std::make_unique<Expression>(std::move(newExpr));
}

ExprPtr Parser::parsePattern() {
  // Check for array destructuring pattern
  if (match(TokenType::LeftBracket)) {
    auto base = parseArrayPattern();
    if (!base) {
      return nullptr;
    }
    if (match(TokenType::Equal)) {
      advance();
      auto init = parseAssignment();
      if (!init) {
        return nullptr;
      }
      return std::make_unique<Expression>(AssignmentPattern{std::move(base), std::move(init)});
    }
    return base;
  }

  // Check for object destructuring pattern
  if (match(TokenType::LeftBrace)) {
    // Peek past balanced {}: if followed by . or [, it's an object literal + member access
    // e.g., [{set y(v){...}}.y] = [23]
    size_t peekPos = pos_ + 1;
    int depth = 1;
    while (depth > 0 && peekPos < tokens_.size()) {
      auto t = tokens_[peekPos].type;
      if (t == TokenType::LeftBrace || t == TokenType::LeftBracket || t == TokenType::LeftParen)
        depth++;
      else if (t == TokenType::RightBrace || t == TokenType::RightBracket || t == TokenType::RightParen)
        depth--;
      peekPos++;
    }
    bool isObjLiteralMember = peekPos < tokens_.size() &&
        (tokens_[peekPos].type == TokenType::Dot ||
         tokens_[peekPos].type == TokenType::LeftBracket);

    if (isObjLiteralMember) {
      // Parse as object literal expression + member access
      auto base = parseMember();
      if (!base) return nullptr;
      if (match(TokenType::Equal)) {
        advance();
        auto init = parseAssignment();
        if (!init) return nullptr;
        return std::make_unique<Expression>(AssignmentPattern{std::move(base), std::move(init)});
      }
      return base;
    }

    auto base = parseObjectPattern();
    if (!base) {
      return nullptr;
    }
    if (match(TokenType::Equal)) {
      advance();
      auto init = parseAssignment();
      if (!init) {
        return nullptr;
      }
      return std::make_unique<Expression>(AssignmentPattern{std::move(base), std::move(init)});
    }
    return base;
  }

  // Otherwise it must be an identifier (binding) or member expression (assignment target)
  if (isIdentifierLikeToken(current().type)) {
    std::string name = current().value;
    advance();
    auto base = std::make_unique<Expression>(Identifier{name});
    // Check for member expression (e.g., x.y, obj['key'])
    while (match(TokenType::Dot) || match(TokenType::LeftBracket)) {
      if (match(TokenType::Dot)) {
        advance();
        if (!isIdentifierNameToken(current().type)) break;
        auto prop = std::make_unique<Expression>(Identifier{current().value});
        advance();
        MemberExpr mem;
        mem.object = std::move(base);
        mem.property = std::move(prop);
        mem.computed = false;
        base = std::make_unique<Expression>(std::move(mem));
      } else {
        advance();
        auto prop = parseAssignment();
        if (!prop || !expect(TokenType::RightBracket)) return nullptr;
        MemberExpr mem;
        mem.object = std::move(base);
        mem.property = std::move(prop);
        mem.computed = true;
        base = std::make_unique<Expression>(std::move(mem));
      }
    }
    if (match(TokenType::Equal)) {
      advance();
      auto init = parseAssignment();
      if (!init) {
        return nullptr;
      }
      return std::make_unique<Expression>(AssignmentPattern{std::move(base), std::move(init)});
    }
    return base;
  }

  return nullptr;
}

ExprPtr Parser::parseArrayPattern() {
  expect(TokenType::LeftBracket);

  ArrayPattern pattern;

  while (!match(TokenType::RightBracket) && pos_ < tokens_.size()) {
    if (!pattern.elements.empty()) {
      expect(TokenType::Comma);
      // Allow trailing comma
      if (match(TokenType::RightBracket)) {
        break;
      }
    }

    // Check for rest element (...rest)
    if (match(TokenType::DotDotDot)) {
      advance();
      pattern.rest = parsePattern();
      if (!pattern.rest) {
        return nullptr;
      }
      // Rest element does not support initializer: [...x = y] is SyntaxError
      if (std::get_if<AssignmentPattern>(&pattern.rest->node)) {
        return nullptr;
      }
      // Rest must be last element; break so the closing ] is consumed by expect() after loop
      break;
    }

    // Check for hole in array pattern (e.g., [a, , c])
    if (match(TokenType::Comma)) {
      pattern.elements.push_back(nullptr);
      continue;
    }

    // Parse nested pattern or identifier
    ExprPtr element = parsePattern();
    if (!element) {
      return nullptr;
    }
    pattern.elements.push_back(std::move(element));
  }

  expect(TokenType::RightBracket);

  return std::make_unique<Expression>(std::move(pattern));
}

ExprPtr Parser::parseObjectPattern() {
  expect(TokenType::LeftBrace);

  ObjectPattern pattern;

  while (!match(TokenType::RightBrace) && pos_ < tokens_.size()) {
    if (!pattern.properties.empty()) {
      expect(TokenType::Comma);
      // Allow trailing comma
      if (match(TokenType::RightBrace)) {
        break;
      }
    }

    // Check for rest properties (...rest)
    if (match(TokenType::DotDotDot)) {
      advance();
      pattern.rest = parsePattern();
      if (!pattern.rest) {
        return nullptr;
      }
      // Rest must be last element; break so the closing } is consumed by expect() after loop
      break;
    }

    // Parse property key
    ExprPtr key;
    bool isComputed = false;
    if (match(TokenType::LeftBracket)) {
      // Computed property key: {[expr]: pattern}
      isComputed = true;
      advance();
      key = parseAssignment();
      if (!key || !expect(TokenType::RightBracket)) {
        return nullptr;
      }
    } else if (isIdentifierNameToken(current().type)) {
      std::string name = current().value;
      advance();
      key = std::make_unique<Expression>(Identifier{name});
    } else if (match(TokenType::String)) {
      std::string strVal = current().value;
      advance();
      key = std::make_unique<Expression>(StringLiteral{strVal});
    } else if (match(TokenType::Number)) {
      std::string numVal = current().value;
      advance();
      key = std::make_unique<Expression>(NumberLiteral{std::stod(numVal)});
    } else {
      return nullptr;
    }

    // Check for shorthand property (e.g., {x} instead of {x: x})
    ExprPtr value;
    if (match(TokenType::Colon)) {
      advance();
      value = parsePattern();
      if (!value) {
        return nullptr;
      }
    } else if (match(TokenType::Equal)) {
      // Shorthand with default initializer (e.g., {x = 1})
      advance();
      auto init = parseAssignment();
      if (!init) {
        return nullptr;
      }
      if (auto* id = std::get_if<Identifier>(&key->node)) {
        auto left = std::make_unique<Expression>(Identifier{id->name});
        value = std::make_unique<Expression>(AssignmentPattern{std::move(left), std::move(init)});
      } else {
        return nullptr;
      }
    } else {
      // Shorthand - use key as both key and value pattern
      if (auto* id = std::get_if<Identifier>(&key->node)) {
        // In strict mode, yield is not a valid IdentifierReference
        if (strictMode_ && id->name == "yield") {
          return nullptr;  // SyntaxError: yield not valid in strict mode
        }
        value = std::make_unique<Expression>(Identifier{id->name});
      } else {
        return nullptr;  // Shorthand only works with identifiers
      }
    }

    ObjectPattern::Property prop;
    prop.key = std::move(key);
    prop.value = std::move(value);
    prop.computed = isComputed;
    pattern.properties.push_back(std::move(prop));
  }

  expect(TokenType::RightBrace);

  return std::make_unique<Expression>(std::move(pattern));
}

}
