#include "lexer_internal.h"
#include "regex_utils.h"
#include <unordered_set>
#if USE_SIMPLE_REGEX
#include "simple_regex.h"
#else
#include <regex>
#endif

namespace lightjs {

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
        j = nameEnd;
      } else if (pattern[j] == '(' && (j + 1 >= len || pattern[j + 1] != '?')) {
        ++groupCount;
      }
    }

    if (hasNamedGroups) {
      namedGroups = extractRegexCaptureGroupNames(pattern);
    }

    // Helper: check if a quantifier follows at position i
    auto isQuantifier = [&](size_t pos) -> bool {
      if (pos >= len) return false;
      char c = pattern[pos];
      return c == '?' || c == '*' || c == '+' || c == '{';
    };

    auto consumeUnicodePropertyEscape = [&](size_t escapeIndex, bool inCharacterClass) -> size_t {
      if (escapeIndex + 2 >= len || pattern[escapeIndex + 2] != '{') {
        throw std::runtime_error("SyntaxError: Invalid regular expression: invalid property escape");
      }
      size_t nameStart = escapeIndex + 3;
      size_t nameEnd = nameStart;
      while (nameEnd < len && pattern[nameEnd] != '}') ++nameEnd;
      if (nameEnd >= len) {
        throw std::runtime_error("SyntaxError: Invalid regular expression: unterminated property escape");
      }
      std::string propertyName = pattern.substr(nameStart, nameEnd - nameStart);
      const bool isStringPropertyName =
          propertyName == "Basic_Emoji" ||
          propertyName == "Emoji_Keycap_Sequence" ||
          propertyName == "RGI_Emoji" ||
          propertyName == "RGI_Emoji_Flag_Sequence" ||
          propertyName == "RGI_Emoji_Modifier_Sequence" ||
          propertyName == "RGI_Emoji_Tag_Sequence" ||
          propertyName == "RGI_Emoji_ZWJ_Sequence";
      if (isStringPropertyName) {
        if (!hasVFlag || pattern[escapeIndex + 1] != 'p' || inCharacterClass) {
          throw std::runtime_error("SyntaxError: Invalid regular expression: unsupported property escape");
        }
        return nameEnd + 1;
      }
      if (propertyName != "Lu" &&
          propertyName != "ASCII" &&
          propertyName != "Script=Han" &&
          propertyName != "Script_Extensions=Han") {
        throw std::runtime_error("SyntaxError: Invalid regular expression: unsupported property escape");
      }
      return nameEnd + 1;
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
        if (c == '\\') {
          if (unicodeMode && i + 1 < len &&
              (pattern[i + 1] == 'p' || pattern[i + 1] == 'P')) {
            i = consumeUnicodePropertyEscape(i, true);
          } else {
            i += 2;
          }
        } else {
          if (hasVFlag) {
            auto isVClassReservedSingle = [](char ch) {
              switch (ch) {
                case '(':
                case ')':
                case '[':
                case '{':
                case '}':
                case '/':
                case '-':
                case '|':
                  return true;
                default:
                  return false;
              }
            };
            auto isVClassReservedDouble = [](char ch) {
              switch (ch) {
                case '&':
                case '!':
                case '#':
                case '$':
                case '%':
                case '*':
                case '+':
                case ',':
                case '.':
                case ':':
                case ';':
                case '<':
                case '=':
                case '>':
                case '?':
                case '@':
                case '`':
                case '~':
                case '^':
                  return true;
                default:
                  return false;
              }
            };
            if (isVClassReservedSingle(c) ||
                (isVClassReservedDouble(c) && i + 1 < len && pattern[i + 1] == c)) {
              throw std::runtime_error("SyntaxError: Invalid regular expression");
            }
          }
          ++i;
        }
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
            } else if (next == 'p' || next == 'P') {
              i = consumeUnicodePropertyEscape(i, false);
              continue;
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
          if (assertEnd < len) ++assertEnd;  // skip closing ')'

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
              if ((esc == 'p' || esc == 'P') && i + 2 < len && pattern[i + 2] == '{') {
                isCharClass = true;
                size_t nameEnd = i + 3;
                while (nameEnd < len && pattern[nameEnd] != '}') ++nameEnd;
                if (nameEnd >= len) {
                  throw std::runtime_error("SyntaxError: Invalid regular expression: unterminated property escape");
                }
                i = nameEnd + 1;
              } else {
                i += 2;
              }
              // Check if followed by dash and another atom (creating a range)
              if (isCharClass && i < len && pattern[i] == '-' && i + 1 < len && pattern[i + 1] != ']') {
                throw std::runtime_error("SyntaxError: Invalid regular expression: invalid character class range in unicode mode");
              }
            } else {
              char first = pattern[i];
              ++i;
              if (i < len && pattern[i] == '-' && i + 1 < len && pattern[i + 1] != ']') {
                ++i;  // skip -
                if (i < len && pattern[i] == '\\') {
                  char esc = (i + 1 < len) ? pattern[i + 1] : 0;
                  bool isCharClass = (esc == 'd' || esc == 'D' || esc == 'w' || esc == 'W' ||
                                      esc == 's' || esc == 'S');
                  if ((esc == 'p' || esc == 'P') && i + 2 < len && pattern[i + 2] == '{') {
                    isCharClass = true;
                  }
                  if (isCharClass) {
                    throw std::runtime_error("SyntaxError: Invalid regular expression: invalid character class range in unicode mode");
                  }
                  if ((esc == 'p' || esc == 'P') && i + 2 < len && pattern[i + 2] == '{') {
                    size_t nameEnd = i + 3;
                    while (nameEnd < len && pattern[nameEnd] != '}') ++nameEnd;
                    if (nameEnd >= len) {
                      throw std::runtime_error("SyntaxError: Invalid regular expression: unterminated property escape");
                    }
                    i = nameEnd + 1;
                  } else {
                    i += 2;
                  }
                } else if (i < len) {
                  ++i;
                }
                (void)first;
              }
            }
          }
          if (i < len) ++i;  // skip ]
          continue;
        }
        ++i;
      }
    }
  }

  if (hasNamedGroups) {
    validateRegexNamedGroups(pattern, flags);
  }
  validateRegexModifierGroups(pattern);

  // Validate pattern+flags early using std::regex/simple_regex for basic syntax.
  // For patterns with features std::regex doesn't support (named groups, unicode escapes),
  // transform the pattern before validation.
  if (!unicodeMode) {
    std::string validationPattern = pattern;
    if (shouldBypassRegexEngineValidation(pattern)) {
      validationPattern = "(?:)";
    }
    // Strip named group syntax (?<name>...) -> (...) for std::regex validation
    if (validationPattern != "(?:)" && hasNamedGroups) {
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
    if (validationPattern != "(?:)") {
      validationPattern = normalizeRegexPatternForEngine(validationPattern, flags);
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

}  // namespace lightjs
