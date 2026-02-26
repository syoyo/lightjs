#pragma once

#include "token.h"
#include "ast.h"
#include <vector>
#include <optional>
#include <set>

namespace lightjs {

class Parser {
public:
  explicit Parser(std::vector<Token> tokens, bool isModule = false);

  std::optional<Program> parse();
  void setStrictMode(bool strict) { strictMode_ = strict; }

private:
  std::vector<Token> tokens_;
  size_t pos_ = 0;
  bool isModule_ = false;
  bool strictMode_ = false;
  bool error_ = false;  // Set on syntax errors to abort parsing
  bool inSingleStatementPosition_ = false;  // True when parsing body of for/while/if/etc.
  int superCallDisallowDepth_ = 0;
  int functionDepth_ = 0;
  int asyncFunctionDepth_ = 0;
  int generatorFunctionDepth_ = 0;
  std::vector<bool> awaitContextStack_;
  size_t arrowDestructureTempCounter_ = 0;
  std::set<std::string> iterationLabels_;  // Labels wrapping iteration statements (for continue validation)
  std::set<std::string> activeLabels_;      // All active labels (for break validation)

  const Token& current() const;
  const Token& peek(size_t offset = 1) const;
  const Token& advance();
  bool match(TokenType type) const;
  bool expect(TokenType type);
  void consumeSemicolon();
  bool consumeSemicolonOrASI();
  bool canUseAwaitAsIdentifier() const;
  bool canParseAwaitExpression() const;
  bool canUseYieldAsIdentifier() const;
  bool isIdentifierLikeToken(TokenType type) const;

  StmtPtr parseStatement(bool allowModuleItem = false);
  StmtPtr parseVarDeclaration();
  StmtPtr parseFunctionDeclaration();
  StmtPtr parseClassDeclaration();
  StmtPtr parseReturnStatement();
  StmtPtr parseIfStatement();
  StmtPtr parseWhileStatement();
  StmtPtr parseWithStatement();
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
  ExprPtr parseBitwiseOr();
  ExprPtr parseBitwiseXor();
  ExprPtr parseBitwiseAnd();
  ExprPtr parseEquality();
  ExprPtr parseRelational();
  ExprPtr parseShift();
  ExprPtr parseAdditive();
  ExprPtr parseMultiplicative();
  ExprPtr parseExponentiation();
  ExprPtr parseUnary();
  ExprPtr parsePostfix();
  ExprPtr parseCall();
  ExprPtr parseMember();
  ExprPtr parseMemberSuffix(ExprPtr expr, bool inOptionalChain = false);
  ExprPtr parsePrimary();
  ExprPtr parseArrayExpression();
  ExprPtr parseObjectExpression();
  ExprPtr parseFunctionExpression();
  ExprPtr parseClassExpression();
  ExprPtr parseNewExpression();
  ExprPtr parsePattern();  // Parse destructuring patterns
  ExprPtr parseArrayPattern();
  ExprPtr parseObjectPattern();

  // Helper to create expression with source location
  template<typename T>
  ExprPtr makeExpr(T&& node, const Token& tok) {
    auto expr = std::make_unique<Expression>(std::forward<T>(node));
    expr->loc = SourceLocation(tok.line, tok.column);
    return expr;
  }

  // Helper to create statement with source location
  template<typename T>
  StmtPtr makeStmt(T&& node, const Token& tok) {
    auto stmt = std::make_unique<Statement>(std::forward<T>(node));
    stmt->loc = SourceLocation(tok.line, tok.column);
    return stmt;
  }
};

}
