#include "parser_internal.h"

namespace lightjs {

bool Parser::parseDecoratorList(size_t& count) {
  count = 0;
  while (match(TokenType::At)) {
    advance();
    auto decorator = parseCall();
    if (!decorator) {
      return false;
    }
    count++;
  }
  return true;
}

StmtPtr Parser::parseVarDeclaration() {
  const Token& startTok = current();
  VarDeclaration::Kind kind;
  switch (current().type) {
    case TokenType::Let: kind = VarDeclaration::Kind::Let; break;
    case TokenType::Const: kind = VarDeclaration::Kind::Const; break;
    case TokenType::Var: kind = VarDeclaration::Kind::Var; break;
    case TokenType::Using:
      if (!isUsingDeclarationStart(true)) {
        return nullptr;
      }
      kind = VarDeclaration::Kind::Using;
      break;
    case TokenType::Await:
      if (!isAwaitUsingDeclarationStart(true)) {
        return nullptr;
      }
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

  VarDeclaration decl;
  decl.kind = kind;

  do {
    if (!decl.declarations.empty()) {
      if (!expect(TokenType::Comma)) {
        return nullptr;
      }
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
      // Parse pattern (identifier, array pattern, or object pattern)
      pattern = parsePattern();
    }
    if (!pattern) {
      return nullptr;
    }

    // 'this' is not a valid BindingIdentifier
    auto* checkNode = &pattern->node;
    if (auto* ap = std::get_if<AssignmentPattern>(checkNode)) {
      if (ap->left) checkNode = &ap->left->node;
    }
    if (std::holds_alternative<ThisExpr>(*checkNode)) {
      error_ = true;
      return nullptr;
    }

    // Reject 'let' as a BindingIdentifier in LexicalDeclarations (13.3.1.1)
    if (kind == VarDeclaration::Kind::Let ||
        kind == VarDeclaration::Kind::Const ||
        kind == VarDeclaration::Kind::Using ||
        kind == VarDeclaration::Kind::AwaitUsing) {
      std::vector<std::string> boundNames;
      collectBoundNames(*pattern, boundNames);
      for (auto& name : boundNames) {
        if (name == "let") {
          error_ = true;
          return nullptr;
        }
        // Strict mode: also reject 'eval' and 'arguments'
        if (strictMode_ && isStrictModeRestrictedIdentifier(name)) {
          return nullptr;
        }
      }
    } else if (strictMode_) {
      // Strict mode: reject 'eval' and 'arguments' in var declarations
      std::vector<std::string> boundNames;
      collectBoundNames(*pattern, boundNames);
      for (auto& name : boundNames) {
        if (isStrictModeRestrictedIdentifier(name)) {
          return nullptr;
        }
      }
    }

    ExprPtr init = nullptr;
    // If parsePattern() consumed the '=' and created an AssignmentPattern,
    // unwrap it: the left side is the pattern, right side is the initializer
    if (auto* assignPat = std::get_if<AssignmentPattern>(&pattern->node)) {
      init = std::move(assignPat->right);
      pattern = std::move(assignPat->left);
    } else if (match(TokenType::Equal)) {
      advance();
      init = parseAssignment();
      if (!init) {
        return nullptr;
      }
    }

    // const declarations must have initializers
    if ((kind == VarDeclaration::Kind::Const ||
         kind == VarDeclaration::Kind::Using ||
         kind == VarDeclaration::Kind::AwaitUsing) && !init) {
      // Check if pattern is a simple identifier (not destructuring - destructuring gets init from AssignmentPattern)
      if (std::get_if<Identifier>(&pattern->node)) {
        error_ = true;
        return nullptr;  // SyntaxError: Missing initializer in const/using declaration
      }
    }

    decl.declarations.push_back({std::move(pattern), std::move(init)});
  } while (match(TokenType::Comma));

  if (!consumeSemicolonOrASI()) {
    return nullptr;
  }
  return makeStmt(std::move(decl), startTok);
}

StmtPtr Parser::parseFunctionDeclaration() {
  const Token& startTok = current();
  bool isAsync = false;
  if (match(TokenType::Async)) {
    if (current().escaped) {
      return nullptr;
    }
    isAsync = true;
    advance();
  }

  expect(TokenType::Function);

  // Check for generator function (function*)
  bool isGenerator = false;
  if (match(TokenType::Star)) {
    isGenerator = true;
    advance();
  }

  std::string name;
  {
    if (!isIdentifierLikeToken(current().type) &&
        !(match(TokenType::Yield) && !strictMode_)) {
      return nullptr;
    }
    name = current().value;
    // Reject 'await' as function name inside async context or static blocks
    if (name == "await" && (!canUseAwaitAsIdentifier() || staticBlockDepth_ > 0)) {
      return nullptr;
    }
    // Reject 'yield' as function name inside generator context or strict mode
    if (name == "yield" && !canUseYieldAsIdentifier()) {
      return nullptr;
    }
    advance();
  }

  ++functionDepth_;
  ++newTargetDepth_;
  awaitContextStack_.push_back(isAsync);
  yieldContextStack_.push_back(isGenerator);
  if (isAsync) {
    ++asyncFunctionDepth_;
  }

  if (!expect(TokenType::LeftParen)) {
    return nullptr;
  }

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
      // Rest parameter (identifier or binding pattern)
      hasNonSimpleParams = true;
      advance();
      if (isIdentifierLikeToken(current().type)) {
        restParam = Identifier{current().value};
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
        if (!pattern) {
          return nullptr;
        }
        // Rest parameters cannot have initializers.
        if (std::get_if<AssignmentPattern>(&pattern->node)) return nullptr;

        std::vector<std::string> names;
        collectBoundNames(*pattern, names);
        for (auto& name : names) {
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
    bool hasDestructurePattern = false;

    if (isIdentifierLikeToken(current().type)) {
      param.name = Identifier{current().value};
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
        if (!param.defaultValue) {
          return nullptr;
        }
        if (expressionContainsSuper(*param.defaultValue)) {
          hasSuperInParams = true;
        }
      }
    } else if (match(TokenType::LeftBracket) || match(TokenType::LeftBrace)) {
      hasNonSimpleParams = true;
      hasDestructurePattern = true;
      auto pattern = parsePattern();
      if (!pattern) {
        return nullptr;
      }

      // Unwrap top-level AssignmentPattern: it's a parameter default.
      if (auto* assignPat = std::get_if<AssignmentPattern>(&pattern->node)) {
        param.defaultValue = std::move(assignPat->right);
        if (!param.defaultValue) return nullptr;
        if (expressionContainsSuper(*param.defaultValue)) {
          hasSuperInParams = true;
        }
        pattern = std::move(assignPat->left);
        if (!pattern) return nullptr;
      }

      std::vector<std::string> names;
      collectBoundNames(*pattern, names);
      for (auto& name : names) {
        boundParamNames.push_back(name);
        if (!seenParamNames.insert(name).second) {
          hasDuplicateParams = true;
        }
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
      if (match(TokenType::RightParen)) break;  // trailing comma
    } else {
      break;
    }
  }
  expect(TokenType::RightParen);

  if (isGenerator) {
    ++generatorFunctionDepth_;
  }
  // Break/continue/labels do not cross function boundaries.
  auto savedIterationLabels = std::move(iterationLabels_);
  iterationLabels_.clear();
  auto savedActiveLabels = std::move(activeLabels_);
  activeLabels_.clear();
  int savedLoopDepth = loopDepth_;
  loopDepth_ = 0;
  int savedSwitchDepth = switchDepth_;
  switchDepth_ = 0;
  int savedStaticBlockDepth = staticBlockDepth_;
  staticBlockDepth_ = 0;
  // Pre-scan for 'use strict' directive before parsing body
  bool savedStrict = strictMode_;
  parsingFunctionBody_ = true;
  if (!strictMode_ && match(TokenType::LeftBrace)) {
    // Peek at first statement: check for "use strict" string literal (after the '{')
    auto peekPos = pos_ + 1;
    if (peekPos < tokens_.size() && tokens_[peekPos].type == TokenType::String &&
        tokens_[peekPos].value == "use strict" && !tokens_[peekPos].escaped) {
      strictMode_ = true;
    }
  }
  auto block = parseBlockStatement();
  parsingFunctionBody_ = false;
  strictMode_ = savedStrict;
  staticBlockDepth_ = savedStaticBlockDepth;
  switchDepth_ = savedSwitchDepth;
  loopDepth_ = savedLoopDepth;
  iterationLabels_ = std::move(savedIterationLabels);
  activeLabels_ = std::move(savedActiveLabels);
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

  bool hadLegacyEscapeBeforeStrict = false;
  bool hasUseStrictDirective = hasUseStrictDirectiveInBody(blockStmt->body, &hadLegacyEscapeBeforeStrict);
  bool strictFunctionCode = strictMode_ || hasUseStrictDirective;
  if (hasUseStrictDirective && hadLegacyEscapeBeforeStrict) {
    error_ = true;
    return nullptr;
  }
  if (strictFunctionCode && (name == "eval" || name == "arguments")) {
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
    bool strictFnCode = strictMode_ || hasUseStrictDirective;
    // Collect function declaration names for sloppy-mode exemption
    std::set<std::string> funcDeclNamesInBody;
    if (!strictFnCode) {
      for (const auto& stmt : blockStmt->body) {
        if (!stmt) continue;
        if (auto* fnDecl = std::get_if<FunctionDeclaration>(&stmt->node)) {
          funcDeclNamesInBody.insert(fnDecl->id.name);
        }
      }
    }
    for (const auto& lexName : lexicalNames) {
      if (paramNameSet.count(lexName) != 0) {
        // In sloppy mode, function declarations can shadow parameters
        if (!strictFnCode && funcDeclNamesInBody.count(lexName) != 0) {
          continue;
        }
        return nullptr;
      }
    }
  }
  // Early errors: non-method functions do not have a super binding.
  // Reject `super` in parameters or body for all `function` declarations.
  if (hasSuperInParams) {
    return nullptr;
  }
  for (const auto& stmt : blockStmt->body) {
    if (stmt && statementContainsSuper(*stmt)) {
      return nullptr;
    }
  }

  FunctionDeclaration funcDecl;
  funcDecl.id = {name};
  funcDecl.params = std::move(params);
  funcDecl.restParam = restParam;
  funcDecl.body = std::move(blockStmt->body);
  funcDecl.destructurePrologue = std::move(destructurePrologue);
  funcDecl.isAsync = isAsync;
  funcDecl.isGenerator = isGenerator;
  // Extract source text for Function.prototype.toString
  if (!source_.empty() && startTok.offset < source_.size()) {
    uint32_t endOff = (pos_ > 0 && pos_ <= tokens_.size()) ? tokens_[pos_ - 1].endOffset : static_cast<uint32_t>(source_.size());
    if (endOff > startTok.offset && endOff <= source_.size()) {
      funcDecl.sourceText = source_.substr(startTok.offset, endOff - startTok.offset);
    }
  }

  return makeStmt(std::move(funcDecl), startTok);
}

StmtPtr Parser::parseClassDeclaration() {
  size_t decoratorCount = 0;
  if (!parseDecoratorList(decoratorCount)) {
    return nullptr;
  }
  expect(TokenType::Class);

  if (!isIdentifierLikeToken(current().type)) {
    return nullptr;
  }
  std::string className = current().value;
  if (isAlwaysReservedIdentifierWord(className) ||
      isStrictFutureReservedIdentifier(className)) {
    return nullptr;
  }
  advance();

  ExprPtr superClass;
  if (match(TokenType::Extends)) {
    advance();
    // Class definitions are strict mode code; parse the heritage expression in strict
    // mode so early errors (e.g. `with`) are enforced (Test262: class/strict-mode/with.js).
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
    // Class heritage is evaluated in the *outer* PrivateEnvironment.
    // Any private name access here is invalid (unless coming from an already-validated nested class).
    const std::set<std::string> kNoPrivateNames;
    if (expressionHasUndeclaredPrivateName(*superClass, kNoPrivateNames)) {
      return nullptr;
    }
  }

  expect(TokenType::LeftBrace);

  // Class bodies are always strict mode code (ES2015+).
  struct StrictModeGuard {
    Parser* parser;
    bool saved;
    explicit StrictModeGuard(Parser* p) : parser(p), saved(p->strictMode_) { parser->strictMode_ = true; }
    ~StrictModeGuard() { parser->strictMode_ = saved; }
  } strictGuard(this);
  ++classBodyDepth_;
  struct ClassBodyDepthGuard1 {
    int& depth;
    explicit ClassBodyDepthGuard1(int& d) : depth(d) {}
    ~ClassBodyDepthGuard1() { --depth; }
  } classBodyGuard1(classBodyDepth_);
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
    // Private names share a single namespace within the class.
    // Allow exactly one getter+setter pair; all other duplicates/conflicts are early errors.
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
      case MethodDefinition::Kind::Constructor: add = kMethod; break;  // shouldn't happen for private names
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
    // Skip semicolons
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

    // ClassStaticBlock: `static { ... }`
    // Note: Escaped `static` must not start a static block.
    if (match(TokenType::Static) && !current().escaped && peek().type == TokenType::LeftBrace) {
      method.kind = MethodDefinition::Kind::StaticBlock;
      method.isStatic = true;
      method.key.name.clear();
      advance();  // consume 'static'

      // Static blocks are control-flow boundaries for break/continue/labels, and
      // use StatementList[~Yield, +Await, ~Return].
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

    // Escaped 'static' must not be treated as the static modifier.
    if (match(TokenType::Static) &&
        current().escaped &&
        peek().type != TokenType::LeftParen &&
        peek().type != TokenType::Equal &&
        peek().type != TokenType::Semicolon &&
        peek().type != TokenType::RightBrace) {
      return nullptr;
    }

    // Check for static modifier.
    if (match(TokenType::Static) &&
        peek().type != TokenType::LeftParen &&
        peek().type != TokenType::Equal &&
        peek().type != TokenType::Semicolon &&
        peek().type != TokenType::RightBrace) {
      method.isStatic = true;
      advance();
    }

    // Check for async modifier (must be same line and not escaped).
    if (match(TokenType::Async) &&
        !current().escaped &&
        current().line == peek().line &&
        peek().type != TokenType::LeftParen &&
        peek().type != TokenType::Semicolon &&
        peek().type != TokenType::Equal && peek().type != TokenType::RightBrace) {
      method.isAsync = true;
      advance();
    }

    // Check for generator method marker.
    if (match(TokenType::Star)) {
      method.isGenerator = true;
      advance();
    }

    // Check for getter/setter - only if followed by name + '('
    if ((match(TokenType::Get) || match(TokenType::Set)) &&
        (isIdentifierNameToken(peek().type) ||
         peek().type == TokenType::PrivateIdentifier ||
         peek().type == TokenType::LeftBracket ||
         peek().type == TokenType::String ||
         peek().type == TokenType::Number ||
         peek().type == TokenType::BigInt)) {
      // Peek ahead: if the token after the name is '(', it's a getter/setter
      // Otherwise treat get/set as a field name
      size_t saved = pos_;
      TokenType savedType = current().type;
      advance(); // consume get/set
      bool isGetterSetter = false;
      if (match(TokenType::LeftBracket)) {
        advance(); // [
        bool savedAllowIn = allowIn_;
        allowIn_ = true;
        bool savedError = error_;
        auto keyExpr = parseAssignment();
        error_ = savedError;
        allowIn_ = savedAllowIn;
        if (keyExpr && match(TokenType::RightBracket)) {
          advance(); // ]
          if (match(TokenType::LeftParen)) {
            isGetterSetter = true;
          }
        }
      } else if (isIdentifierNameToken(current().type) ||
                 match(TokenType::PrivateIdentifier) ||
                 match(TokenType::String) ||
                 match(TokenType::Number) ||
                 match(TokenType::BigInt)) {
        advance(); // consume name
        if (match(TokenType::LeftParen)) {
          isGetterSetter = true;
        }
      }
      // Restore position
      pos_ = saved;

      if (isGetterSetter) {
        if (savedType == TokenType::Get) {
          method.kind = MethodDefinition::Kind::Get;
        } else {
          method.kind = MethodDefinition::Kind::Set;
        }
        advance(); // consume get/set
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

    // Member name (identifier, private identifier, string/number/bigint literal)
    if (match(TokenType::PrivateIdentifier)) {
      method.isPrivate = true;
      if (current().value == "#constructor") {
        return nullptr;
      }
      method.key.name = current().value; // includes '#' prefix
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

    // Distinguish field vs method: if '(' follows, it's a method; otherwise a field
    if (match(TokenType::LeftParen)) {
      if (method.kind == MethodDefinition::Kind::AutoAccessor) {
        return nullptr;
      }
      // Method/constructor/getter/setter
      if (method.kind == MethodDefinition::Kind::Constructor &&
          (method.isAsync || method.isGenerator)) {
        return nullptr;
      }
      // Instance getters/setters named "constructor" are early errors.
      if (!method.isStatic &&
          !method.computed &&
          !method.isPrivate &&
          method.key.name == "constructor" &&
          (method.kind == MethodDefinition::Kind::Get ||
           method.kind == MethodDefinition::Kind::Set)) {
        return nullptr;
      }
      // Static methods cannot be named "prototype".
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
      advance(); // consume '('

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
            // Rest parameters cannot have initializers.
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
          break;  // rest must be last
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
          for (auto& name : names) {
            if (name == "eval" || name == "arguments") return nullptr;
            if (!seenParamNames.insert(name).second) hasDuplicateParams = true;
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

      // Accessor parameter arity early errors.
      if (method.kind == MethodDefinition::Kind::Get) {
        if (!method.params.empty() || method.restParam.has_value()) {
          return nullptr;
        }
      }
      if (method.kind == MethodDefinition::Kind::Set) {
        if (method.params.size() != 1 || method.restParam.has_value()) {
          return nullptr;
        }
      }

      // Method body
      bool allowSuperCall = superClass && !method.isStatic &&
                            method.kind == MethodDefinition::Kind::Constructor;
      if (!allowSuperCall) {
        ++superCallDisallowDepth_;
      }
      if (method.isGenerator) {
        ++generatorFunctionDepth_;
      }
      // Break/continue/labels do not cross function boundaries.
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
      // Field declaration
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
        advance(); // consume '='
        method.initializer = parseAssignment();
        if (!method.initializer) {
          return nullptr;
        }
        // Early errors: field initializers must not contain super() or arguments
        if (fieldInitContainsSuperCall(*method.initializer)) {
          return nullptr;
        }
        if (fieldInitContainsArguments(*method.initializer)) {
          return nullptr;
        }
      }
      uint32_t fieldEndLine = tokens_[pos_ - 1].line;
      // Consume optional semicolon
      bool consumedSemicolon = false;
      if (match(TokenType::Semicolon)) {
        advance();
        consumedSemicolon = true;
      }
      // ASI for class fields only inserts a semicolon at a LineTerminator.
      // `x y` on the same line is not two fields.
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

  auto decl = std::make_unique<Statement>(ClassDeclaration{
    {className},
    std::move(superClass),
    std::move(methods)
  });

  return decl;
}


}  // namespace lightjs
