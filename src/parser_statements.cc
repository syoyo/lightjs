#include "parser_internal.h"

namespace lightjs {

StmtPtr Parser::parseStatement(bool allowModuleItem) {
  if (++parseDepth_ > kMaxParseDepth) {
    --parseDepth_;
    throw std::runtime_error("SyntaxError: Maximum parse depth exceeded");
  }
  struct ParseDepthGuard { int& d; ~ParseDepthGuard() { --d; } } _pdg{parseDepth_};

  if (isIdentifierLikeToken(current().type) && peek().type == TokenType::Colon) {
    const Token& tok = current();
    std::string label = tok.value;
    advance();
    advance();
    if (match(TokenType::Let)) {
      if (!strictMode_) {
        auto nextType = peek().type;
        auto nextLine = peek().line;
        auto curLine = current().line;
        if (curLine != nextLine && nextType == TokenType::LeftBracket) {
          error_ = true;
          return nullptr;
        } else if (curLine != nextLine) {
        } else if (nextType != TokenType::Identifier && nextType != TokenType::LeftBracket &&
                   nextType != TokenType::LeftBrace && nextType != TokenType::Await &&
                   nextType != TokenType::Yield && nextType != TokenType::Async &&
                   nextType != TokenType::Get && nextType != TokenType::Set &&
                   nextType != TokenType::From && nextType != TokenType::As &&
                   nextType != TokenType::Of && nextType != TokenType::Static &&
                   nextType != TokenType::Let) {
        } else {
          error_ = true;
          return nullptr;
        }
      } else {
        error_ = true;
        return nullptr;
      }
    } else if (isUsingDeclarationStart()) {
      error_ = true;
      return nullptr;
    } else if (isAwaitUsingDeclarationStart()) {
      error_ = true;
      return nullptr;
    } else if (match(TokenType::Const) || match(TokenType::Class)) {
      error_ = true;
      return nullptr;
    }
    if (match(TokenType::Async) && peek().type == TokenType::Function) {
      error_ = true;
      return nullptr;
    }
    if (match(TokenType::Function) && peek().type == TokenType::Star) {
      error_ = true;
      return nullptr;
    }
    if (strictMode_ && match(TokenType::Function)) {
      error_ = true;
      return nullptr;
    }
    bool isIterationLabel = false;
    {
      size_t lookPos = pos_;
      while (lookPos + 1 < tokens_.size() &&
             isIdentifierLikeToken(tokens_[lookPos].type) &&
             tokens_[lookPos + 1].type == TokenType::Colon) {
        lookPos += 2;
      }
      if (lookPos < tokens_.size()) {
        auto bodyType = tokens_[lookPos].type;
        isIterationLabel = (bodyType == TokenType::For ||
                           bodyType == TokenType::While ||
                           bodyType == TokenType::Do);
      }
    }
    if (isIterationLabel) {
      iterationLabels_.insert(label);
    }
    if (activeLabels_.count(label) != 0) {
      error_ = true;
      return nullptr;
    }
    activeLabels_.insert(label);
    auto body = parseStatement();
    activeLabels_.erase(label);
    if (isIterationLabel) {
      iterationLabels_.erase(label);
    }
    if (!body) return nullptr;
    return makeStmt(LabelledStmt{label, std::move(body)}, tok);
  }

  switch (current().type) {
    case TokenType::Semicolon: {
      const Token& tok = current();
      advance();
      return makeStmt(EmptyStmt{}, tok);
    }
    case TokenType::Let:
      if (current().escaped) {
        return parseExpressionStatement();
      }
      if (!strictMode_) {
        if (inSingleStatementPosition_ &&
            current().line != peek().line) {
          if (peek().type == TokenType::LeftBracket) {
            return nullptr;
          }
          const Token& tok = current();
          advance();
          if (!consumeSemicolonOrASI()) {
            return nullptr;
          }
          return std::make_unique<Statement>(ExpressionStmt{makeExpr(Identifier{"let"}, tok)});
        }
        auto nextType = peek().type;
        if (inSingleStatementPosition_) {
          return parseExpressionStatement();
        }
        if (nextType != TokenType::Identifier && nextType != TokenType::LeftBracket &&
            nextType != TokenType::LeftBrace && nextType != TokenType::Await &&
            nextType != TokenType::Yield && nextType != TokenType::Async &&
            nextType != TokenType::Get && nextType != TokenType::Set &&
            nextType != TokenType::From && nextType != TokenType::As &&
            nextType != TokenType::Of && nextType != TokenType::Static &&
            nextType != TokenType::Let) {
          return parseExpressionStatement();
        }
      }
      [[fallthrough]];
    case TokenType::Const:
    case TokenType::Var:
      return parseVarDeclaration();
    case TokenType::Using:
      if (isUsingDeclarationStart()) {
        return parseVarDeclaration();
      }
      return parseExpressionStatement();
    case TokenType::Await:
      if (isAwaitUsingDeclarationStart()) {
        return parseVarDeclaration();
      }
      return parseExpressionStatement();
    case TokenType::Async:
      if (!current().escaped &&
          current().line == peek().line &&
          peek().type == TokenType::Function) {
        return parseFunctionDeclaration();
      }
      return parseExpressionStatement();
    case TokenType::Function:
      return parseFunctionDeclaration();
    case TokenType::At:
      return parseClassDeclaration();
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
      if (label.empty() && loopDepth_ == 0 && switchDepth_ == 0) {
        error_ = true;
        return nullptr;
      }
      if (!label.empty() && activeLabels_.find(label) == activeLabels_.end()) {
        error_ = true;
        return nullptr;
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
      if (label.empty() && loopDepth_ == 0) {
        error_ = true;
        return nullptr;
      }
      if (!label.empty() && iterationLabels_.find(label) == iterationLabels_.end()) {
        error_ = true;
        return nullptr;
      }
      consumeSemicolon();
      return makeStmt(ContinueStmt{label}, tok);
    }
    case TokenType::Debugger: {
      const Token& tok = current();
      advance();
      consumeSemicolon();
      return makeStmt(DebuggerStmt{}, tok);
    }
    case TokenType::Throw: {
      const Token& tok = current();
      advance();
      if (current().line != tok.line) {
        error_ = true;
        return nullptr;
      }
      auto argument = parseExpression();
      if (!argument) {
        return nullptr;
      }
      consumeSemicolon();
      return makeStmt(ThrowStmt{std::move(argument)}, tok);
    }
    case TokenType::Try:
      return parseTryStatement();
    case TokenType::Import:
      if (current().escaped) {
        error_ = true;
        return nullptr;
      }
      if (peek().type == TokenType::Dot || peek().type == TokenType::LeftParen) {
        return parseExpressionStatement();
      }
      if (!isModule_ || !allowModuleItem) {
        error_ = true;
        return nullptr;
      }
      return parseImportDeclaration();
    case TokenType::Export:
      if (!isModule_ || !allowModuleItem) {
        error_ = true;
        return nullptr;
      }
      return parseExportDeclaration();
    case TokenType::LeftBrace:
      return parseBlockStatement();
    default:
      return parseExpressionStatement();
  }
}

StmtPtr Parser::parseReturnStatement() {
  if (functionDepth_ == 0 || returnDisallowDepth_ > 0) {
    error_ = true;
    return nullptr;
  }
  const Token& startTok = current();
  expect(TokenType::Return);

  ExprPtr argument = nullptr;
  if (!match(TokenType::Semicolon) &&
      !match(TokenType::EndOfFile) &&
      current().line == startTok.line) {
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

  if (match(TokenType::Const) || match(TokenType::Class)) {
    error_ = true;
    return nullptr;
  }
  if (match(TokenType::Let)) {
    if (peek().type == TokenType::LeftBracket) {
      error_ = true;
      return nullptr;
    }
    if (strictMode_) {
      error_ = true;
      return nullptr;
    }
    if (peek().line == current().line &&
        (peek().type == TokenType::Identifier || peek().type == TokenType::LeftBrace)) {
      error_ = true;
      return nullptr;
    }
  }
  if (match(TokenType::Async) && peek().type == TokenType::Function) {
    error_ = true;
    return nullptr;
  }
  bool prevSingleStmt = inSingleStatementPosition_;
  inSingleStatementPosition_ = true;
  auto consequent = parseStatement();
  inSingleStatementPosition_ = prevSingleStmt;
  if (!consequent) {
    return nullptr;
  }
  {
    const Statement* check = consequent.get();
    bool isLabelledFunction = false;
    while (auto* lab = std::get_if<LabelledStmt>(&check->node)) {
      isLabelledFunction = true;
      check = lab->body.get();
    }
    if (auto* fn = std::get_if<FunctionDeclaration>(&check->node)) {
      if (isLabelledFunction || fn->isGenerator || strictMode_) {
        error_ = true;
        return nullptr;
      }
    }
  }
  StmtPtr alternate = nullptr;

  if (match(TokenType::Else)) {
    advance();
    if (match(TokenType::Const) || match(TokenType::Class)) {
      error_ = true;
      return nullptr;
    }
    if (match(TokenType::Let)) {
      if (peek().type == TokenType::LeftBracket) {
        error_ = true;
        return nullptr;
      }
      if (strictMode_) {
        error_ = true;
        return nullptr;
      }
      if (peek().line == current().line &&
          (peek().type == TokenType::Identifier || peek().type == TokenType::LeftBrace)) {
        error_ = true;
        return nullptr;
      }
    }
    if (match(TokenType::Async) && peek().type == TokenType::Function) {
      error_ = true;
      return nullptr;
    }
    prevSingleStmt = inSingleStatementPosition_;
    inSingleStatementPosition_ = true;
    alternate = parseStatement();
    inSingleStatementPosition_ = prevSingleStmt;
    if (!alternate) {
      return nullptr;
    }
    {
      const Statement* check = alternate.get();
      bool isLabelledFunction = false;
      while (auto* lab = std::get_if<LabelledStmt>(&check->node)) {
        isLabelledFunction = true;
        check = lab->body.get();
      }
      if (auto* fn = std::get_if<FunctionDeclaration>(&check->node)) {
        if (isLabelledFunction || fn->isGenerator || strictMode_) {
          error_ = true;
          return nullptr;
        }
      }
    }
  }

  return makeStmt(IfStmt{
    std::move(test),
    std::move(consequent),
    std::move(alternate)
  }, startTok);
}

StmtPtr Parser::parseWhileStatement() {
  struct DepthGuard {
    int& depth;
    explicit DepthGuard(int& d) : depth(d) { ++depth; }
    ~DepthGuard() { --depth; }
  };

  const Token& startTok = current();
  expect(TokenType::While);
  if (!expect(TokenType::LeftParen)) {
    return nullptr;
  }
  auto test = parseExpression();
  if (!test || !expect(TokenType::RightParen)) {
    return nullptr;
  }
  if (match(TokenType::Const) || match(TokenType::Class)) {
    error_ = true;
    return nullptr;
  }
  if (match(TokenType::Let)) {
    if (peek().type == TokenType::LeftBracket) {
      error_ = true;
      return nullptr;
    }
    if (strictMode_) {
      error_ = true;
      return nullptr;
    }
    if (peek().line == current().line &&
        (peek().type == TokenType::Identifier || peek().type == TokenType::LeftBrace)) {
      error_ = true;
      return nullptr;
    }
  }
  if (match(TokenType::Async) && peek().type == TokenType::Function) {
    error_ = true;
    return nullptr;
  }
  bool prevSingleStmt = inSingleStatementPosition_;
  inSingleStatementPosition_ = true;
  DepthGuard loopGuard(loopDepth_);
  auto body = parseStatement();
  inSingleStatementPosition_ = prevSingleStmt;
  if (!body) {
    return nullptr;
  }
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

  if (match(TokenType::Const) || match(TokenType::Class)) {
    error_ = true;
    return nullptr;
  }
  if (match(TokenType::Let)) {
    if (peek().type == TokenType::LeftBracket) {
      error_ = true;
      return nullptr;
    }
    if (peek().line == current().line &&
        (peek().type == TokenType::Identifier || peek().type == TokenType::LeftBrace)) {
      error_ = true;
      return nullptr;
    }
  }
  if (match(TokenType::Function)) {
    error_ = true;
    return nullptr;
  }
  if (match(TokenType::Async) && peek().type == TokenType::Function) {
    error_ = true;
    return nullptr;
  }
  bool prevSingleStmt = inSingleStatementPosition_;
  inSingleStatementPosition_ = true;
  auto body = parseStatement();
  inSingleStatementPosition_ = prevSingleStmt;
  if (!body) {
    return nullptr;
  }

  {
    auto* check = body.get();
    while (check) {
      if (auto* labeled = std::get_if<LabelledStmt>(&check->node)) {
        check = labeled->body.get();
      } else {
        break;
      }
    }
    if (check && std::get_if<FunctionDeclaration>(&check->node)) {
      error_ = true;
      return nullptr;
    }
  }

  return makeStmt(WithStmt{std::move(object), std::move(body)}, startTok);
}

}  // namespace lightjs
