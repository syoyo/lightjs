#include "parser_internal.h"

namespace lightjs {

Parser::Parser(std::vector<Token> tokens, bool isModule)
  : tokens_(std::move(tokens)), isModule_(isModule), strictMode_(isModule) {}

const Token& Parser::current() const {
  if (pos_ >= tokens_.size()) {
    return tokens_.back();
  }
  return tokens_[pos_];
}

const Token& Parser::peek(size_t offset) const {
  if (pos_ + offset >= tokens_.size()) {
    return tokens_.back();
  }
  return tokens_[pos_ + offset];
}

const Token& Parser::advance() {
  if (pos_ < tokens_.size()) {
    return tokens_[pos_++];
  }
  return tokens_.back();
}

bool Parser::match(TokenType type) const {
  return current().type == type;
}

bool Parser::expect(TokenType type) {
  if (!match(type)) {
    return false;
  }
  advance();
  return true;
}

void Parser::consumeSemicolon() {
  if (match(TokenType::Semicolon)) {
    advance();
  }
}

bool Parser::consumeSemicolonOrASI() {
  if (match(TokenType::Semicolon)) {
    advance();
    return true;
  }

  if (match(TokenType::EndOfFile) || match(TokenType::RightBrace)) {
    return true;
  }

  if (pos_ > 0) {
    const Token& previous = tokens_[pos_ - 1];
    if (current().line > previous.line) {
      return true;
    }
  }

  return false;
}

bool Parser::parseImportAttributes(std::vector<ImportAttribute>& attributes) {
  bool isAssertKeyword =
      match(TokenType::Identifier) &&
      !current().escaped &&
      current().value == "assert" &&
      current().line == peek().line;
  if (!match(TokenType::With) && !isAssertKeyword) {
    return true;
  }

  advance();
  if (!expect(TokenType::LeftBrace)) {
    return false;
  }

  std::unordered_set<std::string> seenKeys;
  while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
    if (!attributes.empty()) {
      if (!expect(TokenType::Comma)) {
        return false;
      }
      if (match(TokenType::RightBrace)) {
        break;
      }
    }

    std::string key;
    if (isIdentifierNameToken(current().type)) {
      key = current().value;
      advance();
    } else if (match(TokenType::String)) {
      if (!isWellFormedUnicodeString(current().value)) {
        error_ = true;
        return false;
      }
      key = current().value;
      advance();
    } else {
      error_ = true;
      return false;
    }

    if (!seenKeys.insert(key).second) {
      error_ = true;
      return false;
    }

    if (!expect(TokenType::Colon)) {
      return false;
    }
    if (!match(TokenType::String)) {
      error_ = true;
      return false;
    }
    if (!isWellFormedUnicodeString(current().value)) {
      error_ = true;
      return false;
    }

    attributes.push_back(ImportAttribute{key, current().value});
    advance();
  }

  return expect(TokenType::RightBrace);
}

bool Parser::canUseAwaitAsIdentifier() const {
  if (!awaitContextStack_.empty()) {
    return !awaitContextStack_.back();
  }
  return !isModule_;
}

bool Parser::canParseAwaitExpression() const {
  if (!awaitContextStack_.empty()) {
    return awaitContextStack_.back();
  }
  return isModule_ && functionDepth_ == 0;
}

bool Parser::canUseYieldAsIdentifier() const {
  if (!yieldContextStack_.empty()) {
    return !yieldContextStack_.back() && !strictMode_;
  }
  return !strictMode_;
}

bool Parser::isIdentifierLikeToken(TokenType type) const {
  if (type == TokenType::Identifier) {
    return true;
  }
  if (type == TokenType::Using) {
    return true;
  }
  if (type == TokenType::Await) {
    return canUseAwaitAsIdentifier();
  }
  if (type == TokenType::Yield) {
    return canUseYieldAsIdentifier();
  }
  // In non-strict mode, 'let' can be used as an identifier
  if (type == TokenType::Let && !strictMode_) {
    return true;
  }
  // 'async' is a contextual keyword, not a reserved word - can be used as identifier
  if (type == TokenType::Async) {
    return true;
  }
  // 'get', 'set', 'from', 'as', 'of', 'static' are contextual keywords
  if (type == TokenType::Get || type == TokenType::Set ||
      type == TokenType::From || type == TokenType::As ||
      type == TokenType::Of || type == TokenType::Static) {
    return true;
  }
  return false;
}

bool Parser::isUsingDeclarationStart(bool allowForHead) const {
  if (!match(TokenType::Using) || current().escaped) {
    return false;
  }
  if (peek().type == TokenType::Of && peek(2).type == TokenType::Of) {
    return false;
  }
  if (peek().line != current().line) {
    return false;
  }
  if (!isIdentifierLikeToken(peek().type)) {
    return false;
  }
  return allowForHead || usingDeclarationAllowedInCurrentContext(false);
}

bool Parser::isAwaitUsingDeclarationStart(bool allowForHead) const {
  if (!match(TokenType::Await) || current().escaped || !canParseAwaitExpression()) {
    return false;
  }
  if (peek().type != TokenType::Using || peek().escaped) {
    return false;
  }
  if (peek().line != current().line || peek(2).line != peek().line) {
    return false;
  }
  if (!isIdentifierLikeToken(peek(2).type)) {
    return false;
  }
  return allowForHead || usingDeclarationAllowedInCurrentContext(false);
}

bool Parser::usingDeclarationAllowedInCurrentContext(bool inForHead) const {
  if (inForHead) {
    return true;
  }
  if (inSingleStatementPosition_) {
    return false;
  }
  if (switchCaseDepth_ > 0) {
    return false;
  }
  if (isModule_) {
    return true;
  }
  return blockDepth_ > 0 || functionDepth_ > 0 || staticBlockDepth_ > 0 || classBodyDepth_ > 0;
}

std::optional<Program> Parser::parse() {
  Program program;
  program.isModule = isModule_;
  bool inDirectivePrologue = true;
  bool prologueHadLegacyEscape = false;
  while (!match(TokenType::EndOfFile)) {
    if (auto stmt = parseStatement(true)) {
      if (inDirectivePrologue) {
        bool isDirectiveString = false;
        if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt->node)) {
          if (exprStmt->expression) {
            if (auto* str = std::get_if<StringLiteral>(&exprStmt->expression->node)) {
              isDirectiveString = true;
              if (str->hasLegacyEscape) {
                prologueHadLegacyEscape = true;
              }
              if (str->value == "use strict" && !str->hasEscape) {
                strictMode_ = true;
                if (prologueHadLegacyEscape) {
                  error_ = true;
                  return std::nullopt;
                }
              }
            }
          }
        }
        if (!isDirectiveString) {
          inDirectivePrologue = false;
        }
      }
      program.body.push_back(std::move(stmt));
    } else {
      return std::nullopt;
    }
  }

  // Early errors:
  // 1. No duplicate lexically declared names in ScriptBody/ModuleBody.
  // 2. No overlap between lexical and var-declared names.
  {
    std::vector<std::string> lexicalNames;
    if (!collectStatementListLexicalNames(program.body, lexicalNames, /*includeFunctionDeclarations*/ false)) {
      return std::nullopt;
    }
    std::vector<std::string> varNames;
    collectStatementListVarNames(program.body, varNames, /*includeFunctionDeclarations*/ true);
    if (hasNameCollision(lexicalNames, varNames)) {
      return std::nullopt;
    }
  }
  {
    for (const auto& stmt : program.body) {
      if (stmt && statementHasUndeclaredPrivateName(*stmt, allowedPrivateNames_)) {
        return std::nullopt;
      }
    }
  }

  return program;
}


}  // namespace lightjs
