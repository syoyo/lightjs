#include "parser_internal.h"

namespace lightjs {

ExprPtr Parser::parseExpression() {
  if (++parseDepth_ > kMaxParseDepth) {
    --parseDepth_;
    throw std::runtime_error("SyntaxError: Maximum parse depth exceeded");
  }
  struct ParseDepthGuard { int& d; ~ParseDepthGuard() { --d; } } _pdg{parseDepth_};

  auto expr = parseAssignment();
  if (!expr) return nullptr;

  if (!match(TokenType::Comma)) return expr;

  SequenceExpr seq;
  seq.expressions.push_back(std::move(expr));
  while (match(TokenType::Comma)) {
    auto savedPos = pos_;
    advance();
    auto next = parseAssignment();
    if (!next) {
      pos_ = savedPos;
      break;
    }
    seq.expressions.push_back(std::move(next));
  }

  if (seq.expressions.size() == 1) {
    return std::move(seq.expressions[0]);
  }
  return std::make_unique<Expression>(std::move(seq));
}

ExprPtr Parser::parseAssignment() {
  auto parseArrowBodyInto = [&](FunctionExpr& func) -> bool {
    auto savedIterLabels = std::move(iterationLabels_);
    iterationLabels_.clear();
    auto savedActLabels = std::move(activeLabels_);
    activeLabels_.clear();
    int savedLoopDepth = loopDepth_;
    loopDepth_ = 0;
    int savedSwitchDepth = switchDepth_;
    switchDepth_ = 0;
    int savedStaticBlockDepth = staticBlockDepth_;
    staticBlockDepth_ = 0;
    if (match(TokenType::LeftBrace)) {
      parsingFunctionBody_ = true;
      auto blockStmt = parseBlockStatement();
      parsingFunctionBody_ = false;
      staticBlockDepth_ = savedStaticBlockDepth;
      switchDepth_ = savedSwitchDepth;
      loopDepth_ = savedLoopDepth;
      iterationLabels_ = std::move(savedIterLabels);
      activeLabels_ = std::move(savedActLabels);
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
    staticBlockDepth_ = savedStaticBlockDepth;
    switchDepth_ = savedSwitchDepth;
    loopDepth_ = savedLoopDepth;
    iterationLabels_ = std::move(savedIterLabels);
    activeLabels_ = std::move(savedActLabels);
    if (!expr) {
      return false;
    }
    auto returnStmt = std::make_unique<Statement>(ReturnStmt{std::move(expr)});
    func.body.push_back(std::move(returnStmt));
    return true;
  };

  auto validateArrowFunction = [&](const FunctionExpr& func,
                                   bool hasNonSimpleParams,
                                   bool hasDuplicateParams,
                                   bool hasYieldExpressionInParams,
                                   const std::vector<std::string>& boundParamNames) -> bool {
    bool hadLegacyEscapeBeforeStrict = false;
    bool hasUseStrictDirective = hasUseStrictDirectiveInBody(func.body, &hadLegacyEscapeBeforeStrict);
    bool strictFunctionCode = strictMode_ || hasUseStrictDirective;
    if (hasUseStrictDirective && hadLegacyEscapeBeforeStrict) {
      error_ = true;
      return false;
    }
    if (hasUseStrictDirective && hasNonSimpleParams) {
      return false;
    }
    if (hasYieldExpressionInParams) {
      return false;
    }
    if (hasDuplicateParams) {
      return false;
    }
    for (const auto& name : boundParamNames) {
      if (name == "enum") {
        return false;
      }
      if (strictFunctionCode &&
          (name == "eval" || name == "arguments" || isStrictFutureReservedIdentifier(name))) {
        return false;
      }
    }
    if (!boundParamNames.empty()) {
      std::vector<std::string> lexNames;
      collectStatementListLexicalNames(func.body, lexNames, false);
      for (const auto& paramName : boundParamNames) {
        for (const auto& lexName : lexNames) {
          if (paramName == lexName) {
            return false;
          }
        }
      }
    }
    if (strictFunctionCode) {
      for (const auto& stmt : func.body) {
        if (stmt && statementContainsStrictRestrictedIdentifierReference(*stmt)) {
          return false;
        }
      }
    }
    return true;
  };

  if (match(TokenType::Async) && !current().escaped) {
    size_t savedPos = pos_;
    uint32_t asyncLine = current().line;
    advance();

    if (isIdentifierLikeToken(current().type) && current().line == asyncLine) {
      auto paramName = current().value;
      advance();
      if (match(TokenType::Arrow) && tokens_[pos_ - 1].line == current().line) {
        advance();
        FunctionExpr func;
        func.isAsync = true;
        func.isArrow = true;
        Parameter param;
        param.name = Identifier{paramName};
        func.params.push_back(std::move(param));
        std::vector<std::string> boundParamNames{paramName};
        ++functionDepth_;
        awaitContextStack_.push_back(true);
        ++asyncFunctionDepth_;
        if (!parseArrowBodyInto(func)) {
          --asyncFunctionDepth_;
          --functionDepth_;
          awaitContextStack_.pop_back();
          return nullptr;
        }
        --asyncFunctionDepth_;
        --functionDepth_;
        awaitContextStack_.pop_back();
        if (!validateArrowFunction(func, false, false, false, boundParamNames)) {
          return nullptr;
        }
        return std::make_unique<Expression>(std::move(func));
      }
      pos_ = savedPos;
    } else if (match(TokenType::LeftParen) && current().line == asyncLine) {
      int prevFunctionDepth = functionDepth_;
      int prevAsyncDepth = asyncFunctionDepth_;
      size_t prevAwaitContextDepth = awaitContextStack_.size();
      ++functionDepth_;
      awaitContextStack_.push_back(true);
      ++asyncFunctionDepth_;
      advance();
      std::vector<Parameter> params;
      std::optional<Identifier> restParam;
      std::vector<StmtPtr> destructurePrologue;
      std::vector<std::string> boundParamNames;
      std::set<std::string> seenParamNames;
      bool hasDuplicateParams = false;
      bool hasNonSimpleParams = false;
      bool validParams = true;

      if (!match(TokenType::RightParen)) {
        do {
          if (match(TokenType::DotDotDot)) {
            hasNonSimpleParams = true;
            advance();
            if (isIdentifierLikeToken(current().type)) {
              restParam = Identifier{current().value};
              boundParamNames.push_back(restParam->name);
              if (!seenParamNames.insert(restParam->name).second) {
                hasDuplicateParams = true;
              }
              advance();
            } else if (match(TokenType::LeftBracket) || match(TokenType::LeftBrace)) {
              auto pattern = parsePattern();
              if (!pattern) {
                validParams = false;
                break;
              }
              std::vector<std::string> names;
              collectBoundNames(*pattern, names);
              for (const auto& name : names) {
                boundParamNames.push_back(name);
                if (!seenParamNames.insert(name).second) {
                  hasDuplicateParams = true;
                }
              }
              std::string tempName = "__arrow_rest_" + std::to_string(arrowDestructureTempCounter_++);
              restParam = Identifier{tempName};

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
            break;
          }

          Parameter param;
          bool hasDestructurePattern = false;
          if (isIdentifierLikeToken(current().type)) {
            param.name = Identifier{current().value};
            boundParamNames.push_back(param.name.name);
            if (!seenParamNames.insert(param.name.name).second) {
              hasDuplicateParams = true;
            }
            advance();
          } else if (match(TokenType::LeftBracket) || match(TokenType::LeftBrace)) {
            hasNonSimpleParams = true;
            auto pattern = parsePattern();
            if (!pattern) {
              validParams = false;
              break;
            }
            std::vector<std::string> names;
            collectBoundNames(*pattern, names);
            for (const auto& name : names) {
              boundParamNames.push_back(name);
              if (!seenParamNames.insert(name).second) {
                hasDuplicateParams = true;
              }
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
            hasNonSimpleParams = true;
            if (hasDestructurePattern) {
              validParams = false;
              break;
            }
            advance();
            param.defaultValue = parseAssignment();
            if (!param.defaultValue) {
              --asyncFunctionDepth_;
              --functionDepth_;
              awaitContextStack_.pop_back();
              return nullptr;
            }
            if (expressionContainsAwaitLike(*param.defaultValue)) {
              --asyncFunctionDepth_;
              --functionDepth_;
              awaitContextStack_.pop_back();
              return nullptr;
            }
          }

          params.push_back(std::move(param));

          if (match(TokenType::Comma)) {
            advance();
            if (match(TokenType::RightParen)) {
              break;
            }
          } else {
            break;
          }
        } while (true);
      }

      if (validParams && match(TokenType::RightParen)) {
        advance();
        if (match(TokenType::Arrow) && tokens_[pos_ - 1].line == current().line) {
          bool hasYieldExpressionInParams = false;
          if (!yieldContextStack_.empty() && yieldContextStack_.back()) {
            for (size_t i = savedPos; i < pos_; ++i) {
              if (tokens_[i].type == TokenType::Yield) {
                hasYieldExpressionInParams = true;
                break;
              }
            }
          }
          advance();
          FunctionExpr func;
          func.isAsync = true;
          func.isArrow = true;
          func.params = std::move(params);
          func.restParam = restParam;
          func.destructurePrologue = std::move(destructurePrologue);
          if (!parseArrowBodyInto(func)) {
            --asyncFunctionDepth_;
            --functionDepth_;
            awaitContextStack_.pop_back();
            return nullptr;
          }
          --asyncFunctionDepth_;
          --functionDepth_;
          awaitContextStack_.pop_back();
          if (!validateArrowFunction(func, hasNonSimpleParams, hasDuplicateParams,
                                     hasYieldExpressionInParams, boundParamNames)) {
            return nullptr;
          }
          return std::make_unique<Expression>(std::move(func));
        }
      }
      functionDepth_ = prevFunctionDepth;
      asyncFunctionDepth_ = prevAsyncDepth;
      awaitContextStack_.resize(prevAwaitContextDepth);
      pos_ = savedPos;
    } else {
      pos_ = savedPos;
    }
  }

  if (isIdentifierLikeToken(current().type)) {
    size_t savedPos = pos_;
    auto paramName = current().value;
    advance();

    if (match(TokenType::Arrow) && tokens_[pos_ - 1].line == current().line) {
      advance();

      FunctionExpr func;
      func.isArrow = true;
      Parameter param;
      param.name = Identifier{paramName};
      func.params.push_back(std::move(param));
      std::vector<std::string> boundParamNames{paramName};

      ++functionDepth_;
      awaitContextStack_.push_back(false);
      if (!parseArrowBodyInto(func)) {
        --functionDepth_;
        awaitContextStack_.pop_back();
        return nullptr;
      }
      --functionDepth_;
      awaitContextStack_.pop_back();
      if (!validateArrowFunction(func, false, false, false, boundParamNames)) {
        return nullptr;
      }

      return std::make_unique<Expression>(std::move(func));
    }

    pos_ = savedPos;
  }

  if (match(TokenType::LeftParen)) {
    size_t savedPos = pos_;
    advance();

    std::vector<Parameter> params;
    std::optional<Identifier> restParam;
    std::vector<StmtPtr> destructurePrologue;
    std::vector<std::string> boundParamNames;
    std::set<std::string> seenParamNames;
    bool hasDuplicateParams = false;
    bool hasNonSimpleParams = false;
    bool isArrowFunc = false;

    if (!match(TokenType::RightParen)) {
      do {
        if (match(TokenType::DotDotDot)) {
          hasNonSimpleParams = true;
          advance();
          if (isIdentifierLikeToken(current().type)) {
            restParam = Identifier{current().value};
            boundParamNames.push_back(restParam->name);
            if (!seenParamNames.insert(restParam->name).second) {
              hasDuplicateParams = true;
            }
            advance();
          } else if (match(TokenType::LeftBracket) || match(TokenType::LeftBrace)) {
            auto pattern = parsePattern();
            if (!pattern) {
              pos_ = savedPos;
              goto normal_parse;
            }
            std::vector<std::string> names;
            collectBoundNames(*pattern, names);
            for (const auto& name : names) {
              boundParamNames.push_back(name);
              if (!seenParamNames.insert(name).second) {
                hasDuplicateParams = true;
              }
            }
            std::string tempName = "__arrow_rest_" + std::to_string(arrowDestructureTempCounter_++);
            restParam = Identifier{tempName};

            VarDeclaration destructDecl;
            destructDecl.kind = VarDeclaration::Kind::Let;
            VarDeclarator destructBinding;
            destructBinding.pattern = std::move(pattern);
            destructBinding.init = std::make_unique<Expression>(Identifier{tempName});
            destructDecl.declarations.push_back(std::move(destructBinding));
            destructurePrologue.push_back(std::make_unique<Statement>(std::move(destructDecl)));
          } else {
            pos_ = savedPos;
            goto normal_parse;
          }
          break;
        }

        Parameter param;
        bool hasDestructurePattern = false;
        if (isIdentifierLikeToken(current().type)) {
          param.name = Identifier{current().value};
          boundParamNames.push_back(param.name.name);
          if (!seenParamNames.insert(param.name.name).second) {
            hasDuplicateParams = true;
          }
          advance();
        } else if (match(TokenType::LeftBracket) || match(TokenType::LeftBrace)) {
          hasNonSimpleParams = true;
          auto pattern = parsePattern();
          if (!pattern) {
            pos_ = savedPos;
            goto normal_parse;
          }
          std::vector<std::string> names;
          collectBoundNames(*pattern, names);
          for (const auto& name : names) {
            boundParamNames.push_back(name);
            if (!seenParamNames.insert(name).second) {
              hasDuplicateParams = true;
            }
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
          pos_ = savedPos;
          goto normal_parse;
        }

        if (match(TokenType::Equal)) {
          hasNonSimpleParams = true;
          if (hasDestructurePattern) {
            pos_ = savedPos;
            goto normal_parse;
          }
          advance();
          param.defaultValue = parseAssignment();
          if (!param.defaultValue) {
            return nullptr;
          }
          if (!canUseAwaitAsIdentifier() &&
              expressionContainsAwaitLike(*param.defaultValue)) {
            return nullptr;
          }
        }

        params.push_back(std::move(param));

        if (match(TokenType::Comma)) {
          advance();
          if (match(TokenType::RightParen)) {
            break;
          }
        } else {
          break;
        }
      } while (true);
    }

    if (match(TokenType::RightParen)) {
      advance();
      if (match(TokenType::Arrow) && tokens_[pos_ - 1].line == current().line) {
        isArrowFunc = true;
      }
    }

    if (isArrowFunc) {
      bool hasAwaitExpressionInParams = false;
      if (!canUseAwaitAsIdentifier()) {
        for (size_t i = savedPos; i < pos_; ++i) {
          if ((tokens_[i].type == TokenType::Await ||
               (tokens_[i].type == TokenType::Identifier && tokens_[i].value == "await")) &&
              !tokens_[i].escaped) {
            hasAwaitExpressionInParams = true;
            break;
          }
        }
      }
      if (hasAwaitExpressionInParams) {
        return nullptr;
      }
      bool hasYieldExpressionInParams = false;
      if (!yieldContextStack_.empty() && yieldContextStack_.back()) {
        for (size_t i = savedPos; i < pos_; ++i) {
          if (tokens_[i].type == TokenType::Yield) {
            hasYieldExpressionInParams = true;
            break;
          }
        }
      }
      advance();

      FunctionExpr func;
      func.isArrow = true;
      func.params = std::move(params);
      func.restParam = restParam;
      func.destructurePrologue = std::move(destructurePrologue);

      ++functionDepth_;
      awaitContextStack_.push_back(false);
      if (!parseArrowBodyInto(func)) {
        --functionDepth_;
        awaitContextStack_.pop_back();
        return nullptr;
      }
      --functionDepth_;
      awaitContextStack_.pop_back();
      if (!validateArrowFunction(func, hasNonSimpleParams, hasDuplicateParams,
                                 hasYieldExpressionInParams, boundParamNames)) {
        return nullptr;
      }

      return std::make_unique<Expression>(std::move(func));
    }

    pos_ = savedPos;
  }

normal_parse:
  if (match(TokenType::LeftBrace) || match(TokenType::LeftBracket)) {
    size_t savedPos = pos_;
    auto opener = current().type;
    auto closer = (opener == TokenType::LeftBrace) ? TokenType::RightBrace : TokenType::RightBracket;
    bool looksLikePatternAssignment = false;
    bool malformedRestAfterSpread = false;
    int depth = 0;
    bool sawTopLevelSpread = false;
    for (size_t i = savedPos; i < tokens_.size(); ++i) {
      auto t = tokens_[i].type;
      if (t == opener) {
        depth++;
      } else if (depth == 1 && t == TokenType::DotDotDot) {
        sawTopLevelSpread = true;
      } else if (depth == 1 && sawTopLevelSpread && t == TokenType::Comma) {
        malformedRestAfterSpread = true;
      } else if (t == closer) {
        depth--;
        if (depth == 0) {
          if (i + 1 < tokens_.size() && tokens_[i + 1].type == TokenType::Equal) {
            looksLikePatternAssignment = true;
          }
          break;
        }
      }
    }
    auto dstrLeft = parsePattern();
    if (!dstrLeft && looksLikePatternAssignment && (strictMode_ || malformedRestAfterSpread)) {
      return nullptr;
    }
    if (dstrLeft) {
      if (auto* assignPat = std::get_if<AssignmentPattern>(&dstrLeft->node)) {
        if (!assignPat->left || !assignPat->right) {
          return nullptr;
        }
        if (strictMode_ && hasStrictInvalidSimpleAssignmentTarget(*assignPat->left)) {
          return nullptr;
        }
        return std::make_unique<Expression>(AssignmentExpr{
          AssignmentExpr::Op::Assign, std::move(assignPat->left), std::move(assignPat->right)});
      }
    }
    if (dstrLeft && match(TokenType::Equal)) {
      advance();
      auto right = parseAssignment();
      if (!right) {
        return nullptr;
      }
      if (strictMode_ && hasStrictInvalidSimpleAssignmentTarget(*dstrLeft)) {
        return nullptr;
      }
      return std::make_unique<Expression>(AssignmentExpr{
        AssignmentExpr::Op::Assign, std::move(dstrLeft), std::move(right)});
    }
    pos_ = savedPos;
  }

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

    if (strictMode_ && hasStrictInvalidSimpleAssignmentTarget(*left)) {
      return nullptr;
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

    if (op == AssignmentExpr::Op::Assign &&
        (std::get_if<ArrayExpr>(&left->node) || std::get_if<ObjectExpr>(&left->node))) {
      if (!convertAssignmentTargetToPattern(*left, canUseYieldAsIdentifier(),
                                            canUseAwaitAsIdentifier(), strictMode_)) {
        return nullptr;
      }
    }

    auto right = parseAssignment();
    if (!right) {
      return nullptr;
    }
    return std::make_unique<Expression>(AssignmentExpr{op, std::move(left), std::move(right)});
  }

  return left;
}

}  // namespace lightjs
