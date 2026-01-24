#pragma once

#include "token.h"
#include "ast.h"
#include <vector>
#include <optional>

namespace tinyjs {

class Parser {
public:
  explicit Parser(std::vector<Token> tokens);

  std::optional<Program> parse();

private:
  std::vector<Token> tokens_;
  size_t pos_ = 0;

  const Token& current() const;
  const Token& peek(size_t offset = 1) const;
  const Token& advance();
  bool match(TokenType type) const;
  bool expect(TokenType type);
  void consumeSemicolon();

  StmtPtr parseStatement();
  StmtPtr parseVarDeclaration();
  StmtPtr parseFunctionDeclaration();
  StmtPtr parseClassDeclaration();
  StmtPtr parseReturnStatement();
  StmtPtr parseIfStatement();
  StmtPtr parseWhileStatement();
  StmtPtr parseDoWhileStatement();
  StmtPtr parseForStatement();
  StmtPtr parseSwitchStatement();
  StmtPtr parseBlockStatement();
  StmtPtr parseExpressionStatement();
  StmtPtr parseTryStatement();
  StmtPtr parseImportDeclaration();
  StmtPtr parseExportDeclaration();

  ExprPtr parseExpression();
  ExprPtr parseAssignment();
  ExprPtr parseConditional();
  ExprPtr parseNullishCoalescing();
  ExprPtr parseLogicalOr();
  ExprPtr parseLogicalAnd();
  ExprPtr parseEquality();
  ExprPtr parseRelational();
  ExprPtr parseAdditive();
  ExprPtr parseMultiplicative();
  ExprPtr parseExponentiation();
  ExprPtr parseUnary();
  ExprPtr parsePostfix();
  ExprPtr parseCall();
  ExprPtr parseMember();
  ExprPtr parseMemberSuffix(ExprPtr expr);
  ExprPtr parsePrimary();
  ExprPtr parseArrayExpression();
  ExprPtr parseObjectExpression();
  ExprPtr parseFunctionExpression();
  ExprPtr parseClassExpression();
  ExprPtr parseNewExpression();
  ExprPtr parsePattern();  // Parse destructuring patterns
  ExprPtr parseArrayPattern();
  ExprPtr parseObjectPattern();
};

}
