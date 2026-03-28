#include "lexer_internal.h"

namespace lightjs {

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
                advance();  // consume '\'
                advance();  // consume 'u'
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
  advance();  // skip opening backtick

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
    advance();  // skip closing backtick
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

  auto it = kLexerKeywords.find(identView);
  if (it != kLexerKeywords.end()) {
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

}  // namespace lightjs
