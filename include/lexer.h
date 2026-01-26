#pragma once

#include "token.h"
#include <vector>
#include <string_view>
#include <optional>

namespace lightjs {

class Lexer {
public:
  explicit Lexer(std::string_view source);

  std::vector<Token> tokenize();

private:
  std::string_view source_;
  size_t pos_ = 0;
  uint32_t line_ = 1;
  uint32_t column_ = 1;

  char current() const;
  char peek(size_t offset = 1) const;
  char advance();
  bool isAtEnd() const;

  void skipWhitespace();
  void skipLineComment();
  void skipBlockComment();

  std::optional<Token> readNumber();
  std::optional<Token> readString(char quote);
  std::optional<Token> readTemplateLiteral();
  std::optional<Token> readIdentifier();
  std::optional<Token> readRegex();
  std::optional<Token> readOperator();

  static bool isDigit(char c);
  static bool isAlpha(char c);
  static bool isAlphaNumeric(char c);
  static bool expectsRegex(TokenType type);
};

}