#include "lexer.h"
#include "string_table.h"
#include <unordered_map>
#include <cctype>
#include <stdexcept>

namespace lightjs {

namespace {
int hexDigitValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

void appendUtf8(std::string& out, uint32_t codepoint) {
  if (codepoint <= 0x7F) {
    out.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0x10FFFF) {
    out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    throw std::runtime_error("Invalid unicode escape");
  }
}
}  // namespace

static const std::unordered_map<std::string_view, TokenType> keywords = {
  {"let", TokenType::Let},
  {"const", TokenType::Const},
  {"var", TokenType::Var},
  {"function", TokenType::Function},
  {"async", TokenType::Async},
  {"await", TokenType::Await},
  {"yield", TokenType::Yield},
  {"return", TokenType::Return},
  {"if", TokenType::If},
  {"else", TokenType::Else},
  {"while", TokenType::While},
  {"for", TokenType::For},
  {"with", TokenType::With},
  {"in", TokenType::In},
  {"instanceof", TokenType::Instanceof},
  {"of", TokenType::Of},
  {"do", TokenType::Do},
  {"switch", TokenType::Switch},
  {"case", TokenType::Case},
  {"break", TokenType::Break},
  {"continue", TokenType::Continue},
  {"try", TokenType::Try},
  {"catch", TokenType::Catch},
  {"finally", TokenType::Finally},
  {"throw", TokenType::Throw},
  {"new", TokenType::New},
  {"this", TokenType::This},
  {"typeof", TokenType::Typeof},
  {"void", TokenType::Void},
  {"delete", TokenType::Delete},
  {"import", TokenType::Import},
  {"export", TokenType::Export},
  {"from", TokenType::From},
  {"as", TokenType::As},
  {"default", TokenType::Default},
  {"class", TokenType::Class},
  {"extends", TokenType::Extends},
  {"static", TokenType::Static},
  {"super", TokenType::Super},
  {"get", TokenType::Get},
  {"set", TokenType::Set},
  {"true", TokenType::True},
  {"false", TokenType::False},
  {"null", TokenType::Null},
  {"undefined", TokenType::Undefined},
};

Lexer::Lexer(std::string_view source) : source_(source) {}

char Lexer::current() const {
  if (isAtEnd()) return '\0';
  return source_[pos_];
}

char Lexer::peek(size_t offset) const {
  if (pos_ + offset >= source_.size()) return '\0';
  return source_[pos_ + offset];
}

char Lexer::advance() {
  if (isAtEnd()) return '\0';
  char c = source_[pos_++];
  if (c == '\n') {
    line_++;
    column_ = 1;
  } else {
    column_++;
  }
  return c;
}

bool Lexer::isAtEnd() const {
  return pos_ >= source_.size();
}

void Lexer::skipWhitespace() {
  while (!isAtEnd()) {
    unsigned char c = static_cast<unsigned char>(current());
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') {
      advance();
    } else if (c == 0xC2 && static_cast<unsigned char>(peek()) == 0xA0) {
      // U+00A0 NO-BREAK SPACE
      advance();
      advance();
    } else if (c == 0xE2 && static_cast<unsigned char>(peek()) == 0x80) {
      unsigned char third = static_cast<unsigned char>(peek(2));
      if (third == 0xA8 || third == 0xA9) {
        // U+2028 LINE SEPARATOR or U+2029 PARAGRAPH SEPARATOR
        advance();
        advance();
        advance();
      } else {
        break;
      }
    } else if (c == 0xEF && static_cast<unsigned char>(peek()) == 0xBB &&
               static_cast<unsigned char>(peek(2)) == 0xBF) {
      // U+FEFF ZERO WIDTH NO-BREAK SPACE (BOM)
      advance();
      advance();
      advance();
    } else {
      break;
    }
  }
}

void Lexer::skipLineComment() {
  while (!isAtEnd() && current() != '\n') {
    advance();
  }
}

void Lexer::skipBlockComment() {
  advance();
  advance();
  while (!isAtEnd()) {
    if (current() == '*' && peek() == '/') {
      advance();
      advance();
      break;
    }
    advance();
  }
}

bool Lexer::isDigit(char c) {
  return c >= '0' && c <= '9';
}

bool Lexer::isAlpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}

bool Lexer::isAlphaNumeric(char c) {
  return isAlpha(c) || isDigit(c);
}

bool Lexer::isIdentifierStart(unsigned char c) {
  return isAlpha(static_cast<char>(c)) || c >= 0x80;
}

bool Lexer::isIdentifierPart(unsigned char c) {
  return isAlphaNumeric(static_cast<char>(c)) || c >= 0x80;
}

std::optional<Token> Lexer::readNumber() {
  uint32_t startLine = line_;
  uint32_t startColumn = column_;
  size_t start = pos_;
  auto throwSyntaxError = [](const std::string& message) {
    throw std::runtime_error("SyntaxError: " + message);
  };

  auto digitForBase = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
  };

  auto consumeExponent = [&]() {
    if (isAtEnd() || (current() != 'e' && current() != 'E')) return;
    advance();
    if (!isAtEnd() && (current() == '+' || current() == '-')) {
      advance();
    }
    if (isAtEnd() || !isDigit(current())) {
      throwSyntaxError("Invalid exponent in numeric literal");
    }
    while (!isAtEnd() && isDigit(current())) {
      advance();
    }
  };

  // Leading-dot decimal number: .123, .123e1
  if (!isAtEnd() && current() == '.') {
    advance();
    if (isAtEnd() || !isDigit(current())) {
      throwSyntaxError("Invalid numeric literal");
    }
    while (!isAtEnd() && isDigit(current())) {
      advance();
    }
    consumeExponent();
    if (!isAtEnd() && current() == 'n') {
      throwSyntaxError("BigInt literal cannot have a decimal point");
    }
    return Token(TokenType::Number, source_.substr(start, pos_ - start), startLine, startColumn);
  }

  // Non-decimal literal: 0x..., 0o..., 0b...
  if (!isAtEnd() && current() == '0' &&
      !isAtEnd() && (peek() == 'x' || peek() == 'X' || peek() == 'o' || peek() == 'O' || peek() == 'b' || peek() == 'B')) {
    char prefix = peek();
    int base = 10;
    if (prefix == 'x' || prefix == 'X') base = 16;
    if (prefix == 'o' || prefix == 'O') base = 8;
    if (prefix == 'b' || prefix == 'B') base = 2;

    advance();  // 0
    advance();  // x/o/b

    bool sawDigit = false;
    bool prevSeparator = false;

    while (!isAtEnd()) {
      char c = current();
      if (c == 'n') {
        break;
      }
      if (c == '_') {
        if (!sawDigit || prevSeparator) {
          throwSyntaxError("Invalid numeric separator in literal");
        }
        prevSeparator = true;
        advance();
        continue;
      }

      int d = digitForBase(c);
      if (d >= 0) {
        if (d >= base) {
          throwSyntaxError("Invalid digit in numeric literal");
        }
        sawDigit = true;
        prevSeparator = false;
        advance();
        continue;
      }

      if (std::isalnum(static_cast<unsigned char>(c))) {
        throwSyntaxError("Invalid digit in numeric literal");
      }
      break;
    }

    if (!sawDigit || prevSeparator) {
      throwSyntaxError("Invalid numeric literal");
    }

    if (!isAtEnd() && current() == 'n') {
      advance();
      return Token(TokenType::BigInt, source_.substr(start, pos_ - start - 1), startLine, startColumn);
    }

    return Token(TokenType::Number, source_.substr(start, pos_ - start), startLine, startColumn);
  }

  // Decimal literal with optional separators.
  bool prevSeparator = false;
  while (!isAtEnd()) {
    char c = current();
    if (isDigit(c)) {
      prevSeparator = false;
      advance();
      continue;
    }
    if (c == '_') {
      if (pos_ == start || prevSeparator || !isDigit(peek())) {
        throwSyntaxError("Invalid numeric separator in literal");
      }
      prevSeparator = true;
      advance();
      continue;
    }
    break;
  }
  if (prevSeparator) {
    throwSyntaxError("Invalid numeric separator in literal");
  }

  bool hasDot = false;
  bool hasExponent = false;

  if (!isAtEnd() && current() == '.' && isDigit(peek())) {
    hasDot = true;
    advance();
    while (!isAtEnd() && isDigit(current())) {
      advance();
    }
  }

  if (!isAtEnd() && (current() == 'e' || current() == 'E')) {
    hasExponent = true;
    consumeExponent();
  }

  if (!isAtEnd() && current() == 'n') {
    if (hasDot || hasExponent) {
      throwSyntaxError("Invalid BigInt literal");
    }

    std::string raw(source_.substr(start, pos_ - start));
    std::string normalized;
    normalized.reserve(raw.size());
    for (char c : raw) {
      if (c != '_') normalized.push_back(c);
    }
    if (normalized.size() > 1 && normalized[0] == '0') {
      throwSyntaxError("Legacy octal-like BigInt literal is not allowed");
    }

    advance();
    return Token(TokenType::BigInt, raw, startLine, startColumn);
  }

  return Token(TokenType::Number, source_.substr(start, pos_ - start), startLine, startColumn);
}

std::optional<Token> Lexer::readString(char quote) {
  uint32_t startLine = line_;
  uint32_t startColumn = column_;
  advance();

  std::string str;
  while (!isAtEnd() && current() != quote) {
    if (current() == '\\') {
      advance();
      if (!isAtEnd()) {
        char c = current();
        switch (c) {
          case 'n': str += '\n'; break;
          case 't': str += '\t'; break;
          case 'r': str += '\r'; break;
          case '\\': str += '\\'; break;
          case '"': str += '"'; break;
          case '\'': str += '\''; break;
          case 'u': {
            advance();  // consume 'u'
            uint32_t codepoint = 0;
            if (!isAtEnd() && current() == '{') {
              advance();  // consume '{'
              bool sawDigit = false;
              while (!isAtEnd() && current() != '}') {
                int d = hexDigitValue(current());
                if (d < 0) {
                  throw std::runtime_error("Invalid unicode escape in string");
                }
                sawDigit = true;
                codepoint = (codepoint << 4) + static_cast<uint32_t>(d);
                advance();
              }
              if (!sawDigit || isAtEnd() || current() != '}') {
                throw std::runtime_error("Invalid unicode escape in string");
              }
              advance();  // consume '}'
            } else {
              for (int i = 0; i < 4; ++i) {
                if (isAtEnd()) {
                  throw std::runtime_error("Invalid unicode escape in string");
                }
                int d = hexDigitValue(current());
                if (d < 0) {
                  throw std::runtime_error("Invalid unicode escape in string");
                }
                codepoint = (codepoint << 4) + static_cast<uint32_t>(d);
                advance();
              }
            }
            // Handle surrogate pairs: high surrogate followed by \uXXXX low surrogate
            if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
              // Check for low surrogate \uXXXX
              if (!isAtEnd() && current() == '\\' && pos_ + 1 < source_.size() && source_[pos_ + 1] == 'u') {
                size_t savedPos = pos_;
                advance(); // consume '\'
                advance(); // consume 'u'
                uint32_t low = 0;
                bool validLow = true;
                for (int i = 0; i < 4; ++i) {
                  if (isAtEnd()) { validLow = false; break; }
                  int d = hexDigitValue(current());
                  if (d < 0) { validLow = false; break; }
                  low = (low << 4) + static_cast<uint32_t>(d);
                  advance();
                }
                if (validLow && low >= 0xDC00 && low <= 0xDFFF) {
                  // Combine surrogate pair
                  codepoint = 0x10000 + (codepoint - 0xD800) * 0x400 + (low - 0xDC00);
                } else {
                  // Not a valid low surrogate, rewind
                  pos_ = savedPos;
                }
              }
            }
            appendUtf8(str, codepoint);
            continue;
          }
          default: str += c; break;
        }
        advance();
      }
    } else {
      str += current();
      advance();
    }
  }

  if (!isAtEnd()) {
    advance();
  }

  // Intern string literals for memory efficiency (especially for object keys)
  // Only intern small strings (< 256 chars) to avoid memory bloat
  if (str.length() < 256) {
    auto internedStr = StringTable::instance().intern(str);
    return Token(TokenType::String, internedStr, startLine, startColumn);
  }

  return Token(TokenType::String, str, startLine, startColumn);
}

std::optional<Token> Lexer::readTemplateLiteral() {
  uint32_t startLine = line_;
  uint32_t startColumn = column_;
  advance(); // skip opening backtick

  // For now, treat the entire template as a string with ${} markers
  // The parser will need to parse the interpolation expressions
  std::string content;
  while (!isAtEnd() && current() != '`') {
    if (current() == '\\') {
      advance();
      if (!isAtEnd()) {
        char c = current();
        switch (c) {
          case 'n': content += '\n'; break;
          case 't': content += '\t'; break;
          case 'r': content += '\r'; break;
          case '\\': content += '\\'; break;
          case '`': content += '`'; break;
          case '$': content += '$'; break;
          default: content += c; break;
        }
        advance();
      }
    } else {
      content += current();
      advance();
    }
  }

  if (!isAtEnd()) {
    advance(); // skip closing backtick
  }

  return Token(TokenType::TemplateLiteral, content, startLine, startColumn);
}

std::optional<Token> Lexer::readIdentifier() {
  uint32_t startLine = line_;
  uint32_t startColumn = column_;

  std::string ident;
  bool first = true;
  bool hadEscape = false;

  while (!isAtEnd()) {
    unsigned char c = static_cast<unsigned char>(current());
    if (isIdentifierPart(c)) {
      // Check for Unicode whitespace sequences that should NOT be identifier parts
      if (c == 0xC2 && static_cast<unsigned char>(peek()) == 0xA0) {
        break;  // U+00A0 NO-BREAK SPACE
      }
      if (c == 0xE2 && static_cast<unsigned char>(peek()) == 0x80) {
        unsigned char third = static_cast<unsigned char>(peek(2));
        if (third == 0xA8 || third == 0xA9) {
          break;  // U+2028 LINE SEPARATOR or U+2029 PARAGRAPH SEPARATOR
        }
      }
      if (c == 0xEF && static_cast<unsigned char>(peek()) == 0xBB &&
          static_cast<unsigned char>(peek(2)) == 0xBF) {
        break;  // U+FEFF BOM
      }
      ident.push_back(current());
      advance();
      first = false;
      continue;
    }

    if (current() == '\\' && peek() == 'u') {
      hadEscape = true;
      advance();  // '\'
      advance();  // 'u'

      uint32_t codepoint = 0;
      if (current() == '{') {
        advance();  // '{'
        bool sawDigit = false;
        while (!isAtEnd() && current() != '}') {
          int d = hexDigitValue(current());
          if (d < 0) {
            throw std::runtime_error("Invalid unicode escape in identifier");
          }
          sawDigit = true;
          codepoint = (codepoint << 4) + static_cast<uint32_t>(d);
          advance();
        }
        if (!sawDigit || isAtEnd() || current() != '}') {
          throw std::runtime_error("Invalid unicode escape in identifier");
        }
        advance();  // '}'
      } else {
        for (int i = 0; i < 4; i++) {
          if (isAtEnd()) {
            throw std::runtime_error("Invalid unicode escape in identifier");
          }
          int d = hexDigitValue(current());
          if (d < 0) {
            throw std::runtime_error("Invalid unicode escape in identifier");
          }
          codepoint = (codepoint << 4) + static_cast<uint32_t>(d);
          advance();
        }
      }

      bool validStart = codepoint > 0x7f || isAlpha(static_cast<char>(codepoint));
      bool validPart = codepoint > 0x7f || isAlphaNumeric(static_cast<char>(codepoint));
      if ((first && !validStart) || (!first && !validPart)) {
        throw std::runtime_error("Invalid identifier escape");
      }
      appendUtf8(ident, codepoint);
      first = false;
      continue;
    }

    break;
  }

  std::string_view identView(ident);

  auto it = keywords.find(identView);
  if (it != keywords.end()) {
    // Keywords: intern for memory efficiency
    auto internedKeyword = StringTable::instance().intern(identView);
    Token tok(it->second, internedKeyword, startLine, startColumn);
    tok.escaped = hadEscape;
    return tok;
  }

  // Regular identifiers: intern for property name deduplication
  auto internedIdent = StringTable::instance().intern(identView);
  Token tok(TokenType::Identifier, internedIdent, startLine, startColumn);
  tok.escaped = hadEscape;
  return tok;
}

std::optional<Token> Lexer::readRegex() {
  uint32_t startLine = line_;
  uint32_t startColumn = column_;
  advance();

  std::string pattern;
  while (!isAtEnd() && current() != '/') {
    if (current() == '\\') {
      pattern += current();
      advance();
      if (!isAtEnd()) {
        pattern += current();
        advance();
      }
    } else if (current() == '\n') {
      return std::nullopt;
    } else {
      pattern += current();
      advance();
    }
  }

  if (isAtEnd()) {
    return std::nullopt;
  }

  advance();

  std::string flags;
  while (!isAtEnd() && isAlpha(current())) {
    flags += current();
    advance();
  }

  std::string value = pattern + "||" + flags;
  return Token(TokenType::Regex, value, startLine, startColumn);
}

bool Lexer::expectsRegex(TokenType type) {
  return type == TokenType::Equal ||
         type == TokenType::LeftParen ||
         type == TokenType::LeftBracket ||
         type == TokenType::Comma ||
         type == TokenType::Semicolon ||
         type == TokenType::Await ||
         type == TokenType::Yield ||
         type == TokenType::Return ||
         type == TokenType::Colon ||
         type == TokenType::Question ||
         type == TokenType::AmpAmp ||
         type == TokenType::PipePipe ||
         type == TokenType::Bang ||
         type == TokenType::EqualEqual ||
         type == TokenType::EqualEqualEqual ||
         type == TokenType::BangEqual ||
         type == TokenType::BangEqualEqual ||
         type == TokenType::Less ||
         type == TokenType::Greater ||
         type == TokenType::LessEqual ||
         type == TokenType::GreaterEqual ||
         type == TokenType::Plus ||
         type == TokenType::Minus ||
         type == TokenType::Tilde ||
         type == TokenType::Star ||
         type == TokenType::Slash ||
         type == TokenType::Percent ||
         type == TokenType::Amp ||
         type == TokenType::Pipe ||
         type == TokenType::Caret ||
         type == TokenType::LeftShift ||
         type == TokenType::RightShift ||
         type == TokenType::UnsignedRightShift ||
         type == TokenType::Void ||
         type == TokenType::LeftBrace;
}

std::vector<Token> Lexer::tokenize() {
  std::vector<Token> tokens;

  while (!isAtEnd()) {
    skipWhitespace();
    if (isAtEnd()) break;

    uint32_t startLine = line_;
    uint32_t startColumn = column_;
    char c = current();

    if (c == '/' && peek() == '/') {
      skipLineComment();
      continue;
    }

    if (c == '/' && peek() == '*') {
      skipBlockComment();
      continue;
    }

    if (isDigit(c)) {
      if (auto token = readNumber()) {
        tokens.push_back(*token);
      }
      continue;
    }

    if (c == '.' && isDigit(peek())) {
      if (auto token = readNumber()) {
        tokens.push_back(*token);
      }
      continue;
    }

    if (c == '"' || c == '\'') {
      if (auto token = readString(c)) {
        tokens.push_back(*token);
      }
      continue;
    }

    if (c == '`') {
      if (auto token = readTemplateLiteral()) {
        tokens.push_back(*token);
      }
      continue;
    }

    if (isIdentifierStart(static_cast<unsigned char>(c)) || (c == '\\' && peek() == 'u')) {
      if (!tokens.empty() &&
          (tokens.back().type == TokenType::Number || tokens.back().type == TokenType::BigInt) &&
          tokens.back().line == startLine) {
        // Only error if the identifier immediately follows the number (no whitespace)
        auto& prevToken = tokens.back();
        size_t numEnd = prevToken.column + prevToken.value.size();
        if (prevToken.type == TokenType::BigInt) numEnd++; // BigInt has 'n' suffix not in value
        if (startColumn <= numEnd) {
          throw std::runtime_error("Invalid numeric literal");
        }
      }
      if (auto token = readIdentifier()) {
        tokens.push_back(*token);
      }
      continue;
    }

    switch (c) {
      case '(':
        tokens.emplace_back(TokenType::LeftParen, startLine, startColumn);
        advance();
        break;
      case ')':
        tokens.emplace_back(TokenType::RightParen, startLine, startColumn);
        advance();
        break;
      case '{':
        tokens.emplace_back(TokenType::LeftBrace, startLine, startColumn);
        advance();
        break;
      case '}':
        tokens.emplace_back(TokenType::RightBrace, startLine, startColumn);
        advance();
        break;
      case '[':
        tokens.emplace_back(TokenType::LeftBracket, startLine, startColumn);
        advance();
        break;
      case ']':
        tokens.emplace_back(TokenType::RightBracket, startLine, startColumn);
        advance();
        break;
      case ';':
        tokens.emplace_back(TokenType::Semicolon, startLine, startColumn);
        advance();
        break;
      case ',':
        tokens.emplace_back(TokenType::Comma, startLine, startColumn);
        advance();
        break;
      case '.':
        // Check for ... (spread/rest operator)
        if (peek(1) == '.' && peek(2) == '.') {
          tokens.emplace_back(TokenType::DotDotDot, startLine, startColumn);
          advance(); // first .
          advance(); // second .
          advance(); // third .
        } else {
          tokens.emplace_back(TokenType::Dot, startLine, startColumn);
          advance();
        }
        break;
      case '?':
        if (peek() == '?') {
          if (peek(2) == '=') {
            tokens.emplace_back(TokenType::QuestionQuestionEqual, startLine, startColumn);
            advance();
            advance();
            advance();
          } else {
            tokens.emplace_back(TokenType::QuestionQuestion, startLine, startColumn);
            advance();
            advance();
          }
        } else if (peek() == '.' && !isDigit(peek(2))) {
          tokens.emplace_back(TokenType::QuestionDot, startLine, startColumn);
          advance();
          advance();
        } else {
          tokens.emplace_back(TokenType::Question, startLine, startColumn);
          advance();
        }
        break;
      case ':':
        tokens.emplace_back(TokenType::Colon, startLine, startColumn);
        advance();
        break;
      case '+':
        if (peek() == '+') {
          tokens.emplace_back(TokenType::PlusPlus, startLine, startColumn);
          advance();
          advance();
        } else if (peek() == '=') {
          tokens.emplace_back(TokenType::PlusEqual, startLine, startColumn);
          advance();
          advance();
        } else {
          tokens.emplace_back(TokenType::Plus, startLine, startColumn);
          advance();
        }
        break;
      case '-':
        if (peek() == '-') {
          tokens.emplace_back(TokenType::MinusMinus, startLine, startColumn);
          advance();
          advance();
        } else if (peek() == '=') {
          tokens.emplace_back(TokenType::MinusEqual, startLine, startColumn);
          advance();
          advance();
        } else {
          tokens.emplace_back(TokenType::Minus, startLine, startColumn);
          advance();
        }
        break;
      case '*':
        if (peek() == '*') {
          if (peek(2) == '=') {
            tokens.emplace_back(TokenType::StarStarEqual, startLine, startColumn);
            advance(); advance(); advance();
          } else {
            tokens.emplace_back(TokenType::StarStar, startLine, startColumn);
            advance(); advance();
          }
        } else if (peek() == '=') {
          tokens.emplace_back(TokenType::StarEqual, startLine, startColumn);
          advance(); advance();
        } else {
          tokens.emplace_back(TokenType::Star, startLine, startColumn);
          advance();
        }
        break;
      case '/':
        if (peek() == '=') {
          tokens.emplace_back(TokenType::SlashEqual, startLine, startColumn);
          advance();
          advance();
        } else {
          bool canBeRegex = tokens.empty() || expectsRegex(tokens.back().type);
          if (canBeRegex) {
            if (auto token = readRegex()) {
              tokens.push_back(*token);
              break;
            }
          }
          tokens.emplace_back(TokenType::Slash, startLine, startColumn);
          advance();
        }
        break;
      case '%':
        if (peek() == '=') {
          tokens.emplace_back(TokenType::PercentEqual, startLine, startColumn);
          advance(); advance();
        } else {
          tokens.emplace_back(TokenType::Percent, startLine, startColumn);
          advance();
        }
        break;
      case '=':
        if (peek() == '=') {
          if (peek(2) == '=') {
            tokens.emplace_back(TokenType::EqualEqualEqual, startLine, startColumn);
            advance();
            advance();
            advance();
          } else {
            tokens.emplace_back(TokenType::EqualEqual, startLine, startColumn);
            advance();
            advance();
          }
        } else if (peek() == '>') {
          tokens.emplace_back(TokenType::Arrow, startLine, startColumn);
          advance();
          advance();
        } else {
          tokens.emplace_back(TokenType::Equal, startLine, startColumn);
          advance();
        }
        break;
      case '!':
        if (peek() == '=') {
          if (peek(2) == '=') {
            tokens.emplace_back(TokenType::BangEqualEqual, startLine, startColumn);
            advance();
            advance();
            advance();
          } else {
            tokens.emplace_back(TokenType::BangEqual, startLine, startColumn);
            advance();
            advance();
          }
        } else {
          tokens.emplace_back(TokenType::Bang, startLine, startColumn);
          advance();
        }
        break;
      case '<':
        if (peek() == '<') {
          if (peek(2) == '=') {
            tokens.emplace_back(TokenType::LeftShiftEqual, startLine, startColumn);
            advance(); advance(); advance();
          } else {
            tokens.emplace_back(TokenType::LeftShift, startLine, startColumn);
            advance(); advance();
          }
        } else if (peek() == '=') {
          tokens.emplace_back(TokenType::LessEqual, startLine, startColumn);
          advance(); advance();
        } else {
          tokens.emplace_back(TokenType::Less, startLine, startColumn);
          advance();
        }
        break;
      case '>':
        if (peek() == '>') {
          if (peek(2) == '>') {
            if (peek(3) == '=') {
              tokens.emplace_back(TokenType::UnsignedRightShiftEqual, startLine, startColumn);
              advance(); advance(); advance(); advance();
            } else {
              tokens.emplace_back(TokenType::UnsignedRightShift, startLine, startColumn);
              advance(); advance(); advance();
            }
          } else if (peek(2) == '=') {
            tokens.emplace_back(TokenType::RightShiftEqual, startLine, startColumn);
            advance(); advance(); advance();
          } else {
            tokens.emplace_back(TokenType::RightShift, startLine, startColumn);
            advance(); advance();
          }
        } else if (peek() == '=') {
          tokens.emplace_back(TokenType::GreaterEqual, startLine, startColumn);
          advance(); advance();
        } else {
          tokens.emplace_back(TokenType::Greater, startLine, startColumn);
          advance();
        }
        break;
      case '&':
        if (peek() == '&') {
          if (peek(2) == '=') {
            tokens.emplace_back(TokenType::AmpAmpEqual, startLine, startColumn);
            advance(); advance(); advance();
          } else {
            tokens.emplace_back(TokenType::AmpAmp, startLine, startColumn);
            advance(); advance();
          }
        } else if (peek() == '=') {
          tokens.emplace_back(TokenType::AmpEqual, startLine, startColumn);
          advance(); advance();
        } else {
          tokens.emplace_back(TokenType::Amp, startLine, startColumn);
          advance();
        }
        break;
      case '|':
        if (peek() == '|') {
          if (peek(2) == '=') {
            tokens.emplace_back(TokenType::PipePipeEqual, startLine, startColumn);
            advance(); advance(); advance();
          } else {
            tokens.emplace_back(TokenType::PipePipe, startLine, startColumn);
            advance(); advance();
          }
        } else if (peek() == '=') {
          tokens.emplace_back(TokenType::PipeEqual, startLine, startColumn);
          advance(); advance();
        } else {
          tokens.emplace_back(TokenType::Pipe, startLine, startColumn);
          advance();
        }
        break;
      case '^':
        if (peek() == '=') {
          tokens.emplace_back(TokenType::CaretEqual, startLine, startColumn);
          advance(); advance();
        } else {
          tokens.emplace_back(TokenType::Caret, startLine, startColumn);
          advance();
        }
        break;
      case '~':
        tokens.emplace_back(TokenType::Tilde, startLine, startColumn);
        advance();
        break;
      case '#': {
        advance(); // consume '#'
        std::string privName = "#";
        while (!isAtEnd() && isIdentifierPart(static_cast<unsigned char>(current()))) {
          privName.push_back(current());
          advance();
        }
        if (privName.size() == 1) {
          throw std::runtime_error("SyntaxError: Invalid private field at line " +
                                   std::to_string(startLine) + ", column " + std::to_string(startColumn));
        }
        tokens.emplace_back(TokenType::PrivateIdentifier, privName, startLine, startColumn);
        break;
      }
      default:
        throw std::runtime_error("Unexpected character '" + std::string(1, c) + "' at line " +
                                 std::to_string(startLine) + ", column " + std::to_string(startColumn));
        break;
    }
  }

  tokens.emplace_back(TokenType::EndOfFile, line_, column_);
  return tokens;
}

}
