#include "parser_internal.h"

namespace lightjs {

ExprPtr Parser::parsePrimary() {
  if (match(TokenType::Number)) {
    const Token& tok = current();
    double value = 0.0;
    if (!parseNumberLiteral(tok.value, strictMode_, value)) {
      return nullptr;
    }
    advance();
    return makeExpr(NumberLiteral{value}, tok);
  }

  if (match(TokenType::BigInt)) {
    const Token& tok = current();
    bigint::BigIntValue value = 0;
    if (!parseBigIntLiteral64(tok.value, value)) {
      return nullptr;
    }
    advance();
    return makeExpr(BigIntLiteral{value}, tok);
  }

  if (match(TokenType::String)) {
    const Token& tok = current();
    if (tok.hasLegacyEscape && strictMode_) {
      error_ = true;
      return nullptr;
    }
    std::string value = tok.value;
    advance();
    auto strLit = StringLiteral{value};
    strLit.hasLegacyEscape = tok.hasLegacyEscape;
    strLit.hasEscape = tok.escaped;
    return makeExpr(strLit, tok);
  }

  if (match(TokenType::TemplateLiteral)) {
    const Token& tok = current();
    std::string content = tok.value;
    advance();

    std::vector<TemplateElement> quasis;
    std::vector<ExprPtr> expressions;

    auto cookQuasi = [&](const std::string& raw) -> std::optional<std::string> {
      std::string cooked;
      cooked.reserve(raw.size());
      auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
      };
      auto appendUtf8Local = [](std::string& out, uint32_t codepoint) {
        if (codepoint <= 0x7F) {
          out.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FF) {
          out.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
          out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint <= 0xFFFF) {
          out.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
          out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
          out.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
          out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
      };

      for (size_t j = 0; j < raw.size(); j++) {
        char c = raw[j];
        if (c != '\\') {
          cooked.push_back(c);
          continue;
        }
        if (j + 1 >= raw.size()) return std::nullopt;
        char n = raw[++j];
        unsigned char un = static_cast<unsigned char>(n);
        if (un == 0xE2 && j + 2 < raw.size() &&
            static_cast<unsigned char>(raw[j + 1]) == 0x80) {
          unsigned char third = static_cast<unsigned char>(raw[j + 2]);
          if (third == 0xA8 || third == 0xA9) {
            j += 2;
            continue;
          }
        }
        switch (n) {
          case 'n': cooked.push_back('\n'); break;
          case 't': cooked.push_back('\t'); break;
          case 'r': cooked.push_back('\r'); break;
          case 'b': cooked.push_back('\b'); break;
          case 'f': cooked.push_back('\f'); break;
          case 'v': cooked.push_back('\v'); break;
          case '\\': cooked.push_back('\\'); break;
          case '`': cooked.push_back('`'); break;
          case '$': cooked.push_back('$'); break;
          case '\n': break;
          case '\r':
            if (j + 1 < raw.size() && raw[j + 1] == '\n') j++;
            break;
          case '0': {
            if (j + 1 < raw.size() && std::isdigit(static_cast<unsigned char>(raw[j + 1])) != 0) {
              return std::nullopt;
            }
            cooked.push_back('\0');
            break;
          }
          case 'x': {
            if (j + 2 >= raw.size()) return std::nullopt;
            int hi = hexVal(raw[j + 1]);
            int lo = hexVal(raw[j + 2]);
            if (hi < 0 || lo < 0) return std::nullopt;
            cooked.push_back(static_cast<char>((hi << 4) | lo));
            j += 2;
            break;
          }
          case 'u': {
            if (j + 1 < raw.size() && raw[j + 1] == '{') {
              j += 2;
              uint32_t cp = 0;
              bool saw = false;
              while (j < raw.size() && raw[j] != '}') {
                int hv = hexVal(raw[j]);
                if (hv < 0) return std::nullopt;
                saw = true;
                cp = (cp << 4) | static_cast<uint32_t>(hv);
                if (cp > 0x10FFFF) return std::nullopt;
                j++;
              }
              if (!saw) return std::nullopt;
              if (j >= raw.size() || raw[j] != '}') return std::nullopt;
              appendUtf8Local(cooked, cp);
            } else {
              if (j + 4 >= raw.size()) return std::nullopt;
              uint32_t cp = 0;
              for (int k = 0; k < 4; k++) {
                int hv = hexVal(raw[j + 1 + k]);
                if (hv < 0) return std::nullopt;
                cp = (cp << 4) | static_cast<uint32_t>(hv);
              }
              j += 4;
              appendUtf8Local(cooked, cp);
            }
            break;
          }
          default: {
            if (std::isdigit(static_cast<unsigned char>(n)) != 0) {
              return std::nullopt;
            }
            cooked.push_back(n);
            break;
          }
        }
      }
      return cooked;
    };

    std::string currentRawQuasi;
    currentRawQuasi.reserve(content.size());
    auto isEscaped = [&](size_t pos) -> bool {
      size_t backslashes = 0;
      while (pos > backslashes && content[pos - 1 - backslashes] == '\\') {
        backslashes++;
      }
      return (backslashes % 2) == 1;
    };

    size_t i = 0;
    while (i < content.length()) {
      if (i + 1 < content.length() && content[i] == '$' && content[i + 1] == '{' && !isEscaped(i)) {
        quasis.push_back(TemplateElement{currentRawQuasi, cookQuasi(currentRawQuasi)});
        currentRawQuasi.clear();

        i += 2;
        int braceCount = 1;
        std::string exprStr;
        while (i < content.length() && braceCount > 0) {
          if (content[i] == '{') braceCount++;
          else if (content[i] == '}') braceCount--;

          if (braceCount > 0) {
            exprStr += content[i];
          }
          i++;
        }

        Lexer exprLexer(exprStr);
        auto exprTokens = exprLexer.tokenize();
        Parser exprParser(exprTokens, isModule_);
        exprParser.setStrictMode(strictMode_);
        exprParser.allowedPrivateNames_ = allowedPrivateNames_;
        exprParser.inSingleStatementPosition_ = inSingleStatementPosition_;
        exprParser.superCallDisallowDepth_ = superCallDisallowDepth_;
        exprParser.loopDepth_ = loopDepth_;
        exprParser.switchDepth_ = switchDepth_;
        exprParser.functionDepth_ = functionDepth_;
        exprParser.asyncFunctionDepth_ = asyncFunctionDepth_;
        exprParser.generatorFunctionDepth_ = generatorFunctionDepth_;
        exprParser.awaitContextStack_ = awaitContextStack_;
        exprParser.yieldContextStack_ = yieldContextStack_;
        exprParser.allowIn_ = allowIn_;
        if (auto expr = exprParser.parseExpression()) {
          expressions.push_back(std::move(expr));
        }
      } else {
        currentRawQuasi += content[i];
        i++;
      }
    }
    quasis.push_back(TemplateElement{currentRawQuasi, cookQuasi(currentRawQuasi)});

    if (!inTaggedTemplate_) {
      for (const auto& q : quasis) {
        if (!q.cooked.has_value()) {
          error_ = true;
          return nullptr;
        }
      }
    }

    return makeExpr(TemplateLiteral{std::move(quasis), std::move(expressions)}, tok);
  }

  if (match(TokenType::Regex)) {
    const Token& tok = current();
    std::string value = tok.value;
    advance();
    size_t colonPos = value.find(':');
    size_t patLen = std::stoul(value.substr(0, colonPos));
    std::string pattern = value.substr(colonPos + 1, patLen);
    std::string flags = value.substr(colonPos + 1 + patLen);
    return makeExpr(RegexLiteral{pattern, flags}, tok);
  }

  if (match(TokenType::True)) {
    const Token& tok = current();
    if (tok.escaped) { error_ = true; return nullptr; }
    advance();
    return makeExpr(BoolLiteral{true}, tok);
  }

  if (match(TokenType::False)) {
    const Token& tok = current();
    if (tok.escaped) { error_ = true; return nullptr; }
    advance();
    return makeExpr(BoolLiteral{false}, tok);
  }

  if (match(TokenType::Null)) {
    const Token& tok = current();
    if (tok.escaped) { error_ = true; return nullptr; }
    advance();
    return makeExpr(NullLiteral{}, tok);
  }

  if (match(TokenType::Undefined)) {
    const Token& tok = current();
    advance();
    return makeExpr(Identifier{"undefined"}, tok);
  }

  if (match(TokenType::Identifier) || match(TokenType::Using)) {
    const Token& tok = current();
    std::string name = tok.value;
    if (strictMode_ && isStrictFutureReservedIdentifier(name)) {
      return nullptr;
    }
    advance();
    return makeExpr(Identifier{name}, tok);
  }

  if (match(TokenType::Let) && !strictMode_) {
    const Token& tok = current();
    advance();
    return makeExpr(Identifier{"let"}, tok);
  }

  if (match(TokenType::Get) || match(TokenType::Set) ||
      match(TokenType::From) || match(TokenType::As) ||
      match(TokenType::Of) || match(TokenType::Static)) {
    const Token& tok = current();
    advance();
    return makeExpr(Identifier{tok.value}, tok);
  }

  if (match(TokenType::Await) && canUseAwaitAsIdentifier()) {
    const Token& tok = current();
    std::string name = tok.value;
    advance();
    return makeExpr(Identifier{name}, tok);
  }

  if (match(TokenType::Yield) && canUseYieldAsIdentifier()) {
    const Token& tok = current();
    std::string name = tok.value;
    advance();
    return makeExpr(Identifier{name}, tok);
  }

  if (match(TokenType::Let) && !strictMode_) {
    const Token& tok = current();
    advance();
    return makeExpr(Identifier{"let"}, tok);
  }

  if (match(TokenType::Import)) {
    Token importTok = current();
    bool importEscaped = importTok.escaped;
    advance();
    if (importEscaped) {
      return nullptr;
    }
    if (match(TokenType::LeftParen)) {
      auto importId = std::make_unique<Expression>(Identifier{"import"});

      advance();
      if (match(TokenType::RightParen)) {
        return nullptr;
      }

      std::vector<ExprPtr> args;
      bool savedAllowIn = allowIn_;
      allowIn_ = true;
      auto specifier = parseAssignment();
      allowIn_ = savedAllowIn;
      if (!specifier) {
        return nullptr;
      }
      args.push_back(std::move(specifier));

      if (match(TokenType::Comma)) {
        advance();
        if (!match(TokenType::RightParen)) {
          savedAllowIn = allowIn_;
          allowIn_ = true;
          auto options = parseAssignment();
          allowIn_ = savedAllowIn;
          if (!options) {
            return nullptr;
          }
          args.push_back(std::move(options));
          if (match(TokenType::Comma)) {
            advance();
          }
        }
      }
      if (!expect(TokenType::RightParen)) {
        return nullptr;
      }
      return std::make_unique<Expression>(CallExpr{std::move(importId), std::move(args)});
    }
    if (match(TokenType::Dot)) {
      advance();
      if (match(TokenType::Identifier) && !current().escaped) {
        if (current().value == "meta") {
          if (!isModule_) {
            return nullptr;
          }
          advance();
          return makeExpr(MetaProperty{"meta", ""}, importTok);
        }
        if (current().value == "source" || current().value == "defer") {
          const bool isSourcePhase = (current().value == "source");
          advance();

          if (!match(TokenType::LeftParen)) {
            return nullptr;
          }
          advance();
          if (match(TokenType::RightParen)) {
            return nullptr;
          }

          auto importId = std::make_unique<Expression>(Identifier{"import"});
          std::vector<ExprPtr> args;
          bool savedAllowIn = allowIn_;
          allowIn_ = true;
          auto specifier = parseAssignment();
          allowIn_ = savedAllowIn;
          if (!specifier) {
            return nullptr;
          }
          args.push_back(std::move(specifier));
          args.push_back(std::make_unique<Expression>(StringLiteral{
            isSourcePhase ? kImportPhaseSourceSentinel : kImportPhaseDeferSentinel
          }));

          if (match(TokenType::Comma)) {
            return nullptr;
          }
          if (!expect(TokenType::RightParen)) {
            return nullptr;
          }
          return std::make_unique<Expression>(CallExpr{std::move(importId), std::move(args)});
        }
      }
    }
    return nullptr;
  }

  if (match(TokenType::Async)) {
    if (!current().escaped &&
        current().line == peek().line &&
        peek().type == TokenType::Function) {
      return parseFunctionExpression();
    }
    const Token& tok = current();
    advance();
    return makeExpr(Identifier{"async"}, tok);
  }

  if (match(TokenType::Function)) {
    return parseFunctionExpression();
  }

  if (match(TokenType::Class)) {
    return parseClassExpression();
  }
  if (match(TokenType::At)) {
    return parseClassExpression();
  }

  if (match(TokenType::New)) {
    return parseNewExpression();
  }

  if (match(TokenType::This)) {
    if (current().escaped) { error_ = true; return nullptr; }
    advance();
    return std::make_unique<Expression>(ThisExpr{});
  }

  if (match(TokenType::Super)) {
    if (newTargetDepth_ == 0 && staticBlockDepth_ == 0 && classBodyDepth_ == 0 && !isEvalContext_) {
      error_ = true;
      return nullptr;
    }
    advance();
    return std::make_unique<Expression>(SuperExpr{});
  }

  if (match(TokenType::LeftParen)) {
    advance();
    auto expr = parseAssignment();
    if (!expr) {
      return nullptr;
    }
    if (match(TokenType::Comma)) {
      std::vector<ExprPtr> sequence;
      sequence.push_back(std::move(expr));
      while (match(TokenType::Comma)) {
        advance();
        auto next = parseAssignment();
        if (!next) {
          return nullptr;
        }
        sequence.push_back(std::move(next));
      }
      expr = std::make_unique<Expression>(SequenceExpr{std::move(sequence)});
    }
    if (!expect(TokenType::RightParen)) {
      return nullptr;
    }
    if (expr) {
      expr->parenthesized = true;
    }
    return expr;
  }

  if (match(TokenType::LeftBracket)) {
    return parseArrayExpression();
  }

  if (match(TokenType::LeftBrace)) {
    return parseObjectExpression();
  }

  return nullptr;
}

}  // namespace lightjs
