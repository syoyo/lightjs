#include "parser_internal.h"

namespace lightjs {

StmtPtr Parser::parseBlockStatement() {
  expect(TokenType::LeftBrace);
  blockDepth_++;
  bool isFunctionBody = parsingFunctionBody_;
  parsingFunctionBody_ = false;

  bool prevSingleStmt = inSingleStatementPosition_;
  inSingleStatementPosition_ = false;

  std::vector<StmtPtr> body;
  while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
    auto stmt = parseStatement();
    if (!stmt) {
      blockDepth_--;
      inSingleStatementPosition_ = prevSingleStmt;
      return nullptr;
    }

    if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt->node)) {
      if (auto* seqExpr = std::get_if<SequenceExpr>(&exprStmt->expression->node)) {
        for (auto& childExpr : seqExpr->expressions) {
          auto newStmt = std::make_unique<Statement>(ExpressionStmt{std::move(childExpr)});
          newStmt->loc = stmt->loc;
          body.push_back(std::move(newStmt));
        }
        continue;
      }
    }

    body.push_back(std::move(stmt));
  }
  inSingleStatementPosition_ = prevSingleStmt;

  expect(TokenType::RightBrace);
  blockDepth_--;

  {
    std::vector<std::string> lexicalNames;
    if (!collectStatementListLexicalNames(body, lexicalNames,
                                          !isFunctionBody)) {
      return nullptr;
    }
    std::vector<std::string> varNames;
    collectStatementListVarNames(body, varNames,
                                 isFunctionBody);
    if (hasNameCollision(lexicalNames, varNames)) {
      return nullptr;
    }
  }

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
  if (!consumeSemicolonOrASI()) {
    return nullptr;
  }
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
      if (match(TokenType::RightParen)) {
        error_ = true;
        return nullptr;
      }
      auto pattern = parsePattern();
      if (!pattern) {
        return nullptr;
      }
      std::vector<std::string> catchParamNames;
      collectBoundNames(*pattern, catchParamNames);

      {
        std::set<std::string> seen;
        for (const auto& name : catchParamNames) {
          if (!seen.insert(name).second) {
            error_ = true;
            return nullptr;
          }
        }
      }

      if (strictMode_) {
        for (const auto& name : catchParamNames) {
          if (name == "eval" || name == "arguments") {
            error_ = true;
            return nullptr;
          }
        }
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

    if (!handler.param.name.empty() || handler.paramPattern) {
      std::vector<std::string> catchParamNames;
      if (!handler.param.name.empty()) {
        catchParamNames.push_back(handler.param.name);
      }
      if (handler.paramPattern) {
        collectBoundNames(*handler.paramPattern, catchParamNames);
      }
      std::set<std::string> paramSet(catchParamNames.begin(), catchParamNames.end());
      for (const auto& stmt : catchBlockStmt->body) {
        if (auto* varDecl = std::get_if<VarDeclaration>(&stmt->node)) {
          if (varDecl->kind == VarDeclaration::Kind::Let ||
              varDecl->kind == VarDeclaration::Kind::Const ||
              varDecl->kind == VarDeclaration::Kind::AwaitUsing) {
            for (const auto& decl : varDecl->declarations) {
              if (auto* ident = std::get_if<Identifier>(&decl.pattern->node)) {
                if (paramSet.count(ident->name)) {
                  error_ = true;
                  return nullptr;
                }
              }
            }
          }
        } else if (auto* funcDecl = std::get_if<FunctionDeclaration>(&stmt->node)) {
          if (paramSet.count(funcDecl->id.name)) {
            error_ = true;
            return nullptr;
          }
        } else if (auto* classDecl = std::get_if<ClassDeclaration>(&stmt->node)) {
          if (paramSet.count(classDecl->id.name)) {
            error_ = true;
            return nullptr;
          }
        }
      }
    }

    handler.body = std::move(catchBlockStmt->body);
  }

  if (match(TokenType::Finally)) {
    advance();
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

}  // namespace lightjs
