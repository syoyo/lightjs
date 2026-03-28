#include "parser_internal.h"

namespace lightjs {

StmtPtr Parser::parseForStatement() {
  struct DepthGuard {
    int& depth;
    explicit DepthGuard(int& d) : depth(d) { ++depth; }
    ~DepthGuard() { --depth; }
  };

  expect(TokenType::For);
  bool isAwait = false;
  if (match(TokenType::Await)) {
    isAwait = true;
    advance();
  }
  if (!expect(TokenType::LeftParen)) {
    return nullptr;
  }

  size_t savedPos = pos_;
  bool isForInOrOf = false;
  bool isForOf = false;

  if ((match(TokenType::Let) && !current().escaped) || match(TokenType::Const) ||
      match(TokenType::Var) || isUsingDeclarationStart(true) ||
      isAwaitUsingDeclarationStart(true)) {
    auto declType = current().type;
    advance();
    if (declType == TokenType::Await) {
      if (!match(TokenType::Using)) {
        pos_ = savedPos;
        return nullptr;
      }
      advance();
      declType = TokenType::Await;
    }
    if (!strictMode_ && declType == TokenType::Let && match(TokenType::In)) {
      isForInOrOf = true;
      isForOf = false;
    } else if (!strictMode_ && declType == TokenType::Let && match(TokenType::Of)) {
      pos_ = savedPos;
      return nullptr;
    } else if (isIdentifierLikeToken(current().type)) {
      advance();
      if (match(TokenType::In)) {
        isForInOrOf = true;
        isForOf = false;
      } else if (match(TokenType::Of)) {
        isForInOrOf = true;
        isForOf = true;
      }
    } else if (match(TokenType::LeftBracket) || match(TokenType::LeftBrace)) {
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
    while (match(TokenType::Dot) || match(TokenType::LeftBracket)) {
      if (match(TokenType::Dot)) {
        advance();
        if (match(TokenType::Identifier) ||
            isIdentifierLikeToken(current().type) ||
            match(TokenType::PrivateIdentifier)) {
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
  } else if (match(TokenType::LeftParen)) {
    int parenDepth = 1;
    advance();
    while (parenDepth > 0 && !match(TokenType::EndOfFile)) {
      if (match(TokenType::LeftParen)) parenDepth++;
      else if (match(TokenType::RightParen)) parenDepth--;
      advance();
    }
    while (match(TokenType::Dot) || match(TokenType::LeftBracket)) {
      if (match(TokenType::Dot)) {
        advance();
        if (match(TokenType::Identifier) ||
            isIdentifierLikeToken(current().type) ||
            match(TokenType::PrivateIdentifier)) {
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
  } else if (isIdentifierLikeToken(current().type) ||
             match(TokenType::PrivateIdentifier) ||
             match(TokenType::This)) {
    bool startsWithAsync = match(TokenType::Async);
    advance();
    while (match(TokenType::Dot) || match(TokenType::LeftBracket)) {
      if (match(TokenType::Dot)) {
        advance();
        if (match(TokenType::Identifier) ||
            isIdentifierLikeToken(current().type) ||
            match(TokenType::PrivateIdentifier)) {
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
      if (!(startsWithAsync && peek().type == TokenType::Arrow)) {
        isForInOrOf = true;
        isForOf = true;
      }
    }
  }

  pos_ = savedPos;

  if (isAwait && (!isForInOrOf || !isForOf)) {
    return nullptr;
  }

  if (isForInOrOf) {
    StmtPtr left = nullptr;
    bool letAsIdentifier = (!strictMode_ && match(TokenType::Let) &&
      peek().type == TokenType::In) || (match(TokenType::Let) && current().escaped);
    if (!letAsIdentifier &&
        ((match(TokenType::Let) && !current().escaped) || match(TokenType::Const) ||
         match(TokenType::Var) || isUsingDeclarationStart(true) ||
         isAwaitUsingDeclarationStart(true))) {
      VarDeclaration::Kind kind;
      switch (current().type) {
        case TokenType::Let: kind = VarDeclaration::Kind::Let; break;
        case TokenType::Const: kind = VarDeclaration::Kind::Const; break;
        case TokenType::Var: kind = VarDeclaration::Kind::Var; break;
        case TokenType::Using: kind = VarDeclaration::Kind::Using; break;
        case TokenType::Await:
          advance();
          if (!expect(TokenType::Using)) {
            return nullptr;
          }
          kind = VarDeclaration::Kind::AwaitUsing;
          break;
        default: return nullptr;
      }
      if (kind != VarDeclaration::Kind::AwaitUsing) {
        advance();
      }

      ExprPtr pattern;
      if (kind == VarDeclaration::Kind::Using ||
          kind == VarDeclaration::Kind::AwaitUsing) {
        if (!isIdentifierLikeToken(current().type)) {
          return nullptr;
        }
        pattern = makeExpr(Identifier{current().value}, current());
        advance();
      } else {
        pattern = parsePattern();
      }
      if (!pattern) {
        return nullptr;
      }

      if (strictMode_) {
        std::vector<std::string> boundNames;
        collectBoundNames(*pattern, boundNames);
        for (auto& name : boundNames) {
          if (name == "eval" || name == "arguments") {
            return nullptr;
          }
        }
      }

      if (kind != VarDeclaration::Kind::Var && hasDuplicateBoundNames(*pattern)) {
        return nullptr;
      }

      if (kind != VarDeclaration::Kind::Var) {
        std::vector<std::string> boundNames;
        collectBoundNames(*pattern, boundNames);
        for (auto& name : boundNames) {
          if (name == "let") return nullptr;
        }
      }
      if ((kind == VarDeclaration::Kind::Using ||
           kind == VarDeclaration::Kind::AwaitUsing) && !isForOf) {
        return nullptr;
      }

      VarDeclaration decl;
      decl.kind = kind;
      decl.declarations.push_back({std::move(pattern), nullptr});
      left = std::make_unique<Statement>(std::move(decl));
    } else if (match(TokenType::LeftBracket) || match(TokenType::LeftBrace)) {
      size_t peekPos = pos_;
      int peekDepth = 1;
      peekPos++;
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
        auto expr = parseMember();
        if (!expr) return nullptr;
        left = std::make_unique<Statement>(ExpressionStmt{std::move(expr)});
      } else {
        auto pattern = parsePattern();
        if (!pattern) {
          return nullptr;
        }
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
      if (strictMode_ && hasStrictModeInvalidTargets(*expr)) {
        return nullptr;
      }
      left = std::make_unique<Statement>(ExpressionStmt{std::move(expr)});
    }

    bool isOf = match(TokenType::Of);
    if (isOf) {
      if (current().escaped) {
        return nullptr;
      }
      if (left) {
        auto* exprStmt = std::get_if<ExpressionStmt>(&left->node);
        if (exprStmt && exprStmt->expression) {
          auto* ident = std::get_if<Identifier>(&exprStmt->expression->node);
          if (ident && ident->name == "async") {
            if (!isAwait &&
                pos_ >= 2 &&
                tokens_[pos_ - 1].type == TokenType::Async &&
                !tokens_[pos_ - 1].escaped) {
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

    auto right = parseAssignment();
    if (!right) return nullptr;
    if (match(TokenType::Comma)) {
      if (isOf) {
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

    if (match(TokenType::Function) || match(TokenType::Class)) {
      return nullptr;
    }
    if (match(TokenType::Const)) {
      return nullptr;
    }
    if (match(TokenType::Let)) {
      if (peek().type == TokenType::LeftBracket) {
        return nullptr;
      }
      if (strictMode_) {
        return nullptr;
      }
      if (peek().line == current().line &&
          (peek().type == TokenType::Identifier || peek().type == TokenType::LeftBrace)) {
        return nullptr;
      }
    }
    if (match(TokenType::Async) && peek().type == TokenType::Function) {
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
        return nullptr;
      }
    }

    if (left) {
      if (auto* varDecl = std::get_if<VarDeclaration>(&left->node)) {
        if (varDecl->kind == VarDeclaration::Kind::Let ||
            varDecl->kind == VarDeclaration::Kind::Const ||
            varDecl->kind == VarDeclaration::Kind::AwaitUsing) {
          std::vector<std::string> headNames;
          for (auto& decl : varDecl->declarations) {
            if (decl.pattern) collectBoundNames(*decl.pattern, headNames);
          }
          std::vector<std::string> bodyVarNames;
          collectVarDeclaredNames(*body, bodyVarNames, false);
          std::set<std::string> headSet(headNames.begin(), headNames.end());
          for (auto& name : bodyVarNames) {
            if (headSet.count(name)) {
              return nullptr;
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

  StmtPtr init = nullptr;
  if (!match(TokenType::Semicolon)) {
    bool isLetDecl = match(TokenType::Let) && !current().escaped;
    if (isLetDecl && !strictMode_) {
      auto nextType = peek().type;
      if (nextType != TokenType::Identifier && nextType != TokenType::LeftBracket &&
          nextType != TokenType::LeftBrace && nextType != TokenType::Await &&
          nextType != TokenType::Yield && nextType != TokenType::Async &&
          nextType != TokenType::Get && nextType != TokenType::Set &&
          nextType != TokenType::From && nextType != TokenType::As &&
          nextType != TokenType::Of && nextType != TokenType::Static &&
          nextType != TokenType::Let) {
        isLetDecl = false;
      }
    }
    if (isLetDecl || match(TokenType::Const) || match(TokenType::Var) ||
        isUsingDeclarationStart(true) || isAwaitUsingDeclarationStart(true)) {
      bool savedAllowIn = allowIn_;
      allowIn_ = false;
      init = parseVarDeclaration();
      allowIn_ = savedAllowIn;
      if (!init) {
        return nullptr;
      }
    } else {
      bool savedAllowIn = allowIn_;
      allowIn_ = false;
      auto expr = parseExpression();
      allowIn_ = savedAllowIn;
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
  if (init) {
    if (auto* varDecl = std::get_if<VarDeclaration>(&init->node)) {
      if (varDecl->kind == VarDeclaration::Kind::Let ||
          varDecl->kind == VarDeclaration::Kind::Const ||
          varDecl->kind == VarDeclaration::Kind::AwaitUsing) {
        std::vector<std::string> headNames;
        for (auto& decl : varDecl->declarations) {
          if (decl.pattern) collectBoundNames(*decl.pattern, headNames);
        }
        std::vector<std::string> bodyVarNames;
        collectVarDeclaredNames(*body, bodyVarNames, false);
        std::set<std::string> headSet(headNames.begin(), headNames.end());
        for (auto& name : bodyVarNames) {
          if (headSet.count(name)) {
            error_ = true;
            return nullptr;
          }
        }
      }
    }
  }

  return std::make_unique<Statement>(ForStmt{
    std::move(init),
    std::move(test),
    std::move(update),
    std::move(body)
  });
}

StmtPtr Parser::parseDoWhileStatement() {
  struct DepthGuard {
    int& depth;
    explicit DepthGuard(int& d) : depth(d) { ++depth; }
    ~DepthGuard() { --depth; }
  };

  expect(TokenType::Do);
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
  struct DepthGuard {
    int& depth;
    explicit DepthGuard(int& d) : depth(d) { ++depth; }
    ~DepthGuard() { --depth; }
  };

  expect(TokenType::Switch);
  if (!expect(TokenType::LeftParen)) {
    error_ = true;
    return nullptr;
  }
  if (match(TokenType::RightParen)) {
    error_ = true;
    return nullptr;
  }
  auto discriminant = parseExpression();
  if (!discriminant) {
    error_ = true;
    return nullptr;
  }
  if (!expect(TokenType::RightParen)) {
    error_ = true;
    return nullptr;
  }
  if (!expect(TokenType::LeftBrace)) {
    error_ = true;
    return nullptr;
  }

  std::vector<SwitchCase> cases;
  bool hasDefault = false;
  DepthGuard switchGuard(switchDepth_);
  while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
    if (match(TokenType::Case)) {
      advance();
      if (match(TokenType::Colon)) {
        error_ = true;
        return nullptr;
      }
      auto test = parseExpression();
      if (!test) {
        error_ = true;
        return nullptr;
      }
      expect(TokenType::Colon);

      std::vector<StmtPtr> consequent;
      while (!match(TokenType::Case) && !match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
        if (match(TokenType::Default)) {
          break;
        }
        if (error_) {
          return nullptr;
        }
        switchCaseDepth_++;
        if (auto stmt = parseStatement()) {
          switchCaseDepth_--;
          consequent.push_back(std::move(stmt));
        } else {
          switchCaseDepth_--;
          error_ = true;
          return nullptr;
        }
      }

      cases.push_back(SwitchCase{std::move(test), std::move(consequent)});
    } else if (match(TokenType::Default)) {
      if (hasDefault) {
        error_ = true;
        return nullptr;
      }
      hasDefault = true;
      advance();
      expect(TokenType::Colon);

      std::vector<StmtPtr> consequent;
      while (!match(TokenType::Case) && !match(TokenType::Default) &&
             !match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
        if (error_) {
          return nullptr;
        }
        switchCaseDepth_++;
        if (auto stmt = parseStatement()) {
          switchCaseDepth_--;
          consequent.push_back(std::move(stmt));
        } else {
          switchCaseDepth_--;
          error_ = true;
          return nullptr;
        }
      }

      cases.push_back(SwitchCase{nullptr, std::move(consequent)});
    } else {
      break;
    }
  }

  expect(TokenType::RightBrace);

  {
    std::vector<std::string> lexNames;
    std::vector<std::string> varNames;
    for (const auto& sc : cases) {
      for (const auto& stmt : sc.consequent) {
        if (auto* varDecl = std::get_if<VarDeclaration>(&stmt->node)) {
          if (varDecl->kind == VarDeclaration::Kind::Let ||
              varDecl->kind == VarDeclaration::Kind::Const ||
              varDecl->kind == VarDeclaration::Kind::AwaitUsing) {
            for (const auto& decl : varDecl->declarations) {
              if (auto* ident = std::get_if<Identifier>(&decl.pattern->node)) {
                for (const auto& existing : lexNames) {
                  if (existing == ident->name) {
                    error_ = true;
                    return nullptr;
                  }
                }
                lexNames.push_back(ident->name);
              }
            }
          } else {
            for (const auto& decl : varDecl->declarations) {
              if (auto* ident = std::get_if<Identifier>(&decl.pattern->node)) {
                varNames.push_back(ident->name);
              }
            }
          }
        } else if (auto* funcDecl = std::get_if<FunctionDeclaration>(&stmt->node)) {
          for (const auto& existing : lexNames) {
            if (existing == funcDecl->id.name) {
              error_ = true;
              return nullptr;
            }
          }
          lexNames.push_back(funcDecl->id.name);
        } else if (auto* classDecl = std::get_if<ClassDeclaration>(&stmt->node)) {
          for (const auto& existing : lexNames) {
            if (existing == classDecl->id.name) {
              error_ = true;
              return nullptr;
            }
          }
          lexNames.push_back(classDecl->id.name);
        }
      }
    }
    for (const auto& v : varNames) {
      for (const auto& l : lexNames) {
        if (v == l) {
          error_ = true;
          return nullptr;
        }
      }
    }
  }

  return std::make_unique<Statement>(SwitchStmt{std::move(discriminant), std::move(cases)});
}

}  // namespace lightjs
