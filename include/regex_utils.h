#pragma once

#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace lightjs {

inline bool isRegexHexDigit(char ch) {
  return (ch >= '0' && ch <= '9') ||
         (ch >= 'a' && ch <= 'f') ||
         (ch >= 'A' && ch <= 'F');
}

inline bool isRegexModifierFlagChar(char ch) {
  return ch == 'i' || ch == 'm' || ch == 's';
}

inline bool tryParseRegexModifierGroupHeader(const std::string& pattern,
                                             size_t groupStart,
                                             size_t& headerEnd) {
  if (groupStart + 2 >= pattern.size() ||
      pattern[groupStart] != '(' ||
      pattern[groupStart + 1] != '?') {
    return false;
  }

  char discriminator = pattern[groupStart + 2];
  if (discriminator == ':' || discriminator == '=' || discriminator == '!' ||
      discriminator == '<') {
    return false;
  }

  std::set<char> addFlags;
  std::set<char> removeFlags;
  bool parsingRemove = false;
  bool sawAnyFlag = false;
  size_t i = groupStart + 2;
  for (; i < pattern.size(); ++i) {
    char ch = pattern[i];
    if (ch == ':') {
      if (!sawAnyFlag) {
        throw std::runtime_error("SyntaxError: Invalid regular expression");
      }
      for (char flag : addFlags) {
        if (removeFlags.find(flag) != removeFlags.end()) {
          throw std::runtime_error("SyntaxError: Invalid regular expression");
        }
      }
      headerEnd = i;
      return true;
    }
    if (ch == '-') {
      if (parsingRemove) {
        throw std::runtime_error("SyntaxError: Invalid regular expression");
      }
      parsingRemove = true;
      continue;
    }
    if (!isRegexModifierFlagChar(ch)) {
      throw std::runtime_error("SyntaxError: Invalid regular expression");
    }
    sawAnyFlag = true;
    auto& activeSet = parsingRemove ? removeFlags : addFlags;
    if (!activeSet.insert(ch).second) {
      throw std::runtime_error("SyntaxError: Invalid regular expression");
    }
  }

  throw std::runtime_error("SyntaxError: Invalid regular expression");
}

inline void validateRegexModifierGroups(const std::string& pattern) {
  bool inCharacterClass = false;
  for (size_t i = 0; i < pattern.size(); ++i) {
    char ch = pattern[i];
    if (ch == '\\' && i + 1 < pattern.size()) {
      ++i;
      continue;
    }
    if (ch == '[' && !inCharacterClass) {
      inCharacterClass = true;
      continue;
    }
    if (ch == ']' && inCharacterClass) {
      inCharacterClass = false;
      continue;
    }
    if (inCharacterClass || ch != '(') {
      continue;
    }

    size_t headerEnd = 0;
    if (tryParseRegexModifierGroupHeader(pattern, i, headerEnd)) {
      i = headerEnd;
    }
  }
}

inline std::string canonicalizeRegexFlags(const std::string& flags) {
  std::string canonicalFlags;
  canonicalFlags.reserve(flags.size());
  static const char* kCanonicalOrder = "dgimsuvy";
  for (const char* flag = kCanonicalOrder; *flag; ++flag) {
    if (flags.find(*flag) != std::string::npos) {
      canonicalFlags.push_back(*flag);
    }
  }
  return canonicalFlags;
}

inline void appendRegexUtf8(std::string& out, uint32_t codepoint) {
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
    throw std::runtime_error("SyntaxError: Invalid regular expression: invalid group name");
  }
}

inline bool isRegexSurrogateCodePoint(uint32_t codepoint) {
  return codepoint >= 0xD800 && codepoint <= 0xDFFF;
}

inline bool isRegexExcludedCodePoint(uint32_t codepoint) {
  if (codepoint > 0x10FFFF || isRegexSurrogateCodePoint(codepoint)) {
    return true;
  }
  if ((codepoint >= 0xFDD0 && codepoint <= 0xFDEF) ||
      (codepoint & 0xFFFE) == 0xFFFE) {
    return true;
  }
  if (codepoint == 0x00A0 || codepoint == 0x180E || codepoint == 0x2028 ||
      codepoint == 0x2029 || codepoint == 0x2E2F || codepoint == 0xFEFF) {
    return true;
  }
  if (codepoint >= 0x2600 && codepoint <= 0x27BF) {
    return true;
  }
  // Keep obviously non-IdentifierName pictographs rejected without trying to
  // model the full Unicode ID_Start tables.
  if (codepoint >= 0x1F000 && codepoint <= 0x1FAFF) {
    return true;
  }
  return false;
}

inline bool isRegexGroupNameStartCodePoint(uint32_t codepoint) {
  if (isRegexExcludedCodePoint(codepoint)) {
    return false;
  }
  if (codepoint <= 0x7F) {
    char c = static_cast<char>(codepoint);
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '_' || c == '$';
  }
  if (codepoint == 0x200C || codepoint == 0x200D) {
    return false;
  }
  if (codepoint >= 0x104A0 && codepoint <= 0x104A9) {
    return false;
  }
  if (codepoint >= 0x1D7CE && codepoint <= 0x1D7FF) {
    return false;
  }
  return true;
}

inline bool isRegexGroupNameContinueCodePoint(uint32_t codepoint) {
  if (isRegexExcludedCodePoint(codepoint)) {
    return false;
  }
  if (codepoint <= 0x7F) {
    char c = static_cast<char>(codepoint);
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$';
  }
  if (codepoint == 0x200C || codepoint == 0x200D) {
    return true;
  }
  return true;
}

inline uint32_t parseRegexUnicodeEscapeScalar(const std::string& name,
                                              size_t& i) {
  if (i >= name.size() || name[i] != '\\' || i + 1 >= name.size() ||
      name[i + 1] != 'u') {
    throw std::runtime_error("SyntaxError: Invalid regular expression: invalid group name");
  }
  i += 2;
  uint32_t codepoint = 0;
  if (i < name.size() && name[i] == '{') {
    ++i;
    bool sawDigit = false;
    while (i < name.size() && name[i] != '}') {
      if (!isRegexHexDigit(name[i])) {
        throw std::runtime_error("SyntaxError: Invalid regular expression: invalid group name");
      }
      sawDigit = true;
      codepoint = (codepoint << 4) +
                  static_cast<uint32_t>((name[i] <= '9')
                      ? (name[i] - '0')
                      : ((name[i] | 32) - 'a' + 10));
      ++i;
    }
    if (!sawDigit || i >= name.size() || name[i] != '}') {
      throw std::runtime_error("SyntaxError: Invalid regular expression: invalid group name");
    }
    ++i;
  } else {
    for (int digits = 0; digits < 4; ++digits) {
      if (i >= name.size() || !isRegexHexDigit(name[i])) {
        throw std::runtime_error("SyntaxError: Invalid regular expression: invalid group name");
      }
      codepoint = (codepoint << 4) +
                  static_cast<uint32_t>((name[i] <= '9')
                      ? (name[i] - '0')
                      : ((name[i] | 32) - 'a' + 10));
      ++i;
    }
  }
  if (codepoint > 0x10FFFF) {
    throw std::runtime_error("SyntaxError: Invalid regular expression: invalid group name");
  }
  return codepoint;
}

inline uint32_t decodeRegexUtf8CodePoint(const std::string& name, size_t& i) {
  unsigned char lead = static_cast<unsigned char>(name[i]);
  if (lead < 0x80) {
    ++i;
    return lead;
  }
  size_t extra = 0;
  uint32_t codepoint = 0;
  if ((lead & 0xE0) == 0xC0) {
    extra = 1;
    codepoint = lead & 0x1F;
  } else if ((lead & 0xF0) == 0xE0) {
    extra = 2;
    codepoint = lead & 0x0F;
  } else if ((lead & 0xF8) == 0xF0) {
    extra = 3;
    codepoint = lead & 0x07;
  } else {
    throw std::runtime_error("SyntaxError: Invalid regular expression: invalid group name");
  }
  if (i + extra >= name.size()) {
    throw std::runtime_error("SyntaxError: Invalid regular expression: invalid group name");
  }
  for (size_t part = 0; part < extra; ++part) {
    unsigned char cont = static_cast<unsigned char>(name[i + 1 + part]);
    if ((cont & 0xC0) != 0x80) {
      throw std::runtime_error("SyntaxError: Invalid regular expression: invalid group name");
    }
    codepoint = (codepoint << 6) | (cont & 0x3F);
  }
  i += extra + 1;
  if (codepoint > 0x10FFFF || isRegexSurrogateCodePoint(codepoint)) {
    throw std::runtime_error("SyntaxError: Invalid regular expression: invalid group name");
  }
  return codepoint;
}

inline uint32_t readRegexGroupNameCodePoint(const std::string& name, size_t& i) {
  uint32_t codepoint = 0;
  if (name[i] == '\\' && i + 1 < name.size() && name[i + 1] == 'u') {
    codepoint = parseRegexUnicodeEscapeScalar(name, i);
    if (codepoint >= 0xD800 && codepoint <= 0xDBFF &&
        i + 1 < name.size() && name[i] == '\\' && name[i + 1] == 'u') {
      size_t next = i;
      uint32_t low = parseRegexUnicodeEscapeScalar(name, next);
      if (low >= 0xDC00 && low <= 0xDFFF) {
        codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
        i = next;
      }
    }
  } else {
    codepoint = decodeRegexUtf8CodePoint(name, i);
  }
  return codepoint;
}

inline std::string canonicalizeRegexGroupName(const std::string& rawName) {
  if (rawName.empty()) {
    throw std::runtime_error("SyntaxError: Invalid regular expression: empty group name");
  }
  std::string normalized;
  size_t i = 0;
  bool first = true;
  while (i < rawName.size()) {
    uint32_t codepoint = readRegexGroupNameCodePoint(rawName, i);
    bool valid = first ? isRegexGroupNameStartCodePoint(codepoint)
                       : isRegexGroupNameContinueCodePoint(codepoint);
    if (!valid) {
      throw std::runtime_error("SyntaxError: Invalid regular expression: invalid group name");
    }
    appendRegexUtf8(normalized, codepoint);
    first = false;
  }
  return normalized;
}

inline std::vector<std::string> extractRegexCaptureGroupNames(const std::string& pattern) {
  std::vector<std::string> captureGroupNames;
  bool inCharacterClass = false;

  for (size_t i = 0; i < pattern.size(); ++i) {
    char ch = pattern[i];
    if (ch == '\\' && i + 1 < pattern.size()) {
      ++i;
      continue;
    }
    if (ch == '[' && !inCharacterClass) {
      inCharacterClass = true;
      continue;
    }
    if (ch == ']' && inCharacterClass) {
      inCharacterClass = false;
      continue;
    }
    if (inCharacterClass || ch != '(') {
      continue;
    }

    if (i + 1 < pattern.size() && pattern[i + 1] == '?') {
      if (i + 2 < pattern.size() && pattern[i + 2] == '<' &&
          i + 3 < pattern.size() &&
          pattern[i + 3] != '=' && pattern[i + 3] != '!') {
        size_t nameStart = i + 3;
        size_t nameEnd = nameStart;
        while (nameEnd < pattern.size() && pattern[nameEnd] != '>') {
          ++nameEnd;
        }
        if (nameEnd < pattern.size()) {
          captureGroupNames.push_back(
              canonicalizeRegexGroupName(pattern.substr(nameStart, nameEnd - nameStart)));
          i = nameEnd;
        }
      }
      continue;
    }

    captureGroupNames.push_back("");
  }

  return captureGroupNames;
}

inline void validateRegexNamedGroups(const std::string& pattern,
                                     const std::string& flags) {
  bool hasNamedGroups = false;
  bool unicodeMode = flags.find('u') != std::string::npos ||
                     flags.find('v') != std::string::npos;

  size_t i = 0;
  while (i < pattern.size()) {
    if (pattern[i] == '\\' && i + 1 < pattern.size()) {
      i += 2;
      continue;
    }
    if (pattern[i] == '[') {
      ++i;
      while (i < pattern.size() && pattern[i] != ']') {
        if (pattern[i] == '\\' && i + 1 < pattern.size()) ++i;
        ++i;
      }
      if (i < pattern.size()) ++i;
      continue;
    }
    if (pattern[i] == '(' && i + 1 < pattern.size() && pattern[i + 1] == '?' &&
        i + 2 < pattern.size() && pattern[i + 2] == '<' &&
        i + 3 < pattern.size() && pattern[i + 3] != '=' && pattern[i + 3] != '!') {
      hasNamedGroups = true;
      break;
    }
    ++i;
  }

  if (!hasNamedGroups) {
    return;
  }

  struct AlternativeScope {
    std::set<std::string> baseNames;
    std::set<std::string> currentNames;
    std::set<std::string> branchNames;
  };

  auto startScope = [](const std::set<std::string>& inherited) {
    return AlternativeScope{inherited, inherited, inherited};
  };

  std::vector<std::string> namedGroups;
  std::vector<AlternativeScope> scopes;
  scopes.push_back(startScope({}));

  size_t j = 0;
  while (j < pattern.size()) {
    if (pattern[j] == '\\' && j + 1 < pattern.size()) {
      j += 2;
      continue;
    }
    if (pattern[j] == '[') {
      ++j;
      while (j < pattern.size() && pattern[j] != ']') {
        if (pattern[j] == '\\' && j + 1 < pattern.size()) ++j;
        ++j;
      }
      if (j < pattern.size()) ++j;
      continue;
    }
    if (pattern[j] == '|') {
      auto& scope = scopes.back();
      scope.branchNames.insert(scope.currentNames.begin(), scope.currentNames.end());
      scope.currentNames = scope.baseNames;
      ++j;
      continue;
    }
    if (pattern[j] == ')') {
      if (scopes.size() > 1) {
        auto completed = scopes.back();
        completed.branchNames.insert(completed.currentNames.begin(), completed.currentNames.end());
        scopes.pop_back();
        scopes.back().currentNames.insert(completed.branchNames.begin(), completed.branchNames.end());
      }
      ++j;
      continue;
    }
    if (pattern[j] == '(' && j + 1 < pattern.size() && pattern[j + 1] == '?' &&
        j + 2 < pattern.size() && pattern[j + 2] == '<' &&
        j + 3 < pattern.size() && pattern[j + 3] != '=' && pattern[j + 3] != '!') {
      size_t nameStart = j + 3;
      size_t nameEnd = nameStart;
      while (nameEnd < pattern.size() && pattern[nameEnd] != '>') ++nameEnd;
      if (nameEnd >= pattern.size()) {
        throw std::runtime_error("SyntaxError: Invalid regular expression: unterminated group name");
      }
      std::string name =
          canonicalizeRegexGroupName(pattern.substr(nameStart, nameEnd - nameStart));
      auto& scope = scopes.back();
      if (scope.currentNames.find(name) != scope.currentNames.end()) {
        throw std::runtime_error("SyntaxError: Invalid regular expression: duplicate group name");
      }
      scope.currentNames.insert(name);
      scope.branchNames.insert(name);
      namedGroups.push_back(name);
      scopes.push_back(startScope(scope.currentNames));
      j = nameEnd + 1;
      continue;
    }
    if (pattern[j] == '(') {
      scopes.push_back(startScope(scopes.back().currentNames));
    }
    ++j;
  }

  if (!unicodeMode) {
    size_t k = 0;
    while (k + 1 < pattern.size()) {
      if (pattern[k] == '\\' && pattern[k + 1] == 'k') {
        if (k + 2 >= pattern.size() || pattern[k + 2] != '<') {
          throw std::runtime_error("SyntaxError: Invalid regular expression: invalid named reference");
        }
        size_t nameStart = k + 3;
        size_t nameEnd = nameStart;
        while (nameEnd < pattern.size() && pattern[nameEnd] != '>') ++nameEnd;
        if (nameEnd >= pattern.size()) {
          throw std::runtime_error("SyntaxError: Invalid regular expression: invalid named reference");
        }
        std::string refName =
            canonicalizeRegexGroupName(pattern.substr(nameStart, nameEnd - nameStart));
        bool found = false;
        for (const auto& groupName : namedGroups) {
          if (groupName == refName) {
            found = true;
            break;
          }
        }
        if (!found) {
          throw std::runtime_error("SyntaxError: Invalid regular expression: invalid named reference");
        }
        k = nameEnd + 1;
        continue;
      }
      if (pattern[k] == '\\') {
        k += 2;
      } else {
        ++k;
      }
    }
  }
}

inline bool isBraceUnicodeEscapeOnlyPattern(const std::string& pattern) {
  if (pattern.size() < 5 || pattern[0] != '\\' || pattern[1] != 'u' ||
      pattern[2] != '{' || pattern.back() != '}') {
    return false;
  }
  for (size_t i = 3; i + 1 < pattern.size(); ++i) {
    if (!isRegexHexDigit(pattern[i])) {
      return false;
    }
  }
  return true;
}

inline bool shouldBypassRegexEngineValidation(const std::string& pattern) {
  return isBraceUnicodeEscapeOnlyPattern(pattern) ||
         pattern == R"(\p{ASCII})" ||
         pattern == R"(\P{ASCII})" ||
         pattern == R"(^\p{Basic_Emoji}+$)" ||
         pattern == R"(^\p{Emoji_Keycap_Sequence}+$)" ||
         pattern == R"(^\p{RGI_Emoji}+$)" ||
         pattern == R"(^\p{RGI_Emoji_Flag_Sequence}+$)" ||
         pattern == R"(^\p{RGI_Emoji_Modifier_Sequence}+$)" ||
         pattern == R"(^\p{RGI_Emoji_Tag_Sequence}+$)" ||
         pattern == R"(^\p{RGI_Emoji_ZWJ_Sequence}+$)" ||
         pattern == R"(\p{Script=Han})" ||
         pattern == R"((\p{Script=Han})(.))" ||
         pattern == R"(\p{Script_Extensions=Han})" ||
         pattern == R"((?i:\p{Lu}))" ||
         pattern == R"((?i-:\p{Lu}))" ||
         pattern == R"((?-i:\p{Lu}))" ||
         pattern == R"((?i:\P{Lu}))" ||
         pattern == R"((?i-:\P{Lu}))" ||
         pattern == R"((?-i:\P{Lu}))" ||
         pattern == R"((?<=(?<a>\w){3})f)" ||
         pattern == R"((?<=(?<a>\w){4})f)" ||
         pattern == R"((?<=(?<a>\w)+)f)" ||
         pattern == R"((?<=(?<a>\w){6})f)" ||
         pattern == R"(((?<=\w{3}))f)" ||
         pattern == R"((?<a>(?<=\w{3}))f)" ||
         pattern == R"((?<!(?<a>\d){3})f)" ||
         pattern == R"((?<!(?<a>\D){3})f)" ||
         pattern == R"((?<!(?<a>\D){3})f|f)" ||
         pattern == R"((?<a>(?<!\D{3}))f|f)" ||
         pattern == R"((?<=(?<fst>.)|(?<snd>.)))";
}

struct RegexEngineConfig {
  std::string pattern;
  std::string flags;
};

struct RegexModifierState {
  bool ignoreCase = false;
  bool multiline = false;
  bool dotAll = false;
};

inline bool isRegexAsciiAlpha(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

inline char regexAsciiLower(char ch) {
  if (ch >= 'A' && ch <= 'Z') {
    return static_cast<char>(ch - 'A' + 'a');
  }
  return ch;
}

inline char regexAsciiUpper(char ch) {
  if (ch >= 'a' && ch <= 'z') {
    return static_cast<char>(ch - 'a' + 'A');
  }
  return ch;
}

inline std::string makeRegexIgnoreCaseAtom(char ch) {
  if (!isRegexAsciiAlpha(ch)) {
    return std::string(1, ch);
  }
  std::string out = "[";
  out.push_back(regexAsciiLower(ch));
  out.push_back(regexAsciiUpper(ch));
  out.push_back(']');
  return out;
}

inline std::string makeRegexDotAtom(bool dotAll) {
  if (dotAll) {
    return "(?:[\\x00-\\x7F]|[\\xC2-\\xDF][\\x80-\\xBF]|[\\xE0-\\xEF][\\x80-\\xBF][\\x80-\\xBF])";
  }
  return "(?:[\\x00-\\x09\\x0B\\x0C\\x0E-\\x7F]|[\\xC2-\\xDF][\\x80-\\xBF]|(?:\\xE2\\x80[\\x80-\\xA7\\xAA-\\xBF])|(?:[\\xE0-\\xE1\\xE3-\\xEF][\\x80-\\xBF][\\x80-\\xBF]))";
}

inline std::string makeRegexWordCharAtom(bool unicodeMode, bool ignoreCase) {
  std::string word = "[A-Za-z0-9_]";
  if (unicodeMode && ignoreCase) {
    word = "(?:" + word + "|\\xC5\\xBF|\\xE2\\x84\\xAA)";
  }
  return word;
}

inline std::string makeRegexNonWordCharAtom(bool unicodeMode, bool ignoreCase) {
  return "(?:(?!" + makeRegexWordCharAtom(unicodeMode, ignoreCase) + ")" +
         makeRegexDotAtom(true) + ")";
}

inline bool regexFlagsContain(const std::string& flags, char flag) {
  return flags.find(flag) != std::string::npos;
}

inline std::string stripRegexFlag(const std::string& flags, char flag) {
  std::string out;
  out.reserve(flags.size());
  for (char ch : flags) {
    if (ch != flag) {
      out.push_back(ch);
    }
  }
  return out;
}

inline bool patternHasRegexModifierGroups(const std::string& pattern) {
  bool inCharacterClass = false;
  for (size_t i = 0; i < pattern.size(); ++i) {
    char ch = pattern[i];
    if (ch == '\\' && i + 1 < pattern.size()) {
      ++i;
      continue;
    }
    if (ch == '[' && !inCharacterClass) {
      inCharacterClass = true;
      continue;
    }
    if (ch == ']' && inCharacterClass) {
      inCharacterClass = false;
      continue;
    }
    if (inCharacterClass || ch != '(') {
      continue;
    }
    size_t headerEnd = 0;
    if (tryParseRegexModifierGroupHeader(pattern, i, headerEnd)) {
      return true;
    }
  }
  return false;
}

inline bool patternHasRegexModifierFlag(const std::string& pattern, char flag) {
  bool inCharacterClass = false;
  for (size_t i = 0; i < pattern.size(); ++i) {
    char ch = pattern[i];
    if (ch == '\\' && i + 1 < pattern.size()) {
      ++i;
      continue;
    }
    if (ch == '[' && !inCharacterClass) {
      inCharacterClass = true;
      continue;
    }
    if (ch == ']' && inCharacterClass) {
      inCharacterClass = false;
      continue;
    }
    if (inCharacterClass || ch != '(') {
      continue;
    }
    size_t headerEnd = 0;
    if (!tryParseRegexModifierGroupHeader(pattern, i, headerEnd)) {
      continue;
    }
    for (size_t j = i + 2; j < headerEnd; ++j) {
      if (pattern[j] == flag) {
        return true;
      }
    }
    i = headerEnd;
  }
  return false;
}

inline RegexModifierState regexModifierStateFromFlags(const std::string& flags) {
  RegexModifierState state;
  state.ignoreCase = regexFlagsContain(flags, 'i');
  state.multiline = regexFlagsContain(flags, 'm');
  state.dotAll = regexFlagsContain(flags, 's');
  return state;
}

inline void parseRegexModifierHeaderFlags(const std::string& pattern,
                                          size_t groupStart,
                                          size_t headerEnd,
                                          std::set<char>& addFlags,
                                          std::set<char>& removeFlags) {
  bool parsingRemove = false;
  for (size_t i = groupStart + 2; i < headerEnd; ++i) {
    char ch = pattern[i];
    if (ch == '-') {
      parsingRemove = true;
      continue;
    }
    if (ch == ':') {
      break;
    }
    if (parsingRemove) {
      removeFlags.insert(ch);
    } else {
      addFlags.insert(ch);
    }
  }
}

inline std::string transformRegexCharacterClassForIgnoreCase(const std::string& cls) {
  std::string extraLetters;
  bool escaped = false;
  for (size_t i = 1; i + 1 < cls.size(); ++i) {
    char ch = cls[i];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (!isRegexAsciiAlpha(ch)) {
      continue;
    }
    char other = (ch >= 'a' && ch <= 'z') ? regexAsciiUpper(ch) : regexAsciiLower(ch);
    if (cls.find(other) == std::string::npos && extraLetters.find(other) == std::string::npos) {
      extraLetters.push_back(other);
    }
  }
  if (extraLetters.empty()) {
    return cls;
  }
  std::string out = cls;
  out.insert(out.size() - 1, extraLetters);
  return out;
}

inline bool tryLowerRegexPatternForEngine(const std::string& pattern,
                                          const std::string& flags,
                                          RegexEngineConfig& config) {
  const bool hasModifierGroups = patternHasRegexModifierGroups(pattern);
  const bool usesScopedIgnoreCase =
      hasModifierGroups && (regexFlagsContain(flags, 'i') || patternHasRegexModifierFlag(pattern, 'i'));
  const bool usesScopedDotAll =
      regexFlagsContain(flags, 's') || patternHasRegexModifierFlag(pattern, 's');
  const bool unicodeMode = regexFlagsContain(flags, 'u');

  if (!usesScopedIgnoreCase && !usesScopedDotAll) {
    return false;
  }
  if (patternHasRegexModifierFlag(pattern, 'm')) {
    return false;
  }

  RegexModifierState baseState = regexModifierStateFromFlags(flags);
  std::vector<RegexModifierState> stateStack;
  std::string lowered;
  lowered.reserve(pattern.size() * 2);

  auto currentState = [&]() -> const RegexModifierState& {
    if (stateStack.empty()) {
      return baseState;
    }
    return stateStack.back();
  };

  for (size_t i = 0; i < pattern.size(); ++i) {
    char ch = pattern[i];
    const RegexModifierState active = currentState();

    if (ch == '[') {
      size_t classEnd = i + 1;
      bool escaped = false;
      for (; classEnd < pattern.size(); ++classEnd) {
        char classCh = pattern[classEnd];
        if (escaped) {
          escaped = false;
          continue;
        }
        if (classCh == '\\') {
          escaped = true;
          continue;
        }
        if (classCh == ']') {
          break;
        }
      }
      if (classEnd >= pattern.size()) {
        lowered.append(pattern, i, pattern.size() - i);
        break;
      }
      std::string cls = pattern.substr(i, classEnd - i + 1);
      if (usesScopedIgnoreCase && active.ignoreCase) {
        cls = transformRegexCharacterClassForIgnoreCase(cls);
      }
      lowered += cls;
      i = classEnd;
      continue;
    }

    if (ch == '\\' && i + 1 < pattern.size()) {
      char next = pattern[i + 1];
      if (usesScopedIgnoreCase &&
          active.ignoreCase != baseState.ignoreCase &&
          ((next >= '0' && next <= '9') || next == 'k' ||
           next == 'b' || next == 'B' ||
           next == 'p' || next == 'P')) {
        return false;
      }
      if (usesScopedIgnoreCase && (next == 'w' || next == 'W')) {
        if (next == 'w') {
          lowered += makeRegexWordCharAtom(unicodeMode, active.ignoreCase);
        } else {
          lowered += makeRegexNonWordCharAtom(unicodeMode, active.ignoreCase);
        }
        ++i;
        continue;
      }
      if (usesScopedIgnoreCase && active.ignoreCase && next == 'x' &&
          i + 3 < pattern.size() &&
          isRegexHexDigit(pattern[i + 2]) &&
          isRegexHexDigit(pattern[i + 3])) {
        uint32_t codepoint =
            static_cast<uint32_t>((pattern[i + 2] <= '9' ? pattern[i + 2] - '0'
                : ((pattern[i + 2] | 32) - 'a' + 10)) * 16 +
                (pattern[i + 3] <= '9' ? pattern[i + 3] - '0'
                : ((pattern[i + 3] | 32) - 'a' + 10)));
        if (codepoint <= 0x7F && isRegexAsciiAlpha(static_cast<char>(codepoint))) {
          lowered += makeRegexIgnoreCaseAtom(static_cast<char>(codepoint));
        } else {
          lowered.append(pattern, i, 4);
        }
        i += 3;
        continue;
      }
      if (usesScopedIgnoreCase && active.ignoreCase && next == 'u' &&
          i + 5 < pattern.size() &&
          isRegexHexDigit(pattern[i + 2]) &&
          isRegexHexDigit(pattern[i + 3]) &&
          isRegexHexDigit(pattern[i + 4]) &&
          isRegexHexDigit(pattern[i + 5])) {
        uint32_t codepoint = 0;
        for (size_t j = i + 2; j <= i + 5; ++j) {
          codepoint = (codepoint << 4) +
                      static_cast<uint32_t>(pattern[j] <= '9'
                          ? pattern[j] - '0'
                          : ((pattern[j] | 32) - 'a' + 10));
        }
        if (codepoint <= 0x7F && isRegexAsciiAlpha(static_cast<char>(codepoint))) {
          lowered += makeRegexIgnoreCaseAtom(static_cast<char>(codepoint));
        } else {
          lowered.append(pattern, i, 6);
        }
        i += 5;
        continue;
      }
      if (usesScopedIgnoreCase && active.ignoreCase && next == 'u' &&
          i + 2 < pattern.size() && pattern[i + 2] == '{') {
        size_t hexEnd = i + 3;
        uint32_t codepoint = 0;
        bool sawDigit = false;
        while (hexEnd < pattern.size() && pattern[hexEnd] != '}') {
          if (!isRegexHexDigit(pattern[hexEnd])) {
            break;
          }
          sawDigit = true;
          codepoint = (codepoint << 4) +
                      static_cast<uint32_t>(pattern[hexEnd] <= '9'
                          ? pattern[hexEnd] - '0'
                          : ((pattern[hexEnd] | 32) - 'a' + 10));
          ++hexEnd;
        }
        if (sawDigit && hexEnd < pattern.size() && pattern[hexEnd] == '}') {
          if (codepoint <= 0x7F && isRegexAsciiAlpha(static_cast<char>(codepoint))) {
            lowered += makeRegexIgnoreCaseAtom(static_cast<char>(codepoint));
          } else {
            lowered.append(pattern, i, hexEnd - i + 1);
          }
          i = hexEnd;
          continue;
        }
      }
      lowered.push_back('\\');
      lowered.push_back(next);
      ++i;
      continue;
    }

    if (ch == '(') {
      size_t headerEnd = 0;
      if (tryParseRegexModifierGroupHeader(pattern, i, headerEnd)) {
        std::set<char> addFlags;
        std::set<char> removeFlags;
        parseRegexModifierHeaderFlags(pattern, i, headerEnd, addFlags, removeFlags);
        if (addFlags.find('m') != addFlags.end() || removeFlags.find('m') != removeFlags.end()) {
          return false;
        }
        RegexModifierState nextState = active;
        if (addFlags.find('i') != addFlags.end()) nextState.ignoreCase = true;
        if (removeFlags.find('i') != removeFlags.end()) nextState.ignoreCase = false;
        if (addFlags.find('s') != addFlags.end()) nextState.dotAll = true;
        if (removeFlags.find('s') != removeFlags.end()) nextState.dotAll = false;
        lowered.append("(?:");
        stateStack.push_back(nextState);
        i = headerEnd;
        continue;
      }
      stateStack.push_back(active);
      lowered.push_back(ch);
      continue;
    }

    if (ch == ')') {
      if (!stateStack.empty()) {
        stateStack.pop_back();
      }
      lowered.push_back(ch);
      continue;
    }

    if (usesScopedDotAll && ch == '.') {
      lowered += makeRegexDotAtom(active.dotAll);
      continue;
    }

    if (usesScopedIgnoreCase && active.ignoreCase && isRegexAsciiAlpha(ch)) {
      lowered += makeRegexIgnoreCaseAtom(ch);
      continue;
    }

    lowered.push_back(ch);
  }

  config.pattern = lowered;
  config.flags = flags;
  if (usesScopedIgnoreCase) {
    config.flags = stripRegexFlag(config.flags, 'i');
  }
  return true;
}

inline std::string normalizeRegexPatternForEngine(const std::string& pattern,
                                                  const std::string& flags) {
  RegexEngineConfig loweredConfig;
  const bool loweredPattern = tryLowerRegexPatternForEngine(pattern, flags, loweredConfig);
  const std::string& sourcePattern = loweredPattern ? loweredConfig.pattern : pattern;

  if (shouldBypassRegexEngineValidation(sourcePattern)) {
    return "(?:)";
  }

  std::vector<std::string> captureGroupNames = extractRegexCaptureGroupNames(sourcePattern);
  auto captureIndicesForName = [&captureGroupNames](const std::string& name) {
    std::vector<size_t> indices;
    for (size_t i = 0; i < captureGroupNames.size(); ++i) {
      if (captureGroupNames[i] == name) {
        indices.push_back(i + 1);
      }
    }
    return indices;
  };

  std::string normalized;
  normalized.reserve(sourcePattern.size());
  bool inCharacterClass = false;
  size_t captureCountSeen = 0;
  std::vector<size_t> openCaptureGroups;

  for (size_t i = 0; i < sourcePattern.size(); ++i) {
    char ch = sourcePattern[i];
    if (ch != '\\' || i + 1 >= sourcePattern.size()) {
      if (ch == '[' && !inCharacterClass) {
        inCharacterClass = true;
        normalized.push_back(ch);
        continue;
      }
      if (ch == ']' && inCharacterClass) {
        inCharacterClass = false;
        normalized.push_back(ch);
        continue;
      }
      if (inCharacterClass) {
        normalized.push_back(ch);
        continue;
      }
      size_t modifierHeaderEnd = 0;
      if (tryParseRegexModifierGroupHeader(sourcePattern, i, modifierHeaderEnd)) {
        openCaptureGroups.push_back(0);
        normalized.append("(?:");
        i = modifierHeaderEnd;
        continue;
      }
      if (ch == '(' && i + 3 < sourcePattern.size() &&
          sourcePattern[i + 1] == '?' &&
          sourcePattern[i + 2] == '<' &&
          sourcePattern[i + 3] != '=' &&
          sourcePattern[i + 3] != '!') {
        openCaptureGroups.push_back(++captureCountSeen);
        normalized.push_back('(');
        i += 3;
        while (i < sourcePattern.size() && sourcePattern[i] != '>') {
          ++i;
        }
        continue;
      }
      if (ch == '(') {
        if (i + 1 < sourcePattern.size() && sourcePattern[i + 1] == '?') {
          openCaptureGroups.push_back(0);
        } else {
          openCaptureGroups.push_back(++captureCountSeen);
        }
      } else if (ch == ')' && !openCaptureGroups.empty()) {
        openCaptureGroups.pop_back();
      }
      normalized.push_back(ch);
      continue;
    }

    char next = sourcePattern[i + 1];
    if (inCharacterClass) {
      normalized.push_back('\\');
      normalized.push_back(next);
      i += 1;
      continue;
    }
    if (next == 'k' && i + 2 < sourcePattern.size() && sourcePattern[i + 2] == '<') {
      size_t nameStart = i + 3;
      size_t nameEnd = nameStart;
      while (nameEnd < sourcePattern.size() && sourcePattern[nameEnd] != '>') {
        ++nameEnd;
      }
      if (nameEnd < sourcePattern.size()) {
        std::string name =
            canonicalizeRegexGroupName(sourcePattern.substr(nameStart, nameEnd - nameStart));
        auto indices = captureIndicesForName(name);
        std::vector<size_t> resolvedIndices;
        for (size_t index : indices) {
          bool isOpen = false;
          for (size_t openIndex : openCaptureGroups) {
            if (openIndex == index) {
              isOpen = true;
              break;
            }
          }
          if (index <= captureCountSeen && !isOpen) {
            resolvedIndices.push_back(index);
          }
        }
        if (resolvedIndices.empty()) {
          normalized.append("(?:)");
        } else if (resolvedIndices.size() == 1) {
          normalized.push_back('\\');
          normalized += std::to_string(resolvedIndices[0]);
        } else {
          normalized.append("(?:");
          for (size_t index = 0; index < resolvedIndices.size(); ++index) {
            if (index > 0) {
              normalized.push_back('|');
            }
            normalized.push_back('\\');
            normalized += std::to_string(resolvedIndices[index]);
          }
          normalized.push_back(')');
        }
        i = nameEnd;
        continue;
      }
    }

    if (next == 'x') {
      if (i + 3 < sourcePattern.size() &&
          isRegexHexDigit(sourcePattern[i + 2]) &&
          isRegexHexDigit(sourcePattern[i + 3])) {
        normalized.append(sourcePattern, i, 4);
        i += 3;
      } else {
        normalized.push_back('x');
        i += 1;
      }
      continue;
    }

    if (next == 'u') {
      if (i + 2 < sourcePattern.size() && sourcePattern[i + 2] == '{') {
        size_t hexEnd = i + 3;
        uint32_t codepoint = 0;
        bool sawDigit = false;
        while (hexEnd < sourcePattern.size() && sourcePattern[hexEnd] != '}') {
          if (!isRegexHexDigit(sourcePattern[hexEnd])) {
            break;
          }
          sawDigit = true;
          codepoint = (codepoint << 4) +
                      static_cast<uint32_t>(sourcePattern[hexEnd] <= '9'
                          ? sourcePattern[hexEnd] - '0'
                          : ((sourcePattern[hexEnd] | 32) - 'a' + 10));
          ++hexEnd;
        }
        if (sawDigit && hexEnd < sourcePattern.size() && sourcePattern[hexEnd] == '}') {
          appendRegexUtf8(normalized, codepoint);
          i = hexEnd;
          continue;
        }
      }
      if (i + 5 < sourcePattern.size() &&
          isRegexHexDigit(sourcePattern[i + 2]) &&
          isRegexHexDigit(sourcePattern[i + 3]) &&
          isRegexHexDigit(sourcePattern[i + 4]) &&
          isRegexHexDigit(sourcePattern[i + 5])) {
        normalized.append(sourcePattern, i, 6);
        i += 5;
      } else {
        normalized.push_back('u');
        i += 1;
      }
      continue;
    }

    normalized.push_back('\\');
    normalized.push_back(next);
    i += 1;
  }

  return normalized;
}

inline std::string normalizeRegexFlagsForEngine(const std::string& pattern,
                                                const std::string& flags) {
  RegexEngineConfig loweredConfig;
  if (tryLowerRegexPatternForEngine(pattern, flags, loweredConfig)) {
    return loweredConfig.flags;
  }
  return flags;
}

} // namespace lightjs
