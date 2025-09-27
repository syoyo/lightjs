#pragma once

#include <string>
#include <string_view>
#include <cstdint>

namespace tinyjs {

enum class TokenType {
  EndOfFile,
  Number,
  String,
  Identifier,

  True,
  False,
  Null,
  Undefined,

  Let,
  Const,
  Var,
  Function,
  Return,
  If,
  Else,
  While,
  For,
  Break,
  Continue,
  Try,
  Catch,
  Finally,
  Throw,
  New,
  This,
  Typeof,

  Plus,
  Minus,
  Star,
  Slash,
  Percent,

  Equal,
  EqualEqual,
  EqualEqualEqual,
  BangEqual,
  BangEqualEqual,
  Less,
  Greater,
  LessEqual,
  GreaterEqual,

  AmpAmp,
  PipePipe,
  Bang,

  PlusEqual,
  MinusEqual,
  StarEqual,
  SlashEqual,

  PlusPlus,
  MinusMinus,

  Question,
  Colon,

  LeftParen,
  RightParen,
  LeftBrace,
  RightBrace,
  LeftBracket,
  RightBracket,

  Semicolon,
  Comma,
  Dot,

  Arrow,

  Error,
};

struct Token {
  TokenType type;
  std::string value;
  uint32_t line;
  uint32_t column;

  Token() : type(TokenType::Error), line(0), column(0) {}
  Token(TokenType t, std::string_view v, uint32_t l, uint32_t c)
    : type(t), value(v), line(l), column(c) {}
  Token(TokenType t, uint32_t l, uint32_t c)
    : type(t), line(l), column(c) {}
};

}