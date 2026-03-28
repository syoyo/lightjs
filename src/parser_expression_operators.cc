#include "parser_internal.h"

namespace lightjs {

ExprPtr Parser::parseConditional() {
  auto expr = parseNullishCoalescing();
  if (!expr) {
    return nullptr;
  }

  if (match(TokenType::Question)) {
    advance();
    // In ConditionalExpression, the consequent AssignmentExpression is always parsed with +In.
    bool savedAllowIn = allowIn_;
    allowIn_ = true;
    auto consequent = parseAssignment();
    allowIn_ = savedAllowIn;
    if (!consequent || !expect(TokenType::Colon)) {
      return nullptr;
    }
    auto alternate = parseAssignment();
    if (!alternate) {
      return nullptr;
    }
    return std::make_unique<Expression>(ConditionalExpr{
      std::move(expr),
      std::move(consequent),
      std::move(alternate)
    });
  }

  return expr;
}

ExprPtr Parser::parseNullishCoalescing() {
  auto left = parseLogicalOr();
  if (!left) {
    return nullptr;
  }

  while (match(TokenType::QuestionQuestion)) {
    if (hasUnparenthesizedLogicalOp(left)) {
      return nullptr;
    }
    advance();
    auto right = parseLogicalOr();
    if (!right) {
      return nullptr;
    }
    if (hasUnparenthesizedLogicalOp(right)) {
      return nullptr;
    }
    left = std::make_unique<Expression>(BinaryExpr{
      BinaryExpr::Op::NullishCoalescing,
      std::move(left),
      std::move(right)
    });
  }

  return left;
}

ExprPtr Parser::parseLogicalOr() {
  auto left = parseLogicalAnd();
  if (!left) {
    return nullptr;
  }

  while (match(TokenType::PipePipe)) {
    advance();
    auto right = parseLogicalAnd();
    if (!right) {
      return nullptr;
    }
    left = std::make_unique<Expression>(BinaryExpr{
      BinaryExpr::Op::LogicalOr,
      std::move(left),
      std::move(right)
    });
  }

  return left;
}

ExprPtr Parser::parseLogicalAnd() {
  auto left = parseBitwiseOr();
  if (!left) {
    return nullptr;
  }

  while (match(TokenType::AmpAmp)) {
    advance();
    auto right = parseBitwiseOr();
    if (!right) {
      return nullptr;
    }
    left = std::make_unique<Expression>(BinaryExpr{
      BinaryExpr::Op::LogicalAnd,
      std::move(left),
      std::move(right)
    });
  }

  return left;
}

ExprPtr Parser::parseBitwiseOr() {
  auto left = parseBitwiseXor();
  if (!left) {
    return nullptr;
  }

  while (match(TokenType::Pipe)) {
    advance();
    auto right = parseBitwiseXor();
    if (!right) {
      return nullptr;
    }
    left = std::make_unique<Expression>(BinaryExpr{
      BinaryExpr::Op::BitwiseOr,
      std::move(left),
      std::move(right)
    });
  }

  return left;
}

ExprPtr Parser::parseBitwiseXor() {
  auto left = parseBitwiseAnd();
  if (!left) {
    return nullptr;
  }

  while (match(TokenType::Caret)) {
    advance();
    auto right = parseBitwiseAnd();
    if (!right) {
      return nullptr;
    }
    left = std::make_unique<Expression>(BinaryExpr{
      BinaryExpr::Op::BitwiseXor,
      std::move(left),
      std::move(right)
    });
  }

  return left;
}

ExprPtr Parser::parseBitwiseAnd() {
  auto left = parseEquality();
  if (!left) {
    return nullptr;
  }

  while (match(TokenType::Amp)) {
    advance();
    auto right = parseEquality();
    if (!right) {
      return nullptr;
    }
    left = std::make_unique<Expression>(BinaryExpr{
      BinaryExpr::Op::BitwiseAnd,
      std::move(left),
      std::move(right)
    });
  }

  return left;
}

ExprPtr Parser::parseEquality() {
  auto left = parseRelational();
  if (!left) {
    return nullptr;
  }

  while (match(TokenType::EqualEqual) || match(TokenType::EqualEqualEqual) ||
         match(TokenType::BangEqual) || match(TokenType::BangEqualEqual)) {

    BinaryExpr::Op op;
    switch (current().type) {
      case TokenType::EqualEqual: op = BinaryExpr::Op::Equal; break;
      case TokenType::EqualEqualEqual: op = BinaryExpr::Op::StrictEqual; break;
      case TokenType::BangEqual: op = BinaryExpr::Op::NotEqual; break;
      case TokenType::BangEqualEqual: op = BinaryExpr::Op::StrictNotEqual; break;
      default: op = BinaryExpr::Op::Equal; break;
    }
    advance();

    auto right = parseRelational();
    if (!right) {
      return nullptr;
    }
    left = std::make_unique<Expression>(BinaryExpr{op, std::move(left), std::move(right)});
  }

  return left;
}

ExprPtr Parser::parseRelational() {
  ExprPtr left;
  // Grammar support: PrivateIdentifier in ShiftExpression
  if (allowIn_ && match(TokenType::PrivateIdentifier) && peek().type == TokenType::In) {
    const Token& tok = current();
    left = makeExpr(Identifier{tok.value}, tok);
    advance();
  } else {
    left = parseShift();
  }
  if (!left) {
    return nullptr;
  }

  while (match(TokenType::Less) || match(TokenType::Greater) ||
         match(TokenType::LessEqual) || match(TokenType::GreaterEqual) ||
         (allowIn_ && match(TokenType::In)) || match(TokenType::Instanceof)) {

    BinaryExpr::Op op;
    switch (current().type) {
      case TokenType::Less: op = BinaryExpr::Op::Less; break;
      case TokenType::Greater: op = BinaryExpr::Op::Greater; break;
      case TokenType::LessEqual: op = BinaryExpr::Op::LessEqual; break;
      case TokenType::GreaterEqual: op = BinaryExpr::Op::GreaterEqual; break;
      case TokenType::In: op = BinaryExpr::Op::In; break;
      case TokenType::Instanceof: op = BinaryExpr::Op::Instanceof; break;
      default: op = BinaryExpr::Op::Less; break;
    }
    advance();

    auto right = parseShift();
    if (!right) {
      return nullptr;
    }
    left = std::make_unique<Expression>(BinaryExpr{op, std::move(left), std::move(right)});
  }

  return left;
}

ExprPtr Parser::parseShift() {
  auto left = parseAdditive();
  if (!left) {
    return nullptr;
  }

  while (match(TokenType::LeftShift) || match(TokenType::RightShift) ||
         match(TokenType::UnsignedRightShift)) {
    BinaryExpr::Op op;
    switch (current().type) {
      case TokenType::LeftShift: op = BinaryExpr::Op::LeftShift; break;
      case TokenType::RightShift: op = BinaryExpr::Op::RightShift; break;
      case TokenType::UnsignedRightShift: op = BinaryExpr::Op::UnsignedRightShift; break;
      default: op = BinaryExpr::Op::LeftShift; break;
    }
    advance();

    auto right = parseAdditive();
    if (!right) {
      return nullptr;
    }
    left = std::make_unique<Expression>(BinaryExpr{op, std::move(left), std::move(right)});
  }

  return left;
}

ExprPtr Parser::parseAdditive() {
  auto left = parseMultiplicative();
  if (!left) {
    return nullptr;
  }

  while (match(TokenType::Plus) || match(TokenType::Minus)) {
    BinaryExpr::Op op = match(TokenType::Plus) ? BinaryExpr::Op::Add : BinaryExpr::Op::Sub;
    advance();
    auto right = parseMultiplicative();
    if (!right) {
      return nullptr;
    }
    left = std::make_unique<Expression>(BinaryExpr{op, std::move(left), std::move(right)});
  }

  return left;
}

ExprPtr Parser::parseMultiplicative() {
  auto left = parseExponentiation();
  if (!left) {
    return nullptr;
  }

  while (match(TokenType::Star) || match(TokenType::Slash) || match(TokenType::Percent)) {
    BinaryExpr::Op op;
    switch (current().type) {
      case TokenType::Star: op = BinaryExpr::Op::Mul; break;
      case TokenType::Slash: op = BinaryExpr::Op::Div; break;
      case TokenType::Percent: op = BinaryExpr::Op::Mod; break;
      default: op = BinaryExpr::Op::Mul; break;
    }
    advance();
    auto right = parseExponentiation();
    if (!right) {
      return nullptr;
    }
    left = std::make_unique<Expression>(BinaryExpr{op, std::move(left), std::move(right)});
  }

  return left;
}

ExprPtr Parser::parseExponentiation() {
  auto left = parseUnary();
  if (!left) {
    return nullptr;
  }

  // Right-to-left associativity for **
  if (match(TokenType::StarStar)) {
    // ES early errors: UnaryExpression is not allowed as the left-hand side of
    // ** unless it is parenthesized. This rejects cases like `-1 ** 2`,
    // `delete x ** 2`, `typeof x ** 2`, etc. while permitting `(-1) ** 2`.
    if (!left->parenthesized) {
      if (std::holds_alternative<UnaryExpr>(left->node) ||
          std::holds_alternative<AwaitExpr>(left->node)) {
        return nullptr;
      }
    }
    advance();
    auto right = parseExponentiation();  // Right associative
    if (!right) {
      return nullptr;
    }
    return std::make_unique<Expression>(BinaryExpr{BinaryExpr::Op::Exp, std::move(left), std::move(right)});
  }

  return left;
}

ExprPtr Parser::parseUnary() {
  if (match(TokenType::Await) && canParseAwaitExpression()) {
    if (current().escaped) {
      return nullptr;
    }
    uint32_t awaitLine = current().line;
    advance();
    // await [no LineTerminator here] UnaryExpression
    if (current().line != awaitLine) {
      // In ES, if await is followed by newline, it might be an error or just await without arg
      // But await always requires an argument in async functions.
      return nullptr;
    }
    auto argument = parseUnary();
    if (!argument) {
      return nullptr;
    }
    return std::make_unique<Expression>(AwaitExpr{std::move(argument)});
  }

  if (match(TokenType::Yield) && !canUseYieldAsIdentifier()) {
    // YieldExpression is only valid when the current parsing context has +Yield.
    if (!yieldContextStack_.empty() && !yieldContextStack_.back()) {
      return nullptr;
    }
    // `yield` is only an expression keyword inside generator function bodies.
    if (generatorFunctionDepth_ == 0) {
      return nullptr;
    }
    uint32_t yieldLine = current().line;
    advance();
    bool delegate = false;
    if (match(TokenType::Star) && current().line != yieldLine) {
      return nullptr;
    }

    // Check for yield* (delegate to another iterator)
    if (match(TokenType::Star) && current().line == yieldLine) {
      delegate = true;
      advance();
    }

    // yield can be used without an argument if followed by newline or certain tokens
    // BUT yield* ALWAYS requires an argument and allows it on a new line
    ExprPtr argument = nullptr;
    if (delegate || (current().line == yieldLine &&
        !match(TokenType::EndOfFile) &&
        !match(TokenType::Semicolon) && !match(TokenType::RightBrace) &&
        !match(TokenType::RightParen) && !match(TokenType::RightBracket) &&
        !match(TokenType::Comma) && !match(TokenType::Colon))) {
      argument = parseAssignment();
      if (!argument) {
        return nullptr;
      }
      if (std::holds_alternative<BinaryExpr>(argument->node)) {
        auto containsYieldExpr = [&](const Expression& e, const auto& self) -> bool {
          if (std::holds_alternative<YieldExpr>(e.node)) return true;
          if (auto* b = std::get_if<BinaryExpr>(&e.node)) {
            return (b->left && self(*b->left, self)) || (b->right && self(*b->right, self));
          }
          if (auto* u = std::get_if<UnaryExpr>(&e.node)) {
            return u->argument && self(*u->argument, self);
          }
          if (auto* c = std::get_if<CallExpr>(&e.node)) {
            if (c->callee && self(*c->callee, self)) return true;
            for (const auto& argExpr : c->arguments) {
              if (argExpr && self(*argExpr, self)) return true;
            }
            return false;
          }
          if (auto* a = std::get_if<ArrayExpr>(&e.node)) {
            for (const auto& elem : a->elements) {
              if (elem && self(*elem, self)) return true;
            }
            return false;
          }
          return false;
        };
        if (containsYieldExpr(*argument, containsYieldExpr)) {
          return nullptr;
        }
      }
    }

    return std::make_unique<Expression>(YieldExpr{std::move(argument), delegate});
  }

  if (match(TokenType::Bang) || match(TokenType::Minus) ||
      match(TokenType::Plus) || match(TokenType::Typeof) ||
      match(TokenType::Void) || match(TokenType::Tilde) ||
      match(TokenType::Delete)) {

    UnaryExpr::Op op;
    switch (current().type) {
      case TokenType::Bang: op = UnaryExpr::Op::Not; break;
      case TokenType::Minus: op = UnaryExpr::Op::Minus; break;
      case TokenType::Plus: op = UnaryExpr::Op::Plus; break;
      case TokenType::Typeof: op = UnaryExpr::Op::Typeof; break;
      case TokenType::Void: op = UnaryExpr::Op::Void; break;
      case TokenType::Tilde: op = UnaryExpr::Op::BitNot; break;
      case TokenType::Delete: op = UnaryExpr::Op::Delete; break;
      default: op = UnaryExpr::Op::Not; break;
    }
    advance();

    auto argument = parseUnary();
    if (!argument) {
      return nullptr;
    }
    if (std::holds_alternative<YieldExpr>(argument->node)) {
      return nullptr;
    }
    if (op == UnaryExpr::Op::Delete && strictMode_) {
      // ES strict-mode early errors: `delete IdentifierReference` and delete applied to private names.
      // Class bodies are always strict, so this also handles Test262 class-element delete early errors.
      if (std::holds_alternative<Identifier>(argument->node)) {
        return nullptr;
      }
      if (auto* mem = std::get_if<MemberExpr>(&argument->node)) {
        if (!mem->computed && mem->property &&
            std::holds_alternative<Identifier>(mem->property->node)) {
          const auto& id = std::get<Identifier>(mem->property->node);
          if (!id.name.empty() && id.name[0] == '#') {
            return nullptr;
          }
        }
      }
    }
    return std::make_unique<Expression>(UnaryExpr{op, std::move(argument)});
  }

  if (match(TokenType::PlusPlus) || match(TokenType::MinusMinus)) {
    UpdateExpr::Op op = match(TokenType::PlusPlus) ?
      UpdateExpr::Op::Increment : UpdateExpr::Op::Decrement;
    advance();
    auto argument = parseUnary();
    if (!argument || !isUpdateTarget(*argument)) {
      return nullptr;
    }
    if (strictMode_) {
      if (auto* id = std::get_if<Identifier>(&argument->node)) {
        if (id->name == "eval" || id->name == "arguments") {
          return nullptr;
        }
      }
    }
    return std::make_unique<Expression>(UpdateExpr{op, std::move(argument), true});
  }

  return parsePostfix();
}

ExprPtr Parser::parsePostfix() {
  auto expr = parseCall();

  if (match(TokenType::PlusPlus) || match(TokenType::MinusMinus)) {
    // Postfix update has a [no LineTerminator here] restriction.
    if (pos_ > 0) {
      const Token& previous = tokens_[pos_ - 1];
      if (current().line > previous.line) {
        return expr;
      }
    }
    UpdateExpr::Op op = match(TokenType::PlusPlus) ?
      UpdateExpr::Op::Increment : UpdateExpr::Op::Decrement;
    advance();
    if (!expr || !isUpdateTarget(*expr)) {
      return nullptr;
    }
    if (strictMode_) {
      if (auto* id = std::get_if<Identifier>(&expr->node)) {
        if (id->name == "eval" || id->name == "arguments") {
          return nullptr;
        }
      }
    }
    return std::make_unique<Expression>(UpdateExpr{op, std::move(expr), false});
  }

  return expr;
}

}  // namespace lightjs
