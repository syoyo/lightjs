#pragma once

#include <string>
#include <string_view>
#include <cstdint>
#include <memory>

namespace lightjs {

enum class TokenType {
  EndOfFile,
  Number,
  BigInt,
  String,
  TemplateLiteral,
  Regex,
  Identifier,

  True,
  False,
  Null,
  Undefined,

  Let,
  Const,
  Var,
  Function,
  Async,
  Await,
  Yield,
  Return,
  If,
  Else,
  While,
  For,
  In,
  Instanceof,
  Of,
  Do,
  Switch,
  Case,
  Break,
  Continue,
  Try,
  Catch,
  Finally,
  Throw,
  New,
  This,
  Typeof,
  Void,
  Delete,
  Import,
  Export,
  From,
  As,
  Default,
  Class,
  Extends,
  Static,
  Super,
  Get,
  Set,

  Plus,
  Minus,
  Star,
  StarStar,  // ** exponentiation
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
  Amp,   // &
  Pipe,  // |
  Caret, // ^
  Tilde, // ~
  Bang,

  PlusEqual,
  MinusEqual,
  StarEqual,
  SlashEqual,
  AmpAmpEqual,      // &&=
  PipePipeEqual,    // ||=
  QuestionQuestionEqual,  // ??=

  PlusPlus,
  MinusMinus,

  Question,
  QuestionDot,  // Optional chaining ?.
  QuestionQuestion,  // Nullish coalescing ??
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
  DotDotDot,

  Arrow,

  Error,
};

struct Token {
  TokenType type;
  std::string value;
  std::shared_ptr<std::string> internedValue;  // For interned strings (identifiers)
  uint32_t line;
  uint32_t column;
  bool escaped;

  Token() : type(TokenType::Error), line(0), column(0), escaped(false) {}
  Token(TokenType t, std::string_view v, uint32_t l, uint32_t c)
    : type(t), value(v), line(l), column(c), escaped(false) {}
  Token(TokenType t, uint32_t l, uint32_t c)
    : type(t), line(l), column(c), escaped(false) {}

  // Constructor for interned strings
  Token(TokenType t, std::shared_ptr<std::string> interned, uint32_t l, uint32_t c)
    : type(t), internedValue(interned), value(*interned), line(l), column(c), escaped(false) {}

  // Get the string value (interned or regular)
  const std::string& getString() const {
    return internedValue ? *internedValue : value;
  }

  // Check if this token has an interned value
  bool isInterned() const {
    return internedValue != nullptr;
  }
};

}
