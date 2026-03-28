#include "parser_internal.h"

namespace lightjs {

ExprPtr Parser::parseCall() {
  auto expr = parsePrimary();
  if (!expr) {
    return nullptr;
  }
  expr = parseMemberSuffix(std::move(expr));
  if (!expr) {
    return nullptr;
  }

  // Track whether we're inside an optional chain for short-circuit propagation
  auto isExprInOptionalChain = [](const Expression& e) -> bool {
    if (auto* m = std::get_if<MemberExpr>(&e.node)) {
      return m->optional || m->inOptionalChain;
    }
    if (auto* c = std::get_if<CallExpr>(&e.node)) {
      return c->optional || c->inOptionalChain;
    }
    return false;
  };

  // Loop to handle both calls and member access after calls
  // This enables chaining like: arr.slice(1).length or obj.method().prop.method2()
  while (true) {
    bool optChain = expr && isExprInOptionalChain(*expr);

    if (match(TokenType::QuestionDot) && peek().type == TokenType::LeftParen) {
      optChain = true;
      advance();  // consume ?.
      advance();  // consume (
      std::vector<ExprPtr> args;

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
      CallExpr call;
      call.callee = std::move(expr);
      call.arguments = std::move(args);
      call.optional = true;
      call.inOptionalChain = true;
      expr = std::make_unique<Expression>(std::move(call));
      expr = parseMemberSuffix(std::move(expr), true);
      if (!expr) {
        return nullptr;
      }
    } else if (match(TokenType::LeftParen)) {
      if (superCallDisallowDepth_ > 0 &&
          expr && std::holds_alternative<SuperExpr>(expr->node)) {
        return nullptr;
      }
      advance();
      std::vector<ExprPtr> args;

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
      CallExpr call;
      call.callee = std::move(expr);
      call.arguments = std::move(args);
      if (auto* member = std::get_if<MemberExpr>(&call.callee->node)) {
        call.optional = member->optional;
      }
      call.inOptionalChain = optChain;
      expr = std::make_unique<Expression>(std::move(call));
      expr = parseMemberSuffix(std::move(expr), optChain);
      if (!expr) {
        return nullptr;
      }
    } else if (match(TokenType::TemplateLiteral)) {
      if (isOptionalChain(*expr)) {
        return nullptr;
      }
      std::vector<ExprPtr> args;
      inTaggedTemplate_ = true;
      auto templateArg = parsePrimary();
      inTaggedTemplate_ = false;
      if (!templateArg) {
        return nullptr;
      }
      if (auto* templateLiteral = std::get_if<TemplateLiteral>(&templateArg->node)) {
        auto objExpr = std::make_unique<Expression>(TemplateObjectExpr{std::move(templateLiteral->quasis)});
        objExpr->loc = templateArg->loc;
        args.push_back(std::move(objExpr));
        for (auto& subst : templateLiteral->expressions) {
          args.push_back(std::move(subst));
        }
      } else {
        args.push_back(std::move(templateArg));
      }

      CallExpr call;
      call.callee = std::move(expr);
      call.arguments = std::move(args);
      if (auto* member = std::get_if<MemberExpr>(&call.callee->node)) {
        call.optional = member->optional;
      }
      call.inOptionalChain = optChain;
      expr = std::make_unique<Expression>(std::move(call));
      expr = parseMemberSuffix(std::move(expr), optChain);
      if (!expr) {
        return nullptr;
      }
    } else {
      break;
    }
  }

  return expr;
}

ExprPtr Parser::parseMember() {
  auto expr = parsePrimary();
  return parseMemberSuffix(std::move(expr));
}

ExprPtr Parser::parseMemberSuffix(ExprPtr expr, bool inOptionalChain) {
  if (!expr) {
    return nullptr;
  }
  while (true) {
    if (match(TokenType::QuestionDot) && peek().type == TokenType::LeftParen) {
      break;
    }

    if (match(TokenType::QuestionDot)) {
      inOptionalChain = true;
      advance();
      if (match(TokenType::LeftBracket)) {
        advance();
        auto prop = parseAssignment();
        if (!prop) {
          return nullptr;
        }
        while (match(TokenType::Comma)) {
          advance();
          prop = parseAssignment();
          if (!prop) {
            return nullptr;
          }
        }
        if (!expect(TokenType::RightBracket)) {
          return nullptr;
        }
        MemberExpr member;
        member.object = std::move(expr);
        member.property = std::move(prop);
        member.computed = true;
        member.optional = true;
        member.inOptionalChain = true;
        expr = std::make_unique<Expression>(std::move(member));
        continue;
      }

      if (isIdentifierNameToken(current().type)) {
        auto prop = std::make_unique<Expression>(Identifier{current().value});
        advance();
        MemberExpr member;
        member.object = std::move(expr);
        member.property = std::move(prop);
        member.computed = false;
        member.optional = true;
        member.inOptionalChain = true;
        expr = std::make_unique<Expression>(std::move(member));
        continue;
      }
      if (match(TokenType::PrivateIdentifier)) {
        auto prop = std::make_unique<Expression>(Identifier{current().value});
        advance();
        MemberExpr member;
        member.object = std::move(expr);
        member.property = std::move(prop);
        member.computed = false;
        member.privateIdentifier = true;
        member.optional = true;
        member.inOptionalChain = true;
        expr = std::make_unique<Expression>(std::move(member));
        continue;
      }
      return nullptr;
    } else if (match(TokenType::Dot)) {
      advance();
      if (match(TokenType::PrivateIdentifier)) {
        if (expr && std::holds_alternative<SuperExpr>(expr->node)) {
          return nullptr;
        }
        auto prop = std::make_unique<Expression>(Identifier{current().value});
        advance();
        MemberExpr member;
        member.object = std::move(expr);
        member.property = std::move(prop);
        member.computed = false;
        member.privateIdentifier = true;
        member.optional = false;
        member.inOptionalChain = inOptionalChain;
        expr = std::make_unique<Expression>(std::move(member));
        continue;
      }
      if (isIdentifierNameToken(current().type)) {
        auto prop = std::make_unique<Expression>(Identifier{current().value});
        advance();
        MemberExpr member;
        member.object = std::move(expr);
        member.property = std::move(prop);
        member.computed = false;
        member.optional = false;
        member.inOptionalChain = inOptionalChain;
        expr = std::make_unique<Expression>(std::move(member));
        continue;
      }
      break;
    } else if (match(TokenType::LeftBracket)) {
      advance();
      auto prop = parseAssignment();
      if (!prop) {
        return nullptr;
      }
      while (match(TokenType::Comma)) {
        advance();
        prop = parseAssignment();
        if (!prop) {
          return nullptr;
        }
      }
      if (!expect(TokenType::RightBracket)) {
        return nullptr;
      }
      MemberExpr member;
      member.object = std::move(expr);
      member.property = std::move(prop);
      member.computed = true;
      member.optional = false;
      member.inOptionalChain = inOptionalChain;
      expr = std::make_unique<Expression>(std::move(member));
    } else if (match(TokenType::TemplateLiteral)) {
      if (isOptionalChain(*expr)) {
        return nullptr;
      }
      std::vector<ExprPtr> args;
      inTaggedTemplate_ = true;
      auto templateArg = parsePrimary();
      inTaggedTemplate_ = false;
      if (!templateArg) {
        return nullptr;
      }
      if (auto* templateLiteral = std::get_if<TemplateLiteral>(&templateArg->node)) {
        auto objExpr = std::make_unique<Expression>(TemplateObjectExpr{std::move(templateLiteral->quasis)});
        objExpr->loc = templateArg->loc;
        args.push_back(std::move(objExpr));
        for (auto& subst : templateLiteral->expressions) {
          args.push_back(std::move(subst));
        }
      } else {
        args.push_back(std::move(templateArg));
      }

      CallExpr call;
      call.callee = std::move(expr);
      call.arguments = std::move(args);
      if (auto* member = std::get_if<MemberExpr>(&call.callee->node)) {
        call.optional = member->optional;
      }
      call.inOptionalChain = inOptionalChain;
      expr = std::make_unique<Expression>(std::move(call));
    } else {
      break;
    }
  }
  return expr;
}

}  // namespace lightjs
