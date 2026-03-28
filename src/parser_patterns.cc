#include "parser_internal.h"

namespace lightjs {

ExprPtr Parser::parseNewExpression() {
  Token newTok = current();
  expect(TokenType::New);

  if (match(TokenType::Dot) &&
      peek().type == TokenType::Identifier &&
      peek().value == "target") {
    if (newTok.escaped) {
      error_ = true;
      return nullptr;
    }
    if (newTargetDepth_ == 0 && staticBlockDepth_ == 0 && !isEvalContext_) {
      error_ = true;
      return nullptr;
    }
    advance();
    if (current().escaped) {
      error_ = true;
      return nullptr;
    }
    advance();
    return makeExpr(MetaProperty{"new", "target"}, newTok);
  }

  auto callee = parseMember();
  if (!callee) {
    return nullptr;
  }
  if (isDirectDynamicImportCall(callee)) {
    return nullptr;
  }

  std::vector<ExprPtr> args;
  if (match(TokenType::LeftParen)) {
    advance();
    while (!match(TokenType::RightParen)) {
      if (!args.empty()) {
        if (!expect(TokenType::Comma)) {
          return nullptr;
        }
        if (match(TokenType::RightParen)) {
          break;
        }
      }

      if (match(TokenType::DotDotDot)) {
        advance();
        auto arg = parseAssignment();
        if (!arg) {
          return nullptr;
        }
        args.push_back(std::make_unique<Expression>(SpreadElement{std::move(arg)}));
      } else {
        auto arg = parseAssignment();
        if (!arg) {
          return nullptr;
        }
        args.push_back(std::move(arg));
      }
    }
    if (!expect(TokenType::RightParen)) {
      return nullptr;
    }
  }

  NewExpr newExpr;
  newExpr.callee = std::move(callee);
  newExpr.arguments = std::move(args);

  return std::make_unique<Expression>(std::move(newExpr));
}

ExprPtr Parser::parsePattern() {
  if (match(TokenType::LeftBracket)) {
    auto base = parseArrayPattern();
    if (!base) {
      return nullptr;
    }
    if (match(TokenType::Equal)) {
      advance();
      auto init = parseAssignment();
      if (!init) {
        return nullptr;
      }
      return std::make_unique<Expression>(AssignmentPattern{std::move(base), std::move(init)});
    }
    return base;
  }

  if (match(TokenType::LeftBrace)) {
    size_t peekPos = pos_ + 1;
    int depth = 1;
    while (depth > 0 && peekPos < tokens_.size()) {
      auto t = tokens_[peekPos].type;
      if (t == TokenType::LeftBrace || t == TokenType::LeftBracket || t == TokenType::LeftParen)
        depth++;
      else if (t == TokenType::RightBrace || t == TokenType::RightBracket || t == TokenType::RightParen)
        depth--;
      peekPos++;
    }
    bool isObjLiteralMember = peekPos < tokens_.size() &&
        (tokens_[peekPos].type == TokenType::Dot ||
         tokens_[peekPos].type == TokenType::LeftBracket);

    if (isObjLiteralMember) {
      auto base = parseMember();
      if (!base) return nullptr;
      if (match(TokenType::Equal)) {
        advance();
        auto init = parseAssignment();
        if (!init) return nullptr;
        return std::make_unique<Expression>(AssignmentPattern{std::move(base), std::move(init)});
      }
      return base;
    }

    auto base = parseObjectPattern();
    if (!base) {
      return nullptr;
    }
    if (match(TokenType::Equal)) {
      advance();
      auto init = parseAssignment();
      if (!init) {
        return nullptr;
      }
      return std::make_unique<Expression>(AssignmentPattern{std::move(base), std::move(init)});
    }
    return base;
  }

  if (isIdentifierLikeToken(current().type) || match(TokenType::This)) {
    ExprPtr base;
    if (match(TokenType::This)) {
      base = std::make_unique<Expression>(ThisExpr{});
      advance();
    } else {
      std::string name = current().value;
      advance();
      base = std::make_unique<Expression>(Identifier{name});
    }
    while (match(TokenType::Dot) || match(TokenType::LeftBracket)) {
      if (match(TokenType::Dot)) {
        advance();
        if (!isIdentifierNameToken(current().type) && !match(TokenType::PrivateIdentifier)) break;
        auto prop = std::make_unique<Expression>(Identifier{current().value});
        bool isPrivate = match(TokenType::PrivateIdentifier);
        advance();
        MemberExpr mem;
        mem.object = std::move(base);
        mem.property = std::move(prop);
        mem.computed = false;
        mem.privateIdentifier = isPrivate;
        base = std::make_unique<Expression>(std::move(mem));
      } else {
        advance();
        auto prop = parseAssignment();
        if (!prop || !expect(TokenType::RightBracket)) return nullptr;
        MemberExpr mem;
        mem.object = std::move(base);
        mem.property = std::move(prop);
        mem.computed = true;
        base = std::make_unique<Expression>(std::move(mem));
      }
    }
    if (match(TokenType::Equal)) {
      advance();
      auto init = parseAssignment();
      if (!init) {
        return nullptr;
      }
      return std::make_unique<Expression>(AssignmentPattern{std::move(base), std::move(init)});
    }
    return base;
  }

  return nullptr;
}

ExprPtr Parser::parseArrayPattern() {
  expect(TokenType::LeftBracket);

  ArrayPattern pattern;

  while (!match(TokenType::RightBracket) && pos_ < tokens_.size()) {
    if (!pattern.elements.empty()) {
      expect(TokenType::Comma);
      if (match(TokenType::RightBracket)) {
        break;
      }
    }

    if (match(TokenType::DotDotDot)) {
      advance();
      pattern.rest = parsePattern();
      if (!pattern.rest) {
        return nullptr;
      }
      if (std::get_if<AssignmentPattern>(&pattern.rest->node)) {
        return nullptr;
      }
      break;
    }

    if (match(TokenType::Comma)) {
      pattern.elements.push_back(nullptr);
      continue;
    }

    ExprPtr element = parsePattern();
    if (!element) {
      return nullptr;
    }
    pattern.elements.push_back(std::move(element));
  }

  if (!expect(TokenType::RightBracket)) {
    return nullptr;
  }

  return std::make_unique<Expression>(std::move(pattern));
}

ExprPtr Parser::parseObjectPattern() {
  expect(TokenType::LeftBrace);

  ObjectPattern pattern;

  while (!match(TokenType::RightBrace) && pos_ < tokens_.size()) {
    if (!pattern.properties.empty()) {
      expect(TokenType::Comma);
      if (match(TokenType::RightBrace)) {
        break;
      }
    }

    if (match(TokenType::DotDotDot)) {
      advance();
      pattern.rest = parsePattern();
      if (!pattern.rest) {
        return nullptr;
      }
      break;
    }

    ExprPtr key;
    bool isComputed = false;
    if (match(TokenType::LeftBracket)) {
      isComputed = true;
      advance();
      key = parseAssignment();
      if (!key || !expect(TokenType::RightBracket)) {
        return nullptr;
      }
    } else if (isIdentifierNameToken(current().type)) {
      std::string name = current().value;
      advance();
      key = std::make_unique<Expression>(Identifier{name});
    } else if (match(TokenType::String)) {
      std::string strVal = current().value;
      advance();
      key = std::make_unique<Expression>(StringLiteral{strVal});
    } else if (match(TokenType::Number)) {
      std::string numVal = current().value;
      advance();
      key = std::make_unique<Expression>(NumberLiteral{std::stod(numVal)});
    } else if (match(TokenType::BigInt)) {
      std::string bigintVal = current().value;
      advance();
      bigint::BigIntValue parsed = 0;
      if (!parseBigIntLiteral64(bigintVal, parsed)) {
        return nullptr;
      }
      key = std::make_unique<Expression>(BigIntLiteral{parsed});
    } else {
      return nullptr;
    }

    ExprPtr value;
    if (match(TokenType::Colon)) {
      advance();
      value = parsePattern();
      if (!value) {
        return nullptr;
      }
    } else if (match(TokenType::Equal)) {
      advance();
      auto init = parseAssignment();
      if (!init) {
        return nullptr;
      }
      if (auto* id = std::get_if<Identifier>(&key->node)) {
        if ((id->name == "yield" && !canUseYieldAsIdentifier()) ||
            (id->name == "await" && !canUseAwaitAsIdentifier())) {
          return nullptr;
        }
        if (isAlwaysReservedIdentifierWord(id->name)) {
          if (!(id->name == "yield" && canUseYieldAsIdentifier()) &&
              !(id->name == "await" && canUseAwaitAsIdentifier())) {
            return nullptr;
          }
        }
        if (strictMode_ && (isStrictFutureReservedIdentifier(id->name) ||
                            id->name == "eval" || id->name == "arguments")) {
          return nullptr;
        }
        auto left = std::make_unique<Expression>(Identifier{id->name});
        value = std::make_unique<Expression>(AssignmentPattern{std::move(left), std::move(init)});
      } else {
        return nullptr;
      }
    } else {
      if (auto* id = std::get_if<Identifier>(&key->node)) {
        if ((id->name == "yield" && !canUseYieldAsIdentifier()) ||
            (id->name == "await" && !canUseAwaitAsIdentifier())) {
          return nullptr;
        }
        if (isAlwaysReservedIdentifierWord(id->name)) {
          if (!(id->name == "yield" && canUseYieldAsIdentifier()) &&
              !(id->name == "await" && canUseAwaitAsIdentifier())) {
            return nullptr;
          }
        }
        if (strictMode_ && (isStrictFutureReservedIdentifier(id->name) ||
                            id->name == "eval" || id->name == "arguments")) {
          return nullptr;
        }
        value = std::make_unique<Expression>(Identifier{id->name});
      } else {
        return nullptr;
      }
    }

    ObjectPattern::Property prop;
    prop.key = std::move(key);
    prop.value = std::move(value);
    prop.computed = isComputed;
    pattern.properties.push_back(std::move(prop));
  }

  if (!expect(TokenType::RightBrace)) {
    return nullptr;
  }

  return std::make_unique<Expression>(std::move(pattern));
}

}  // namespace lightjs
