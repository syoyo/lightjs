#pragma once

#include "lexer.h"
#include "string_table.h"
#include <stdexcept>
#include <unordered_map>

namespace lightjs {

inline int hexDigitValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

inline bool isSurrogateCodePoint(uint32_t codepoint) {
  return codepoint >= 0xD800 && codepoint <= 0xDFFF;
}

inline bool isDisallowedIdentifierCodePoint(uint32_t codepoint) {
  // Treat some code points as never-valid in identifiers. This matches existing
  // non-escaped identifier scanning which breaks on these sequences.
  return codepoint == 0x00A0 ||  // NO-BREAK SPACE
         codepoint == 0x180E ||  // MONGOLIAN VOWEL SEPARATOR (Cf, not ID_Start/ID_Continue)
         codepoint == 0x2028 ||  // LINE SEPARATOR
         codepoint == 0x2029 ||  // PARAGRAPH SEPARATOR
         codepoint == 0x2E2F ||  // VERTICAL TILDE (not ID_Start/ID_Continue)
         codepoint == 0xFEFF;    // BOM
}

inline bool isIdentifierStartCodePoint(uint32_t codepoint) {
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

inline bool isIdentifierPartCodePoint(uint32_t codepoint) {
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

inline void appendUtf8(std::string& out, uint32_t codepoint) {
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

inline const std::unordered_map<std::string_view, TokenType> kLexerKeywords = {
  {"let", TokenType::Let},
  {"const", TokenType::Const},
  {"var", TokenType::Var},
  {"function", TokenType::Function},
  {"async", TokenType::Async},
  {"await", TokenType::Await},
  {"using", TokenType::Using},
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

}  // namespace lightjs
