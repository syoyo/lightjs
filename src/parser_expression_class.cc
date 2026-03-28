#include "parser_internal.h"

namespace lightjs {

ExprPtr Parser::parseClassExpression() {
  size_t decoratorCount = 0;
  if (!parseDecoratorList(decoratorCount)) {
    return nullptr;
  }
  expect(TokenType::Class);

  std::string className;
  if (isIdentifierLikeToken(current().type)) {
    className = current().value;
    if (isAlwaysReservedIdentifierWord(className) ||
        isStrictFutureReservedIdentifier(className)) {
      return nullptr;
    }
    advance();
  }

  ExprPtr superClass;
  if (match(TokenType::Extends)) {
    advance();
    struct HeritageStrictModeGuard {
      Parser* parser;
      bool saved;
      explicit HeritageStrictModeGuard(Parser* p) : parser(p), saved(p->strictMode_) { parser->strictMode_ = true; }
      ~HeritageStrictModeGuard() { parser->strictMode_ = saved; }
    } heritageStrictGuard(this);
    superClass = parseCall();
    if (!superClass) {
      return nullptr;
    }
    const std::set<std::string> kNoPrivateNames;
    if (expressionHasUndeclaredPrivateName(*superClass, kNoPrivateNames)) {
      return nullptr;
    }
  }

  expect(TokenType::LeftBrace);

  struct StrictModeGuard {
    Parser* parser;
    bool saved;
    explicit StrictModeGuard(Parser* p) : parser(p), saved(p->strictMode_) { parser->strictMode_ = true; }
    ~StrictModeGuard() { parser->strictMode_ = saved; }
  } strictGuard(this);
  ++classBodyDepth_;
  struct ClassBodyDepthGuard2 {
    int& depth;
    explicit ClassBodyDepthGuard2(int& d) : depth(d) {}
    ~ClassBodyDepthGuard2() { --depth; }
  } classBodyGuard2(classBodyDepth_);
  struct PrivateNameScopeGuard {
    std::vector<std::set<std::string>>& scopes;
    explicit PrivateNameScopeGuard(std::vector<std::set<std::string>>& s) : scopes(s) {
      scopes.emplace_back();
    }
    ~PrivateNameScopeGuard() { scopes.pop_back(); }
  } privateScopeGuard(privateNameScopeStack());

  std::vector<MethodDefinition> methods;
  bool hasInstanceConstructor = false;
  std::unordered_map<std::string, uint8_t> privateNameKinds;
  std::unordered_map<std::string, bool> privateNameStaticness;

  auto recordPrivateName = [&](const MethodDefinition& m) -> bool {
    constexpr uint8_t kField = 1;
    constexpr uint8_t kMethod = 2;
    constexpr uint8_t kGet = 4;
    constexpr uint8_t kSet = 8;
    uint8_t add = 0;
    switch (m.kind) {
      case MethodDefinition::Kind::Field: add = kField; break;
      case MethodDefinition::Kind::Get: add = kGet; break;
      case MethodDefinition::Kind::Set: add = kSet; break;
      case MethodDefinition::Kind::Method: add = kMethod; break;
      case MethodDefinition::Kind::AutoAccessor: add = kField; break;
      case MethodDefinition::Kind::Constructor: add = kMethod; break;
      case MethodDefinition::Kind::StaticBlock: add = 0; break;
    }
    auto staticIt = privateNameStaticness.find(m.key.name);
    if (staticIt == privateNameStaticness.end()) {
      privateNameStaticness[m.key.name] = m.isStatic;
    } else if (staticIt->second != m.isStatic) {
      return false;
    }
    uint8_t& mask = privateNameKinds[m.key.name];
    if (add == kField || add == kMethod) {
      if (mask != 0) return false;
      mask |= add;
      privateNameScopeStack().back().insert(m.key.name);
      return true;
    }
    if (add == kGet) {
      if (mask & (kField | kMethod | kGet)) return false;
      mask |= add;
      privateNameScopeStack().back().insert(m.key.name);
      return true;
    }
    if (add == kSet) {
      if (mask & (kField | kMethod | kSet)) return false;
      mask |= add;
      privateNameScopeStack().back().insert(m.key.name);
      return true;
    }
    return false;
  };
  while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
    if (match(TokenType::Semicolon)) {
      advance();
      continue;
    }

    size_t elementDecoratorCount = 0;
    if (!parseDecoratorList(elementDecoratorCount)) {
      return nullptr;
    }
    if (elementDecoratorCount > 0 &&
        match(TokenType::Static) &&
        !current().escaped &&
        peek().type == TokenType::LeftBrace) {
      return nullptr;
    }

    MethodDefinition method;

    if (match(TokenType::Static) && !current().escaped && peek().type == TokenType::LeftBrace) {
      method.kind = MethodDefinition::Kind::StaticBlock;
      method.isStatic = true;
      method.key.name.clear();
      advance();

      ++superCallDisallowDepth_;
      ++returnDisallowDepth_;
      awaitContextStack_.push_back(true);
      yieldContextStack_.push_back(false);
      ++staticBlockDepth_;
      auto savedStaticIterationLabels = std::move(iterationLabels_);
      iterationLabels_.clear();
      auto savedStaticActiveLabels = std::move(activeLabels_);
      activeLabels_.clear();
      int savedStaticLoopDepth = loopDepth_;
      loopDepth_ = 0;
      int savedStaticSwitchDepth = switchDepth_;
      switchDepth_ = 0;

      expect(TokenType::LeftBrace);
      while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
        auto stmt = parseStatement();
        if (!stmt) {
          switchDepth_ = savedStaticSwitchDepth;
          loopDepth_ = savedStaticLoopDepth;
          activeLabels_ = std::move(savedStaticActiveLabels);
          iterationLabels_ = std::move(savedStaticIterationLabels);
          yieldContextStack_.pop_back();
          awaitContextStack_.pop_back();
          --staticBlockDepth_;
          --returnDisallowDepth_;
          --superCallDisallowDepth_;
          return nullptr;
        }
        method.body.push_back(std::move(stmt));
      }
      expect(TokenType::RightBrace);

      switchDepth_ = savedStaticSwitchDepth;
      loopDepth_ = savedStaticLoopDepth;
      activeLabels_ = std::move(savedStaticActiveLabels);
      iterationLabels_ = std::move(savedStaticIterationLabels);
      yieldContextStack_.pop_back();
      awaitContextStack_.pop_back();
      --staticBlockDepth_;
      --returnDisallowDepth_;
      --superCallDisallowDepth_;

      if (staticBlockStatementListContainsAwait(method.body)) return nullptr;
      if (staticBlockStatementListContainsArguments(method.body)) return nullptr;
      if (staticBlockStatementListHasDuplicateLexNames(method.body)) return nullptr;
      if (staticBlockStatementListHasLexVarConflict(method.body)) return nullptr;

      methods.push_back(std::move(method));
      continue;
    }

    if (match(TokenType::Static) &&
        current().escaped &&
        peek().type != TokenType::LeftParen &&
        peek().type != TokenType::Equal &&
        peek().type != TokenType::Semicolon &&
        peek().type != TokenType::RightBrace) {
      return nullptr;
    }

    if (match(TokenType::Static) &&
        peek().type != TokenType::LeftParen &&
        peek().type != TokenType::Equal &&
        peek().type != TokenType::Semicolon &&
        peek().type != TokenType::RightBrace) {
      method.isStatic = true;
      advance();
    }

    if (match(TokenType::Async) &&
        !current().escaped &&
        current().line == peek().line &&
        peek().type != TokenType::LeftParen &&
        peek().type != TokenType::Semicolon &&
        peek().type != TokenType::Equal && peek().type != TokenType::RightBrace) {
      method.isAsync = true;
      advance();
    }

    if (match(TokenType::Star)) {
      method.isGenerator = true;
      advance();
    }

    if ((match(TokenType::Get) || match(TokenType::Set)) &&
        (isIdentifierNameToken(peek().type) ||
         peek().type == TokenType::PrivateIdentifier ||
         peek().type == TokenType::LeftBracket ||
         peek().type == TokenType::String ||
         peek().type == TokenType::Number ||
         peek().type == TokenType::BigInt)) {
      size_t saved = pos_;
      TokenType savedType = current().type;
      advance();
      bool isGetterSetter = false;
      if (match(TokenType::LeftBracket)) {
        advance();
        bool savedAllowIn = allowIn_;
        allowIn_ = true;
        bool savedError = error_;
        auto keyExpr = parseAssignment();
        error_ = savedError;
        allowIn_ = savedAllowIn;
        if (keyExpr && match(TokenType::RightBracket)) {
          advance();
          if (match(TokenType::LeftParen)) {
            isGetterSetter = true;
          }
        }
      } else if (isIdentifierNameToken(current().type) ||
                 match(TokenType::PrivateIdentifier) ||
                 match(TokenType::String) ||
                 match(TokenType::Number) ||
                 match(TokenType::BigInt)) {
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

    if (method.kind == MethodDefinition::Kind::Method &&
        isIdentifierNameToken(current().type) &&
        !current().escaped &&
        current().value == "accessor" &&
        current().line == peek().line &&
        (isIdentifierNameToken(peek().type) ||
         peek().type == TokenType::PrivateIdentifier ||
         peek().type == TokenType::LeftBracket ||
         peek().type == TokenType::String ||
         peek().type == TokenType::Number ||
         peek().type == TokenType::BigInt)) {
      method.kind = MethodDefinition::Kind::AutoAccessor;
      advance();
    }

    if (match(TokenType::PrivateIdentifier)) {
      method.isPrivate = true;
      if (current().value == "#constructor") {
        return nullptr;
      }
      method.key.name = current().value;
      advance();
    } else if (match(TokenType::LeftBracket)) {
      method.computed = true;
      advance();
      bool savedAllowIn = allowIn_;
      allowIn_ = true;
      method.computedKey = parseAssignment();
      allowIn_ = savedAllowIn;
      if (!method.computedKey || !expect(TokenType::RightBracket)) {
        return nullptr;
      }
    } else if (isIdentifierNameToken(current().type)) {
      std::string memberName = current().value;
      if (!method.isStatic &&
          memberName == "constructor" &&
          method.kind == MethodDefinition::Kind::Method) {
        method.kind = MethodDefinition::Kind::Constructor;
      }
      method.key.name = memberName;
      advance();
    } else if (match(TokenType::Number)) {
      double numberValue = 0.0;
      if (!parseNumberLiteral(current().value, strictMode_, numberValue)) {
        return nullptr;
      }
      method.key.name = numberToPropertyKey(numberValue);
      advance();
    } else if (match(TokenType::BigInt)) {
      bigint::BigIntValue bigintValue = 0;
      if (!parseBigIntLiteral64(current().value, bigintValue)) {
        return nullptr;
      }
      method.key.name = bigint::toString(bigintValue);
      advance();
    } else if (match(TokenType::String)) {
      method.key.name = current().value;
      advance();
    } else {
      return nullptr;
    }

    if (!method.computed &&
        !method.isStatic &&
        !method.isPrivate &&
        method.kind == MethodDefinition::Kind::Method &&
        method.key.name == "constructor") {
      method.kind = MethodDefinition::Kind::Constructor;
    }

    if (match(TokenType::LeftParen)) {
      if (method.kind == MethodDefinition::Kind::AutoAccessor) {
        return nullptr;
      }
      if (method.kind == MethodDefinition::Kind::Constructor &&
          (method.isAsync || method.isGenerator)) {
        return nullptr;
      }
      if (!method.isStatic &&
          !method.computed &&
          !method.isPrivate &&
          method.key.name == "constructor" &&
          (method.kind == MethodDefinition::Kind::Get ||
           method.kind == MethodDefinition::Kind::Set)) {
        return nullptr;
      }
      if (method.isStatic &&
          !method.computed &&
          !method.isPrivate &&
          method.key.name == "prototype") {
        return nullptr;
      }
      ++functionDepth_;
      ++newTargetDepth_;
      awaitContextStack_.push_back(method.isAsync);
      yieldContextStack_.push_back(method.isGenerator);
      if (method.isAsync) {
        ++asyncFunctionDepth_;
      }
      advance();

      std::vector<StmtPtr> destructurePrologue;
      bool hasNonSimpleParams = false;
      std::set<std::string> seenParamNames;
      bool hasDuplicateParams = false;

      while (!match(TokenType::RightParen)) {
        if (match(TokenType::DotDotDot)) {
          hasNonSimpleParams = true;
          advance();
          if (isIdentifierLikeToken(current().type)) {
            method.restParam = Identifier{current().value};
            if (method.restParam->name == "eval" || method.restParam->name == "arguments") {
              return nullptr;
            }
            if (!seenParamNames.insert(method.restParam->name).second) {
              hasDuplicateParams = true;
            }
            advance();
          } else if (match(TokenType::LeftBracket) || match(TokenType::LeftBrace)) {
            auto pattern = parsePattern();
            if (!pattern) return nullptr;
            if (std::get_if<AssignmentPattern>(&pattern->node)) return nullptr;
            if (hasDuplicateBoundNames(*pattern)) return nullptr;

            std::vector<std::string> names;
            collectBoundNames(*pattern, names);
            for (auto& name : names) {
              if (name == "eval" || name == "arguments") return nullptr;
              if (!seenParamNames.insert(name).second) hasDuplicateParams = true;
            }

            std::string tempName = "__rest_param_" + std::to_string(arrowDestructureTempCounter_++);
            method.restParam = Identifier{tempName};
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
          if (param.name.name == "eval" || param.name.name == "arguments") {
            return nullptr;
          }
          if (!seenParamNames.insert(param.name.name).second) {
            hasDuplicateParams = true;
          }
          advance();
          if (match(TokenType::Equal)) {
            hasNonSimpleParams = true;
            advance();
            param.defaultValue = parseAssignment();
            if (!param.defaultValue) return nullptr;
            if (expressionContainsSuperCall(*param.defaultValue)) return nullptr;
          }
        } else if (match(TokenType::LeftBracket) || match(TokenType::LeftBrace)) {
          hasNonSimpleParams = true;
          auto pattern = parsePattern();
          if (!pattern) return nullptr;
          if (auto* assignPat = std::get_if<AssignmentPattern>(&pattern->node)) {
            param.defaultValue = std::move(assignPat->right);
            pattern = std::move(assignPat->left);
            if (!pattern) return nullptr;
            if (param.defaultValue && expressionContainsSuperCall(*param.defaultValue)) return nullptr;
          }
          if (hasDuplicateBoundNames(*pattern)) return nullptr;

          std::vector<std::string> names;
          collectBoundNames(*pattern, names);
          for (auto& name2 : names) {
            if (name2 == "eval" || name2 == "arguments") return nullptr;
            if (!seenParamNames.insert(name2).second) hasDuplicateParams = true;
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
          break;
        }
        method.params.push_back(std::move(param));
        if (match(TokenType::Comma)) {
          advance();
          if (match(TokenType::RightParen)) {
            break;
          }
        }
      }
      expect(TokenType::RightParen);

      if (method.kind == MethodDefinition::Kind::Get) {
        if (!method.params.empty() || method.restParam.has_value()) {
          return nullptr;
        }
      } else if (method.kind == MethodDefinition::Kind::Set) {
        if (method.params.size() != 1 || method.restParam.has_value()) {
          return nullptr;
        }
      }

      bool allowSuperCall = superClass && !method.isStatic &&
                            method.kind == MethodDefinition::Kind::Constructor;
      if (!allowSuperCall) {
        ++superCallDisallowDepth_;
      }
      if (method.isGenerator) {
        ++generatorFunctionDepth_;
      }
      auto savedIterLabelsMethod = std::move(iterationLabels_);
      iterationLabels_.clear();
      auto savedActiveLabelsMethod = std::move(activeLabels_);
      activeLabels_.clear();
      int savedLoopDepthMethod = loopDepth_;
      loopDepth_ = 0;
      int savedSwitchDepthMethod = switchDepth_;
      switchDepth_ = 0;
      int savedStaticBlockDepthMethod = staticBlockDepth_;
      staticBlockDepth_ = 0;
      parsingFunctionBody_ = true;
      expect(TokenType::LeftBrace);
      while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
        auto stmt = parseStatement();
        if (!stmt) {
          parsingFunctionBody_ = false;
          staticBlockDepth_ = savedStaticBlockDepthMethod;
          switchDepth_ = savedSwitchDepthMethod;
          loopDepth_ = savedLoopDepthMethod;
          activeLabels_ = std::move(savedActiveLabelsMethod);
          iterationLabels_ = std::move(savedIterLabelsMethod);
          if (method.isGenerator) {
            --generatorFunctionDepth_;
          }
          if (method.isAsync) {
            --asyncFunctionDepth_;
          }
          --newTargetDepth_;
          --functionDepth_;
          awaitContextStack_.pop_back();
          yieldContextStack_.pop_back();
          if (!allowSuperCall) {
            --superCallDisallowDepth_;
          }
          return nullptr;
        }
        method.body.push_back(std::move(stmt));
      }
      parsingFunctionBody_ = false;
      staticBlockDepth_ = savedStaticBlockDepthMethod;
      switchDepth_ = savedSwitchDepthMethod;
      loopDepth_ = savedLoopDepthMethod;
      activeLabels_ = std::move(savedActiveLabelsMethod);
      iterationLabels_ = std::move(savedIterLabelsMethod);
      expect(TokenType::RightBrace);
      if (method.isGenerator) {
        --generatorFunctionDepth_;
      }
      if (method.isAsync) {
        --asyncFunctionDepth_;
      }
      --newTargetDepth_;
      --functionDepth_;
      awaitContextStack_.pop_back();
      yieldContextStack_.pop_back();
      if (!allowSuperCall) {
        --superCallDisallowDepth_;
      }

      if (hasDuplicateParams) {
        return nullptr;
      }
      if (hasUseStrictDirectiveInBody(method.body) && hasNonSimpleParams) {
        return nullptr;
      }
      method.destructurePrologue = std::move(destructurePrologue);

      if (method.kind == MethodDefinition::Kind::Constructor) {
        if (hasInstanceConstructor) {
          return nullptr;
        }
        hasInstanceConstructor = true;
      }
    } else {
      if (method.kind != MethodDefinition::Kind::AutoAccessor) {
        method.kind = MethodDefinition::Kind::Field;
      }
      if (!method.computed && !method.isPrivate && method.key.name == "constructor") {
        return nullptr;
      }
      if (method.isStatic && !method.computed && !method.isPrivate && method.key.name == "prototype") {
        return nullptr;
      }
      if (match(TokenType::Equal)) {
        advance();
        method.initializer = parseAssignment();
        if (!method.initializer) {
          return nullptr;
        }
        if (fieldInitContainsSuperCall(*method.initializer)) {
          return nullptr;
        }
        if (fieldInitContainsArguments(*method.initializer)) {
          return nullptr;
        }
      }
      uint32_t fieldEndLine = tokens_[pos_ - 1].line;
      bool consumedSemicolon = false;
      if (match(TokenType::Semicolon)) {
        advance();
        consumedSemicolon = true;
      }
      if (!consumedSemicolon &&
          !match(TokenType::RightBrace) &&
          !match(TokenType::EndOfFile) &&
          current().line == fieldEndLine) {
        return nullptr;
      }
    }

    if (method.isPrivate) {
      if (!recordPrivateName(method)) {
        return nullptr;
      }
    }
    methods.push_back(std::move(method));
  }
  expect(TokenType::RightBrace);

  std::set<std::string> declaredPrivateNames;
  for (const auto& kv : privateNameKinds) {
    declaredPrivateNames.insert(kv.first);
  }
  if (privateNameScopeStack().size() >= 2) {
    const auto& outer = privateNameScopeStack()[privateNameScopeStack().size() - 2];
    declaredPrivateNames.insert(outer.begin(), outer.end());
  }
  for (const auto& m : methods) {
    if (m.computedKey && expressionHasUndeclaredPrivateName(*m.computedKey, declaredPrivateNames)) {
      return nullptr;
    }
    for (const auto& param : m.params) {
      if (param.defaultValue && expressionHasUndeclaredPrivateName(*param.defaultValue, declaredPrivateNames)) {
        return nullptr;
      }
    }
    if (m.initializer && expressionHasUndeclaredPrivateName(*m.initializer, declaredPrivateNames)) {
      return nullptr;
    }
    for (const auto& stmt : m.body) {
      if (stmt && statementHasUndeclaredPrivateName(*stmt, declaredPrivateNames)) {
        return nullptr;
      }
    }
  }

  ClassExpr classExpr;
  classExpr.name = className;
  classExpr.superClass = std::move(superClass);
  classExpr.methods = std::move(methods);

  return std::make_unique<Expression>(std::move(classExpr));
}

}  // namespace lightjs
