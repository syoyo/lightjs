#include "lexer_internal.h"
#include <cctype>

namespace lightjs {

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

}  // namespace lightjs
