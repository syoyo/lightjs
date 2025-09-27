#include "parser.h"
#include <stdexcept>

namespace tinyjs {

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

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

std::optional<Program> Parser::parse() {
  Program program;
  while (!match(TokenType::EndOfFile)) {
    if (auto stmt = parseStatement()) {
      program.body.push_back(std::move(stmt));
    } else {
      return std::nullopt;
    }
  }
  return program;
}

StmtPtr Parser::parseStatement() {
  switch (current().type) {
    case TokenType::Let:
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
    case TokenType::Return:
      return parseReturnStatement();
    case TokenType::If:
      return parseIfStatement();
    case TokenType::While:
      return parseWhileStatement();
    case TokenType::For:
      return parseForStatement();
    case TokenType::Break:
      advance();
      consumeSemicolon();
      return std::make_unique<Statement>(BreakStmt{});
    case TokenType::Continue:
      advance();
      consumeSemicolon();
      return std::make_unique<Statement>(ContinueStmt{});
    case TokenType::Throw:
      return std::make_unique<Statement>(ThrowStmt{parseExpression()});
    case TokenType::Try:
      return parseTryStatement();
    case TokenType::LeftBrace:
      return parseBlockStatement();
    default:
      return parseExpressionStatement();
  }
}

StmtPtr Parser::parseVarDeclaration() {
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
      expect(TokenType::Comma);
    }

    if (!match(TokenType::Identifier)) {
      return nullptr;
    }
    std::string name = current().value;
    advance();

    ExprPtr init = nullptr;
    if (match(TokenType::Equal)) {
      advance();
      init = parseExpression();
    }

    decl.declarations.push_back({Identifier{name}, std::move(init)});
  } while (match(TokenType::Comma));

  consumeSemicolon();
  return std::make_unique<Statement>(std::move(decl));
}

StmtPtr Parser::parseFunctionDeclaration() {
  bool isAsync = false;
  if (match(TokenType::Async)) {
    isAsync = true;
    advance();
  }

  expect(TokenType::Function);

  if (!match(TokenType::Identifier)) {
    return nullptr;
  }
  std::string name = current().value;
  advance();

  expect(TokenType::LeftParen);

  std::vector<Identifier> params;
  while (!match(TokenType::RightParen)) {
    if (!params.empty()) {
      expect(TokenType::Comma);
    }
    if (match(TokenType::Identifier)) {
      params.push_back({current().value});
      advance();
    }
  }
  expect(TokenType::RightParen);

  auto block = parseBlockStatement();
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
  funcDecl.body = std::move(blockStmt->body);
  funcDecl.isAsync = isAsync;

  return std::make_unique<Statement>(std::move(funcDecl));
}

StmtPtr Parser::parseReturnStatement() {
  expect(TokenType::Return);

  ExprPtr argument = nullptr;
  if (!match(TokenType::Semicolon) && !match(TokenType::EndOfFile)) {
    argument = parseExpression();
  }

  consumeSemicolon();
  return std::make_unique<Statement>(ReturnStmt{std::move(argument)});
}

StmtPtr Parser::parseIfStatement() {
  expect(TokenType::If);
  expect(TokenType::LeftParen);
  auto test = parseExpression();
  expect(TokenType::RightParen);

  auto consequent = parseStatement();
  StmtPtr alternate = nullptr;

  if (match(TokenType::Else)) {
    advance();
    alternate = parseStatement();
  }

  return std::make_unique<Statement>(IfStmt{
    std::move(test),
    std::move(consequent),
    std::move(alternate)
  });
}

StmtPtr Parser::parseWhileStatement() {
  expect(TokenType::While);
  expect(TokenType::LeftParen);
  auto test = parseExpression();
  expect(TokenType::RightParen);
  auto body = parseStatement();

  return std::make_unique<Statement>(WhileStmt{std::move(test), std::move(body)});
}

StmtPtr Parser::parseForStatement() {
  expect(TokenType::For);
  expect(TokenType::LeftParen);

  StmtPtr init = nullptr;
  if (!match(TokenType::Semicolon)) {
    if (match(TokenType::Let) || match(TokenType::Const) || match(TokenType::Var)) {
      init = parseVarDeclaration();
    } else {
      auto expr = parseExpression();
      expect(TokenType::Semicolon);
      init = std::make_unique<Statement>(ExpressionStmt{std::move(expr)});
    }
  } else {
    advance();
  }

  ExprPtr test = nullptr;
  if (!match(TokenType::Semicolon)) {
    test = parseExpression();
  }
  expect(TokenType::Semicolon);

  ExprPtr update = nullptr;
  if (!match(TokenType::RightParen)) {
    update = parseExpression();
  }
  expect(TokenType::RightParen);

  auto body = parseStatement();

  return std::make_unique<Statement>(ForStmt{
    std::move(init),
    std::move(test),
    std::move(update),
    std::move(body)
  });
}

StmtPtr Parser::parseBlockStatement() {
  expect(TokenType::LeftBrace);

  std::vector<StmtPtr> body;
  while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
    if (auto stmt = parseStatement()) {
      body.push_back(std::move(stmt));
    }
  }

  expect(TokenType::RightBrace);
  return std::make_unique<Statement>(BlockStmt{std::move(body)});
}

StmtPtr Parser::parseExpressionStatement() {
  auto expr = parseExpression();
  consumeSemicolon();
  return std::make_unique<Statement>(ExpressionStmt{std::move(expr)});
}

StmtPtr Parser::parseTryStatement() {
  expect(TokenType::Try);
  auto tryBlock = parseBlockStatement();

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
      if (match(TokenType::Identifier)) {
        handler.param = {current().value};
        advance();
      }
      expect(TokenType::RightParen);
    }

    auto catchBlock = parseBlockStatement();
    auto catchBlockStmt = std::get_if<BlockStmt>(&catchBlock->node);
    if (catchBlockStmt) {
      handler.body = std::move(catchBlockStmt->body);
    }
  }

  if (match(TokenType::Finally)) {
    advance();
    hasFinalizer = true;
    auto finallyBlock = parseBlockStatement();
    auto finallyBlockStmt = std::get_if<BlockStmt>(&finallyBlock->node);
    if (finallyBlockStmt) {
      finalizer = std::move(finallyBlockStmt->body);
    }
  }

  return std::make_unique<Statement>(TryStmt{
    std::move(tryBlockStmt->body),
    std::move(handler),
    std::move(finalizer),
    hasHandler,
    hasFinalizer
  });
}

ExprPtr Parser::parseExpression() {
  return parseAssignment();
}

ExprPtr Parser::parseAssignment() {
  auto left = parseConditional();

  if (match(TokenType::Equal) || match(TokenType::PlusEqual) ||
      match(TokenType::MinusEqual) || match(TokenType::StarEqual) ||
      match(TokenType::SlashEqual)) {

    AssignmentExpr::Op op;
    switch (current().type) {
      case TokenType::Equal: op = AssignmentExpr::Op::Assign; break;
      case TokenType::PlusEqual: op = AssignmentExpr::Op::AddAssign; break;
      case TokenType::MinusEqual: op = AssignmentExpr::Op::SubAssign; break;
      case TokenType::StarEqual: op = AssignmentExpr::Op::MulAssign; break;
      case TokenType::SlashEqual: op = AssignmentExpr::Op::DivAssign; break;
      default: op = AssignmentExpr::Op::Assign; break;
    }
    advance();

    auto right = parseAssignment();
    return std::make_unique<Expression>(AssignmentExpr{op, std::move(left), std::move(right)});
  }

  return left;
}

ExprPtr Parser::parseConditional() {
  auto expr = parseLogicalOr();

  if (match(TokenType::Question)) {
    advance();
    auto consequent = parseExpression();
    expect(TokenType::Colon);
    auto alternate = parseExpression();
    return std::make_unique<Expression>(ConditionalExpr{
      std::move(expr),
      std::move(consequent),
      std::move(alternate)
    });
  }

  return expr;
}

ExprPtr Parser::parseLogicalOr() {
  auto left = parseLogicalAnd();

  while (match(TokenType::PipePipe)) {
    advance();
    auto right = parseLogicalAnd();
    left = std::make_unique<Expression>(BinaryExpr{
      BinaryExpr::Op::LogicalOr,
      std::move(left),
      std::move(right)
    });
  }

  return left;
}

ExprPtr Parser::parseLogicalAnd() {
  auto left = parseEquality();

  while (match(TokenType::AmpAmp)) {
    advance();
    auto right = parseEquality();
    left = std::make_unique<Expression>(BinaryExpr{
      BinaryExpr::Op::LogicalAnd,
      std::move(left),
      std::move(right)
    });
  }

  return left;
}

ExprPtr Parser::parseEquality() {
  auto left = parseRelational();

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
    left = std::make_unique<Expression>(BinaryExpr{op, std::move(left), std::move(right)});
  }

  return left;
}

ExprPtr Parser::parseRelational() {
  auto left = parseAdditive();

  while (match(TokenType::Less) || match(TokenType::Greater) ||
         match(TokenType::LessEqual) || match(TokenType::GreaterEqual)) {

    BinaryExpr::Op op;
    switch (current().type) {
      case TokenType::Less: op = BinaryExpr::Op::Less; break;
      case TokenType::Greater: op = BinaryExpr::Op::Greater; break;
      case TokenType::LessEqual: op = BinaryExpr::Op::LessEqual; break;
      case TokenType::GreaterEqual: op = BinaryExpr::Op::GreaterEqual; break;
      default: op = BinaryExpr::Op::Less; break;
    }
    advance();

    auto right = parseAdditive();
    left = std::make_unique<Expression>(BinaryExpr{op, std::move(left), std::move(right)});
  }

  return left;
}

ExprPtr Parser::parseAdditive() {
  auto left = parseMultiplicative();

  while (match(TokenType::Plus) || match(TokenType::Minus)) {
    BinaryExpr::Op op = match(TokenType::Plus) ? BinaryExpr::Op::Add : BinaryExpr::Op::Sub;
    advance();
    auto right = parseMultiplicative();
    left = std::make_unique<Expression>(BinaryExpr{op, std::move(left), std::move(right)});
  }

  return left;
}

ExprPtr Parser::parseMultiplicative() {
  auto left = parseUnary();

  while (match(TokenType::Star) || match(TokenType::Slash) || match(TokenType::Percent)) {
    BinaryExpr::Op op;
    switch (current().type) {
      case TokenType::Star: op = BinaryExpr::Op::Mul; break;
      case TokenType::Slash: op = BinaryExpr::Op::Div; break;
      case TokenType::Percent: op = BinaryExpr::Op::Mod; break;
      default: op = BinaryExpr::Op::Mul; break;
    }
    advance();
    auto right = parseUnary();
    left = std::make_unique<Expression>(BinaryExpr{op, std::move(left), std::move(right)});
  }

  return left;
}

ExprPtr Parser::parseUnary() {
  if (match(TokenType::Await)) {
    advance();
    auto argument = parseUnary();
    return std::make_unique<Expression>(AwaitExpr{std::move(argument)});
  }

  if (match(TokenType::Bang) || match(TokenType::Minus) ||
      match(TokenType::Plus) || match(TokenType::Typeof)) {

    UnaryExpr::Op op;
    switch (current().type) {
      case TokenType::Bang: op = UnaryExpr::Op::Not; break;
      case TokenType::Minus: op = UnaryExpr::Op::Minus; break;
      case TokenType::Plus: op = UnaryExpr::Op::Plus; break;
      case TokenType::Typeof: op = UnaryExpr::Op::Typeof; break;
      default: op = UnaryExpr::Op::Not; break;
    }
    advance();

    auto argument = parseUnary();
    return std::make_unique<Expression>(UnaryExpr{op, std::move(argument)});
  }

  if (match(TokenType::PlusPlus) || match(TokenType::MinusMinus)) {
    UpdateExpr::Op op = match(TokenType::PlusPlus) ?
      UpdateExpr::Op::Increment : UpdateExpr::Op::Decrement;
    advance();
    auto argument = parseUnary();
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
    return std::make_unique<Expression>(UpdateExpr{op, std::move(expr), false});
  }

  return expr;
}

ExprPtr Parser::parseCall() {
  auto expr = parseMember();

  while (match(TokenType::LeftParen)) {
    advance();
    std::vector<ExprPtr> args;

    while (!match(TokenType::RightParen)) {
      if (!args.empty()) {
        expect(TokenType::Comma);
      }
      args.push_back(parseExpression());
    }

    expect(TokenType::RightParen);
    expr = std::make_unique<Expression>(CallExpr{std::move(expr), std::move(args)});
  }

  return expr;
}

ExprPtr Parser::parseMember() {
  auto expr = parsePrimary();

  while (true) {
    if (match(TokenType::Dot)) {
      advance();
      if (match(TokenType::Identifier)) {
        auto prop = std::make_unique<Expression>(Identifier{current().value});
        advance();
        expr = std::make_unique<Expression>(MemberExpr{
          std::move(expr),
          std::move(prop),
          false
        });
      }
    } else if (match(TokenType::LeftBracket)) {
      advance();
      auto prop = parseExpression();
      expect(TokenType::RightBracket);
      expr = std::make_unique<Expression>(MemberExpr{
        std::move(expr),
        std::move(prop),
        true
      });
    } else {
      break;
    }
  }

  return expr;
}

ExprPtr Parser::parsePrimary() {
  if (match(TokenType::Number)) {
    double value = std::stod(current().value);
    advance();
    return std::make_unique<Expression>(NumberLiteral{value});
  }

  if (match(TokenType::BigInt)) {
    int64_t value = std::stoll(current().value);
    advance();
    return std::make_unique<Expression>(BigIntLiteral{value});
  }

  if (match(TokenType::String)) {
    std::string value = current().value;
    advance();
    return std::make_unique<Expression>(StringLiteral{value});
  }

  if (match(TokenType::True)) {
    advance();
    return std::make_unique<Expression>(BoolLiteral{true});
  }

  if (match(TokenType::False)) {
    advance();
    return std::make_unique<Expression>(BoolLiteral{false});
  }

  if (match(TokenType::Null)) {
    advance();
    return std::make_unique<Expression>(NullLiteral{});
  }

  if (match(TokenType::Identifier)) {
    std::string name = current().value;
    advance();
    return std::make_unique<Expression>(Identifier{name});
  }

  if (match(TokenType::Async)) {
    if (peek().type == TokenType::Function) {
      return parseFunctionExpression();
    }
  }

  if (match(TokenType::Function)) {
    return parseFunctionExpression();
  }

  if (match(TokenType::LeftParen)) {
    advance();
    auto expr = parseExpression();
    expect(TokenType::RightParen);
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
    elements.push_back(parseExpression());
  }

  expect(TokenType::RightBracket);
  return std::make_unique<Expression>(ArrayExpr{std::move(elements)});
}

ExprPtr Parser::parseObjectExpression() {
  expect(TokenType::LeftBrace);

  std::vector<ObjectProperty> properties;
  while (!match(TokenType::RightBrace)) {
    if (!properties.empty()) {
      expect(TokenType::Comma);
      if (match(TokenType::RightBrace)) break;
    }

    ExprPtr key;
    if (match(TokenType::Identifier)) {
      key = std::make_unique<Expression>(Identifier{current().value});
      advance();
    } else if (match(TokenType::String)) {
      key = std::make_unique<Expression>(StringLiteral{current().value});
      advance();
    }

    expect(TokenType::Colon);
    auto value = parseExpression();

    properties.push_back({std::move(key), std::move(value)});
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

  std::string name;
  if (match(TokenType::Identifier)) {
    name = current().value;
    advance();
  }

  expect(TokenType::LeftParen);

  std::vector<Identifier> params;
  while (!match(TokenType::RightParen)) {
    if (!params.empty()) {
      expect(TokenType::Comma);
    }
    if (match(TokenType::Identifier)) {
      params.push_back({current().value});
      advance();
    }
  }

  expect(TokenType::RightParen);

  auto block = parseBlockStatement();
  auto blockStmt = std::get_if<BlockStmt>(&block->node);

  FunctionExpr funcExpr;
  funcExpr.params = std::move(params);
  funcExpr.name = name;
  funcExpr.isAsync = isAsync;
  if (blockStmt) {
    funcExpr.body = std::move(blockStmt->body);
  }

  return std::make_unique<Expression>(std::move(funcExpr));
}

}