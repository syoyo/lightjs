#include "lexer.h"
#include "string_table.h"
#include <unordered_map>
#include <cctype>

namespace lightjs {

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
  {"in", TokenType::In},
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
    char c = current();
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
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

std::optional<Token> Lexer::readNumber() {
  uint32_t startLine = line_;
  uint32_t startColumn = column_;
  size_t start = pos_;
  bool hasDot = false;

  while (!isAtEnd() && isDigit(current())) {
    advance();
  }

  if (!isAtEnd() && current() == '.' && isDigit(peek())) {
    hasDot = true;
    advance();
    while (!isAtEnd() && isDigit(current())) {
      advance();
    }
  }

  if (!isAtEnd() && (current() == 'e' || current() == 'E')) {
    advance();
    if (!isAtEnd() && (current() == '+' || current() == '-')) {
      advance();
    }
    while (!isAtEnd() && isDigit(current())) {
      advance();
    }
  }

  if (!isAtEnd() && current() == 'n' && !hasDot) {
    advance();
    return Token(TokenType::BigInt, source_.substr(start, pos_ - start - 1), startLine, startColumn);
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
  size_t start = pos_;

  while (!isAtEnd() && isAlphaNumeric(current())) {
    advance();
  }

  std::string_view ident = source_.substr(start, pos_ - start);

  auto it = keywords.find(ident);
  if (it != keywords.end()) {
    // Keywords: intern for memory efficiency
    auto internedKeyword = StringTable::instance().intern(ident);
    return Token(it->second, internedKeyword, startLine, startColumn);
  }

  // Regular identifiers: intern for property name deduplication
  auto internedIdent = StringTable::instance().intern(ident);
  return Token(TokenType::Identifier, internedIdent, startLine, startColumn);
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
         type == TokenType::Star ||
         type == TokenType::Slash ||
         type == TokenType::Percent ||
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

    if (isAlpha(c)) {
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
        } else if (peek() == '.') {
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
          tokens.emplace_back(TokenType::StarStar, startLine, startColumn);
          advance();
          advance();
        } else if (peek() == '=') {
          tokens.emplace_back(TokenType::StarEqual, startLine, startColumn);
          advance();
          advance();
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
        tokens.emplace_back(TokenType::Percent, startLine, startColumn);
        advance();
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
        if (peek() == '=') {
          tokens.emplace_back(TokenType::LessEqual, startLine, startColumn);
          advance();
          advance();
        } else {
          tokens.emplace_back(TokenType::Less, startLine, startColumn);
          advance();
        }
        break;
      case '>':
        if (peek() == '=') {
          tokens.emplace_back(TokenType::GreaterEqual, startLine, startColumn);
          advance();
          advance();
        } else {
          tokens.emplace_back(TokenType::Greater, startLine, startColumn);
          advance();
        }
        break;
      case '&':
        if (peek() == '&') {
          if (peek(2) == '=') {
            tokens.emplace_back(TokenType::AmpAmpEqual, startLine, startColumn);
            advance();
            advance();
            advance();
          } else {
            tokens.emplace_back(TokenType::AmpAmp, startLine, startColumn);
            advance();
            advance();
          }
        } else {
          advance();
        }
        break;
      case '|':
        if (peek() == '|') {
          if (peek(2) == '=') {
            tokens.emplace_back(TokenType::PipePipeEqual, startLine, startColumn);
            advance();
            advance();
            advance();
          } else {
            tokens.emplace_back(TokenType::PipePipe, startLine, startColumn);
            advance();
            advance();
          }
        } else {
          advance();
        }
        break;
      default:
        advance();
        break;
    }
  }

  tokens.emplace_back(TokenType::EndOfFile, line_, column_);
  return tokens;
}

}