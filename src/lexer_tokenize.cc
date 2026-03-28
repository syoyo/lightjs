#include "lexer_internal.h"

namespace lightjs {

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
        if (prevToken.type == TokenType::BigInt) numEnd++;  // BigInt has 'n' suffix not in value
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
          advance();  // first .
          advance();  // second .
          advance();  // third .
        } else {
          tokens.emplace_back(TokenType::Dot, startLine, startColumn);
          advance();
        }
        break;
      case '@':
        tokens.emplace_back(TokenType::At, startLine, startColumn);
        advance();
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
        advance();  // consume '#'
        std::string privName = "#";
        bool first = true;
        bool hadEscape = false;

        while (!isAtEnd()) {
          unsigned char identChar = static_cast<unsigned char>(current());
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

          if (!isIdentifierPart(identChar)) {
            break;
          }
          if (first && !isIdentifierStart(identChar)) {
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

}  // namespace lightjs
