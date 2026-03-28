#include "parser_internal.h"

namespace lightjs {

ExprPtr Parser::parseArrayExpression() {
  expect(TokenType::LeftBracket);

  std::vector<ExprPtr> elements;
  while (!match(TokenType::RightBracket)) {
    if (!elements.empty()) {
      expect(TokenType::Comma);
      if (match(TokenType::RightBracket)) break;
    }

    if (match(TokenType::Comma)) {
      elements.push_back(nullptr);
      continue;
    }

    if (match(TokenType::DotDotDot)) {
      advance();
      auto arg = parseAssignment();
      if (!arg) {
        return nullptr;
      }
      elements.push_back(std::make_unique<Expression>(SpreadElement{std::move(arg)}));
    } else {
      auto element = parseAssignment();
      if (!element) {
        return nullptr;
      }
      elements.push_back(std::move(element));
    }
  }

  if (!expect(TokenType::RightBracket)) {
    return nullptr;
  }
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

    if (match(TokenType::DotDotDot)) {
      advance();
      auto spreadExpr = parseAssignment();
      if (!spreadExpr) {
        return nullptr;
      }
      ObjectProperty prop;
      prop.isSpread = true;
      prop.value = std::move(spreadExpr);
      properties.push_back(std::move(prop));
    } else {
      ExprPtr key;
      bool isComputed = false;
      bool isGenerator = false;
      bool isAsync = false;

      if (match(TokenType::Async) &&
          !current().escaped &&
          current().line == peek().line) {
        isAsync = true;
        advance();
      }

      if (match(TokenType::Star)) {
        isGenerator = true;
        advance();
      }

      if (match(TokenType::LeftBracket)) {
        advance();
        bool savedAllowIn = allowIn_;
        allowIn_ = true;
        key = parseAssignment();
        allowIn_ = savedAllowIn;
        expect(TokenType::RightBracket);
        isComputed = true;
      } else if (isIdentifierNameToken(current().type) &&
                 (!match(TokenType::Get) || current().escaped) &&
                 (!match(TokenType::Set) || current().escaped)) {
        std::string identName = current().value;
        const bool shorthandIdentifierReference =
            isIdentifierLikeToken(current().type) &&
            !current().escaped;
        key = std::make_unique<Expression>(Identifier{identName});
        advance();

        if (!isGenerator && !isAsync &&
            (match(TokenType::Comma) || match(TokenType::RightBrace))) {
          if (!shorthandIdentifierReference) {
            return nullptr;
          }
          if (strictMode_ &&
              (identName == "implements" || identName == "interface" ||
               identName == "let" || identName == "package" ||
               identName == "private" || identName == "protected" ||
               identName == "public" || identName == "static" ||
               identName == "yield")) {
            return nullptr;
          }
          if (identName == "await" && !canUseAwaitAsIdentifier()) {
            return nullptr;
          }
          ObjectProperty prop;
          prop.key = std::move(key);
          prop.value = std::make_unique<Expression>(Identifier{identName});
          prop.isSpread = false;
          properties.push_back(std::move(prop));
          continue;
        }
      } else if (match(TokenType::Get) || match(TokenType::Set)) {
        if (current().escaped) {
          return nullptr;
        }
        bool isGetter = current().type == TokenType::Get;
        std::string keyName = current().value;
        advance();

        if (match(TokenType::Colon)) {
          key = std::make_unique<Expression>(Identifier{keyName});
        } else if (match(TokenType::LeftBracket)) {
          advance();
          bool savedAllowIn = allowIn_;
          allowIn_ = true;
          key = parseAssignment();
          allowIn_ = savedAllowIn;
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
          ++functionDepth_;
          ++newTargetDepth_;
          awaitContextStack_.push_back(false);
          yieldContextStack_.push_back(false);
          struct AccessorParseGuard {
            Parser* parser;
            bool active = true;
            ~AccessorParseGuard() {
              if (!active) return;
              --parser->functionDepth_;
              parser->awaitContextStack_.pop_back();
              parser->yieldContextStack_.pop_back();
            }
          } accessorGuard{this};
          std::vector<Parameter> params;
          if (isGetter) {
            if (!match(TokenType::RightParen)) {
              return nullptr;
            }
          } else {
            if (!isIdentifierLikeToken(current().type)) {
              return nullptr;
            }
            Parameter param;
            param.name = Identifier{current().value};
            advance();
            if (match(TokenType::Equal)) {
              advance();
              param.defaultValue = parseAssignment();
              if (!param.defaultValue) {
                return nullptr;
              }
            }
            params.push_back(std::move(param));
            if (match(TokenType::Comma)) {
              return nullptr;
            }
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

          bool hadLegacyEscape7571 = false;
          bool hasUseStrictDirective = hasUseStrictDirectiveInBody(body, &hadLegacyEscape7571);
          bool strictAccessorCode = strictMode_ || hasUseStrictDirective;
          if (hasUseStrictDirective && hadLegacyEscape7571) { error_ = true; return nullptr; }
          if (!isGetter) {
            if (params.empty()) {
              return nullptr;
            }
            const auto& paramName = params[0].name.name;
            if (hasUseStrictDirective && params[0].defaultValue) {
              return nullptr;
            }
            if (strictAccessorCode &&
                (paramName == "eval" || paramName == "arguments")) {
              return nullptr;
            }
          }
          if (strictAccessorCode) {
            for (const auto& stmt : body) {
              if (stmt &&
                  (statementContainsStrictRestrictedWrite(*stmt) ||
                   statementContainsStrictRestrictedIdentifierReference(*stmt))) {
                return nullptr;
              }
            }
          }
          FunctionExpr funcExpr;
          funcExpr.params = std::move(params);
          funcExpr.body = std::move(body);
          funcExpr.isAsync = false;
          funcExpr.isGenerator = false;
          funcExpr.isArrow = false;
          funcExpr.isMethod = true;

          ObjectProperty prop;
          prop.key = std::move(key);
          prop.value = std::make_unique<Expression>(std::move(funcExpr));
          prop.isSpread = false;
          prop.isComputed = true;
          prop.isGetter = isGetter;
          prop.isSetter = !isGetter;
          properties.push_back(std::move(prop));
          continue;
        } else if (isIdentifierNameToken(current().type) ||
                   match(TokenType::String) ||
                   match(TokenType::Number) ||
                   match(TokenType::BigInt)) {
          std::string propName;
          if (match(TokenType::Number)) {
            double numberValue = 0.0;
            if (!parseNumberLiteral(current().value, strictMode_, numberValue)) {
              return nullptr;
            }
            propName = numberToPropertyKey(numberValue);
            key = std::make_unique<Expression>(NumberLiteral{numberValue});
          } else if (match(TokenType::BigInt)) {
            propName = current().value;
            bigint::BigIntValue value = 0;
            if (!parseBigIntLiteral64(propName, value)) {
              return nullptr;
            }
            propName = bigint::toString(value);
            key = std::make_unique<Expression>(BigIntLiteral{value});
          } else {
            propName = current().value;
            key = std::make_unique<Expression>(Identifier{propName});
          }
          advance();

          if (!expect(TokenType::LeftParen)) {
            return nullptr;
          }
          ++functionDepth_;
          ++newTargetDepth_;
          awaitContextStack_.push_back(false);
          yieldContextStack_.push_back(false);
          struct AccessorParseGuard {
            Parser* parser;
            bool active = true;
            ~AccessorParseGuard() {
              if (!active) return;
              --parser->functionDepth_;
              parser->awaitContextStack_.pop_back();
              parser->yieldContextStack_.pop_back();
            }
          } accessorGuard{this};
          std::vector<Parameter> params;

          if (isGetter) {
            if (!match(TokenType::RightParen)) {
              return nullptr;
            }
          } else {
            if (!isIdentifierLikeToken(current().type)) {
              return nullptr;
            }
            Parameter param;
            param.name = Identifier{current().value};
            advance();
            if (match(TokenType::Equal)) {
              advance();
              param.defaultValue = parseAssignment();
              if (!param.defaultValue) {
                return nullptr;
              }
            }
            params.push_back(std::move(param));
            if (match(TokenType::Comma)) {
              return nullptr;
            }
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

          bool hadLegacyEscape7702 = false;
          bool hasUseStrictDirective = hasUseStrictDirectiveInBody(body, &hadLegacyEscape7702);
          bool strictAccessorCode = strictMode_ || hasUseStrictDirective;
          if (hasUseStrictDirective && hadLegacyEscape7702) { error_ = true; return nullptr; }
          if (!isGetter) {
            if (params.empty()) {
              return nullptr;
            }
            const auto& paramName = params[0].name.name;
            if (hasUseStrictDirective && params[0].defaultValue) {
              return nullptr;
            }
            if (strictAccessorCode &&
                (paramName == "eval" || paramName == "arguments")) {
              return nullptr;
            }
          }
          if (strictAccessorCode) {
            for (const auto& stmt : body) {
              if (stmt &&
                  (statementContainsStrictRestrictedWrite(*stmt) ||
                   statementContainsStrictRestrictedIdentifierReference(*stmt))) {
                return nullptr;
              }
            }
          }
          FunctionExpr funcExpr;
          funcExpr.params = std::move(params);
          funcExpr.body = std::move(body);
          funcExpr.isAsync = false;
          funcExpr.isGenerator = false;
          funcExpr.isArrow = false;
          funcExpr.isMethod = true;

          ObjectProperty prop;
          prop.key = std::move(key);
          prop.value = std::make_unique<Expression>(std::move(funcExpr));
          prop.isSpread = false;
          prop.isComputed = false;

          auto actualKey = std::make_unique<Expression>(Identifier{(isGetter ? "__get_" : "__set_") + propName});
          prop.key = std::move(actualKey);

          properties.push_back(std::move(prop));
          continue;
        } else if (match(TokenType::LeftParen)) {
          key = std::make_unique<Expression>(Identifier{keyName});
        } else {
          key = std::make_unique<Expression>(Identifier{keyName});
        }
      } else if (match(TokenType::String)) {
        key = std::make_unique<Expression>(StringLiteral{current().value});
        advance();
      } else if (match(TokenType::Number)) {
        key = std::make_unique<Expression>(NumberLiteral{std::stod(current().value)});
        advance();
      } else if (match(TokenType::BigInt)) {
        bigint::BigIntValue value = 0;
        if (!parseBigIntLiteral64(current().value, value)) {
          return nullptr;
        }
        key = std::make_unique<Expression>(BigIntLiteral{value});
        advance();
      }

      if (!key) {
        return nullptr;
      }

      if (match(TokenType::LeftParen)) {
        advance();
        ++superCallDisallowDepth_;
        ++functionDepth_;
        ++newTargetDepth_;
        awaitContextStack_.push_back(isAsync);
        yieldContextStack_.push_back(isGenerator);
        if (isAsync) {
          ++asyncFunctionDepth_;
        }
        if (isGenerator) {
          ++generatorFunctionDepth_;
        }
        struct MethodParseGuard {
          Parser* parser;
          bool isAsync;
          bool isGenerator;
          bool active = true;
          ~MethodParseGuard() {
            if (!active) return;
            if (isGenerator) {
              --parser->generatorFunctionDepth_;
            }
            if (isAsync) {
              --parser->asyncFunctionDepth_;
            }
            --parser->functionDepth_;
            parser->awaitContextStack_.pop_back();
            parser->yieldContextStack_.pop_back();
            --parser->superCallDisallowDepth_;
          }
        } methodParseGuard{this, isAsync, isGenerator};

        std::vector<Parameter> params;
        std::optional<Identifier> restParam;
        std::vector<StmtPtr> destructurePrologue;
        bool hasNonSimpleParams = false;
        std::vector<std::string> boundParamNames;
        std::set<std::string> seenParamNames;
        bool hasDuplicateParams = false;
        bool hasRestrictedParamNames = false;
        bool hasSuperCallInParams = false;

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
                if (!seenParamNames.insert(name).second) {
                  hasDuplicateParams = true;
                }
                boundParamNames.push_back(name);
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
            if (!seenParamNames.insert(param.name.name).second) {
              hasDuplicateParams = true;
            }
            boundParamNames.push_back(param.name.name);
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
              if (expressionContainsSuperCall(*param.defaultValue)) {
                hasSuperCallInParams = true;
              }
              if (isAsync && expressionContainsAwaitLike(*param.defaultValue)) {
                return nullptr;
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
              if (expressionContainsSuperCall(*param.defaultValue)) {
                hasSuperCallInParams = true;
              }
              if (isAsync && expressionContainsAwaitLike(*param.defaultValue)) {
                return nullptr;
              }
              pattern = std::move(assignPat->left);
              if (!pattern) return nullptr;
            }

            std::vector<std::string> names;
            collectBoundNames(*pattern, names);
            for (auto& name : names) {
              if (isGenerator && name == "yield") {
                return nullptr;
              }
              if (isAsync && name == "await") {
                return nullptr;
              }
              if (!seenParamNames.insert(name).second) {
                hasDuplicateParams = true;
              }
              boundParamNames.push_back(name);
              if (name == "eval" || name == "arguments") {
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
            if (match(TokenType::RightParen)) {
              break;
            }
          } else {
            break;
          }
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

        bool hadLegacyEscape7993 = false;
        bool hasUseStrictDirective = hasUseStrictDirectiveInBody(body, &hadLegacyEscape7993);
        bool strictMethodCode = strictMode_ || hasUseStrictDirective;
        if (hasUseStrictDirective && hadLegacyEscape7993) { error_ = true; return nullptr; }
        if (strictMethodCode && hasRestrictedParamNames) {
          return nullptr;
        }
        if (hasUseStrictDirective && hasNonSimpleParams) {
          return nullptr;
        }
        if (hasDuplicateParams) {
          return nullptr;
        }
        if (hasSuperCallInParams) {
          return nullptr;
        }
        {
          std::vector<std::string> lexicalNames;
          collectTopLevelLexicallyDeclaredNames(body, lexicalNames);
          std::set<std::string> paramNameSet(boundParamNames.begin(), boundParamNames.end());
          for (const auto& lexName : lexicalNames) {
            if (paramNameSet.count(lexName) != 0) {
              return nullptr;
            }
          }
        }

        FunctionExpr funcExpr;
        funcExpr.params = std::move(params);
        funcExpr.restParam = std::move(restParam);
        funcExpr.body = std::move(body);
        funcExpr.destructurePrologue = std::move(destructurePrologue);
        funcExpr.isGenerator = isGenerator;
        funcExpr.isAsync = isAsync;
        funcExpr.isMethod = true;

        ObjectProperty prop;
        prop.key = std::move(key);
        prop.value = std::make_unique<Expression>(std::move(funcExpr));
        prop.isSpread = false;
        prop.isComputed = isComputed;
        properties.push_back(std::move(prop));
      } else {
        if (!expect(TokenType::Colon)) {
          return nullptr;
        }
        auto value = parseAssignment();
        if (!value) {
          return nullptr;
        }

        ObjectProperty prop;
        prop.key = std::move(key);
        prop.value = std::move(value);
        prop.isSpread = false;
        prop.isComputed = isComputed;
        if (!prop.isComputed && prop.key) {
          if (auto* ident = std::get_if<Identifier>(&prop.key->node)) {
            prop.isProtoSetter = ident->name == "__proto__";
          } else if (auto* str = std::get_if<StringLiteral>(&prop.key->node)) {
            prop.isProtoSetter = str->value == "__proto__";
          }
        }
        if (prop.isProtoSetter) {
          bool allowsDuplicateProto = false;
          int depth = 1;
          for (size_t i = pos_; i < tokens_.size(); ++i) {
            auto t = tokens_[i].type;
            if (t == TokenType::LeftBrace) {
              depth++;
            } else if (t == TokenType::RightBrace) {
              depth--;
              if (depth == 0) {
                size_t j = i + 1;
                while (j < tokens_.size() && tokens_[j].type == TokenType::RightParen) {
                  ++j;
                }
                if (j < tokens_.size() && tokens_[j].type == TokenType::Equal) {
                  allowsDuplicateProto = true;
                }
                break;
              }
            }
          }
          if (!allowsDuplicateProto) {
            for (const auto& existing : properties) {
              if (existing.isProtoSetter) {
                return nullptr;
              }
            }
          }
        }
        properties.push_back(std::move(prop));
      }
    }
  }

  if (!expect(TokenType::RightBrace)) {
    return nullptr;
  }
  return std::make_unique<Expression>(ObjectExpr{std::move(properties)});
}

}  // namespace lightjs
