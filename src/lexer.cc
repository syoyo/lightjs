#include "lexer.h"
#include "string_table.h"
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <cctype>
#if USE_SIMPLE_REGEX
#include "simple_regex.h"
#else
#include <regex>
#endif
#include <stdexcept>

namespace lightjs {

namespace {
int hexDigitValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

bool isSurrogateCodePoint(uint32_t codepoint) {
  return codepoint >= 0xD800 && codepoint <= 0xDFFF;
}

bool isDisallowedIdentifierCodePoint(uint32_t codepoint) {
  // Treat some code points as never-valid in identifiers. This matches existing
  // non-escaped identifier scanning which breaks on these sequences.
  return codepoint == 0x00A0 ||  // NO-BREAK SPACE
         codepoint == 0x180E ||  // MONGOLIAN VOWEL SEPARATOR (Cf, not ID_Start/ID_Continue)
         codepoint == 0x2028 ||  // LINE SEPARATOR
         codepoint == 0x2029 ||  // PARAGRAPH SEPARATOR
         codepoint == 0x2E2F ||  // VERTICAL TILDE (not ID_Start/ID_Continue)
         codepoint == 0xFEFF;    // BOM
}

bool isIdentifierStartCodePoint(uint32_t codepoint) {
  // Minimal IdentifierStart check for code points produced by \u escapes.
  // This intentionally doesn't implement full Unicode ID_Start, but it does
  // correctly handle ZWNJ/ZWJ positioning and common early errors in Test262.
  if (codepoint > 0x10FFFF || isSurrogateCodePoint(codepoint) || isDisallowedIdentifierCodePoint(codepoint)) {
    return false;
  }
  if (codepoint <= 0x7F) {
    char c = static_cast<char>(codepoint);
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
  }
  // ZWNJ/ZWJ are IdentifierPart only.
  if (codepoint == 0x200C || codepoint == 0x200D) {
    return false;
  }
  return true;
}

bool isIdentifierPartCodePoint(uint32_t codepoint) {
  if (codepoint > 0x10FFFF || isSurrogateCodePoint(codepoint) || isDisallowedIdentifierCodePoint(codepoint)) {
    return false;
  }
  if (codepoint <= 0x7F) {
    char c = static_cast<char>(codepoint);
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$';
  }
  // ZWNJ/ZWJ are explicitly allowed in IdentifierPart.
  if (codepoint == 0x200C || codepoint == 0x200D) {
    return true;
  }
  return true;
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
  {"debugger", TokenType::Debugger},
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
  {"enum", TokenType::Enum},
  {"true", TokenType::True},
  {"false", TokenType::False},
  {"null", TokenType::Null},
};

Lexer::Lexer(std::string_view source) {
  // Normalize CR and CRLF to LF.  U+2028/U+2029 are kept as-is because
  // ES2019 allows them inside string literals (json-superset).
  owned_source_.reserve(source.size());
  for (size_t i = 0; i < source.size(); i++) {
    unsigned char c = static_cast<unsigned char>(source[i]);
    if (c == '\r') {
      // CRLF -> LF
      if (i + 1 < source.size() && source[i + 1] == '\n') {
        i++;
      }
      owned_source_.push_back('\n');
      continue;
    }
    owned_source_.push_back(static_cast<char>(c));
  }
  source_ = std::string_view(owned_source_);
}

void Lexer::skipHashbangAtStart() {
  // Hashbang comments are only recognized at the start of the source text,
  // optionally preceded by a UTF-8 BOM.
  //
  // https://tc39.es/ecma262/#sec-hashbang-comments
  if (pos_ != 0) {
    return;
  }

  // Consume BOM if present (UTF-8: EF BB BF). This matches skipWhitespace()'s
  // behavior, but we need to do it here so that `#!` immediately after the BOM
  // is treated as a HashbangComment rather than as a private identifier error.
  if (source_.size() >= 3 &&
      static_cast<unsigned char>(source_[0]) == 0xEF &&
      static_cast<unsigned char>(source_[1]) == 0xBB &&
      static_cast<unsigned char>(source_[2]) == 0xBF) {
    advance();
    advance();
    advance();
  }

  if (!isAtEnd() && current() == '#' && peek() == '!') {
    // Skip "#!" and then the rest of the line.
    advance();
    advance();
    skipLineComment();
  }
}

uint32_t Lexer::readUnicodeEscape(const std::string& errMsg) {
  uint32_t codepoint = 0;
  if (current() == '{') {
    advance();  // '{'
    bool sawDigit = false;
    while (!isAtEnd() && current() != '}') {
      int d = hexDigitValue(current());
      if (d < 0) throw std::runtime_error(errMsg);
      sawDigit = true;
      if (codepoint > 0x10FFFF) throw std::runtime_error(errMsg);
      codepoint = (codepoint << 4) + static_cast<uint32_t>(d);
      advance();
    }
    if (!sawDigit || isAtEnd() || current() != '}') throw std::runtime_error(errMsg);
    advance();  // '}'
  } else {
    for (int i = 0; i < 4; i++) {
      if (isAtEnd()) throw std::runtime_error(errMsg);
      int d = hexDigitValue(current());
      if (d < 0) throw std::runtime_error(errMsg);
      codepoint = (codepoint << 4) + static_cast<uint32_t>(d);
      advance();
    }
  }
  if (codepoint > 0x10FFFF || isSurrogateCodePoint(codepoint)) throw std::runtime_error(errMsg);
  return codepoint;
}

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
    } else if (c == 0xE1 && static_cast<unsigned char>(peek()) == 0x9A &&
               static_cast<unsigned char>(peek(2)) == 0x80) {
      // U+1680 OGHAM SPACE MARK
      advance(); advance(); advance();
    } else if (c == 0xE2 && static_cast<unsigned char>(peek()) == 0x80) {
      unsigned char third = static_cast<unsigned char>(peek(2));
      if (third >= 0x80 && third <= 0x8A) {
        // U+2000-U+200A: EN QUAD through HAIR SPACE
        advance(); advance(); advance();
      } else if (third == 0xA8 || third == 0xA9) {
        // U+2028 LINE SEPARATOR or U+2029 PARAGRAPH SEPARATOR
        advance();
        advance();
        advance();
        // Treat Unicode line/paragraph separators as line terminators.
        line_++;
        column_ = 1;
      } else if (third == 0xAF) {
        // U+202F NARROW NO-BREAK SPACE
        advance(); advance(); advance();
      } else {
        break;
      }
    } else if (c == 0xE2 && static_cast<unsigned char>(peek()) == 0x81 &&
               static_cast<unsigned char>(peek(2)) == 0x9F) {
      // U+205F MEDIUM MATHEMATICAL SPACE
      advance(); advance(); advance();
    } else if (c == 0xE3 && static_cast<unsigned char>(peek()) == 0x80 &&
               static_cast<unsigned char>(peek(2)) == 0x80) {
      // U+3000 IDEOGRAPHIC SPACE
      advance(); advance(); advance();
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
    // U+2028/U+2029 are also line terminators
    unsigned char c = static_cast<unsigned char>(current());
    if (c == 0xE2 && pos_ + 2 < source_.size() &&
        static_cast<unsigned char>(peek()) == 0x80) {
      unsigned char third = static_cast<unsigned char>(peek(2));
      if (third == 0xA8 || third == 0xA9) break;
    }
    advance();
  }
}

void Lexer::skipBlockComment() {
  advance();
  advance();
  while (!isAtEnd()) {
    unsigned char c = static_cast<unsigned char>(current());
    if (c == '*' && peek() == '/') {
      advance();
      advance();
      return;
    }
    if (c == '\r') {
      advance();
      if (!isAtEnd() && current() == '\n') {
        advance();
      }
      line_++;
      column_ = 1;
      continue;
    }
    // U+2028 LINE SEPARATOR (E2 80 A8) and U+2029 PARAGRAPH SEPARATOR (E2 80 A9)
    if (c == 0xE2 && pos_ + 2 < source_.size()) {
      unsigned char c2 = static_cast<unsigned char>(source_[pos_ + 1]);
      unsigned char c3 = static_cast<unsigned char>(source_[pos_ + 2]);
      if (c2 == 0x80 && (c3 == 0xA8 || c3 == 0xA9)) {
        advance(); advance(); advance();
        line_++;
        column_ = 1;
        continue;
      }
    }
    advance();
  }
  // Unterminated multi-line comment
  throw std::runtime_error("SyntaxError: Unterminated comment");
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
    bool sawDigit = false;
    bool prevSeparator = false;
    while (!isAtEnd()) {
      char c = current();
      if (isDigit(c)) {
        sawDigit = true;
        prevSeparator = false;
        advance();
        continue;
      }
      if (c == '_') {
        if (!sawDigit || prevSeparator || !isDigit(peek())) {
          throwSyntaxError("Invalid exponent in numeric literal");
        }
        prevSeparator = true;
        advance();
        continue;
      }
      break;
    }
    if (!sawDigit || prevSeparator) {
      throwSyntaxError("Invalid exponent in numeric literal");
    }
  };

  // Leading-dot decimal number: .123, .123e1
  if (!isAtEnd() && current() == '.') {
    advance();
    if (isAtEnd() || !isDigit(current())) {
      throwSyntaxError("Invalid numeric literal");
    }
    bool sawDigit = false;
    bool prevSeparator = false;
    while (!isAtEnd()) {
      char c = current();
      if (isDigit(c)) {
        sawDigit = true;
        prevSeparator = false;
        advance();
        continue;
      }
      if (c == '_') {
        if (!sawDigit || prevSeparator || !isDigit(peek())) {
          throwSyntaxError("Invalid numeric separator in literal");
        }
        prevSeparator = true;
        advance();
        continue;
      }
      break;
    }
    if (!sawDigit || prevSeparator) {
      throwSyntaxError("Invalid numeric separator in literal");
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

  // Decimal integer literals cannot use separators immediately after a leading zero.
  if (pos_ > start + 1 && source_[start] == '0') {
    bool allDigitsOrSeparators = true;
    bool hasSeparator = false;
    for (size_t i = start + 1; i < pos_; ++i) {
      char c = source_[i];
      if (c == '_') {
        hasSeparator = true;
      } else if (!isDigit(c)) {
        allDigitsOrSeparators = false;
        break;
      }
    }
    if (allDigitsOrSeparators && hasSeparator) {
      throwSyntaxError("Invalid numeric separator in literal");
    }
  }

  bool hasDot = false;
  bool hasExponent = false;

  if (!isAtEnd() && current() == '.') {
    if (peek() == '_') {
      throwSyntaxError("Invalid numeric separator in literal");
    }
    hasDot = true;
    advance();
    if (!isAtEnd() && isDigit(current())) {
      bool sawFractionDigit = false;
      bool prevFractionSeparator = false;
      while (!isAtEnd()) {
        char c = current();
        if (isDigit(c)) {
          sawFractionDigit = true;
          prevFractionSeparator = false;
          advance();
          continue;
        }
        if (c == '_') {
          if (!sawFractionDigit || prevFractionSeparator || !isDigit(peek())) {
            throwSyntaxError("Invalid numeric separator in literal");
          }
          prevFractionSeparator = true;
          advance();
          continue;
        }
        break;
      }
      if (!sawFractionDigit || prevFractionSeparator) {
        throwSyntaxError("Invalid numeric separator in literal");
      }
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
  bool hasLegacyEscape = false;
  bool hasAnyEscape = false;
  while (!isAtEnd() && current() != quote) {
    if (current() == '\n' || current() == '\r') {
      throw std::runtime_error("SyntaxError: Unterminated string literal");
    }
    if (current() == '\\') {
      hasAnyEscape = true;
      advance();
      if (!isAtEnd()) {
        unsigned char uc = static_cast<unsigned char>(current());
        // LineContinuation with U+2028/U+2029
        if (uc == 0xE2 && pos_ + 2 < source_.size() &&
            static_cast<unsigned char>(peek()) == 0x80) {
          unsigned char third = static_cast<unsigned char>(peek(2));
          if (third == 0xA8 || third == 0xA9) {
            advance(); advance(); advance();
            continue;
          }
        }
        char c = current();
        switch (c) {
          case '\r':
            // LineContinuation: backslash + CR (optionally followed by LF)
            if (!isAtEnd() && peek() == '\n') {
              advance();  // skip the LF after CR
            }
            break;
          case '\n':
            // LineContinuation: backslash + line terminator is removed from string value
            break;
          case 'n': str += '\n'; break;
          case 't': str += '\t'; break;
          case 'r': str += '\r'; break;
          case 'b': str += '\b'; break;
          case 'f': str += '\f'; break;
          case 'v': str += '\v'; break;
          case '0':
            // \0 is null character, but \00-\07 are legacy octal
            if (!isAtEnd() && peek() >= '0' && peek() <= '7') {
              // Legacy octal - handle below in default
              hasLegacyEscape = true;
              goto handle_default_escape;
            }
            // \08 and \09: \0 (null) followed by literal '8' or '9'
            // But in strict mode, \0 followed by any digit is legacy
            if (!isAtEnd() && (peek() == '8' || peek() == '9')) {
              hasLegacyEscape = true;
            }
            str += '\0';
            break;
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
          case 'x': {
            advance();  // consume 'x'
            if (isAtEnd()) {
              throw std::runtime_error("Invalid hexadecimal escape in string");
            }
            int hi = hexDigitValue(current());
            if (hi < 0) {
              throw std::runtime_error("Invalid hexadecimal escape in string");
            }
            advance();
            if (isAtEnd()) {
              throw std::runtime_error("Invalid hexadecimal escape in string");
            }
            int lo = hexDigitValue(current());
            if (lo < 0) {
              throw std::runtime_error("Invalid hexadecimal escape in string");
            }
            appendUtf8(str, static_cast<uint32_t>((hi << 4) | lo));
            break;
          }
          default:
          handle_default_escape:
            if (c >= '0' && c <= '7') {
              // Legacy octal escape sequence
              if (c >= '1') hasLegacyEscape = true;
              int val = c - '0';
              if (!isAtEnd() && peek() >= '0' && peek() <= '7') {
                advance();
                val = val * 8 + (current() - '0');
                if (c <= '3' && !isAtEnd() && peek() >= '0' && peek() <= '7') {
                  advance();
                  val = val * 8 + (current() - '0');
                }
              }
              if (c == '0' && val == 0) {
                str += '\0';
              } else {
                hasLegacyEscape = true;
                appendUtf8(str, static_cast<uint32_t>(val));
              }
            } else if (c == '8' || c == '9') {
              // Legacy non-octal decimal escape \8 \9
              hasLegacyEscape = true;
              str += c;
            } else {
              str += c;
            }
            break;
        }
        advance();
      }
    } else {
      str += current();
      advance();
    }
  }

  if (isAtEnd()) {
    throw std::runtime_error("SyntaxError: Unterminated string literal");
  }
  advance();  // closing quote

  // Intern string literals for memory efficiency (especially for object keys)
  // Only intern small strings (< 256 chars) to avoid memory bloat
  if (str.length() < 256) {
    auto internedStr = StringTable::instance().intern(str);
    auto tok = Token(TokenType::String, internedStr, startLine, startColumn);
    tok.hasLegacyEscape = hasLegacyEscape;
    tok.escaped = hasAnyEscape;
    return tok;
  }

  auto tok = Token(TokenType::String, str, startLine, startColumn);
  tok.hasLegacyEscape = hasLegacyEscape;
  tok.escaped = hasAnyEscape;
  return tok;
}

std::optional<Token> Lexer::readTemplateLiteral() {
  uint32_t startLine = line_;
  uint32_t startColumn = column_;
  advance(); // skip opening backtick

  // Keep the template content in *raw source form* (backslashes preserved).
  // The parser/interpreter will compute cooked/raw values for each quasi.
  std::string content;
  while (!isAtEnd() && current() != '`') {
    if (current() == '\\') {
      // Copy backslash + next char verbatim so `${` can be escaped as `\${`.
      content += current();
      advance();
      if (isAtEnd()) break;
      content += current();
      advance();
      continue;
    }
    // Handle ${...} expression interpolations - must track nesting to handle
    // nested template literals inside expressions like `foo ${`bar ${x} baz`}`
    if (current() == '$' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '{') {
      content += '$';
      advance();
      content += '{';
      advance();
      // Track brace depth and nested template literals
      int braceDepth = 1;
      while (!isAtEnd() && braceDepth > 0) {
        if (current() == '`') {
          // Nested template literal - read it recursively into content
          content += '`';
          advance();
          while (!isAtEnd() && current() != '`') {
            if (current() == '\\') {
              content += current();
              advance();
              if (isAtEnd()) break;
              content += current();
              advance();
              continue;
            }
            if (current() == '$' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '{') {
              content += '$';
              advance();
              content += '{';
              advance();
              // Nested expression in nested template
              int innerBraceDepth = 1;
              while (!isAtEnd() && innerBraceDepth > 0) {
                if (current() == '{') innerBraceDepth++;
                else if (current() == '}') innerBraceDepth--;
                if (innerBraceDepth > 0) {
                  content += current();
                  advance();
                }
              }
              if (!isAtEnd()) {
                content += '}';
                advance();
              }
              continue;
            }
            content += current();
            advance();
          }
          if (!isAtEnd()) {
            content += '`';
            advance();
          }
          continue;
        }
        if (current() == '\'') {
          // Single-quoted string
          content += current();
          advance();
          while (!isAtEnd() && current() != '\'') {
            if (current() == '\\') { content += current(); advance(); if (isAtEnd()) break; }
            content += current();
            advance();
          }
          if (!isAtEnd()) { content += current(); advance(); }
          continue;
        }
        if (current() == '"') {
          // Double-quoted string
          content += current();
          advance();
          while (!isAtEnd() && current() != '"') {
            if (current() == '\\') { content += current(); advance(); if (isAtEnd()) break; }
            content += current();
            advance();
          }
          if (!isAtEnd()) { content += current(); advance(); }
          continue;
        }
        if (current() == '{') braceDepth++;
        else if (current() == '}') braceDepth--;
        if (braceDepth > 0) {
          content += current();
          advance();
        }
      }
      if (!isAtEnd()) {
        content += '}';
        advance();
      }
      continue;
    }
    content += current();
    advance();
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

  static const std::string kIdentEscErr = "Invalid unicode escape in identifier";

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
      // U+2E2F VERTICAL TILDE is not ID_Start/ID_Continue (UTF-8: E2 B8 AF)
      if (c == 0xE2 && static_cast<unsigned char>(peek()) == 0xB8 &&
          static_cast<unsigned char>(peek(2)) == 0xAF) {
        break;
      }
      // U+180E MONGOLIAN VOWEL SEPARATOR (UTF-8: E1 A0 8E)
      if (c == 0xE1 && static_cast<unsigned char>(peek()) == 0xA0 &&
          static_cast<unsigned char>(peek(2)) == 0x8E) {
        break;
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

      uint32_t codepoint = readUnicodeEscape(kIdentEscErr);
      bool validStart = isIdentifierStartCodePoint(codepoint);
      bool validPart = isIdentifierPartCodePoint(codepoint);
      if ((first && !validStart) || (!first && !validPart)) {
        throw std::runtime_error("Invalid identifier escape");
      }
      appendUtf8(ident, codepoint);
      first = false;
      continue;
    }

    break;
  }

  if (ident.empty()) {
    return std::nullopt;  // No valid identifier chars consumed
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
  bool inCharClass = false;
  auto isRegexLineTerminator = [this]() -> bool {
    if (isAtEnd()) return false;
    char c = current();
    if (c == '\n' || c == '\r') return true;
    // U+2028/U+2029 encoded as UTF-8.
    if (pos_ + 2 < source_.size() &&
        static_cast<unsigned char>(c) == 0xE2 &&
        static_cast<unsigned char>(peek()) == 0x80) {
      char third = source_[pos_ + 2];
      if (static_cast<unsigned char>(third) == 0xA8 ||
          static_cast<unsigned char>(third) == 0xA9) {
        return true;
      }
    }
    return false;
  };
  while (!isAtEnd()) {
    char c = current();
    if (isRegexLineTerminator()) {
      throw std::runtime_error("SyntaxError: Unterminated regular expression literal");
    }
    if (c == '\\') {
      pattern += c;
      advance();
      if (isAtEnd()) {
        throw std::runtime_error("SyntaxError: Unterminated regular expression literal");
      }
      if (isRegexLineTerminator()) {
        throw std::runtime_error("SyntaxError: Unterminated regular expression literal");
      }
      pattern += current();
      advance();
      continue;
    }
    if (c == '[') {
      inCharClass = true;
      pattern += c;
      advance();
      continue;
    }
    if (c == ']' && inCharClass) {
      inCharClass = false;
      pattern += c;
      advance();
      continue;
    }
    if (c == '/' && !inCharClass) {
      break;
    }
    pattern += c;
    advance();
  }

  if (isAtEnd() || current() != '/') {
    throw std::runtime_error("SyntaxError: Unterminated regular expression literal");
  }

  advance();  // closing '/'

  std::string flags;
  std::unordered_set<char> seen;
  auto isAsciiLetter = [](char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
  };
  while (!isAtEnd() && isAsciiLetter(current())) {
    char f = current();
    if (seen.count(f)) {
      throw std::runtime_error("SyntaxError: Invalid regular expression flags");
    }
    // Only accept known ASCII flag letters.
    if (!(f == 'g' || f == 'i' || f == 'm' || f == 's' || f == 'u' || f == 'y' ||
          f == 'd' || f == 'v')) {
      throw std::runtime_error("SyntaxError: Invalid regular expression flags");
    }
    seen.insert(f);
    flags += f;
    advance();
  }

  // --- ECMAScript-specific regex validation (beyond what std::regex catches) ---
  bool hasUFlag = flags.find('u') != std::string::npos;
  bool hasVFlag = flags.find('v') != std::string::npos;
  bool unicodeMode = hasUFlag || hasVFlag;

  // u and v flags are mutually exclusive
  if (hasUFlag && hasVFlag) {
    throw std::runtime_error("SyntaxError: Invalid regular expression flags");
  }

  // Validate the pattern for ECMAScript-specific rules
  bool hasNamedGroups = false;
  std::vector<std::string> namedGroups;
  {
    size_t i = 0;
    size_t len = pattern.size();
    int groupCount = 0;

    // Count capture groups and collect named group names for validation
    for (size_t j = 0; j < len; ++j) {
      if (pattern[j] == '\\' && j + 1 < len) { ++j; continue; }
      if (pattern[j] == '[') {
        ++j;
        while (j < len && pattern[j] != ']') { if (pattern[j] == '\\' && j + 1 < len) ++j; ++j; }
        continue;
      }
      if (pattern[j] == '(' && j + 1 < len && pattern[j + 1] == '?' &&
          j + 2 < len && pattern[j + 2] == '<' && j + 3 < len &&
          pattern[j + 3] != '=' && pattern[j + 3] != '!') {
        // Named capture group (?<name>...)
        ++groupCount;
        hasNamedGroups = true;
        size_t nameStart = j + 3;
        size_t nameEnd = nameStart;
        while (nameEnd < len && pattern[nameEnd] != '>') ++nameEnd;
        if (nameEnd < len) {
          namedGroups.push_back(pattern.substr(nameStart, nameEnd - nameStart));
        }
        j = nameEnd;
      } else if (pattern[j] == '(' && (j + 1 >= len || pattern[j + 1] != '?')) {
        ++groupCount;
      }
    }

    // Helper: check if a quantifier follows at position i
    auto isQuantifier = [&](size_t pos) -> bool {
      if (pos >= len) return false;
      char c = pattern[pos];
      return c == '?' || c == '*' || c == '+' || c == '{';
    };

    // Walk through the pattern checking for invalid constructs
    i = 0;
    bool inCC = false;
    while (i < len) {
      char c = pattern[i];

      // Track character classes
      if (c == '[' && !inCC) { inCC = true; ++i; continue; }
      if (c == ']' && inCC) { inCC = false; ++i; continue; }
      if (inCC) {
        if (c == '\\') { i += 2; } else { ++i; }
        continue;
      }

      // Check for InvalidBracedQuantifier: {n}, {n,}, {n,m} at Atom position
      // (i.e. not preceded by a quantifiable atom)
      if (c == '{' && i == 0) {
        // Check if this is a braced quantifier pattern
        size_t j = i + 1;
        bool isDigit = false;
        while (j < len && pattern[j] >= '0' && pattern[j] <= '9') { isDigit = true; ++j; }
        if (isDigit && j < len) {
          if (pattern[j] == '}') {
            throw std::runtime_error("SyntaxError: Invalid regular expression: Nothing to repeat");
          }
          if (pattern[j] == ',') {
            ++j;
            while (j < len && pattern[j] >= '0' && pattern[j] <= '9') ++j;
            if (j < len && pattern[j] == '}') {
              throw std::runtime_error("SyntaxError: Invalid regular expression: Nothing to repeat");
            }
          }
        }
      }

      // Handle escape sequences
      if (c == '\\') {
        if (i + 1 < len) {
          char next = pattern[i + 1];

          if (unicodeMode) {
            // In unicode mode, identity escapes are restricted to SyntaxCharacter and /
            // SyntaxCharacter: ^ $ \ . * + ? ( ) [ ] { } |
            auto isSyntaxChar = [](char ch) {
              return ch == '^' || ch == '$' || ch == '\\' || ch == '.' ||
                     ch == '*' || ch == '+' || ch == '?' || ch == '(' ||
                     ch == ')' || ch == '[' || ch == ']' || ch == '{' ||
                     ch == '}' || ch == '|' || ch == '/';
            };

            // Check for legacy octal escapes in unicode mode
            if (next >= '1' && next <= '9') {
              // Could be a backreference - check if it's a valid group number
              size_t refEnd = i + 1;
              int refNum = 0;
              while (refEnd < len && pattern[refEnd] >= '0' && pattern[refEnd] <= '9') {
                refNum = refNum * 10 + (pattern[refEnd] - '0');
                ++refEnd;
              }
              if (refNum > groupCount) {
                throw std::runtime_error("SyntaxError: Invalid regular expression: Invalid escape in unicode mode");
              }
            } else if (next == '0') {
              // \0 is allowed (null character), but \00, \01 etc are octal
              if (i + 2 < len && pattern[i + 2] >= '0' && pattern[i + 2] <= '9') {
                throw std::runtime_error("SyntaxError: Invalid regular expression: Invalid escape in unicode mode");
              }
            } else if (next == 'c') {
              // \cA-\cZ and \ca-\cz are valid, \c0 etc are not
              if (i + 2 < len) {
                char ctrl = pattern[i + 2];
                if (!((ctrl >= 'A' && ctrl <= 'Z') || (ctrl >= 'a' && ctrl <= 'z'))) {
                  throw std::runtime_error("SyntaxError: Invalid regular expression: Invalid escape in unicode mode");
                }
              }
            } else if (next == 'k') {
              // \k in unicode mode requires a valid named backreference \k<name>
              if (i + 2 >= len || pattern[i + 2] != '<') {
                throw std::runtime_error("SyntaxError: Invalid regular expression: Invalid named reference");
              }
              size_t nameStart = i + 3;
              size_t nameEnd = nameStart;
              while (nameEnd < len && pattern[nameEnd] != '>') ++nameEnd;
              if (nameEnd >= len) {
                throw std::runtime_error("SyntaxError: Invalid regular expression: Invalid named reference");
              }
              std::string refName = pattern.substr(nameStart, nameEnd - nameStart);
              if (refName.empty()) {
                throw std::runtime_error("SyntaxError: Invalid regular expression: Invalid named reference");
              }
              // Check that the referenced group exists
              bool found = false;
              for (const auto& gn : namedGroups) {
                if (gn == refName) { found = true; break; }
              }
              if (!found) {
                throw std::runtime_error("SyntaxError: Invalid regular expression: Invalid named reference");
              }
              i = nameEnd + 1;
              continue;
            } else if (next == 'u') {
              // Validate \u escape in unicode mode
              if (i + 2 < len && pattern[i + 2] == '{') {
                // \u{HHHH} - validate hex digits and range
                size_t braceStart = i + 3;
                size_t braceEnd = braceStart;
                while (braceEnd < len && pattern[braceEnd] != '}') ++braceEnd;
                if (braceEnd >= len) {
                  throw std::runtime_error("SyntaxError: Invalid regular expression: unterminated unicode escape");
                }
                std::string hexStr = pattern.substr(braceStart, braceEnd - braceStart);
                if (hexStr.empty()) {
                  throw std::runtime_error("SyntaxError: Invalid regular expression: invalid unicode escape");
                }
                // Validate all characters are hex digits
                for (char hc : hexStr) {
                  if (!((hc >= '0' && hc <= '9') || (hc >= 'a' && hc <= 'f') || (hc >= 'A' && hc <= 'F'))) {
                    throw std::runtime_error("SyntaxError: Invalid regular expression: invalid unicode escape");
                  }
                }
                // Check range: must be <= 0x10FFFF
                unsigned long codePoint = std::stoul(hexStr, nullptr, 16);
                if (codePoint > 0x10FFFF) {
                  throw std::runtime_error("SyntaxError: Invalid regular expression: unicode escape out of range");
                }
                i = braceEnd + 1;
                continue;
              }
              // \uHHHH - 4 hex digits required in unicode mode
              // (handled by existing i += 2 and next iteration)
            } else if (!isSyntaxChar(next) &&
                       next != 'b' && next != 'B' && next != 'd' && next != 'D' &&
                       next != 'w' && next != 'W' && next != 's' && next != 'S' &&
                       next != 'f' && next != 'n' && next != 'r' && next != 't' &&
                       next != 'v' && next != 'x' && next != 'c' &&
                       next != '0' && next != 'k' && next != 'p' && next != 'P') {
              throw std::runtime_error("SyntaxError: Invalid regular expression: Invalid escape in unicode mode");
            }
          }
        }
        i += 2;
        continue;
      }

      // Check for assertions (lookahead/lookbehind) and quantifiers on them
      if (c == '(' && i + 1 < len && pattern[i + 1] == '?') {
        bool isLookbehind = false;
        bool isLookahead = false;
        size_t assertEnd = i;

        if (i + 2 < len) {
          if (pattern[i + 2] == '=' || pattern[i + 2] == '!') {
            isLookahead = true;
          } else if (pattern[i + 2] == '<' && i + 3 < len) {
            if (pattern[i + 3] == '=' || pattern[i + 3] == '!') {
              isLookbehind = true;
            }
          }
        }

        if (isLookahead || isLookbehind) {
          // Find the matching closing paren
          int depth = 1;
          assertEnd = (isLookbehind ? i + 4 : i + 3);
          while (assertEnd < len && depth > 0) {
            if (pattern[assertEnd] == '\\') { assertEnd += 2; continue; }
            if (pattern[assertEnd] == '(') ++depth;
            if (pattern[assertEnd] == ')') --depth;
            if (depth > 0) ++assertEnd;
          }
          if (assertEnd < len) ++assertEnd; // skip closing ')'

          // Check if followed by a quantifier
          if (assertEnd < len && isQuantifier(assertEnd)) {
            if (isLookbehind) {
              // Lookbehinds are NEVER quantifiable
              throw std::runtime_error("SyntaxError: Invalid regular expression: quantifier on lookbehind");
            }
            if (unicodeMode) {
              // In unicode mode, lookaheads are also not quantifiable
              throw std::runtime_error("SyntaxError: Invalid regular expression: quantifier on assertion in unicode mode");
            }
          }
          i = assertEnd;
          continue;
        }
      }

      // In unicode mode, lone { and } are not allowed as pattern characters
      if (unicodeMode && (c == '{' || c == '}')) {
        if (c == '{') {
          // Check if this is a valid quantifier {n}, {n,}, {n,m}
          size_t j = i + 1;
          bool validQuantifier = false;
          size_t quantEnd = 0;
          bool hasDigit = false;
          while (j < len && pattern[j] >= '0' && pattern[j] <= '9') { hasDigit = true; ++j; }
          if (hasDigit && j < len) {
            if (pattern[j] == '}') { validQuantifier = true; quantEnd = j; }
            else if (pattern[j] == ',') {
              ++j;
              while (j < len && pattern[j] >= '0' && pattern[j] <= '9') ++j;
              if (j < len && pattern[j] == '}') { validQuantifier = true; quantEnd = j; }
            }
          }
          if (!validQuantifier) {
            throw std::runtime_error("SyntaxError: Invalid regular expression: lone { in unicode mode");
          }
          // Skip past the closing } so we don't reject it as a lone }
          i = quantEnd + 1;
          continue;
        } else {
          // lone } not following a valid quantifier opening
          throw std::runtime_error("SyntaxError: Invalid regular expression: lone } in unicode mode");
        }
      }

      ++i;
    }

    // Unicode mode: validate character class ranges
    if (unicodeMode) {
      i = 0;
      while (i < len) {
        if (pattern[i] == '\\') { i += 2; continue; }
        if (pattern[i] == '[') {
          ++i;
          bool negated = (i < len && pattern[i] == '^');
          if (negated) ++i;

          // Walk through class contents looking for invalid ranges
          while (i < len && pattern[i] != ']') {
            if (pattern[i] == '\\') {
              char esc = (i + 1 < len) ? pattern[i + 1] : 0;
              bool isCharClass = (esc == 'd' || esc == 'D' || esc == 'w' || esc == 'W' ||
                                  esc == 's' || esc == 'S');
              i += 2;
              // Check if followed by dash and another atom (creating a range)
              if (isCharClass && i < len && pattern[i] == '-' && i + 1 < len && pattern[i + 1] != ']') {
                throw std::runtime_error("SyntaxError: Invalid regular expression: invalid character class range in unicode mode");
              }
            } else {
              char first = pattern[i];
              ++i;
              if (i < len && pattern[i] == '-' && i + 1 < len && pattern[i + 1] != ']') {
                ++i; // skip -
                if (i < len && pattern[i] == '\\') {
                  char esc = (i + 1 < len) ? pattern[i + 1] : 0;
                  bool isCharClass = (esc == 'd' || esc == 'D' || esc == 'w' || esc == 'W' ||
                                      esc == 's' || esc == 'S');
                  if (isCharClass) {
                    throw std::runtime_error("SyntaxError: Invalid regular expression: invalid character class range in unicode mode");
                  }
                  i += 2;
                } else if (i < len) {
                  ++i;
                }
              }
            }
          }
          if (i < len) ++i; // skip ]
          continue;
        }
        ++i;
      }
    }
  }

  // Custom validation for named group patterns (std::regex doesn't support them)
  if (hasNamedGroups) {
    // Validate named group names and check for duplicates
    std::set<std::string> seenNames;
    size_t j = 0;
    while (j < pattern.size()) {
      if (pattern[j] == '\\' && j + 1 < pattern.size()) { j += 2; continue; }
      if (pattern[j] == '[') { ++j; while (j < pattern.size() && pattern[j] != ']') { if (pattern[j] == '\\') ++j; ++j; } if (j < pattern.size()) ++j; continue; }
      if (pattern[j] == '(' && j + 1 < pattern.size() && pattern[j + 1] == '?' &&
          j + 2 < pattern.size() && pattern[j + 2] == '<' &&
          j + 3 < pattern.size() && pattern[j + 3] != '=' && pattern[j + 3] != '!') {
        // Named capture group (?<name>...)
        size_t nameStart = j + 3;
        size_t nameEnd = nameStart;
        while (nameEnd < pattern.size() && pattern[nameEnd] != '>') ++nameEnd;
        if (nameEnd >= pattern.size()) {
          throw std::runtime_error("SyntaxError: Invalid regular expression: unterminated group name");
        }
        std::string name = pattern.substr(nameStart, nameEnd - nameStart);
        if (name.empty()) {
          throw std::runtime_error("SyntaxError: Invalid regular expression: empty group name");
        }
        // Validate identifier: first char must be letter, _ or $
        char first = name[0];
        if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
              first == '_' || first == '$')) {
          throw std::runtime_error("SyntaxError: Invalid regular expression: invalid group name");
        }
        for (size_t k = 1; k < name.size(); ++k) {
          char ch = name[k];
          if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '_' || ch == '$')) {
            throw std::runtime_error("SyntaxError: Invalid regular expression: invalid group name");
          }
        }
        if (seenNames.count(name)) {
          throw std::runtime_error("SyntaxError: Invalid regular expression: duplicate group name");
        }
        seenNames.insert(name);
        j = nameEnd + 1;
      } else {
        ++j;
      }
    }
  }

  // Also validate \k references in non-unicode mode
  // (unicode mode \k validation is handled above)
  if (!unicodeMode && hasNamedGroups) {
    size_t j = 0;
    while (j + 1 < pattern.size()) {
      if (pattern[j] == '\\' && pattern[j + 1] == 'k') {
        if (j + 2 >= pattern.size() || pattern[j + 2] != '<') {
          throw std::runtime_error("SyntaxError: Invalid regular expression: invalid named reference");
        }
        size_t nameStart = j + 3;
        size_t nameEnd = nameStart;
        while (nameEnd < pattern.size() && pattern[nameEnd] != '>') ++nameEnd;
        if (nameEnd >= pattern.size()) {
          throw std::runtime_error("SyntaxError: Invalid regular expression: invalid named reference");
        }
        std::string refName = pattern.substr(nameStart, nameEnd - nameStart);
        // In non-unicode mode with named groups, dangling \k<name> is a SyntaxError
        // (Annex B only allows \k as identity escape when there are NO named groups)
        bool found = false;
        for (const auto& gn : namedGroups) {
          if (gn == refName) { found = true; break; }
        }
        if (!found) {
          throw std::runtime_error("SyntaxError: Invalid regular expression: invalid named reference");
        }
        j = nameEnd + 1;
      } else if (pattern[j] == '\\') {
        j += 2;
      } else {
        ++j;
      }
    }
  }

  // Validate pattern+flags early using std::regex/simple_regex for basic syntax.
  // For patterns with features std::regex doesn't support (named groups, unicode escapes),
  // transform the pattern before validation.
  if (!unicodeMode) {
    std::string validationPattern = pattern;
    // Strip named group syntax (?<name>...) → (...) for std::regex validation
    if (hasNamedGroups) {
      std::string transformed;
      size_t j = 0;
      while (j < validationPattern.size()) {
        if (validationPattern[j] == '\\' && j + 1 < validationPattern.size()) {
          if (validationPattern[j + 1] == 'k') {
            // Replace \k<name> with a non-capturing group (avoids forward reference issues)
            transformed += "(?:)";
            j += 2;
            if (j < validationPattern.size() && validationPattern[j] == '<') {
              while (j < validationPattern.size() && validationPattern[j] != '>') ++j;
              if (j < validationPattern.size()) ++j;
            }
          } else {
            transformed += validationPattern[j];
            transformed += validationPattern[j + 1];
            j += 2;
          }
          continue;
        }
        if (validationPattern[j] == '(' && j + 1 < validationPattern.size() &&
            validationPattern[j + 1] == '?' && j + 2 < validationPattern.size() &&
            validationPattern[j + 2] == '<' && j + 3 < validationPattern.size() &&
            validationPattern[j + 3] != '=' && validationPattern[j + 3] != '!') {
          // Replace (?<name> with (
          transformed += '(';
          j += 3;
          while (j < validationPattern.size() && validationPattern[j] != '>') ++j;
          if (j < validationPattern.size()) ++j;
        } else {
          transformed += validationPattern[j];
          ++j;
        }
      }
      validationPattern = transformed;
    }
    try {
#if USE_SIMPLE_REGEX
      bool caseInsensitive = flags.find('i') != std::string::npos;
      simple_regex::Regex tmp(validationPattern, caseInsensitive);
      (void)tmp;
#else
      std::regex::flag_type options = std::regex::ECMAScript;
      if (flags.find('i') != std::string::npos) {
        options |= std::regex::icase;
      }
      std::regex tmp(validationPattern, options);
      (void)tmp;
#endif
    } catch (...) {
      throw std::runtime_error("SyntaxError: Invalid regular expression");
    }
  }

  // Encode pattern length as prefix so parser can split unambiguously.
  // Format: "<length>:" + pattern + flags
  std::string value = std::to_string(pattern.size()) + ":" + pattern + flags;
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

  skipHashbangAtStart();

  while (!isAtEnd()) {
    skipWhitespace();
    if (isAtEnd()) break;

    size_t prevTokenCount = tokens.size();
    uint32_t tokenStartOffset = static_cast<uint32_t>(pos_);
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
      } else {
        throw std::runtime_error("SyntaxError: Unexpected token");
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
        {
          auto looksLikeRegexAfterSlash = [this]() -> bool {
            size_t p = pos_ + 1;
            bool inClass = false;
            while (p < source_.size()) {
              unsigned char ch = static_cast<unsigned char>(source_[p]);
              if (ch == '\n' || ch == '\r') return false;
              if (ch == 0xE2 && p + 2 < source_.size() &&
                  static_cast<unsigned char>(source_[p + 1]) == 0x80 &&
                  (static_cast<unsigned char>(source_[p + 2]) == 0xA8 ||
                   static_cast<unsigned char>(source_[p + 2]) == 0xA9)) {
                return false;
              }
              if (ch == '\\') {
                p += 2;
                continue;
              }
              if (ch == '[') {
                inClass = true;
                p++;
                continue;
              }
              if (ch == ']' && inClass) {
                inClass = false;
                p++;
                continue;
              }
              if (ch == '/' && !inClass) {
                return true;
              }
              p++;
            }
            return false;
          };

          bool canBeRegex = tokens.empty() || expectsRegex(tokens.back().type);
          if (!canBeRegex && !tokens.empty() && tokens.back().type == TokenType::RightBrace) {
            char nextChar = peek();
            bool nextIsSpace = (nextChar == ' ' || nextChar == '\t' ||
                                nextChar == '\n' || nextChar == '\r' ||
                                nextChar == '\f' || nextChar == '\v');
            if (!nextIsSpace) {
              canBeRegex = looksLikeRegexAfterSlash();
            }
          }
          if (canBeRegex) {
            if (auto token = readRegex()) {
              tokens.push_back(*token);
              break;
            }
          }
          if (peek() == '=') {
            tokens.emplace_back(TokenType::SlashEqual, startLine, startColumn);
            advance();
            advance();
          } else {
            tokens.emplace_back(TokenType::Slash, startLine, startColumn);
            advance();
          }
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
        bool first = true;
        bool hadEscape = false;

        while (!isAtEnd()) {
          unsigned char c = static_cast<unsigned char>(current());
          if (current() == '\\' && peek() == 'u') {
            hadEscape = true;
            advance();  // '\'
            advance();  // 'u'
            std::string privEscErr = "SyntaxError: Invalid private field at line " +
                                     std::to_string(startLine) + ", column " + std::to_string(startColumn);
            uint32_t codepoint = readUnicodeEscape(privEscErr);
            bool validStart = isIdentifierStartCodePoint(codepoint);
            bool validPart = isIdentifierPartCodePoint(codepoint);
            if ((first && !validStart) || (!first && !validPart)) {
              throw std::runtime_error("SyntaxError: Invalid private field at line " +
                                       std::to_string(startLine) + ", column " + std::to_string(startColumn));
            }
            appendUtf8(privName, codepoint);
            first = false;
            continue;
          }

          if (!isIdentifierPart(c)) {
            break;
          }
          if (first && !isIdentifierStart(c)) {
            break;
          }
          privName.push_back(current());
          advance();
          first = false;
        }

        if (privName.size() == 1 || first) {
          throw std::runtime_error("SyntaxError: Invalid private field at line " +
                                   std::to_string(startLine) + ", column " + std::to_string(startColumn));
        }

        Token tok(TokenType::PrivateIdentifier, privName, startLine, startColumn);
        tok.escaped = hadEscape;
        tokens.push_back(tok);
        break;
      }
      default:
        throw std::runtime_error("Unexpected character '" + std::string(1, c) + "' at line " +
                                 std::to_string(startLine) + ", column " + std::to_string(startColumn));
        break;
    }
    // Set byte offsets on any tokens created in this iteration
    uint32_t tokenEndOffset = static_cast<uint32_t>(pos_);
    for (size_t i = prevTokenCount; i < tokens.size(); i++) {
      tokens[i].offset = tokenStartOffset;
      tokens[i].endOffset = tokenEndOffset;
    }
  }

  tokens.emplace_back(TokenType::EndOfFile, line_, column_);
  if (!tokens.empty()) {
    tokens.back().offset = static_cast<uint32_t>(pos_);
    tokens.back().endOffset = static_cast<uint32_t>(pos_);
  }
  return tokens;
}

}
