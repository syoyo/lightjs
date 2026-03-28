#include "parser_internal.h"

namespace lightjs {

ExprPtr Parser::parseFunctionExpression() {
  const Token& fnStartTok = current();
  bool isAsync = false;
  if (match(TokenType::Async)) {
    if (current().escaped) {
      return nullptr;
    }
    isAsync = true;
    advance();
  }

  expect(TokenType::Function);

  bool isGenerator = false;
  if (match(TokenType::Star)) {
    isGenerator = true;
    advance();
  }

  std::string name;
  {
    awaitContextStack_.push_back(isAsync);
    yieldContextStack_.push_back(isGenerator);
    struct NameContextGuard {
      Parser* parser;
      ~NameContextGuard() {
        parser->awaitContextStack_.pop_back();
        parser->yieldContextStack_.pop_back();
      }
    } nameGuard{this};
    if (isIdentifierLikeToken(current().type) || (match(TokenType::Yield) && !strictMode_)) {
      name = current().value;
      advance();
    }
  }
  if ((isGenerator && name == "yield") || (isAsync && name == "await")) {
    return nullptr;
  }

  ++functionDepth_;
  ++newTargetDepth_;
  awaitContextStack_.push_back(isAsync);
  yieldContextStack_.push_back(isGenerator);
  if (isAsync) {
    ++asyncFunctionDepth_;
  }

  expect(TokenType::LeftParen);

  std::vector<Parameter> params;
  std::optional<Identifier> restParam;
  std::vector<StmtPtr> destructurePrologue;

  bool hasNonSimpleParams = false;
  std::vector<std::string> boundParamNames;
  std::set<std::string> seenParamNames;
  bool hasDuplicateParams = false;
  bool hasSuperInParams = false;
  bool hasRestrictedParamNames = false;

  while (!match(TokenType::RightParen)) {
    if (match(TokenType::DotDotDot)) {
      hasNonSimpleParams = true;
      advance();
      if (isIdentifierLikeToken(current().type)) {
        restParam = Identifier{current().value};
        if (isGenerator && restParam->name == "yield") {
          return nullptr;
        }
        if (isAsync && restParam->name == "await") {
          return nullptr;
        }
        if (!seenParamNames.insert(restParam->name).second) {
          hasDuplicateParams = true;
        }
        boundParamNames.push_back(restParam->name);
        if (restParam->name == "eval" || restParam->name == "arguments") {
          hasRestrictedParamNames = true;
        }
        advance();
      } else if (match(TokenType::LeftBracket) || match(TokenType::LeftBrace)) {
        auto pattern = parsePattern();
        if (!pattern) return nullptr;
        if (std::get_if<AssignmentPattern>(&pattern->node)) return nullptr;

        std::vector<std::string> names;
        collectBoundNames(*pattern, names);
        for (auto& name : names) {
          if (isGenerator && name == "yield") {
            return nullptr;
          }
          if (isAsync && name == "await") {
            return nullptr;
          }
          boundParamNames.push_back(name);
          if (!seenParamNames.insert(name).second) {
            hasDuplicateParams = true;
          }
          if (name == "eval" || name == "arguments") {
            hasRestrictedParamNames = true;
          }
        }

        std::string tempName = "__rest_param_" + std::to_string(arrowDestructureTempCounter_++);
        restParam = Identifier{tempName};
        VarDeclaration destructDecl;
        destructDecl.kind = VarDeclaration::Kind::Let;
        VarDeclarator destructBinding;
        destructBinding.pattern = std::move(pattern);
        destructBinding.init = std::make_unique<Expression>(Identifier{tempName});
        destructDecl.declarations.push_back(std::move(destructBinding));
        destructurePrologue.push_back(std::make_unique<Statement>(std::move(destructDecl)));
      } else {
        return nullptr;
      }
      if (match(TokenType::Comma)) return nullptr;
      break;
    }

    Parameter param;
    if (isIdentifierLikeToken(current().type)) {
      param.name = Identifier{current().value};
      if (isGenerator && param.name.name == "yield") {
        return nullptr;
      }
      if (isAsync && param.name.name == "await") {
        return nullptr;
      }
      boundParamNames.push_back(param.name.name);
      if (!seenParamNames.insert(param.name.name).second) {
        hasDuplicateParams = true;
      }
      if (param.name.name == "eval" || param.name.name == "arguments") {
        hasRestrictedParamNames = true;
      }
      advance();

      if (match(TokenType::Equal)) {
        hasNonSimpleParams = true;
        advance();
        param.defaultValue = parseAssignment();
        if (!param.defaultValue) return nullptr;
        if (isGenerator && expressionContainsYield(*param.defaultValue)) {
          return nullptr;
        }
        if (isAsync && expressionContainsAwaitLike(*param.defaultValue)) {
          return nullptr;
        }
        if (expressionContainsSuper(*param.defaultValue)) {
          hasSuperInParams = true;
        }
      }
    } else if (match(TokenType::LeftBracket) || match(TokenType::LeftBrace)) {
      hasNonSimpleParams = true;
      auto pattern = parsePattern();
      if (!pattern) return nullptr;
      if (auto* assignPat = std::get_if<AssignmentPattern>(&pattern->node)) {
        param.defaultValue = std::move(assignPat->right);
        if (!param.defaultValue) return nullptr;
        if (isGenerator && expressionContainsYield(*param.defaultValue)) {
          return nullptr;
        }
        if (isAsync && expressionContainsAwaitLike(*param.defaultValue)) {
          return nullptr;
        }
        if (expressionContainsSuper(*param.defaultValue)) {
          hasSuperInParams = true;
        }
        pattern = std::move(assignPat->left);
        if (!pattern) return nullptr;
      }

      std::vector<std::string> names;
      collectBoundNames(*pattern, names);
      for (auto& name2 : names) {
        if (isGenerator && name2 == "yield") {
          return nullptr;
        }
        if (isAsync && name2 == "await") {
          return nullptr;
        }
        boundParamNames.push_back(name2);
        if (!seenParamNames.insert(name2).second) {
          hasDuplicateParams = true;
        }
        if (name2 == "eval" || name2 == "arguments") {
          hasRestrictedParamNames = true;
        }
      }

      std::string tempName = "__param_" + std::to_string(arrowDestructureTempCounter_++);
      param.name = Identifier{tempName};

      VarDeclaration destructDecl;
      destructDecl.kind = VarDeclaration::Kind::Let;
      VarDeclarator destructBinding;
      destructBinding.pattern = std::move(pattern);
      destructBinding.init = std::make_unique<Expression>(Identifier{tempName});
      destructDecl.declarations.push_back(std::move(destructBinding));
      destructurePrologue.push_back(std::make_unique<Statement>(std::move(destructDecl)));
    } else {
      return nullptr;
    }

    params.push_back(std::move(param));

    if (match(TokenType::Comma)) {
      advance();
      if (match(TokenType::RightParen)) break;
    } else {
      break;
    }
  }

  expect(TokenType::RightParen);

  if (isGenerator) {
    ++generatorFunctionDepth_;
  }
  auto savedIterationLabels2 = std::move(iterationLabels_);
  iterationLabels_.clear();
  auto savedActiveLabels2 = std::move(activeLabels_);
  activeLabels_.clear();
  int savedLoopDepth2 = loopDepth_;
  loopDepth_ = 0;
  int savedSwitchDepth2 = switchDepth_;
  switchDepth_ = 0;
  int savedStaticBlockDepth2 = staticBlockDepth_;
  staticBlockDepth_ = 0;
  parsingFunctionBody_ = true;
  auto block = parseBlockStatement();
  parsingFunctionBody_ = false;
  staticBlockDepth_ = savedStaticBlockDepth2;
  switchDepth_ = savedSwitchDepth2;
  loopDepth_ = savedLoopDepth2;
  iterationLabels_ = std::move(savedIterationLabels2);
  activeLabels_ = std::move(savedActiveLabels2);
  if (isGenerator) {
    --generatorFunctionDepth_;
  }
  if (isAsync) {
    --asyncFunctionDepth_;
  }
  --newTargetDepth_;
  --functionDepth_;
  awaitContextStack_.pop_back();
  yieldContextStack_.pop_back();
  if (!block) {
    return nullptr;
  }
  auto blockStmt = std::get_if<BlockStmt>(&block->node);
  if (!blockStmt) {
    return nullptr;
  }

  bool hadLegacyEscape8355 = false;
  bool hasUseStrictDirective = hasUseStrictDirectiveInBody(blockStmt->body, &hadLegacyEscape8355);
  bool strictFunctionCode = strictMode_ || hasUseStrictDirective;
  if (hasUseStrictDirective && hadLegacyEscape8355) { error_ = true; return nullptr; }
  if (strictFunctionCode && (!name.empty() && (name == "eval" || name == "arguments"))) {
    return nullptr;
  }
  if (strictFunctionCode && hasRestrictedParamNames) {
    return nullptr;
  }
  if (hasUseStrictDirective && hasNonSimpleParams) {
    return nullptr;
  }
  if ((strictFunctionCode || hasNonSimpleParams) && hasDuplicateParams) {
    return nullptr;
  }
  if (strictFunctionCode) {
    for (const auto& stmt : blockStmt->body) {
      if (stmt && statementContainsStrictRestrictedIdentifierReference(*stmt)) {
        return nullptr;
      }
    }
  }
  {
    std::vector<std::string> lexicalNames;
    collectTopLevelLexicallyDeclaredNames(blockStmt->body, lexicalNames);
    std::set<std::string> paramNameSet(boundParamNames.begin(), boundParamNames.end());
    bool strictFnCode2 = strictMode_ || hasUseStrictDirective;
    std::set<std::string> funcDeclNamesInBody2;
    if (!strictFnCode2) {
      for (const auto& stmt : blockStmt->body) {
        if (!stmt) continue;
        if (auto* fnDecl = std::get_if<FunctionDeclaration>(&stmt->node)) {
          funcDeclNamesInBody2.insert(fnDecl->id.name);
        }
      }
    }
    for (const auto& lexName : lexicalNames) {
      if (paramNameSet.count(lexName) != 0) {
        if (!strictFnCode2 && funcDeclNamesInBody2.count(lexName) != 0) {
          continue;
        }
        return nullptr;
      }
    }
  }
  if (hasSuperInParams) {
    return nullptr;
  }
  for (const auto& stmt : blockStmt->body) {
    if (stmt && statementContainsSuper(*stmt)) {
      return nullptr;
    }
  }

  FunctionExpr funcExpr;
  funcExpr.params = std::move(params);
  funcExpr.restParam = restParam;
  funcExpr.name = name;
  funcExpr.isAsync = isAsync;
  funcExpr.isGenerator = isGenerator;
  funcExpr.body = std::move(blockStmt->body);
  funcExpr.destructurePrologue = std::move(destructurePrologue);
  if (!source_.empty() && fnStartTok.offset < source_.size()) {
    uint32_t endOff = (pos_ > 0 && pos_ <= tokens_.size()) ? tokens_[pos_ - 1].endOffset : static_cast<uint32_t>(source_.size());
    if (endOff > fnStartTok.offset && endOff <= source_.size()) {
      funcExpr.sourceText = source_.substr(fnStartTok.offset, endOff - fnStartTok.offset);
    }
  }

  return std::make_unique<Expression>(std::move(funcExpr));
}

}  // namespace lightjs
