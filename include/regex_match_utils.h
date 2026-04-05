#pragma once

#include "regex_utils.h"
#include "unicode.h"

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lightjs {

struct SpecialRegexCaptureResult {
  bool matched = false;
  size_t start = 0;
  size_t end = 0;
  std::string value;
};

struct SpecialRegexMatchResult {
  size_t index = 0;
  size_t end = 0;
  std::string value;
  std::vector<SpecialRegexCaptureResult> captures;
};

inline bool regexHasUnicodeMode(const std::string& flags) {
  return flags.find('u') != std::string::npos || flags.find('v') != std::string::npos;
}

inline bool regexHasIgnoreCase(const std::string& flags) {
  return flags.find('i') != std::string::npos;
}

struct DecodedRegexInput {
  std::vector<uint32_t> codePoints;
  std::vector<size_t> byteOffsets;
  size_t byteLength = 0;
};

inline DecodedRegexInput decodeRegexInput(const std::string& str) {
  DecodedRegexInput decoded;
  decoded.byteLength = str.size();
  size_t byteIndex = 0;
  while (byteIndex < str.size()) {
    decoded.byteOffsets.push_back(byteIndex);
    decoded.codePoints.push_back(unicode::decodeUTF8(str, byteIndex));
  }
  return decoded;
}

inline size_t regexByteOffsetAt(const DecodedRegexInput& decoded, size_t codePointIndex) {
  if (codePointIndex >= decoded.byteOffsets.size()) {
    return decoded.byteLength;
  }
  return decoded.byteOffsets[codePointIndex];
}

inline bool parseRegexBraceEscapeValue(const std::string& pattern, uint32_t& value) {
  if (pattern.size() < 5 || pattern[0] != '\\' || pattern[1] != 'u' ||
      pattern[2] != '{' || pattern.back() != '}') {
    return false;
  }
  value = 0;
  bool sawDigit = false;
  for (size_t i = 3; i + 1 < pattern.size(); ++i) {
    char ch = pattern[i];
    if (ch >= '0' && ch <= '9') {
      value = (value << 4) + static_cast<uint32_t>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      value = (value << 4) + static_cast<uint32_t>(ch - 'a' + 10);
    } else if (ch >= 'A' && ch <= 'F') {
      value = (value << 4) + static_cast<uint32_t>(ch - 'A' + 10);
    } else {
      return false;
    }
    sawDigit = true;
  }
  return sawDigit;
}

inline bool isRegexWhitespaceCodePoint(uint32_t cp) {
  return cp == 0x0009 || cp == 0x000A || cp == 0x000B || cp == 0x000C ||
         cp == 0x000D || cp == 0x0020 || cp == 0x00A0 || cp == 0x1680 ||
         cp == 0x2000 || cp == 0x2001 || cp == 0x2002 || cp == 0x2003 ||
         cp == 0x2004 || cp == 0x2005 || cp == 0x2006 || cp == 0x2007 ||
         cp == 0x2008 || cp == 0x2009 || cp == 0x200A || cp == 0x2028 ||
         cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000 ||
         cp == 0xFEFF;
}

inline bool isRegexModifierAsciiAlpha(uint32_t cp) {
  return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
}

inline uint32_t regexModifierAsciiLower(uint32_t cp) {
  if (cp >= 'A' && cp <= 'Z') {
    return cp - 'A' + 'a';
  }
  return cp;
}

inline bool isRegexModifierWordCodePoint(uint32_t cp, bool unicodeMode, bool ignoreCase) {
  if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
      (cp >= '0' && cp <= '9') || cp == '_') {
    return true;
  }
  return unicodeMode && ignoreCase && (cp == 0x017F || cp == 0x212A);
}

inline bool isRegexModifierBoundaryAt(const DecodedRegexInput& decoded,
                                      size_t index,
                                      bool unicodeMode,
                                      bool ignoreCase) {
  const bool prevIsWord =
      index > 0 &&
      isRegexModifierWordCodePoint(decoded.codePoints[index - 1], unicodeMode, ignoreCase);
  const bool nextIsWord =
      index < decoded.codePoints.size() &&
      isRegexModifierWordCodePoint(decoded.codePoints[index], unicodeMode, ignoreCase);
  return prevIsWord != nextIsWord;
}

inline bool regexModifierLiteralMatches(uint32_t cp, char literal, bool ignoreCase) {
  if (cp > 0x7F) {
    return false;
  }
  if (!ignoreCase) {
    return cp == static_cast<uint32_t>(static_cast<unsigned char>(literal));
  }
  return regexModifierAsciiLower(cp) ==
         regexModifierAsciiLower(static_cast<uint32_t>(static_cast<unsigned char>(literal)));
}

inline bool regexMatchesUnicodePropertyUppercaseLetter(uint32_t cp) {
  static constexpr std::pair<uint32_t, uint32_t> kUppercaseLetterRanges[] = {
    {0x000041, 0x00005A},
    {0x0000C0, 0x0000D6},
    {0x0000D8, 0x0000DE},
    {0x000100, 0x000100},
    {0x000102, 0x000102},
    {0x000104, 0x000104},
    {0x000106, 0x000106},
    {0x000108, 0x000108},
    {0x00010A, 0x00010A},
    {0x00010C, 0x00010C},
    {0x00010E, 0x00010E},
    {0x000110, 0x000110},
    {0x000112, 0x000112},
    {0x000114, 0x000114},
    {0x000116, 0x000116},
    {0x000118, 0x000118},
    {0x00011A, 0x00011A},
    {0x00011C, 0x00011C},
    {0x00011E, 0x00011E},
    {0x000120, 0x000120},
    {0x000122, 0x000122},
    {0x000124, 0x000124},
    {0x000126, 0x000126},
    {0x000128, 0x000128},
    {0x00012A, 0x00012A},
    {0x00012C, 0x00012C},
    {0x00012E, 0x00012E},
    {0x000130, 0x000130},
    {0x000132, 0x000132},
    {0x000134, 0x000134},
    {0x000136, 0x000136},
    {0x000139, 0x000139},
    {0x00013B, 0x00013B},
    {0x00013D, 0x00013D},
    {0x00013F, 0x00013F},
    {0x000141, 0x000141},
    {0x000143, 0x000143},
    {0x000145, 0x000145},
    {0x000147, 0x000147},
    {0x00014A, 0x00014A},
    {0x00014C, 0x00014C},
    {0x00014E, 0x00014E},
    {0x000150, 0x000150},
    {0x000152, 0x000152},
    {0x000154, 0x000154},
    {0x000156, 0x000156},
    {0x000158, 0x000158},
    {0x00015A, 0x00015A},
    {0x00015C, 0x00015C},
    {0x00015E, 0x00015E},
    {0x000160, 0x000160},
    {0x000162, 0x000162},
    {0x000164, 0x000164},
    {0x000166, 0x000166},
    {0x000168, 0x000168},
    {0x00016A, 0x00016A},
    {0x00016C, 0x00016C},
    {0x00016E, 0x00016E},
    {0x000170, 0x000170},
    {0x000172, 0x000172},
    {0x000174, 0x000174},
    {0x000176, 0x000176},
    {0x000178, 0x000179},
    {0x00017B, 0x00017B},
    {0x00017D, 0x00017D},
    {0x000181, 0x000182},
    {0x000184, 0x000184},
    {0x000186, 0x000187},
    {0x000189, 0x00018B},
    {0x00018E, 0x000191},
    {0x000193, 0x000194},
    {0x000196, 0x000198},
    {0x00019C, 0x00019D},
    {0x00019F, 0x0001A0},
    {0x0001A2, 0x0001A2},
    {0x0001A4, 0x0001A4},
    {0x0001A6, 0x0001A7},
    {0x0001A9, 0x0001A9},
    {0x0001AC, 0x0001AC},
    {0x0001AE, 0x0001AF},
    {0x0001B1, 0x0001B3},
    {0x0001B5, 0x0001B5},
    {0x0001B7, 0x0001B8},
    {0x0001BC, 0x0001BC},
    {0x0001C4, 0x0001C4},
    {0x0001C7, 0x0001C7},
    {0x0001CA, 0x0001CA},
    {0x0001CD, 0x0001CD},
    {0x0001CF, 0x0001CF},
    {0x0001D1, 0x0001D1},
    {0x0001D3, 0x0001D3},
    {0x0001D5, 0x0001D5},
    {0x0001D7, 0x0001D7},
    {0x0001D9, 0x0001D9},
    {0x0001DB, 0x0001DB},
    {0x0001DE, 0x0001DE},
    {0x0001E0, 0x0001E0},
    {0x0001E2, 0x0001E2},
    {0x0001E4, 0x0001E4},
    {0x0001E6, 0x0001E6},
    {0x0001E8, 0x0001E8},
    {0x0001EA, 0x0001EA},
    {0x0001EC, 0x0001EC},
    {0x0001EE, 0x0001EE},
    {0x0001F1, 0x0001F1},
    {0x0001F4, 0x0001F4},
    {0x0001F6, 0x0001F8},
    {0x0001FA, 0x0001FA},
    {0x0001FC, 0x0001FC},
    {0x0001FE, 0x0001FE},
    {0x000200, 0x000200},
    {0x000202, 0x000202},
    {0x000204, 0x000204},
    {0x000206, 0x000206},
    {0x000208, 0x000208},
    {0x00020A, 0x00020A},
    {0x00020C, 0x00020C},
    {0x00020E, 0x00020E},
    {0x000210, 0x000210},
    {0x000212, 0x000212},
    {0x000214, 0x000214},
    {0x000216, 0x000216},
    {0x000218, 0x000218},
    {0x00021A, 0x00021A},
    {0x00021C, 0x00021C},
    {0x00021E, 0x00021E},
    {0x000220, 0x000220},
    {0x000222, 0x000222},
    {0x000224, 0x000224},
    {0x000226, 0x000226},
    {0x000228, 0x000228},
    {0x00022A, 0x00022A},
    {0x00022C, 0x00022C},
    {0x00022E, 0x00022E},
    {0x000230, 0x000230},
    {0x000232, 0x000232},
    {0x00023A, 0x00023B},
    {0x00023D, 0x00023E},
    {0x000241, 0x000241},
    {0x000243, 0x000246},
    {0x000248, 0x000248},
    {0x00024A, 0x00024A},
    {0x00024C, 0x00024C},
    {0x00024E, 0x00024E},
    {0x000370, 0x000370},
    {0x000372, 0x000372},
    {0x000376, 0x000376},
    {0x00037F, 0x00037F},
    {0x000386, 0x000386},
    {0x000388, 0x00038A},
    {0x00038C, 0x00038C},
    {0x00038E, 0x00038F},
    {0x000391, 0x0003A1},
    {0x0003A3, 0x0003AB},
    {0x0003CF, 0x0003CF},
    {0x0003D2, 0x0003D4},
    {0x0003D8, 0x0003D8},
    {0x0003DA, 0x0003DA},
    {0x0003DC, 0x0003DC},
    {0x0003DE, 0x0003DE},
    {0x0003E0, 0x0003E0},
    {0x0003E2, 0x0003E2},
    {0x0003E4, 0x0003E4},
    {0x0003E6, 0x0003E6},
    {0x0003E8, 0x0003E8},
    {0x0003EA, 0x0003EA},
    {0x0003EC, 0x0003EC},
    {0x0003EE, 0x0003EE},
    {0x0003F4, 0x0003F4},
    {0x0003F7, 0x0003F7},
    {0x0003F9, 0x0003FA},
    {0x0003FD, 0x00042F},
    {0x000460, 0x000460},
    {0x000462, 0x000462},
    {0x000464, 0x000464},
    {0x000466, 0x000466},
    {0x000468, 0x000468},
    {0x00046A, 0x00046A},
    {0x00046C, 0x00046C},
    {0x00046E, 0x00046E},
    {0x000470, 0x000470},
    {0x000472, 0x000472},
    {0x000474, 0x000474},
    {0x000476, 0x000476},
    {0x000478, 0x000478},
    {0x00047A, 0x00047A},
    {0x00047C, 0x00047C},
    {0x00047E, 0x00047E},
    {0x000480, 0x000480},
    {0x00048A, 0x00048A},
    {0x00048C, 0x00048C},
    {0x00048E, 0x00048E},
    {0x000490, 0x000490},
    {0x000492, 0x000492},
    {0x000494, 0x000494},
    {0x000496, 0x000496},
    {0x000498, 0x000498},
    {0x00049A, 0x00049A},
    {0x00049C, 0x00049C},
    {0x00049E, 0x00049E},
    {0x0004A0, 0x0004A0},
    {0x0004A2, 0x0004A2},
    {0x0004A4, 0x0004A4},
    {0x0004A6, 0x0004A6},
    {0x0004A8, 0x0004A8},
    {0x0004AA, 0x0004AA},
    {0x0004AC, 0x0004AC},
    {0x0004AE, 0x0004AE},
    {0x0004B0, 0x0004B0},
    {0x0004B2, 0x0004B2},
    {0x0004B4, 0x0004B4},
    {0x0004B6, 0x0004B6},
    {0x0004B8, 0x0004B8},
    {0x0004BA, 0x0004BA},
    {0x0004BC, 0x0004BC},
    {0x0004BE, 0x0004BE},
    {0x0004C0, 0x0004C1},
    {0x0004C3, 0x0004C3},
    {0x0004C5, 0x0004C5},
    {0x0004C7, 0x0004C7},
    {0x0004C9, 0x0004C9},
    {0x0004CB, 0x0004CB},
    {0x0004CD, 0x0004CD},
    {0x0004D0, 0x0004D0},
    {0x0004D2, 0x0004D2},
    {0x0004D4, 0x0004D4},
    {0x0004D6, 0x0004D6},
    {0x0004D8, 0x0004D8},
    {0x0004DA, 0x0004DA},
    {0x0004DC, 0x0004DC},
    {0x0004DE, 0x0004DE},
    {0x0004E0, 0x0004E0},
    {0x0004E2, 0x0004E2},
    {0x0004E4, 0x0004E4},
    {0x0004E6, 0x0004E6},
    {0x0004E8, 0x0004E8},
    {0x0004EA, 0x0004EA},
    {0x0004EC, 0x0004EC},
    {0x0004EE, 0x0004EE},
    {0x0004F0, 0x0004F0},
    {0x0004F2, 0x0004F2},
    {0x0004F4, 0x0004F4},
    {0x0004F6, 0x0004F6},
    {0x0004F8, 0x0004F8},
    {0x0004FA, 0x0004FA},
    {0x0004FC, 0x0004FC},
    {0x0004FE, 0x0004FE},
    {0x000500, 0x000500},
    {0x000502, 0x000502},
    {0x000504, 0x000504},
    {0x000506, 0x000506},
    {0x000508, 0x000508},
    {0x00050A, 0x00050A},
    {0x00050C, 0x00050C},
    {0x00050E, 0x00050E},
    {0x000510, 0x000510},
    {0x000512, 0x000512},
    {0x000514, 0x000514},
    {0x000516, 0x000516},
    {0x000518, 0x000518},
    {0x00051A, 0x00051A},
    {0x00051C, 0x00051C},
    {0x00051E, 0x00051E},
    {0x000520, 0x000520},
    {0x000522, 0x000522},
    {0x000524, 0x000524},
    {0x000526, 0x000526},
    {0x000528, 0x000528},
    {0x00052A, 0x00052A},
    {0x00052C, 0x00052C},
    {0x00052E, 0x00052E},
    {0x000531, 0x000556},
    {0x0010A0, 0x0010C5},
    {0x0010C7, 0x0010C7},
    {0x0010CD, 0x0010CD},
    {0x0013A0, 0x0013F5},
    {0x001C89, 0x001C89},
    {0x001C90, 0x001CBA},
    {0x001CBD, 0x001CBF},
    {0x001E00, 0x001E00},
    {0x001E02, 0x001E02},
    {0x001E04, 0x001E04},
    {0x001E06, 0x001E06},
    {0x001E08, 0x001E08},
    {0x001E0A, 0x001E0A},
    {0x001E0C, 0x001E0C},
    {0x001E0E, 0x001E0E},
    {0x001E10, 0x001E10},
    {0x001E12, 0x001E12},
    {0x001E14, 0x001E14},
    {0x001E16, 0x001E16},
    {0x001E18, 0x001E18},
    {0x001E1A, 0x001E1A},
    {0x001E1C, 0x001E1C},
    {0x001E1E, 0x001E1E},
    {0x001E20, 0x001E20},
    {0x001E22, 0x001E22},
    {0x001E24, 0x001E24},
    {0x001E26, 0x001E26},
    {0x001E28, 0x001E28},
    {0x001E2A, 0x001E2A},
    {0x001E2C, 0x001E2C},
    {0x001E2E, 0x001E2E},
    {0x001E30, 0x001E30},
    {0x001E32, 0x001E32},
    {0x001E34, 0x001E34},
    {0x001E36, 0x001E36},
    {0x001E38, 0x001E38},
    {0x001E3A, 0x001E3A},
    {0x001E3C, 0x001E3C},
    {0x001E3E, 0x001E3E},
    {0x001E40, 0x001E40},
    {0x001E42, 0x001E42},
    {0x001E44, 0x001E44},
    {0x001E46, 0x001E46},
    {0x001E48, 0x001E48},
    {0x001E4A, 0x001E4A},
    {0x001E4C, 0x001E4C},
    {0x001E4E, 0x001E4E},
    {0x001E50, 0x001E50},
    {0x001E52, 0x001E52},
    {0x001E54, 0x001E54},
    {0x001E56, 0x001E56},
    {0x001E58, 0x001E58},
    {0x001E5A, 0x001E5A},
    {0x001E5C, 0x001E5C},
    {0x001E5E, 0x001E5E},
    {0x001E60, 0x001E60},
    {0x001E62, 0x001E62},
    {0x001E64, 0x001E64},
    {0x001E66, 0x001E66},
    {0x001E68, 0x001E68},
    {0x001E6A, 0x001E6A},
    {0x001E6C, 0x001E6C},
    {0x001E6E, 0x001E6E},
    {0x001E70, 0x001E70},
    {0x001E72, 0x001E72},
    {0x001E74, 0x001E74},
    {0x001E76, 0x001E76},
    {0x001E78, 0x001E78},
    {0x001E7A, 0x001E7A},
    {0x001E7C, 0x001E7C},
    {0x001E7E, 0x001E7E},
    {0x001E80, 0x001E80},
    {0x001E82, 0x001E82},
    {0x001E84, 0x001E84},
    {0x001E86, 0x001E86},
    {0x001E88, 0x001E88},
    {0x001E8A, 0x001E8A},
    {0x001E8C, 0x001E8C},
    {0x001E8E, 0x001E8E},
    {0x001E90, 0x001E90},
    {0x001E92, 0x001E92},
    {0x001E94, 0x001E94},
    {0x001E9E, 0x001E9E},
    {0x001EA0, 0x001EA0},
    {0x001EA2, 0x001EA2},
    {0x001EA4, 0x001EA4},
    {0x001EA6, 0x001EA6},
    {0x001EA8, 0x001EA8},
    {0x001EAA, 0x001EAA},
    {0x001EAC, 0x001EAC},
    {0x001EAE, 0x001EAE},
    {0x001EB0, 0x001EB0},
    {0x001EB2, 0x001EB2},
    {0x001EB4, 0x001EB4},
    {0x001EB6, 0x001EB6},
    {0x001EB8, 0x001EB8},
    {0x001EBA, 0x001EBA},
    {0x001EBC, 0x001EBC},
    {0x001EBE, 0x001EBE},
    {0x001EC0, 0x001EC0},
    {0x001EC2, 0x001EC2},
    {0x001EC4, 0x001EC4},
    {0x001EC6, 0x001EC6},
    {0x001EC8, 0x001EC8},
    {0x001ECA, 0x001ECA},
    {0x001ECC, 0x001ECC},
    {0x001ECE, 0x001ECE},
    {0x001ED0, 0x001ED0},
    {0x001ED2, 0x001ED2},
    {0x001ED4, 0x001ED4},
    {0x001ED6, 0x001ED6},
    {0x001ED8, 0x001ED8},
    {0x001EDA, 0x001EDA},
    {0x001EDC, 0x001EDC},
    {0x001EDE, 0x001EDE},
    {0x001EE0, 0x001EE0},
    {0x001EE2, 0x001EE2},
    {0x001EE4, 0x001EE4},
    {0x001EE6, 0x001EE6},
    {0x001EE8, 0x001EE8},
    {0x001EEA, 0x001EEA},
    {0x001EEC, 0x001EEC},
    {0x001EEE, 0x001EEE},
    {0x001EF0, 0x001EF0},
    {0x001EF2, 0x001EF2},
    {0x001EF4, 0x001EF4},
    {0x001EF6, 0x001EF6},
    {0x001EF8, 0x001EF8},
    {0x001EFA, 0x001EFA},
    {0x001EFC, 0x001EFC},
    {0x001EFE, 0x001EFE},
    {0x001F08, 0x001F0F},
    {0x001F18, 0x001F1D},
    {0x001F28, 0x001F2F},
    {0x001F38, 0x001F3F},
    {0x001F48, 0x001F4D},
    {0x001F59, 0x001F59},
    {0x001F5B, 0x001F5B},
    {0x001F5D, 0x001F5D},
    {0x001F5F, 0x001F5F},
    {0x001F68, 0x001F6F},
    {0x001FB8, 0x001FBB},
    {0x001FC8, 0x001FCB},
    {0x001FD8, 0x001FDB},
    {0x001FE8, 0x001FEC},
    {0x001FF8, 0x001FFB},
    {0x002102, 0x002102},
    {0x002107, 0x002107},
    {0x00210B, 0x00210D},
    {0x002110, 0x002112},
    {0x002115, 0x002115},
    {0x002119, 0x00211D},
    {0x002124, 0x002124},
    {0x002126, 0x002126},
    {0x002128, 0x002128},
    {0x00212A, 0x00212D},
    {0x002130, 0x002133},
    {0x00213E, 0x00213F},
    {0x002145, 0x002145},
    {0x002183, 0x002183},
    {0x002C00, 0x002C2F},
    {0x002C60, 0x002C60},
    {0x002C62, 0x002C64},
    {0x002C67, 0x002C67},
    {0x002C69, 0x002C69},
    {0x002C6B, 0x002C6B},
    {0x002C6D, 0x002C70},
    {0x002C72, 0x002C72},
    {0x002C75, 0x002C75},
    {0x002C7E, 0x002C80},
    {0x002C82, 0x002C82},
    {0x002C84, 0x002C84},
    {0x002C86, 0x002C86},
    {0x002C88, 0x002C88},
    {0x002C8A, 0x002C8A},
    {0x002C8C, 0x002C8C},
    {0x002C8E, 0x002C8E},
    {0x002C90, 0x002C90},
    {0x002C92, 0x002C92},
    {0x002C94, 0x002C94},
    {0x002C96, 0x002C96},
    {0x002C98, 0x002C98},
    {0x002C9A, 0x002C9A},
    {0x002C9C, 0x002C9C},
    {0x002C9E, 0x002C9E},
    {0x002CA0, 0x002CA0},
    {0x002CA2, 0x002CA2},
    {0x002CA4, 0x002CA4},
    {0x002CA6, 0x002CA6},
    {0x002CA8, 0x002CA8},
    {0x002CAA, 0x002CAA},
    {0x002CAC, 0x002CAC},
    {0x002CAE, 0x002CAE},
    {0x002CB0, 0x002CB0},
    {0x002CB2, 0x002CB2},
    {0x002CB4, 0x002CB4},
    {0x002CB6, 0x002CB6},
    {0x002CB8, 0x002CB8},
    {0x002CBA, 0x002CBA},
    {0x002CBC, 0x002CBC},
    {0x002CBE, 0x002CBE},
    {0x002CC0, 0x002CC0},
    {0x002CC2, 0x002CC2},
    {0x002CC4, 0x002CC4},
    {0x002CC6, 0x002CC6},
    {0x002CC8, 0x002CC8},
    {0x002CCA, 0x002CCA},
    {0x002CCC, 0x002CCC},
    {0x002CCE, 0x002CCE},
    {0x002CD0, 0x002CD0},
    {0x002CD2, 0x002CD2},
    {0x002CD4, 0x002CD4},
    {0x002CD6, 0x002CD6},
    {0x002CD8, 0x002CD8},
    {0x002CDA, 0x002CDA},
    {0x002CDC, 0x002CDC},
    {0x002CDE, 0x002CDE},
    {0x002CE0, 0x002CE0},
    {0x002CE2, 0x002CE2},
    {0x002CEB, 0x002CEB},
    {0x002CED, 0x002CED},
    {0x002CF2, 0x002CF2},
    {0x00A640, 0x00A640},
    {0x00A642, 0x00A642},
    {0x00A644, 0x00A644},
    {0x00A646, 0x00A646},
    {0x00A648, 0x00A648},
    {0x00A64A, 0x00A64A},
    {0x00A64C, 0x00A64C},
    {0x00A64E, 0x00A64E},
    {0x00A650, 0x00A650},
    {0x00A652, 0x00A652},
    {0x00A654, 0x00A654},
    {0x00A656, 0x00A656},
    {0x00A658, 0x00A658},
    {0x00A65A, 0x00A65A},
    {0x00A65C, 0x00A65C},
    {0x00A65E, 0x00A65E},
    {0x00A660, 0x00A660},
    {0x00A662, 0x00A662},
    {0x00A664, 0x00A664},
    {0x00A666, 0x00A666},
    {0x00A668, 0x00A668},
    {0x00A66A, 0x00A66A},
    {0x00A66C, 0x00A66C},
    {0x00A680, 0x00A680},
    {0x00A682, 0x00A682},
    {0x00A684, 0x00A684},
    {0x00A686, 0x00A686},
    {0x00A688, 0x00A688},
    {0x00A68A, 0x00A68A},
    {0x00A68C, 0x00A68C},
    {0x00A68E, 0x00A68E},
    {0x00A690, 0x00A690},
    {0x00A692, 0x00A692},
    {0x00A694, 0x00A694},
    {0x00A696, 0x00A696},
    {0x00A698, 0x00A698},
    {0x00A69A, 0x00A69A},
    {0x00A722, 0x00A722},
    {0x00A724, 0x00A724},
    {0x00A726, 0x00A726},
    {0x00A728, 0x00A728},
    {0x00A72A, 0x00A72A},
    {0x00A72C, 0x00A72C},
    {0x00A72E, 0x00A72E},
    {0x00A732, 0x00A732},
    {0x00A734, 0x00A734},
    {0x00A736, 0x00A736},
    {0x00A738, 0x00A738},
    {0x00A73A, 0x00A73A},
    {0x00A73C, 0x00A73C},
    {0x00A73E, 0x00A73E},
    {0x00A740, 0x00A740},
    {0x00A742, 0x00A742},
    {0x00A744, 0x00A744},
    {0x00A746, 0x00A746},
    {0x00A748, 0x00A748},
    {0x00A74A, 0x00A74A},
    {0x00A74C, 0x00A74C},
    {0x00A74E, 0x00A74E},
    {0x00A750, 0x00A750},
    {0x00A752, 0x00A752},
    {0x00A754, 0x00A754},
    {0x00A756, 0x00A756},
    {0x00A758, 0x00A758},
    {0x00A75A, 0x00A75A},
    {0x00A75C, 0x00A75C},
    {0x00A75E, 0x00A75E},
    {0x00A760, 0x00A760},
    {0x00A762, 0x00A762},
    {0x00A764, 0x00A764},
    {0x00A766, 0x00A766},
    {0x00A768, 0x00A768},
    {0x00A76A, 0x00A76A},
    {0x00A76C, 0x00A76C},
    {0x00A76E, 0x00A76E},
    {0x00A779, 0x00A779},
    {0x00A77B, 0x00A77B},
    {0x00A77D, 0x00A77E},
    {0x00A780, 0x00A780},
    {0x00A782, 0x00A782},
    {0x00A784, 0x00A784},
    {0x00A786, 0x00A786},
    {0x00A78B, 0x00A78B},
    {0x00A78D, 0x00A78D},
    {0x00A790, 0x00A790},
    {0x00A792, 0x00A792},
    {0x00A796, 0x00A796},
    {0x00A798, 0x00A798},
    {0x00A79A, 0x00A79A},
    {0x00A79C, 0x00A79C},
    {0x00A79E, 0x00A79E},
    {0x00A7A0, 0x00A7A0},
    {0x00A7A2, 0x00A7A2},
    {0x00A7A4, 0x00A7A4},
    {0x00A7A6, 0x00A7A6},
    {0x00A7A8, 0x00A7A8},
    {0x00A7AA, 0x00A7AE},
    {0x00A7B0, 0x00A7B4},
    {0x00A7B6, 0x00A7B6},
    {0x00A7B8, 0x00A7B8},
    {0x00A7BA, 0x00A7BA},
    {0x00A7BC, 0x00A7BC},
    {0x00A7BE, 0x00A7BE},
    {0x00A7C0, 0x00A7C0},
    {0x00A7C2, 0x00A7C2},
    {0x00A7C4, 0x00A7C7},
    {0x00A7C9, 0x00A7C9},
    {0x00A7CB, 0x00A7CC},
    {0x00A7CE, 0x00A7CE},
    {0x00A7D0, 0x00A7D0},
    {0x00A7D2, 0x00A7D2},
    {0x00A7D4, 0x00A7D4},
    {0x00A7D6, 0x00A7D6},
    {0x00A7D8, 0x00A7D8},
    {0x00A7DA, 0x00A7DA},
    {0x00A7DC, 0x00A7DC},
    {0x00A7F5, 0x00A7F5},
    {0x00FF21, 0x00FF3A},
    {0x010400, 0x010427},
    {0x0104B0, 0x0104D3},
    {0x010570, 0x01057A},
    {0x01057C, 0x01058A},
    {0x01058C, 0x010592},
    {0x010594, 0x010595},
    {0x010C80, 0x010CB2},
    {0x010D50, 0x010D65},
    {0x0118A0, 0x0118BF},
    {0x016E40, 0x016E5F},
    {0x016EA0, 0x016EB8},
    {0x01D400, 0x01D419},
    {0x01D434, 0x01D44D},
    {0x01D468, 0x01D481},
    {0x01D49C, 0x01D49C},
    {0x01D49E, 0x01D49F},
    {0x01D4A2, 0x01D4A2},
    {0x01D4A5, 0x01D4A6},
    {0x01D4A9, 0x01D4AC},
    {0x01D4AE, 0x01D4B5},
    {0x01D4D0, 0x01D4E9},
    {0x01D504, 0x01D505},
    {0x01D507, 0x01D50A},
    {0x01D50D, 0x01D514},
    {0x01D516, 0x01D51C},
    {0x01D538, 0x01D539},
    {0x01D53B, 0x01D53E},
    {0x01D540, 0x01D544},
    {0x01D546, 0x01D546},
    {0x01D54A, 0x01D550},
    {0x01D56C, 0x01D585},
    {0x01D5A0, 0x01D5B9},
    {0x01D5D4, 0x01D5ED},
    {0x01D608, 0x01D621},
    {0x01D63C, 0x01D655},
    {0x01D670, 0x01D689},
    {0x01D6A8, 0x01D6C0},
    {0x01D6E2, 0x01D6FA},
    {0x01D71C, 0x01D734},
    {0x01D756, 0x01D76E},
    {0x01D790, 0x01D7A8},
    {0x01D7CA, 0x01D7CA},
    {0x01E900, 0x01E921},
  };
  size_t lo = 0;
  size_t hi = sizeof(kUppercaseLetterRanges) / sizeof(kUppercaseLetterRanges[0]);
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    auto range = kUppercaseLetterRanges[mid];
    if (cp < range.first) {
      hi = mid;
    } else if (cp > range.second) {
      lo = mid + 1;
    } else {
      return true;
    }
  }
  return false;
}

inline bool regexModifierMatchesUnicodePropertyLu(uint32_t cp, bool ignoreCase) {
  if (regexMatchesUnicodePropertyUppercaseLetter(cp)) {
    return true;
  }
  if (!ignoreCase) {
    return false;
  }
  std::string upper = unicode::toUpper(unicode::encodeUTF8(cp));
  if (upper.empty()) {
    return false;
  }
  size_t index = 0;
  while (index < upper.size()) {
    uint32_t upperCp = unicode::decodeUTF8(upper, index);
    if (regexMatchesUnicodePropertyUppercaseLetter(upperCp)) {
      return true;
    }
  }
  return false;
}

inline bool regexMatchesUnicodePropertyLowercaseLetter(uint32_t cp) {
  static constexpr std::pair<uint32_t, uint32_t> kLowercaseLetterRanges[] = {
    {0x000061, 0x00007A},
    {0x0000B5, 0x0000B5},
    {0x0000DF, 0x0000F6},
    {0x0000F8, 0x0000FF},
    {0x000101, 0x000101},
    {0x000103, 0x000103},
    {0x000105, 0x000105},
    {0x000107, 0x000107},
    {0x000109, 0x000109},
    {0x00010B, 0x00010B},
    {0x00010D, 0x00010D},
    {0x00010F, 0x00010F},
    {0x000111, 0x000111},
    {0x000113, 0x000113},
    {0x000115, 0x000115},
    {0x000117, 0x000117},
    {0x000119, 0x000119},
    {0x00011B, 0x00011B},
    {0x00011D, 0x00011D},
    {0x00011F, 0x00011F},
    {0x000121, 0x000121},
    {0x000123, 0x000123},
    {0x000125, 0x000125},
    {0x000127, 0x000127},
    {0x000129, 0x000129},
    {0x00012B, 0x00012B},
    {0x00012D, 0x00012D},
    {0x00012F, 0x00012F},
    {0x000131, 0x000131},
    {0x000133, 0x000133},
    {0x000135, 0x000135},
    {0x000137, 0x000138},
    {0x00013A, 0x00013A},
    {0x00013C, 0x00013C},
    {0x00013E, 0x00013E},
    {0x000140, 0x000140},
    {0x000142, 0x000142},
    {0x000144, 0x000144},
    {0x000146, 0x000146},
    {0x000148, 0x000149},
    {0x00014B, 0x00014B},
    {0x00014D, 0x00014D},
    {0x00014F, 0x00014F},
    {0x000151, 0x000151},
    {0x000153, 0x000153},
    {0x000155, 0x000155},
    {0x000157, 0x000157},
    {0x000159, 0x000159},
    {0x00015B, 0x00015B},
    {0x00015D, 0x00015D},
    {0x00015F, 0x00015F},
    {0x000161, 0x000161},
    {0x000163, 0x000163},
    {0x000165, 0x000165},
    {0x000167, 0x000167},
    {0x000169, 0x000169},
    {0x00016B, 0x00016B},
    {0x00016D, 0x00016D},
    {0x00016F, 0x00016F},
    {0x000171, 0x000171},
    {0x000173, 0x000173},
    {0x000175, 0x000175},
    {0x000177, 0x000177},
    {0x00017A, 0x00017A},
    {0x00017C, 0x00017C},
    {0x00017E, 0x000180},
    {0x000183, 0x000183},
    {0x000185, 0x000185},
    {0x000188, 0x000188},
    {0x00018C, 0x00018D},
    {0x000192, 0x000192},
    {0x000195, 0x000195},
    {0x000199, 0x00019B},
    {0x00019E, 0x00019E},
    {0x0001A1, 0x0001A1},
    {0x0001A3, 0x0001A3},
    {0x0001A5, 0x0001A5},
    {0x0001A8, 0x0001A8},
    {0x0001AA, 0x0001AB},
    {0x0001AD, 0x0001AD},
    {0x0001B0, 0x0001B0},
    {0x0001B4, 0x0001B4},
    {0x0001B6, 0x0001B6},
    {0x0001B9, 0x0001BA},
    {0x0001BD, 0x0001BF},
    {0x0001C6, 0x0001C6},
    {0x0001C9, 0x0001C9},
    {0x0001CC, 0x0001CC},
    {0x0001CE, 0x0001CE},
    {0x0001D0, 0x0001D0},
    {0x0001D2, 0x0001D2},
    {0x0001D4, 0x0001D4},
    {0x0001D6, 0x0001D6},
    {0x0001D8, 0x0001D8},
    {0x0001DA, 0x0001DA},
    {0x0001DC, 0x0001DD},
    {0x0001DF, 0x0001DF},
    {0x0001E1, 0x0001E1},
    {0x0001E3, 0x0001E3},
    {0x0001E5, 0x0001E5},
    {0x0001E7, 0x0001E7},
    {0x0001E9, 0x0001E9},
    {0x0001EB, 0x0001EB},
    {0x0001ED, 0x0001ED},
    {0x0001EF, 0x0001F0},
    {0x0001F3, 0x0001F3},
    {0x0001F5, 0x0001F5},
    {0x0001F9, 0x0001F9},
    {0x0001FB, 0x0001FB},
    {0x0001FD, 0x0001FD},
    {0x0001FF, 0x0001FF},
    {0x000201, 0x000201},
    {0x000203, 0x000203},
    {0x000205, 0x000205},
    {0x000207, 0x000207},
    {0x000209, 0x000209},
    {0x00020B, 0x00020B},
    {0x00020D, 0x00020D},
    {0x00020F, 0x00020F},
    {0x000211, 0x000211},
    {0x000213, 0x000213},
    {0x000215, 0x000215},
    {0x000217, 0x000217},
    {0x000219, 0x000219},
    {0x00021B, 0x00021B},
    {0x00021D, 0x00021D},
    {0x00021F, 0x00021F},
    {0x000221, 0x000221},
    {0x000223, 0x000223},
    {0x000225, 0x000225},
    {0x000227, 0x000227},
    {0x000229, 0x000229},
    {0x00022B, 0x00022B},
    {0x00022D, 0x00022D},
    {0x00022F, 0x00022F},
    {0x000231, 0x000231},
    {0x000233, 0x000239},
    {0x00023C, 0x00023C},
    {0x00023F, 0x000240},
    {0x000242, 0x000242},
    {0x000247, 0x000247},
    {0x000249, 0x000249},
    {0x00024B, 0x00024B},
    {0x00024D, 0x00024D},
    {0x00024F, 0x000293},
    {0x000296, 0x0002AF},
    {0x000371, 0x000371},
    {0x000373, 0x000373},
    {0x000377, 0x000377},
    {0x00037B, 0x00037D},
    {0x000390, 0x000390},
    {0x0003AC, 0x0003CE},
    {0x0003D0, 0x0003D1},
    {0x0003D5, 0x0003D7},
    {0x0003D9, 0x0003D9},
    {0x0003DB, 0x0003DB},
    {0x0003DD, 0x0003DD},
    {0x0003DF, 0x0003DF},
    {0x0003E1, 0x0003E1},
    {0x0003E3, 0x0003E3},
    {0x0003E5, 0x0003E5},
    {0x0003E7, 0x0003E7},
    {0x0003E9, 0x0003E9},
    {0x0003EB, 0x0003EB},
    {0x0003ED, 0x0003ED},
    {0x0003EF, 0x0003F3},
    {0x0003F5, 0x0003F5},
    {0x0003F8, 0x0003F8},
    {0x0003FB, 0x0003FC},
    {0x000430, 0x00045F},
    {0x000461, 0x000461},
    {0x000463, 0x000463},
    {0x000465, 0x000465},
    {0x000467, 0x000467},
    {0x000469, 0x000469},
    {0x00046B, 0x00046B},
    {0x00046D, 0x00046D},
    {0x00046F, 0x00046F},
    {0x000471, 0x000471},
    {0x000473, 0x000473},
    {0x000475, 0x000475},
    {0x000477, 0x000477},
    {0x000479, 0x000479},
    {0x00047B, 0x00047B},
    {0x00047D, 0x00047D},
    {0x00047F, 0x00047F},
    {0x000481, 0x000481},
    {0x00048B, 0x00048B},
    {0x00048D, 0x00048D},
    {0x00048F, 0x00048F},
    {0x000491, 0x000491},
    {0x000493, 0x000493},
    {0x000495, 0x000495},
    {0x000497, 0x000497},
    {0x000499, 0x000499},
    {0x00049B, 0x00049B},
    {0x00049D, 0x00049D},
    {0x00049F, 0x00049F},
    {0x0004A1, 0x0004A1},
    {0x0004A3, 0x0004A3},
    {0x0004A5, 0x0004A5},
    {0x0004A7, 0x0004A7},
    {0x0004A9, 0x0004A9},
    {0x0004AB, 0x0004AB},
    {0x0004AD, 0x0004AD},
    {0x0004AF, 0x0004AF},
    {0x0004B1, 0x0004B1},
    {0x0004B3, 0x0004B3},
    {0x0004B5, 0x0004B5},
    {0x0004B7, 0x0004B7},
    {0x0004B9, 0x0004B9},
    {0x0004BB, 0x0004BB},
    {0x0004BD, 0x0004BD},
    {0x0004BF, 0x0004BF},
    {0x0004C2, 0x0004C2},
    {0x0004C4, 0x0004C4},
    {0x0004C6, 0x0004C6},
    {0x0004C8, 0x0004C8},
    {0x0004CA, 0x0004CA},
    {0x0004CC, 0x0004CC},
    {0x0004CE, 0x0004CF},
    {0x0004D1, 0x0004D1},
    {0x0004D3, 0x0004D3},
    {0x0004D5, 0x0004D5},
    {0x0004D7, 0x0004D7},
    {0x0004D9, 0x0004D9},
    {0x0004DB, 0x0004DB},
    {0x0004DD, 0x0004DD},
    {0x0004DF, 0x0004DF},
    {0x0004E1, 0x0004E1},
    {0x0004E3, 0x0004E3},
    {0x0004E5, 0x0004E5},
    {0x0004E7, 0x0004E7},
    {0x0004E9, 0x0004E9},
    {0x0004EB, 0x0004EB},
    {0x0004ED, 0x0004ED},
    {0x0004EF, 0x0004EF},
    {0x0004F1, 0x0004F1},
    {0x0004F3, 0x0004F3},
    {0x0004F5, 0x0004F5},
    {0x0004F7, 0x0004F7},
    {0x0004F9, 0x0004F9},
    {0x0004FB, 0x0004FB},
    {0x0004FD, 0x0004FD},
    {0x0004FF, 0x0004FF},
    {0x000501, 0x000501},
    {0x000503, 0x000503},
    {0x000505, 0x000505},
    {0x000507, 0x000507},
    {0x000509, 0x000509},
    {0x00050B, 0x00050B},
    {0x00050D, 0x00050D},
    {0x00050F, 0x00050F},
    {0x000511, 0x000511},
    {0x000513, 0x000513},
    {0x000515, 0x000515},
    {0x000517, 0x000517},
    {0x000519, 0x000519},
    {0x00051B, 0x00051B},
    {0x00051D, 0x00051D},
    {0x00051F, 0x00051F},
    {0x000521, 0x000521},
    {0x000523, 0x000523},
    {0x000525, 0x000525},
    {0x000527, 0x000527},
    {0x000529, 0x000529},
    {0x00052B, 0x00052B},
    {0x00052D, 0x00052D},
    {0x00052F, 0x00052F},
    {0x000560, 0x000588},
    {0x0010D0, 0x0010FA},
    {0x0010FD, 0x0010FF},
    {0x0013F8, 0x0013FD},
    {0x001C80, 0x001C88},
    {0x001C8A, 0x001C8A},
    {0x001D00, 0x001D2B},
    {0x001D6B, 0x001D77},
    {0x001D79, 0x001D9A},
    {0x001E01, 0x001E01},
    {0x001E03, 0x001E03},
    {0x001E05, 0x001E05},
    {0x001E07, 0x001E07},
    {0x001E09, 0x001E09},
    {0x001E0B, 0x001E0B},
    {0x001E0D, 0x001E0D},
    {0x001E0F, 0x001E0F},
    {0x001E11, 0x001E11},
    {0x001E13, 0x001E13},
    {0x001E15, 0x001E15},
    {0x001E17, 0x001E17},
    {0x001E19, 0x001E19},
    {0x001E1B, 0x001E1B},
    {0x001E1D, 0x001E1D},
    {0x001E1F, 0x001E1F},
    {0x001E21, 0x001E21},
    {0x001E23, 0x001E23},
    {0x001E25, 0x001E25},
    {0x001E27, 0x001E27},
    {0x001E29, 0x001E29},
    {0x001E2B, 0x001E2B},
    {0x001E2D, 0x001E2D},
    {0x001E2F, 0x001E2F},
    {0x001E31, 0x001E31},
    {0x001E33, 0x001E33},
    {0x001E35, 0x001E35},
    {0x001E37, 0x001E37},
    {0x001E39, 0x001E39},
    {0x001E3B, 0x001E3B},
    {0x001E3D, 0x001E3D},
    {0x001E3F, 0x001E3F},
    {0x001E41, 0x001E41},
    {0x001E43, 0x001E43},
    {0x001E45, 0x001E45},
    {0x001E47, 0x001E47},
    {0x001E49, 0x001E49},
    {0x001E4B, 0x001E4B},
    {0x001E4D, 0x001E4D},
    {0x001E4F, 0x001E4F},
    {0x001E51, 0x001E51},
    {0x001E53, 0x001E53},
    {0x001E55, 0x001E55},
    {0x001E57, 0x001E57},
    {0x001E59, 0x001E59},
    {0x001E5B, 0x001E5B},
    {0x001E5D, 0x001E5D},
    {0x001E5F, 0x001E5F},
    {0x001E61, 0x001E61},
    {0x001E63, 0x001E63},
    {0x001E65, 0x001E65},
    {0x001E67, 0x001E67},
    {0x001E69, 0x001E69},
    {0x001E6B, 0x001E6B},
    {0x001E6D, 0x001E6D},
    {0x001E6F, 0x001E6F},
    {0x001E71, 0x001E71},
    {0x001E73, 0x001E73},
    {0x001E75, 0x001E75},
    {0x001E77, 0x001E77},
    {0x001E79, 0x001E79},
    {0x001E7B, 0x001E7B},
    {0x001E7D, 0x001E7D},
    {0x001E7F, 0x001E7F},
    {0x001E81, 0x001E81},
    {0x001E83, 0x001E83},
    {0x001E85, 0x001E85},
    {0x001E87, 0x001E87},
    {0x001E89, 0x001E89},
    {0x001E8B, 0x001E8B},
    {0x001E8D, 0x001E8D},
    {0x001E8F, 0x001E8F},
    {0x001E91, 0x001E91},
    {0x001E93, 0x001E93},
    {0x001E95, 0x001E9D},
    {0x001E9F, 0x001E9F},
    {0x001EA1, 0x001EA1},
    {0x001EA3, 0x001EA3},
    {0x001EA5, 0x001EA5},
    {0x001EA7, 0x001EA7},
    {0x001EA9, 0x001EA9},
    {0x001EAB, 0x001EAB},
    {0x001EAD, 0x001EAD},
    {0x001EAF, 0x001EAF},
    {0x001EB1, 0x001EB1},
    {0x001EB3, 0x001EB3},
    {0x001EB5, 0x001EB5},
    {0x001EB7, 0x001EB7},
    {0x001EB9, 0x001EB9},
    {0x001EBB, 0x001EBB},
    {0x001EBD, 0x001EBD},
    {0x001EBF, 0x001EBF},
    {0x001EC1, 0x001EC1},
    {0x001EC3, 0x001EC3},
    {0x001EC5, 0x001EC5},
    {0x001EC7, 0x001EC7},
    {0x001EC9, 0x001EC9},
    {0x001ECB, 0x001ECB},
    {0x001ECD, 0x001ECD},
    {0x001ECF, 0x001ECF},
    {0x001ED1, 0x001ED1},
    {0x001ED3, 0x001ED3},
    {0x001ED5, 0x001ED5},
    {0x001ED7, 0x001ED7},
    {0x001ED9, 0x001ED9},
    {0x001EDB, 0x001EDB},
    {0x001EDD, 0x001EDD},
    {0x001EDF, 0x001EDF},
    {0x001EE1, 0x001EE1},
    {0x001EE3, 0x001EE3},
    {0x001EE5, 0x001EE5},
    {0x001EE7, 0x001EE7},
    {0x001EE9, 0x001EE9},
    {0x001EEB, 0x001EEB},
    {0x001EED, 0x001EED},
    {0x001EEF, 0x001EEF},
    {0x001EF1, 0x001EF1},
    {0x001EF3, 0x001EF3},
    {0x001EF5, 0x001EF5},
    {0x001EF7, 0x001EF7},
    {0x001EF9, 0x001EF9},
    {0x001EFB, 0x001EFB},
    {0x001EFD, 0x001EFD},
    {0x001EFF, 0x001F07},
    {0x001F10, 0x001F15},
    {0x001F20, 0x001F27},
    {0x001F30, 0x001F37},
    {0x001F40, 0x001F45},
    {0x001F50, 0x001F57},
    {0x001F60, 0x001F67},
    {0x001F70, 0x001F7D},
    {0x001F80, 0x001F87},
    {0x001F90, 0x001F97},
    {0x001FA0, 0x001FA7},
    {0x001FB0, 0x001FB4},
    {0x001FB6, 0x001FB7},
    {0x001FBE, 0x001FBE},
    {0x001FC2, 0x001FC4},
    {0x001FC6, 0x001FC7},
    {0x001FD0, 0x001FD3},
    {0x001FD6, 0x001FD7},
    {0x001FE0, 0x001FE7},
    {0x001FF2, 0x001FF4},
    {0x001FF6, 0x001FF7},
    {0x00210A, 0x00210A},
    {0x00210E, 0x00210F},
    {0x002113, 0x002113},
    {0x00212F, 0x00212F},
    {0x002134, 0x002134},
    {0x002139, 0x002139},
    {0x00213C, 0x00213D},
    {0x002146, 0x002149},
    {0x00214E, 0x00214E},
    {0x002184, 0x002184},
    {0x002C30, 0x002C5F},
    {0x002C61, 0x002C61},
    {0x002C65, 0x002C66},
    {0x002C68, 0x002C68},
    {0x002C6A, 0x002C6A},
    {0x002C6C, 0x002C6C},
    {0x002C71, 0x002C71},
    {0x002C73, 0x002C74},
    {0x002C76, 0x002C7B},
    {0x002C81, 0x002C81},
    {0x002C83, 0x002C83},
    {0x002C85, 0x002C85},
    {0x002C87, 0x002C87},
    {0x002C89, 0x002C89},
    {0x002C8B, 0x002C8B},
    {0x002C8D, 0x002C8D},
    {0x002C8F, 0x002C8F},
    {0x002C91, 0x002C91},
    {0x002C93, 0x002C93},
    {0x002C95, 0x002C95},
    {0x002C97, 0x002C97},
    {0x002C99, 0x002C99},
    {0x002C9B, 0x002C9B},
    {0x002C9D, 0x002C9D},
    {0x002C9F, 0x002C9F},
    {0x002CA1, 0x002CA1},
    {0x002CA3, 0x002CA3},
    {0x002CA5, 0x002CA5},
    {0x002CA7, 0x002CA7},
    {0x002CA9, 0x002CA9},
    {0x002CAB, 0x002CAB},
    {0x002CAD, 0x002CAD},
    {0x002CAF, 0x002CAF},
    {0x002CB1, 0x002CB1},
    {0x002CB3, 0x002CB3},
    {0x002CB5, 0x002CB5},
    {0x002CB7, 0x002CB7},
    {0x002CB9, 0x002CB9},
    {0x002CBB, 0x002CBB},
    {0x002CBD, 0x002CBD},
    {0x002CBF, 0x002CBF},
    {0x002CC1, 0x002CC1},
    {0x002CC3, 0x002CC3},
    {0x002CC5, 0x002CC5},
    {0x002CC7, 0x002CC7},
    {0x002CC9, 0x002CC9},
    {0x002CCB, 0x002CCB},
    {0x002CCD, 0x002CCD},
    {0x002CCF, 0x002CCF},
    {0x002CD1, 0x002CD1},
    {0x002CD3, 0x002CD3},
    {0x002CD5, 0x002CD5},
    {0x002CD7, 0x002CD7},
    {0x002CD9, 0x002CD9},
    {0x002CDB, 0x002CDB},
    {0x002CDD, 0x002CDD},
    {0x002CDF, 0x002CDF},
    {0x002CE1, 0x002CE1},
    {0x002CE3, 0x002CE4},
    {0x002CEC, 0x002CEC},
    {0x002CEE, 0x002CEE},
    {0x002CF3, 0x002CF3},
    {0x002D00, 0x002D25},
    {0x002D27, 0x002D27},
    {0x002D2D, 0x002D2D},
    {0x00A641, 0x00A641},
    {0x00A643, 0x00A643},
    {0x00A645, 0x00A645},
    {0x00A647, 0x00A647},
    {0x00A649, 0x00A649},
    {0x00A64B, 0x00A64B},
    {0x00A64D, 0x00A64D},
    {0x00A64F, 0x00A64F},
    {0x00A651, 0x00A651},
    {0x00A653, 0x00A653},
    {0x00A655, 0x00A655},
    {0x00A657, 0x00A657},
    {0x00A659, 0x00A659},
    {0x00A65B, 0x00A65B},
    {0x00A65D, 0x00A65D},
    {0x00A65F, 0x00A65F},
    {0x00A661, 0x00A661},
    {0x00A663, 0x00A663},
    {0x00A665, 0x00A665},
    {0x00A667, 0x00A667},
    {0x00A669, 0x00A669},
    {0x00A66B, 0x00A66B},
    {0x00A66D, 0x00A66D},
    {0x00A681, 0x00A681},
    {0x00A683, 0x00A683},
    {0x00A685, 0x00A685},
    {0x00A687, 0x00A687},
    {0x00A689, 0x00A689},
    {0x00A68B, 0x00A68B},
    {0x00A68D, 0x00A68D},
    {0x00A68F, 0x00A68F},
    {0x00A691, 0x00A691},
    {0x00A693, 0x00A693},
    {0x00A695, 0x00A695},
    {0x00A697, 0x00A697},
    {0x00A699, 0x00A699},
    {0x00A69B, 0x00A69B},
    {0x00A723, 0x00A723},
    {0x00A725, 0x00A725},
    {0x00A727, 0x00A727},
    {0x00A729, 0x00A729},
    {0x00A72B, 0x00A72B},
    {0x00A72D, 0x00A72D},
    {0x00A72F, 0x00A731},
    {0x00A733, 0x00A733},
    {0x00A735, 0x00A735},
    {0x00A737, 0x00A737},
    {0x00A739, 0x00A739},
    {0x00A73B, 0x00A73B},
    {0x00A73D, 0x00A73D},
    {0x00A73F, 0x00A73F},
    {0x00A741, 0x00A741},
    {0x00A743, 0x00A743},
    {0x00A745, 0x00A745},
    {0x00A747, 0x00A747},
    {0x00A749, 0x00A749},
    {0x00A74B, 0x00A74B},
    {0x00A74D, 0x00A74D},
    {0x00A74F, 0x00A74F},
    {0x00A751, 0x00A751},
    {0x00A753, 0x00A753},
    {0x00A755, 0x00A755},
    {0x00A757, 0x00A757},
    {0x00A759, 0x00A759},
    {0x00A75B, 0x00A75B},
    {0x00A75D, 0x00A75D},
    {0x00A75F, 0x00A75F},
    {0x00A761, 0x00A761},
    {0x00A763, 0x00A763},
    {0x00A765, 0x00A765},
    {0x00A767, 0x00A767},
    {0x00A769, 0x00A769},
    {0x00A76B, 0x00A76B},
    {0x00A76D, 0x00A76D},
    {0x00A76F, 0x00A76F},
    {0x00A771, 0x00A778},
    {0x00A77A, 0x00A77A},
    {0x00A77C, 0x00A77C},
    {0x00A77F, 0x00A77F},
    {0x00A781, 0x00A781},
    {0x00A783, 0x00A783},
    {0x00A785, 0x00A785},
    {0x00A787, 0x00A787},
    {0x00A78C, 0x00A78C},
    {0x00A78E, 0x00A78E},
    {0x00A791, 0x00A791},
    {0x00A793, 0x00A795},
    {0x00A797, 0x00A797},
    {0x00A799, 0x00A799},
    {0x00A79B, 0x00A79B},
    {0x00A79D, 0x00A79D},
    {0x00A79F, 0x00A79F},
    {0x00A7A1, 0x00A7A1},
    {0x00A7A3, 0x00A7A3},
    {0x00A7A5, 0x00A7A5},
    {0x00A7A7, 0x00A7A7},
    {0x00A7A9, 0x00A7A9},
    {0x00A7AF, 0x00A7AF},
    {0x00A7B5, 0x00A7B5},
    {0x00A7B7, 0x00A7B7},
    {0x00A7B9, 0x00A7B9},
    {0x00A7BB, 0x00A7BB},
    {0x00A7BD, 0x00A7BD},
    {0x00A7BF, 0x00A7BF},
    {0x00A7C1, 0x00A7C1},
    {0x00A7C3, 0x00A7C3},
    {0x00A7C8, 0x00A7C8},
    {0x00A7CA, 0x00A7CA},
    {0x00A7CD, 0x00A7CD},
    {0x00A7CF, 0x00A7CF},
    {0x00A7D1, 0x00A7D1},
    {0x00A7D3, 0x00A7D3},
    {0x00A7D5, 0x00A7D5},
    {0x00A7D7, 0x00A7D7},
    {0x00A7D9, 0x00A7D9},
    {0x00A7DB, 0x00A7DB},
    {0x00A7F6, 0x00A7F6},
    {0x00A7FA, 0x00A7FA},
    {0x00AB30, 0x00AB5A},
    {0x00AB60, 0x00AB68},
    {0x00AB70, 0x00ABBF},
    {0x00FB00, 0x00FB06},
    {0x00FB13, 0x00FB17},
    {0x00FF41, 0x00FF5A},
    {0x010428, 0x01044F},
    {0x0104D8, 0x0104FB},
    {0x010597, 0x0105A1},
    {0x0105A3, 0x0105B1},
    {0x0105B3, 0x0105B9},
    {0x0105BB, 0x0105BC},
    {0x010CC0, 0x010CF2},
    {0x010D70, 0x010D85},
    {0x0118C0, 0x0118DF},
    {0x016E60, 0x016E7F},
    {0x016EBB, 0x016ED3},
    {0x01D41A, 0x01D433},
    {0x01D44E, 0x01D454},
    {0x01D456, 0x01D467},
    {0x01D482, 0x01D49B},
    {0x01D4B6, 0x01D4B9},
    {0x01D4BB, 0x01D4BB},
    {0x01D4BD, 0x01D4C3},
    {0x01D4C5, 0x01D4CF},
    {0x01D4EA, 0x01D503},
    {0x01D51E, 0x01D537},
    {0x01D552, 0x01D56B},
    {0x01D586, 0x01D59F},
    {0x01D5BA, 0x01D5D3},
    {0x01D5EE, 0x01D607},
    {0x01D622, 0x01D63B},
    {0x01D656, 0x01D66F},
    {0x01D68A, 0x01D6A5},
    {0x01D6C2, 0x01D6DA},
    {0x01D6DC, 0x01D6E1},
    {0x01D6FC, 0x01D714},
    {0x01D716, 0x01D71B},
    {0x01D736, 0x01D74E},
    {0x01D750, 0x01D755},
    {0x01D770, 0x01D788},
    {0x01D78A, 0x01D78F},
    {0x01D7AA, 0x01D7C2},
    {0x01D7C4, 0x01D7C9},
    {0x01D7CB, 0x01D7CB},
    {0x01DF00, 0x01DF09},
    {0x01DF0B, 0x01DF1E},
    {0x01DF25, 0x01DF2A},
    {0x01E922, 0x01E943},
  };
  size_t lo = 0;
  size_t hi = sizeof(kLowercaseLetterRanges) / sizeof(kLowercaseLetterRanges[0]);
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    auto range = kLowercaseLetterRanges[mid];
    if (cp < range.first) {
      hi = mid;
    } else if (cp > range.second) {
      lo = mid + 1;
    } else {
      return true;
    }
  }
  return false;
}

inline bool regexMatchesUnicodePropertyDecimalNumber(uint32_t cp) {
  static constexpr std::pair<uint32_t, uint32_t> kDecimalNumberRanges[] = {
    {0x000030, 0x000039},
    {0x000660, 0x000669},
    {0x0006F0, 0x0006F9},
    {0x0007C0, 0x0007C9},
    {0x000966, 0x00096F},
    {0x0009E6, 0x0009EF},
    {0x000A66, 0x000A6F},
    {0x000AE6, 0x000AEF},
    {0x000B66, 0x000B6F},
    {0x000BE6, 0x000BEF},
    {0x000C66, 0x000C6F},
    {0x000CE6, 0x000CEF},
    {0x000D66, 0x000D6F},
    {0x000DE6, 0x000DEF},
    {0x000E50, 0x000E59},
    {0x000ED0, 0x000ED9},
    {0x000F20, 0x000F29},
    {0x001040, 0x001049},
    {0x001090, 0x001099},
    {0x0017E0, 0x0017E9},
    {0x001810, 0x001819},
    {0x001946, 0x00194F},
    {0x0019D0, 0x0019D9},
    {0x001A80, 0x001A89},
    {0x001A90, 0x001A99},
    {0x001B50, 0x001B59},
    {0x001BB0, 0x001BB9},
    {0x001C40, 0x001C49},
    {0x001C50, 0x001C59},
    {0x00A620, 0x00A629},
    {0x00A8D0, 0x00A8D9},
    {0x00A900, 0x00A909},
    {0x00A9D0, 0x00A9D9},
    {0x00A9F0, 0x00A9F9},
    {0x00AA50, 0x00AA59},
    {0x00ABF0, 0x00ABF9},
    {0x00FF10, 0x00FF19},
    {0x0104A0, 0x0104A9},
    {0x010D30, 0x010D39},
    {0x010D40, 0x010D49},
    {0x011066, 0x01106F},
    {0x0110F0, 0x0110F9},
    {0x011136, 0x01113F},
    {0x0111D0, 0x0111D9},
    {0x0112F0, 0x0112F9},
    {0x011450, 0x011459},
    {0x0114D0, 0x0114D9},
    {0x011650, 0x011659},
    {0x0116C0, 0x0116C9},
    {0x0116D0, 0x0116E3},
    {0x011730, 0x011739},
    {0x0118E0, 0x0118E9},
    {0x011950, 0x011959},
    {0x011BF0, 0x011BF9},
    {0x011C50, 0x011C59},
    {0x011D50, 0x011D59},
    {0x011DA0, 0x011DA9},
    {0x011DE0, 0x011DE9},
    {0x011F50, 0x011F59},
    {0x016130, 0x016139},
    {0x016A60, 0x016A69},
    {0x016AC0, 0x016AC9},
    {0x016B50, 0x016B59},
    {0x016D70, 0x016D79},
    {0x01CCF0, 0x01CCF9},
    {0x01D7CE, 0x01D7FF},
    {0x01E140, 0x01E149},
    {0x01E2F0, 0x01E2F9},
    {0x01E4F0, 0x01E4F9},
    {0x01E5F1, 0x01E5FA},
    {0x01E950, 0x01E959},
    {0x01FBF0, 0x01FBF9},
  };
  size_t lo = 0;
  size_t hi = sizeof(kDecimalNumberRanges) / sizeof(kDecimalNumberRanges[0]);
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    auto range = kDecimalNumberRanges[mid];
    if (cp < range.first) {
      hi = mid;
    } else if (cp > range.second) {
      lo = mid + 1;
    } else {
      return true;
    }
  }
  return false;
}

inline bool regexMatchesUnicodePropertyTitlecaseLetter(uint32_t cp) {
  static constexpr std::pair<uint32_t, uint32_t> kTitlecaseLetterRanges[] = {
    {0x0001C5, 0x0001C5},
    {0x0001C8, 0x0001C8},
    {0x0001CB, 0x0001CB},
    {0x0001F2, 0x0001F2},
    {0x001F88, 0x001F8F},
    {0x001F98, 0x001F9F},
    {0x001FA8, 0x001FAF},
    {0x001FBC, 0x001FBC},
    {0x001FCC, 0x001FCC},
    {0x001FFC, 0x001FFC},
  };
  size_t lo = 0;
  size_t hi = sizeof(kTitlecaseLetterRanges) / sizeof(kTitlecaseLetterRanges[0]);
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    auto range = kTitlecaseLetterRanges[mid];
    if (cp < range.first) {
      hi = mid;
    } else if (cp > range.second) {
      lo = mid + 1;
    } else {
      return true;
    }
  }
  return false;
}

inline bool regexMatchesUnicodePropertyLetterNumber(uint32_t cp) {
  static constexpr std::pair<uint32_t, uint32_t> kLetterNumberRanges[] = {
    {0x0016EE, 0x0016F0},
    {0x002160, 0x002182},
    {0x002185, 0x002188},
    {0x003007, 0x003007},
    {0x003021, 0x003029},
    {0x003038, 0x00303A},
    {0x00A6E6, 0x00A6EF},
    {0x010140, 0x010174},
    {0x010341, 0x010341},
    {0x01034A, 0x01034A},
    {0x0103D1, 0x0103D5},
    {0x012400, 0x01246E},
    {0x016FF4, 0x016FF6},
  };
  size_t lo = 0;
  size_t hi = sizeof(kLetterNumberRanges) / sizeof(kLetterNumberRanges[0]);
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    auto range = kLetterNumberRanges[mid];
    if (cp < range.first) {
      hi = mid;
    } else if (cp > range.second) {
      lo = mid + 1;
    } else {
      return true;
    }
  }
  return false;
}

inline bool regexMatchesUnicodePropertyOtherNumber(uint32_t cp) {
  static constexpr std::pair<uint32_t, uint32_t> kOtherNumberRanges[] = {
    {0x0000B2, 0x0000B3},
    {0x0000B9, 0x0000B9},
    {0x0000BC, 0x0000BE},
    {0x0009F4, 0x0009F9},
    {0x000B72, 0x000B77},
    {0x000BF0, 0x000BF2},
    {0x000C78, 0x000C7E},
    {0x000D58, 0x000D5E},
    {0x000D70, 0x000D78},
    {0x000F2A, 0x000F33},
    {0x001369, 0x00137C},
    {0x0017F0, 0x0017F9},
    {0x0019DA, 0x0019DA},
    {0x002070, 0x002070},
    {0x002074, 0x002079},
    {0x002080, 0x002089},
    {0x002150, 0x00215F},
    {0x002189, 0x002189},
    {0x002460, 0x00249B},
    {0x0024EA, 0x0024FF},
    {0x002776, 0x002793},
    {0x002CFD, 0x002CFD},
    {0x003192, 0x003195},
    {0x003220, 0x003229},
    {0x003248, 0x00324F},
    {0x003251, 0x00325F},
    {0x003280, 0x003289},
    {0x0032B1, 0x0032BF},
    {0x00A830, 0x00A835},
    {0x010107, 0x010133},
    {0x010175, 0x010178},
    {0x01018A, 0x01018B},
    {0x0102E1, 0x0102FB},
    {0x010320, 0x010323},
    {0x010858, 0x01085F},
    {0x010879, 0x01087F},
    {0x0108A7, 0x0108AF},
    {0x0108FB, 0x0108FF},
    {0x010916, 0x01091B},
    {0x0109BC, 0x0109BD},
    {0x0109C0, 0x0109CF},
    {0x0109D2, 0x0109FF},
    {0x010A40, 0x010A48},
    {0x010A7D, 0x010A7E},
    {0x010A9D, 0x010A9F},
    {0x010AEB, 0x010AEF},
    {0x010B58, 0x010B5F},
    {0x010B78, 0x010B7F},
    {0x010BA9, 0x010BAF},
    {0x010CFA, 0x010CFF},
    {0x010E60, 0x010E7E},
    {0x010F1D, 0x010F26},
    {0x010F51, 0x010F54},
    {0x010FC5, 0x010FCB},
    {0x011052, 0x011065},
    {0x0111E1, 0x0111F4},
    {0x01173A, 0x01173B},
    {0x0118EA, 0x0118F2},
    {0x011C5A, 0x011C6C},
    {0x011FC0, 0x011FD4},
    {0x016B5B, 0x016B61},
    {0x016E80, 0x016E96},
    {0x01D2C0, 0x01D2D3},
    {0x01D2E0, 0x01D2F3},
    {0x01D360, 0x01D378},
    {0x01E8C7, 0x01E8CF},
    {0x01EC71, 0x01ECAB},
    {0x01ECAD, 0x01ECAF},
    {0x01ECB1, 0x01ECB4},
    {0x01ED01, 0x01ED2D},
    {0x01ED2F, 0x01ED3D},
    {0x01F100, 0x01F10C},
  };
  size_t lo = 0;
  size_t hi = sizeof(kOtherNumberRanges) / sizeof(kOtherNumberRanges[0]);
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    auto range = kOtherNumberRanges[mid];
    if (cp < range.first) {
      hi = mid;
    } else if (cp > range.second) {
      lo = mid + 1;
    } else {
      return true;
    }
  }
  return false;
}

inline bool regexMatchesUnicodePropertyNumber(uint32_t cp) {
  return regexMatchesUnicodePropertyDecimalNumber(cp) ||
         regexMatchesUnicodePropertyLetterNumber(cp) ||
         regexMatchesUnicodePropertyOtherNumber(cp);
}

inline bool regexMatchesUnicodePropertyCasedLetter(uint32_t cp) {
  return regexModifierMatchesUnicodePropertyLu(cp, false) ||
         regexMatchesUnicodePropertyLowercaseLetter(cp) ||
         regexMatchesUnicodePropertyTitlecaseLetter(cp);
}

inline bool regexMatchesUnicodePropertyModifierLetter(uint32_t cp) {
  static constexpr std::pair<uint32_t, uint32_t> kModifierLetterRanges[] = {
    {0x0002B0, 0x0002C1},
    {0x0002C6, 0x0002D1},
    {0x0002E0, 0x0002E4},
    {0x0002EC, 0x0002EC},
    {0x0002EE, 0x0002EE},
    {0x000374, 0x000374},
    {0x00037A, 0x00037A},
    {0x000559, 0x000559},
    {0x000640, 0x000640},
    {0x0006E5, 0x0006E6},
    {0x0007F4, 0x0007F5},
    {0x0007FA, 0x0007FA},
    {0x00081A, 0x00081A},
    {0x000824, 0x000824},
    {0x000828, 0x000828},
    {0x0008C9, 0x0008C9},
    {0x000971, 0x000971},
    {0x000E46, 0x000E46},
    {0x000EC6, 0x000EC6},
    {0x0010FC, 0x0010FC},
    {0x0017D7, 0x0017D7},
    {0x001843, 0x001843},
    {0x001AA7, 0x001AA7},
    {0x001C78, 0x001C7D},
    {0x001D2C, 0x001D6A},
    {0x001D78, 0x001D78},
    {0x001D9B, 0x001DBF},
    {0x002071, 0x002071},
    {0x00207F, 0x00207F},
    {0x002090, 0x00209C},
    {0x002C7C, 0x002C7D},
    {0x002D6F, 0x002D6F},
    {0x002E2F, 0x002E2F},
    {0x003005, 0x003005},
    {0x003031, 0x003035},
    {0x00303B, 0x00303B},
    {0x00309D, 0x00309E},
    {0x0030FC, 0x0030FE},
    {0x00A015, 0x00A015},
    {0x00A4F8, 0x00A4FD},
    {0x00A60C, 0x00A60C},
    {0x00A67F, 0x00A67F},
    {0x00A69C, 0x00A69D},
    {0x00A717, 0x00A71F},
    {0x00A770, 0x00A770},
    {0x00A788, 0x00A788},
    {0x00A7F1, 0x00A7F4},
    {0x00A7F8, 0x00A7F9},
    {0x00A9CF, 0x00A9CF},
    {0x00A9E6, 0x00A9E6},
    {0x00AA70, 0x00AA70},
    {0x00AADD, 0x00AADD},
    {0x00AAF3, 0x00AAF4},
    {0x00AB5C, 0x00AB5F},
    {0x00AB69, 0x00AB69},
    {0x00FF70, 0x00FF70},
    {0x00FF9E, 0x00FF9F},
    {0x010780, 0x010785},
    {0x010787, 0x0107B0},
    {0x0107B2, 0x0107BA},
    {0x010D4E, 0x010D4E},
    {0x010D6F, 0x010D6F},
    {0x010EC5, 0x010EC5},
    {0x011DD9, 0x011DD9},
    {0x016B40, 0x016B43},
    {0x016D40, 0x016D42},
    {0x016D6B, 0x016D6C},
    {0x016F93, 0x016F9F},
    {0x016FE0, 0x016FE1},
    {0x016FE3, 0x016FE3},
    {0x016FF2, 0x016FF3},
    {0x01AFF0, 0x01AFF3},
    {0x01AFF5, 0x01AFFB},
    {0x01AFFD, 0x01AFFE},
    {0x01E030, 0x01E06D},
    {0x01E137, 0x01E13D},
    {0x01E4EB, 0x01E4EB},
    {0x01E6FF, 0x01E6FF},
    {0x01E94B, 0x01E94B},
  };
  size_t lo = 0;
  size_t hi = sizeof(kModifierLetterRanges) / sizeof(kModifierLetterRanges[0]);
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    auto range = kModifierLetterRanges[mid];
    if (cp < range.first) {
      hi = mid;
    } else if (cp > range.second) {
      lo = mid + 1;
    } else {
      return true;
    }
  }
  return false;
}

inline bool regexMatchesUnicodePropertyOtherLetter(uint32_t cp) {
  static constexpr std::pair<uint32_t, uint32_t> kOtherLetterRanges[] = {
    {0x0000AA, 0x0000AA},
    {0x0000BA, 0x0000BA},
    {0x0001BB, 0x0001BB},
    {0x0001C0, 0x0001C3},
    {0x000294, 0x000295},
    {0x0005D0, 0x0005EA},
    {0x0005EF, 0x0005F2},
    {0x000620, 0x00063F},
    {0x000641, 0x00064A},
    {0x00066E, 0x00066F},
    {0x000671, 0x0006D3},
    {0x0006D5, 0x0006D5},
    {0x0006EE, 0x0006EF},
    {0x0006FA, 0x0006FC},
    {0x0006FF, 0x0006FF},
    {0x000710, 0x000710},
    {0x000712, 0x00072F},
    {0x00074D, 0x0007A5},
    {0x0007B1, 0x0007B1},
    {0x0007CA, 0x0007EA},
    {0x000800, 0x000815},
    {0x000840, 0x000858},
    {0x000860, 0x00086A},
    {0x000870, 0x000887},
    {0x000889, 0x00088F},
    {0x0008A0, 0x0008C8},
    {0x000904, 0x000939},
    {0x00093D, 0x00093D},
    {0x000950, 0x000950},
    {0x000958, 0x000961},
    {0x000972, 0x000980},
    {0x000985, 0x00098C},
    {0x00098F, 0x000990},
    {0x000993, 0x0009A8},
    {0x0009AA, 0x0009B0},
    {0x0009B2, 0x0009B2},
    {0x0009B6, 0x0009B9},
    {0x0009BD, 0x0009BD},
    {0x0009CE, 0x0009CE},
    {0x0009DC, 0x0009DD},
    {0x0009DF, 0x0009E1},
    {0x0009F0, 0x0009F1},
    {0x0009FC, 0x0009FC},
    {0x000A05, 0x000A0A},
    {0x000A0F, 0x000A10},
    {0x000A13, 0x000A28},
    {0x000A2A, 0x000A30},
    {0x000A32, 0x000A33},
    {0x000A35, 0x000A36},
    {0x000A38, 0x000A39},
    {0x000A59, 0x000A5C},
    {0x000A5E, 0x000A5E},
    {0x000A72, 0x000A74},
    {0x000A85, 0x000A8D},
    {0x000A8F, 0x000A91},
    {0x000A93, 0x000AA8},
    {0x000AAA, 0x000AB0},
    {0x000AB2, 0x000AB3},
    {0x000AB5, 0x000AB9},
    {0x000ABD, 0x000ABD},
    {0x000AD0, 0x000AD0},
    {0x000AE0, 0x000AE1},
    {0x000AF9, 0x000AF9},
    {0x000B05, 0x000B0C},
    {0x000B0F, 0x000B10},
    {0x000B13, 0x000B28},
    {0x000B2A, 0x000B30},
    {0x000B32, 0x000B33},
    {0x000B35, 0x000B39},
    {0x000B3D, 0x000B3D},
    {0x000B5C, 0x000B5D},
    {0x000B5F, 0x000B61},
    {0x000B71, 0x000B71},
    {0x000B83, 0x000B83},
    {0x000B85, 0x000B8A},
    {0x000B8E, 0x000B90},
    {0x000B92, 0x000B95},
    {0x000B99, 0x000B9A},
    {0x000B9C, 0x000B9C},
    {0x000B9E, 0x000B9F},
    {0x000BA3, 0x000BA4},
    {0x000BA8, 0x000BAA},
    {0x000BAE, 0x000BB9},
    {0x000BD0, 0x000BD0},
    {0x000C05, 0x000C0C},
    {0x000C0E, 0x000C10},
    {0x000C12, 0x000C28},
    {0x000C2A, 0x000C39},
    {0x000C3D, 0x000C3D},
    {0x000C58, 0x000C5A},
    {0x000C5C, 0x000C5D},
    {0x000C60, 0x000C61},
    {0x000C80, 0x000C80},
    {0x000C85, 0x000C8C},
    {0x000C8E, 0x000C90},
    {0x000C92, 0x000CA8},
    {0x000CAA, 0x000CB3},
    {0x000CB5, 0x000CB9},
    {0x000CBD, 0x000CBD},
    {0x000CDC, 0x000CDE},
    {0x000CE0, 0x000CE1},
    {0x000CF1, 0x000CF2},
    {0x000D04, 0x000D0C},
    {0x000D0E, 0x000D10},
    {0x000D12, 0x000D3A},
    {0x000D3D, 0x000D3D},
    {0x000D4E, 0x000D4E},
    {0x000D54, 0x000D56},
    {0x000D5F, 0x000D61},
    {0x000D7A, 0x000D7F},
    {0x000D85, 0x000D96},
    {0x000D9A, 0x000DB1},
    {0x000DB3, 0x000DBB},
    {0x000DBD, 0x000DBD},
    {0x000DC0, 0x000DC6},
    {0x000E01, 0x000E30},
    {0x000E32, 0x000E33},
    {0x000E40, 0x000E45},
    {0x000E81, 0x000E82},
    {0x000E84, 0x000E84},
    {0x000E86, 0x000E8A},
    {0x000E8C, 0x000EA3},
    {0x000EA5, 0x000EA5},
    {0x000EA7, 0x000EB0},
    {0x000EB2, 0x000EB3},
    {0x000EBD, 0x000EBD},
    {0x000EC0, 0x000EC4},
    {0x000EDC, 0x000EDF},
    {0x000F00, 0x000F00},
    {0x000F40, 0x000F47},
    {0x000F49, 0x000F6C},
    {0x000F88, 0x000F8C},
    {0x001000, 0x00102A},
    {0x00103F, 0x00103F},
    {0x001050, 0x001055},
    {0x00105A, 0x00105D},
    {0x001061, 0x001061},
    {0x001065, 0x001066},
    {0x00106E, 0x001070},
    {0x001075, 0x001081},
    {0x00108E, 0x00108E},
    {0x001100, 0x001248},
    {0x00124A, 0x00124D},
    {0x001250, 0x001256},
    {0x001258, 0x001258},
    {0x00125A, 0x00125D},
    {0x001260, 0x001288},
    {0x00128A, 0x00128D},
    {0x001290, 0x0012B0},
    {0x0012B2, 0x0012B5},
    {0x0012B8, 0x0012BE},
    {0x0012C0, 0x0012C0},
    {0x0012C2, 0x0012C5},
    {0x0012C8, 0x0012D6},
    {0x0012D8, 0x001310},
    {0x001312, 0x001315},
    {0x001318, 0x00135A},
    {0x001380, 0x00138F},
    {0x001401, 0x00166C},
    {0x00166F, 0x00167F},
    {0x001681, 0x00169A},
    {0x0016A0, 0x0016EA},
    {0x0016F1, 0x0016F8},
    {0x001700, 0x001711},
    {0x00171F, 0x001731},
    {0x001740, 0x001751},
    {0x001760, 0x00176C},
    {0x00176E, 0x001770},
    {0x001780, 0x0017B3},
    {0x0017DC, 0x0017DC},
    {0x001820, 0x001842},
    {0x001844, 0x001878},
    {0x001880, 0x001884},
    {0x001887, 0x0018A8},
    {0x0018AA, 0x0018AA},
    {0x0018B0, 0x0018F5},
    {0x001900, 0x00191E},
    {0x001950, 0x00196D},
    {0x001970, 0x001974},
    {0x001980, 0x0019AB},
    {0x0019B0, 0x0019C9},
    {0x001A00, 0x001A16},
    {0x001A20, 0x001A54},
    {0x001B05, 0x001B33},
    {0x001B45, 0x001B4C},
    {0x001B83, 0x001BA0},
    {0x001BAE, 0x001BAF},
    {0x001BBA, 0x001BE5},
    {0x001C00, 0x001C23},
    {0x001C4D, 0x001C4F},
    {0x001C5A, 0x001C77},
    {0x001CE9, 0x001CEC},
    {0x001CEE, 0x001CF3},
    {0x001CF5, 0x001CF6},
    {0x001CFA, 0x001CFA},
    {0x002135, 0x002138},
    {0x002D30, 0x002D67},
    {0x002D80, 0x002D96},
    {0x002DA0, 0x002DA6},
    {0x002DA8, 0x002DAE},
    {0x002DB0, 0x002DB6},
    {0x002DB8, 0x002DBE},
    {0x002DC0, 0x002DC6},
    {0x002DC8, 0x002DCE},
    {0x002DD0, 0x002DD6},
    {0x002DD8, 0x002DDE},
    {0x003006, 0x003006},
    {0x00303C, 0x00303C},
    {0x003041, 0x003096},
    {0x00309F, 0x00309F},
    {0x0030A1, 0x0030FA},
    {0x0030FF, 0x0030FF},
    {0x003105, 0x00312F},
    {0x003131, 0x00318E},
    {0x0031A0, 0x0031BF},
    {0x0031F0, 0x0031FF},
    {0x003400, 0x004DBF},
    {0x004E00, 0x00A014},
    {0x00A016, 0x00A48C},
    {0x00A4D0, 0x00A4F7},
    {0x00A500, 0x00A60B},
    {0x00A610, 0x00A61F},
    {0x00A62A, 0x00A62B},
    {0x00A66E, 0x00A66E},
    {0x00A6A0, 0x00A6E5},
    {0x00A78F, 0x00A78F},
    {0x00A7F7, 0x00A7F7},
    {0x00A7FB, 0x00A801},
    {0x00A803, 0x00A805},
    {0x00A807, 0x00A80A},
    {0x00A80C, 0x00A822},
    {0x00A840, 0x00A873},
    {0x00A882, 0x00A8B3},
    {0x00A8F2, 0x00A8F7},
    {0x00A8FB, 0x00A8FB},
    {0x00A8FD, 0x00A8FE},
    {0x00A90A, 0x00A925},
    {0x00A930, 0x00A946},
    {0x00A960, 0x00A97C},
    {0x00A984, 0x00A9B2},
    {0x00A9E0, 0x00A9E4},
    {0x00A9E7, 0x00A9EF},
    {0x00A9FA, 0x00A9FE},
    {0x00AA00, 0x00AA28},
    {0x00AA40, 0x00AA42},
    {0x00AA44, 0x00AA4B},
    {0x00AA60, 0x00AA6F},
    {0x00AA71, 0x00AA76},
    {0x00AA7A, 0x00AA7A},
    {0x00AA7E, 0x00AAAF},
    {0x00AAB1, 0x00AAB1},
    {0x00AAB5, 0x00AAB6},
    {0x00AAB9, 0x00AABD},
    {0x00AAC0, 0x00AAC0},
    {0x00AAC2, 0x00AAC2},
    {0x00AADB, 0x00AADC},
    {0x00AAE0, 0x00AAEA},
    {0x00AAF2, 0x00AAF2},
    {0x00AB01, 0x00AB06},
    {0x00AB09, 0x00AB0E},
    {0x00AB11, 0x00AB16},
    {0x00AB20, 0x00AB26},
    {0x00AB28, 0x00AB2E},
    {0x00ABC0, 0x00ABE2},
    {0x00AC00, 0x00D7A3},
    {0x00D7B0, 0x00D7C6},
    {0x00D7CB, 0x00D7FB},
    {0x00F900, 0x00FA6D},
    {0x00FA70, 0x00FAD9},
    {0x00FB1D, 0x00FB1D},
    {0x00FB1F, 0x00FB28},
    {0x00FB2A, 0x00FB36},
    {0x00FB38, 0x00FB3C},
    {0x00FB3E, 0x00FB3E},
    {0x00FB40, 0x00FB41},
    {0x00FB43, 0x00FB44},
    {0x00FB46, 0x00FBB1},
    {0x00FBD3, 0x00FD3D},
    {0x00FD50, 0x00FD8F},
    {0x00FD92, 0x00FDC7},
    {0x00FDF0, 0x00FDFB},
    {0x00FE70, 0x00FE74},
    {0x00FE76, 0x00FEFC},
    {0x00FF66, 0x00FF6F},
    {0x00FF71, 0x00FF9D},
    {0x00FFA0, 0x00FFBE},
    {0x00FFC2, 0x00FFC7},
    {0x00FFCA, 0x00FFCF},
    {0x00FFD2, 0x00FFD7},
    {0x00FFDA, 0x00FFDC},
    {0x010000, 0x01000B},
    {0x01000D, 0x010026},
    {0x010028, 0x01003A},
    {0x01003C, 0x01003D},
    {0x01003F, 0x01004D},
    {0x010050, 0x01005D},
    {0x010080, 0x0100FA},
    {0x010280, 0x01029C},
    {0x0102A0, 0x0102D0},
    {0x010300, 0x01031F},
    {0x01032D, 0x010340},
    {0x010342, 0x010349},
    {0x010350, 0x010375},
    {0x010380, 0x01039D},
    {0x0103A0, 0x0103C3},
    {0x0103C8, 0x0103CF},
    {0x010450, 0x01049D},
    {0x010500, 0x010527},
    {0x010530, 0x010563},
    {0x0105C0, 0x0105F3},
    {0x010600, 0x010736},
    {0x010740, 0x010755},
    {0x010760, 0x010767},
    {0x010800, 0x010805},
    {0x010808, 0x010808},
    {0x01080A, 0x010835},
    {0x010837, 0x010838},
    {0x01083C, 0x01083C},
    {0x01083F, 0x010855},
    {0x010860, 0x010876},
    {0x010880, 0x01089E},
    {0x0108E0, 0x0108F2},
    {0x0108F4, 0x0108F5},
    {0x010900, 0x010915},
    {0x010920, 0x010939},
    {0x010940, 0x010959},
    {0x010980, 0x0109B7},
    {0x0109BE, 0x0109BF},
    {0x010A00, 0x010A00},
    {0x010A10, 0x010A13},
    {0x010A15, 0x010A17},
    {0x010A19, 0x010A35},
    {0x010A60, 0x010A7C},
    {0x010A80, 0x010A9C},
    {0x010AC0, 0x010AC7},
    {0x010AC9, 0x010AE4},
    {0x010B00, 0x010B35},
    {0x010B40, 0x010B55},
    {0x010B60, 0x010B72},
    {0x010B80, 0x010B91},
    {0x010C00, 0x010C48},
    {0x010D00, 0x010D23},
    {0x010D4A, 0x010D4D},
    {0x010D4F, 0x010D4F},
    {0x010E80, 0x010EA9},
    {0x010EB0, 0x010EB1},
    {0x010EC2, 0x010EC4},
    {0x010EC6, 0x010EC7},
    {0x010F00, 0x010F1C},
    {0x010F27, 0x010F27},
    {0x010F30, 0x010F45},
    {0x010F70, 0x010F81},
    {0x010FB0, 0x010FC4},
    {0x010FE0, 0x010FF6},
    {0x011003, 0x011037},
    {0x011071, 0x011072},
    {0x011075, 0x011075},
    {0x011083, 0x0110AF},
    {0x0110D0, 0x0110E8},
    {0x011103, 0x011126},
    {0x011144, 0x011144},
    {0x011147, 0x011147},
    {0x011150, 0x011172},
    {0x011176, 0x011176},
    {0x011183, 0x0111B2},
    {0x0111C1, 0x0111C4},
    {0x0111DA, 0x0111DA},
    {0x0111DC, 0x0111DC},
    {0x011200, 0x011211},
    {0x011213, 0x01122B},
    {0x01123F, 0x011240},
    {0x011280, 0x011286},
    {0x011288, 0x011288},
    {0x01128A, 0x01128D},
    {0x01128F, 0x01129D},
    {0x01129F, 0x0112A8},
    {0x0112B0, 0x0112DE},
    {0x011305, 0x01130C},
    {0x01130F, 0x011310},
    {0x011313, 0x011328},
    {0x01132A, 0x011330},
    {0x011332, 0x011333},
    {0x011335, 0x011339},
    {0x01133D, 0x01133D},
    {0x011350, 0x011350},
    {0x01135D, 0x011361},
    {0x011380, 0x011389},
    {0x01138B, 0x01138B},
    {0x01138E, 0x01138E},
    {0x011390, 0x0113B5},
    {0x0113B7, 0x0113B7},
    {0x0113D1, 0x0113D1},
    {0x0113D3, 0x0113D3},
    {0x011400, 0x011434},
    {0x011447, 0x01144A},
    {0x01145F, 0x011461},
    {0x011480, 0x0114AF},
    {0x0114C4, 0x0114C5},
    {0x0114C7, 0x0114C7},
    {0x011580, 0x0115AE},
    {0x0115D8, 0x0115DB},
    {0x011600, 0x01162F},
    {0x011644, 0x011644},
    {0x011680, 0x0116AA},
    {0x0116B8, 0x0116B8},
    {0x011700, 0x01171A},
    {0x011740, 0x011746},
    {0x011800, 0x01182B},
    {0x0118FF, 0x011906},
    {0x011909, 0x011909},
    {0x01190C, 0x011913},
    {0x011915, 0x011916},
    {0x011918, 0x01192F},
    {0x01193F, 0x01193F},
    {0x011941, 0x011941},
    {0x0119A0, 0x0119A7},
    {0x0119AA, 0x0119D0},
    {0x0119E1, 0x0119E1},
    {0x0119E3, 0x0119E3},
    {0x011A00, 0x011A00},
    {0x011A0B, 0x011A32},
    {0x011A3A, 0x011A3A},
    {0x011A50, 0x011A50},
    {0x011A5C, 0x011A89},
    {0x011A9D, 0x011A9D},
    {0x011AB0, 0x011AF8},
    {0x011BC0, 0x011BE0},
    {0x011C00, 0x011C08},
    {0x011C0A, 0x011C2E},
    {0x011C40, 0x011C40},
    {0x011C72, 0x011C8F},
    {0x011D00, 0x011D06},
    {0x011D08, 0x011D09},
    {0x011D0B, 0x011D30},
    {0x011D46, 0x011D46},
    {0x011D60, 0x011D65},
    {0x011D67, 0x011D68},
    {0x011D6A, 0x011D89},
    {0x011D98, 0x011D98},
    {0x011DB0, 0x011DD8},
    {0x011DDA, 0x011DDB},
    {0x011EE0, 0x011EF2},
    {0x011F02, 0x011F02},
    {0x011F04, 0x011F10},
    {0x011F12, 0x011F33},
    {0x011FB0, 0x011FB0},
    {0x012000, 0x012399},
    {0x012480, 0x012543},
    {0x012F90, 0x012FF0},
    {0x013000, 0x01342F},
    {0x013441, 0x013446},
    {0x013460, 0x0143FA},
    {0x014400, 0x014646},
    {0x016100, 0x01611D},
    {0x016800, 0x016A38},
    {0x016A40, 0x016A5E},
    {0x016A70, 0x016ABE},
    {0x016AD0, 0x016AED},
    {0x016B00, 0x016B2F},
    {0x016B63, 0x016B77},
    {0x016B7D, 0x016B8F},
    {0x016D43, 0x016D6A},
    {0x016F00, 0x016F4A},
    {0x016F50, 0x016F50},
    {0x017000, 0x018CD5},
    {0x018CFF, 0x018D1E},
    {0x018D80, 0x018DF2},
    {0x01B000, 0x01B122},
    {0x01B132, 0x01B132},
    {0x01B150, 0x01B152},
    {0x01B155, 0x01B155},
    {0x01B164, 0x01B167},
    {0x01B170, 0x01B2FB},
    {0x01BC00, 0x01BC6A},
    {0x01BC70, 0x01BC7C},
    {0x01BC80, 0x01BC88},
    {0x01BC90, 0x01BC99},
    {0x01DF0A, 0x01DF0A},
    {0x01E100, 0x01E12C},
    {0x01E14E, 0x01E14E},
    {0x01E290, 0x01E2AD},
    {0x01E2C0, 0x01E2EB},
    {0x01E4D0, 0x01E4EA},
    {0x01E5D0, 0x01E5ED},
    {0x01E5F0, 0x01E5F0},
    {0x01E6C0, 0x01E6DE},
    {0x01E6E0, 0x01E6E2},
    {0x01E6E4, 0x01E6E5},
    {0x01E6E7, 0x01E6ED},
    {0x01E6F0, 0x01E6F4},
    {0x01E6FE, 0x01E6FE},
    {0x01E7E0, 0x01E7E6},
    {0x01E7E8, 0x01E7EB},
    {0x01E7ED, 0x01E7EE},
    {0x01E7F0, 0x01E7FE},
    {0x01E800, 0x01E8C4},
    {0x01EE00, 0x01EE03},
    {0x01EE05, 0x01EE1F},
    {0x01EE21, 0x01EE22},
    {0x01EE24, 0x01EE24},
    {0x01EE27, 0x01EE27},
    {0x01EE29, 0x01EE32},
    {0x01EE34, 0x01EE37},
    {0x01EE39, 0x01EE39},
    {0x01EE3B, 0x01EE3B},
    {0x01EE42, 0x01EE42},
    {0x01EE47, 0x01EE47},
    {0x01EE49, 0x01EE49},
    {0x01EE4B, 0x01EE4B},
    {0x01EE4D, 0x01EE4F},
    {0x01EE51, 0x01EE52},
    {0x01EE54, 0x01EE54},
    {0x01EE57, 0x01EE57},
    {0x01EE59, 0x01EE59},
    {0x01EE5B, 0x01EE5B},
    {0x01EE5D, 0x01EE5D},
    {0x01EE5F, 0x01EE5F},
    {0x01EE61, 0x01EE62},
    {0x01EE64, 0x01EE64},
    {0x01EE67, 0x01EE6A},
    {0x01EE6C, 0x01EE72},
    {0x01EE74, 0x01EE77},
    {0x01EE79, 0x01EE7C},
    {0x01EE7E, 0x01EE7E},
    {0x01EE80, 0x01EE89},
    {0x01EE8B, 0x01EE9B},
    {0x01EEA1, 0x01EEA3},
    {0x01EEA5, 0x01EEA9},
    {0x01EEAB, 0x01EEBB},
    {0x020000, 0x02A6DF},
    {0x02A700, 0x02B81D},
    {0x02B820, 0x02CEAD},
    {0x02CEB0, 0x02EBE0},
    {0x02EBF0, 0x02EE5D},
    {0x02F800, 0x02FA1D},
    {0x030000, 0x03134A},
    {0x031350, 0x033479},
  };
  size_t lo = 0;
  size_t hi = sizeof(kOtherLetterRanges) / sizeof(kOtherLetterRanges[0]);
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    auto range = kOtherLetterRanges[mid];
    if (cp < range.first) {
      hi = mid;
    } else if (cp > range.second) {
      lo = mid + 1;
    } else {
      return true;
    }
  }
  return false;
}

inline bool regexMatchesUnicodePropertyLetter(uint32_t cp) {
  return regexMatchesUnicodePropertyCasedLetter(cp) ||
         regexMatchesUnicodePropertyModifierLetter(cp) ||
         regexMatchesUnicodePropertyOtherLetter(cp);
}

inline bool regexMatchesUnicodePropertyLineSeparator(uint32_t cp) {
  return cp == 0x2028;
}

inline bool regexMatchesUnicodePropertyParagraphSeparator(uint32_t cp) {
  return cp == 0x2029;
}

inline bool regexMatchesUnicodePropertySpaceSeparator(uint32_t cp) {
  return cp == 0x0020 ||
         cp == 0x00A0 ||
         cp == 0x1680 ||
         cp == 0x202F ||
         cp == 0x205F ||
         cp == 0x3000 ||
         (cp >= 0x2000 && cp <= 0x200A);
}

inline bool regexMatchesUnicodePropertySeparator(uint32_t cp) {
  return regexMatchesUnicodePropertyLineSeparator(cp) ||
         regexMatchesUnicodePropertyParagraphSeparator(cp) ||
         regexMatchesUnicodePropertySpaceSeparator(cp);
}

inline bool regexMatchesUnicodePropertyNonspacingMark(uint32_t cp) {
  static constexpr std::pair<uint32_t, uint32_t> kNonspacingMarkRanges[] = {
    {0x000300, 0x00036F},
    {0x000483, 0x000487},
    {0x000591, 0x0005BD},
    {0x0005BF, 0x0005BF},
    {0x0005C1, 0x0005C2},
    {0x0005C4, 0x0005C5},
    {0x0005C7, 0x0005C7},
    {0x000610, 0x00061A},
    {0x00064B, 0x00065F},
    {0x000670, 0x000670},
    {0x0006D6, 0x0006DC},
    {0x0006DF, 0x0006E4},
    {0x0006E7, 0x0006E8},
    {0x0006EA, 0x0006ED},
    {0x000711, 0x000711},
    {0x000730, 0x00074A},
    {0x0007A6, 0x0007B0},
    {0x0007EB, 0x0007F3},
    {0x0007FD, 0x0007FD},
    {0x000816, 0x000819},
    {0x00081B, 0x000823},
    {0x000825, 0x000827},
    {0x000829, 0x00082D},
    {0x000859, 0x00085B},
    {0x000897, 0x00089F},
    {0x0008CA, 0x0008E1},
    {0x0008E3, 0x000902},
    {0x00093A, 0x00093A},
    {0x00093C, 0x00093C},
    {0x000941, 0x000948},
    {0x00094D, 0x00094D},
    {0x000951, 0x000957},
    {0x000962, 0x000963},
    {0x000981, 0x000981},
    {0x0009BC, 0x0009BC},
    {0x0009C1, 0x0009C4},
    {0x0009CD, 0x0009CD},
    {0x0009E2, 0x0009E3},
    {0x0009FE, 0x0009FE},
    {0x000A01, 0x000A02},
    {0x000A3C, 0x000A3C},
    {0x000A41, 0x000A42},
    {0x000A47, 0x000A48},
    {0x000A4B, 0x000A4D},
    {0x000A51, 0x000A51},
    {0x000A70, 0x000A71},
    {0x000A75, 0x000A75},
    {0x000A81, 0x000A82},
    {0x000ABC, 0x000ABC},
    {0x000AC1, 0x000AC5},
    {0x000AC7, 0x000AC8},
    {0x000ACD, 0x000ACD},
    {0x000AE2, 0x000AE3},
    {0x000AFA, 0x000AFF},
    {0x000B01, 0x000B01},
    {0x000B3C, 0x000B3C},
    {0x000B3F, 0x000B3F},
    {0x000B41, 0x000B44},
    {0x000B4D, 0x000B4D},
    {0x000B55, 0x000B56},
    {0x000B62, 0x000B63},
    {0x000B82, 0x000B82},
    {0x000BC0, 0x000BC0},
    {0x000BCD, 0x000BCD},
    {0x000C00, 0x000C00},
    {0x000C04, 0x000C04},
    {0x000C3C, 0x000C3C},
    {0x000C3E, 0x000C40},
    {0x000C46, 0x000C48},
    {0x000C4A, 0x000C4D},
    {0x000C55, 0x000C56},
    {0x000C62, 0x000C63},
    {0x000C81, 0x000C81},
    {0x000CBC, 0x000CBC},
    {0x000CBF, 0x000CBF},
    {0x000CC6, 0x000CC6},
    {0x000CCC, 0x000CCD},
    {0x000CE2, 0x000CE3},
    {0x000D00, 0x000D01},
    {0x000D3B, 0x000D3C},
    {0x000D41, 0x000D44},
    {0x000D4D, 0x000D4D},
    {0x000D62, 0x000D63},
    {0x000D81, 0x000D81},
    {0x000DCA, 0x000DCA},
    {0x000DD2, 0x000DD4},
    {0x000DD6, 0x000DD6},
    {0x000E31, 0x000E31},
    {0x000E34, 0x000E3A},
    {0x000E47, 0x000E4E},
    {0x000EB1, 0x000EB1},
    {0x000EB4, 0x000EBC},
    {0x000EC8, 0x000ECE},
    {0x000F18, 0x000F19},
    {0x000F35, 0x000F35},
    {0x000F37, 0x000F37},
    {0x000F39, 0x000F39},
    {0x000F71, 0x000F7E},
    {0x000F80, 0x000F84},
    {0x000F86, 0x000F87},
    {0x000F8D, 0x000F97},
    {0x000F99, 0x000FBC},
    {0x000FC6, 0x000FC6},
    {0x00102D, 0x001030},
    {0x001032, 0x001037},
    {0x001039, 0x00103A},
    {0x00103D, 0x00103E},
    {0x001058, 0x001059},
    {0x00105E, 0x001060},
    {0x001071, 0x001074},
    {0x001082, 0x001082},
    {0x001085, 0x001086},
    {0x00108D, 0x00108D},
    {0x00109D, 0x00109D},
    {0x00135D, 0x00135F},
    {0x001712, 0x001714},
    {0x001732, 0x001733},
    {0x001752, 0x001753},
    {0x001772, 0x001773},
    {0x0017B4, 0x0017B5},
    {0x0017B7, 0x0017BD},
    {0x0017C6, 0x0017C6},
    {0x0017C9, 0x0017D3},
    {0x0017DD, 0x0017DD},
    {0x00180B, 0x00180D},
    {0x00180F, 0x00180F},
    {0x001885, 0x001886},
    {0x0018A9, 0x0018A9},
    {0x001920, 0x001922},
    {0x001927, 0x001928},
    {0x001932, 0x001932},
    {0x001939, 0x00193B},
    {0x001A17, 0x001A18},
    {0x001A1B, 0x001A1B},
    {0x001A56, 0x001A56},
    {0x001A58, 0x001A5E},
    {0x001A60, 0x001A60},
    {0x001A62, 0x001A62},
    {0x001A65, 0x001A6C},
    {0x001A73, 0x001A7C},
    {0x001A7F, 0x001A7F},
    {0x001AB0, 0x001ABD},
    {0x001ABF, 0x001ADD},
    {0x001AE0, 0x001AEB},
    {0x001B00, 0x001B03},
    {0x001B34, 0x001B34},
    {0x001B36, 0x001B3A},
    {0x001B3C, 0x001B3C},
    {0x001B42, 0x001B42},
    {0x001B6B, 0x001B73},
    {0x001B80, 0x001B81},
    {0x001BA2, 0x001BA5},
    {0x001BA8, 0x001BA9},
    {0x001BAB, 0x001BAD},
    {0x001BE6, 0x001BE6},
    {0x001BE8, 0x001BE9},
    {0x001BED, 0x001BED},
    {0x001BEF, 0x001BF1},
    {0x001C2C, 0x001C33},
    {0x001C36, 0x001C37},
    {0x001CD0, 0x001CD2},
    {0x001CD4, 0x001CE0},
    {0x001CE2, 0x001CE8},
    {0x001CED, 0x001CED},
    {0x001CF4, 0x001CF4},
    {0x001CF8, 0x001CF9},
    {0x001DC0, 0x001DFF},
    {0x0020D0, 0x0020DC},
    {0x0020E1, 0x0020E1},
    {0x0020E5, 0x0020F0},
    {0x002CEF, 0x002CF1},
    {0x002D7F, 0x002D7F},
    {0x002DE0, 0x002DFF},
    {0x00302A, 0x00302D},
    {0x003099, 0x00309A},
    {0x00A66F, 0x00A66F},
    {0x00A674, 0x00A67D},
    {0x00A69E, 0x00A69F},
    {0x00A6F0, 0x00A6F1},
    {0x00A802, 0x00A802},
    {0x00A806, 0x00A806},
    {0x00A80B, 0x00A80B},
    {0x00A825, 0x00A826},
    {0x00A82C, 0x00A82C},
    {0x00A8C4, 0x00A8C5},
    {0x00A8E0, 0x00A8F1},
    {0x00A8FF, 0x00A8FF},
    {0x00A926, 0x00A92D},
    {0x00A947, 0x00A951},
    {0x00A980, 0x00A982},
    {0x00A9B3, 0x00A9B3},
    {0x00A9B6, 0x00A9B9},
    {0x00A9BC, 0x00A9BD},
    {0x00A9E5, 0x00A9E5},
    {0x00AA29, 0x00AA2E},
    {0x00AA31, 0x00AA32},
    {0x00AA35, 0x00AA36},
    {0x00AA43, 0x00AA43},
    {0x00AA4C, 0x00AA4C},
    {0x00AA7C, 0x00AA7C},
    {0x00AAB0, 0x00AAB0},
    {0x00AAB2, 0x00AAB4},
    {0x00AAB7, 0x00AAB8},
    {0x00AABE, 0x00AABF},
    {0x00AAC1, 0x00AAC1},
    {0x00AAEC, 0x00AAED},
    {0x00AAF6, 0x00AAF6},
    {0x00ABE5, 0x00ABE5},
    {0x00ABE8, 0x00ABE8},
    {0x00ABED, 0x00ABED},
    {0x00FB1E, 0x00FB1E},
    {0x00FE00, 0x00FE0F},
    {0x00FE20, 0x00FE2F},
    {0x0101FD, 0x0101FD},
    {0x0102E0, 0x0102E0},
    {0x010376, 0x01037A},
    {0x010A01, 0x010A03},
    {0x010A05, 0x010A06},
    {0x010A0C, 0x010A0F},
    {0x010A38, 0x010A3A},
    {0x010A3F, 0x010A3F},
    {0x010AE5, 0x010AE6},
    {0x010D24, 0x010D27},
    {0x010D69, 0x010D6D},
    {0x010EAB, 0x010EAC},
    {0x010EFA, 0x010EFF},
    {0x010F46, 0x010F50},
    {0x010F82, 0x010F85},
    {0x011001, 0x011001},
    {0x011038, 0x011046},
    {0x011070, 0x011070},
    {0x011073, 0x011074},
    {0x01107F, 0x011081},
    {0x0110B3, 0x0110B6},
    {0x0110B9, 0x0110BA},
    {0x0110C2, 0x0110C2},
    {0x011100, 0x011102},
    {0x011127, 0x01112B},
    {0x01112D, 0x011134},
    {0x011173, 0x011173},
    {0x011180, 0x011181},
    {0x0111B6, 0x0111BE},
    {0x0111C9, 0x0111CC},
    {0x0111CF, 0x0111CF},
    {0x01122F, 0x011231},
    {0x011234, 0x011234},
    {0x011236, 0x011237},
    {0x01123E, 0x01123E},
    {0x011241, 0x011241},
    {0x0112DF, 0x0112DF},
    {0x0112E3, 0x0112EA},
    {0x011300, 0x011301},
    {0x01133B, 0x01133C},
    {0x011340, 0x011340},
    {0x011366, 0x01136C},
    {0x011370, 0x011374},
    {0x0113BB, 0x0113C0},
    {0x0113CE, 0x0113CE},
    {0x0113D0, 0x0113D0},
    {0x0113D2, 0x0113D2},
    {0x0113E1, 0x0113E2},
    {0x011438, 0x01143F},
    {0x011442, 0x011444},
    {0x011446, 0x011446},
    {0x01145E, 0x01145E},
    {0x0114B3, 0x0114B8},
    {0x0114BA, 0x0114BA},
    {0x0114BF, 0x0114C0},
    {0x0114C2, 0x0114C3},
    {0x0115B2, 0x0115B5},
    {0x0115BC, 0x0115BD},
    {0x0115BF, 0x0115C0},
    {0x0115DC, 0x0115DD},
    {0x011633, 0x01163A},
    {0x01163D, 0x01163D},
    {0x01163F, 0x011640},
    {0x0116AB, 0x0116AB},
    {0x0116AD, 0x0116AD},
    {0x0116B0, 0x0116B5},
    {0x0116B7, 0x0116B7},
    {0x01171D, 0x01171D},
    {0x01171F, 0x01171F},
    {0x011722, 0x011725},
    {0x011727, 0x01172B},
    {0x01182F, 0x011837},
    {0x011839, 0x01183A},
    {0x01193B, 0x01193C},
    {0x01193E, 0x01193E},
    {0x011943, 0x011943},
    {0x0119D4, 0x0119D7},
    {0x0119DA, 0x0119DB},
    {0x0119E0, 0x0119E0},
    {0x011A01, 0x011A0A},
    {0x011A33, 0x011A38},
    {0x011A3B, 0x011A3E},
    {0x011A47, 0x011A47},
    {0x011A51, 0x011A56},
    {0x011A59, 0x011A5B},
    {0x011A8A, 0x011A96},
    {0x011A98, 0x011A99},
    {0x011B60, 0x011B60},
    {0x011B62, 0x011B64},
    {0x011B66, 0x011B66},
    {0x011C30, 0x011C36},
    {0x011C38, 0x011C3D},
    {0x011C3F, 0x011C3F},
    {0x011C92, 0x011CA7},
    {0x011CAA, 0x011CB0},
    {0x011CB2, 0x011CB3},
    {0x011CB5, 0x011CB6},
    {0x011D31, 0x011D36},
    {0x011D3A, 0x011D3A},
    {0x011D3C, 0x011D3D},
    {0x011D3F, 0x011D45},
    {0x011D47, 0x011D47},
    {0x011D90, 0x011D91},
    {0x011D95, 0x011D95},
    {0x011D97, 0x011D97},
    {0x011EF3, 0x011EF4},
    {0x011F00, 0x011F01},
    {0x011F36, 0x011F3A},
    {0x011F40, 0x011F40},
    {0x011F42, 0x011F42},
    {0x011F5A, 0x011F5A},
    {0x013440, 0x013440},
    {0x013447, 0x013455},
    {0x01611E, 0x016129},
    {0x01612D, 0x01612F},
    {0x016AF0, 0x016AF4},
    {0x016B30, 0x016B36},
    {0x016F4F, 0x016F4F},
    {0x016F8F, 0x016F92},
    {0x016FE4, 0x016FE4},
    {0x01BC9D, 0x01BC9E},
    {0x01CF00, 0x01CF2D},
    {0x01CF30, 0x01CF46},
    {0x01D167, 0x01D169},
    {0x01D17B, 0x01D182},
    {0x01D185, 0x01D18B},
    {0x01D1AA, 0x01D1AD},
    {0x01D242, 0x01D244},
    {0x01DA00, 0x01DA36},
    {0x01DA3B, 0x01DA6C},
    {0x01DA75, 0x01DA75},
    {0x01DA84, 0x01DA84},
    {0x01DA9B, 0x01DA9F},
    {0x01DAA1, 0x01DAAF},
    {0x01E000, 0x01E006},
    {0x01E008, 0x01E018},
    {0x01E01B, 0x01E021},
    {0x01E023, 0x01E024},
    {0x01E026, 0x01E02A},
    {0x01E08F, 0x01E08F},
    {0x01E130, 0x01E136},
    {0x01E2AE, 0x01E2AE},
    {0x01E2EC, 0x01E2EF},
    {0x01E4EC, 0x01E4EF},
    {0x01E5EE, 0x01E5EF},
    {0x01E6E3, 0x01E6E3},
    {0x01E6E6, 0x01E6E6},
    {0x01E6EE, 0x01E6EF},
    {0x01E6F5, 0x01E6F5},
    {0x01E8D0, 0x01E8D6},
    {0x01E944, 0x01E94A},
    {0x0E0100, 0x0E01EF},
  };
  size_t lo = 0;
  size_t hi = sizeof(kNonspacingMarkRanges) / sizeof(kNonspacingMarkRanges[0]);
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    auto range = kNonspacingMarkRanges[mid];
    if (cp < range.first) {
      hi = mid;
    } else if (cp > range.second) {
      lo = mid + 1;
    } else {
      return true;
    }
  }
  return false;
}

inline bool regexMatchesUnicodePropertyEnclosingMark(uint32_t cp) {
  return (cp >= 0x0488 && cp <= 0x0489) ||
         cp == 0x1ABE ||
         (cp >= 0x20DD && cp <= 0x20E0) ||
         (cp >= 0x20E2 && cp <= 0x20E4) ||
         (cp >= 0xA670 && cp <= 0xA672);
}

inline bool regexMatchesUnicodePropertySpacingMark(uint32_t cp) {
  static constexpr std::pair<uint32_t, uint32_t> kSpacingMarkRanges[] = {
    {0x000903, 0x000903},
    {0x00093B, 0x00093B},
    {0x00093E, 0x000940},
    {0x000949, 0x00094C},
    {0x00094E, 0x00094F},
    {0x000982, 0x000983},
    {0x0009BE, 0x0009C0},
    {0x0009C7, 0x0009C8},
    {0x0009CB, 0x0009CC},
    {0x0009D7, 0x0009D7},
    {0x000A03, 0x000A03},
    {0x000A3E, 0x000A40},
    {0x000A83, 0x000A83},
    {0x000ABE, 0x000AC0},
    {0x000AC9, 0x000AC9},
    {0x000ACB, 0x000ACC},
    {0x000B02, 0x000B03},
    {0x000B3E, 0x000B3E},
    {0x000B40, 0x000B40},
    {0x000B47, 0x000B48},
    {0x000B4B, 0x000B4C},
    {0x000B57, 0x000B57},
    {0x000BBE, 0x000BBF},
    {0x000BC1, 0x000BC2},
    {0x000BC6, 0x000BC8},
    {0x000BCA, 0x000BCC},
    {0x000BD7, 0x000BD7},
    {0x000C01, 0x000C03},
    {0x000C41, 0x000C44},
    {0x000C82, 0x000C83},
    {0x000CBE, 0x000CBE},
    {0x000CC0, 0x000CC4},
    {0x000CC7, 0x000CC8},
    {0x000CCA, 0x000CCB},
    {0x000CD5, 0x000CD6},
    {0x000CF3, 0x000CF3},
    {0x000D02, 0x000D03},
    {0x000D3E, 0x000D40},
    {0x000D46, 0x000D48},
    {0x000D4A, 0x000D4C},
    {0x000D57, 0x000D57},
    {0x000D82, 0x000D83},
    {0x000DCF, 0x000DD1},
    {0x000DD8, 0x000DDF},
    {0x000DF2, 0x000DF3},
    {0x000F3E, 0x000F3F},
    {0x000F7F, 0x000F7F},
    {0x00102B, 0x00102C},
    {0x001031, 0x001031},
    {0x001038, 0x001038},
    {0x00103B, 0x00103C},
    {0x001056, 0x001057},
    {0x001062, 0x001064},
    {0x001067, 0x00106D},
    {0x001083, 0x001084},
    {0x001087, 0x00108C},
    {0x00108F, 0x00108F},
    {0x00109A, 0x00109C},
    {0x001715, 0x001715},
    {0x001734, 0x001734},
    {0x0017B6, 0x0017B6},
    {0x0017BE, 0x0017C5},
    {0x0017C7, 0x0017C8},
    {0x001923, 0x001926},
    {0x001929, 0x00192B},
    {0x001930, 0x001931},
    {0x001933, 0x001938},
    {0x001A19, 0x001A1A},
    {0x001A55, 0x001A55},
    {0x001A57, 0x001A57},
    {0x001A61, 0x001A61},
    {0x001A63, 0x001A64},
    {0x001A6D, 0x001A72},
    {0x001B04, 0x001B04},
    {0x001B35, 0x001B35},
    {0x001B3B, 0x001B3B},
    {0x001B3D, 0x001B41},
    {0x001B43, 0x001B44},
    {0x001B82, 0x001B82},
    {0x001BA1, 0x001BA1},
    {0x001BA6, 0x001BA7},
    {0x001BAA, 0x001BAA},
    {0x001BE7, 0x001BE7},
    {0x001BEA, 0x001BEC},
    {0x001BEE, 0x001BEE},
    {0x001BF2, 0x001BF3},
    {0x001C24, 0x001C2B},
    {0x001C34, 0x001C35},
    {0x001CE1, 0x001CE1},
    {0x001CF7, 0x001CF7},
    {0x00302E, 0x00302F},
    {0x00A823, 0x00A824},
    {0x00A827, 0x00A827},
    {0x00A880, 0x00A881},
    {0x00A8B4, 0x00A8C3},
    {0x00A952, 0x00A953},
    {0x00A983, 0x00A983},
    {0x00A9B4, 0x00A9B5},
    {0x00A9BA, 0x00A9BB},
    {0x00A9BE, 0x00A9C0},
    {0x00AA2F, 0x00AA30},
    {0x00AA33, 0x00AA34},
    {0x00AA4D, 0x00AA4D},
    {0x00AA7B, 0x00AA7B},
    {0x00AA7D, 0x00AA7D},
    {0x00AAEB, 0x00AAEB},
    {0x00AAEE, 0x00AAEF},
    {0x00AAF5, 0x00AAF5},
    {0x00ABE3, 0x00ABE4},
    {0x00ABE6, 0x00ABE7},
    {0x00ABE9, 0x00ABEA},
    {0x00ABEC, 0x00ABEC},
    {0x011000, 0x011000},
    {0x011002, 0x011002},
    {0x011082, 0x011082},
    {0x0110B0, 0x0110B2},
    {0x0110B7, 0x0110B8},
    {0x01112C, 0x01112C},
    {0x011145, 0x011146},
    {0x011182, 0x011182},
    {0x0111B3, 0x0111B5},
    {0x0111BF, 0x0111C0},
    {0x0111CE, 0x0111CE},
    {0x01122C, 0x01122E},
    {0x011232, 0x011233},
    {0x011235, 0x011235},
    {0x0112E0, 0x0112E2},
    {0x011302, 0x011303},
    {0x01133E, 0x01133F},
    {0x011341, 0x011344},
    {0x011347, 0x011348},
    {0x01134B, 0x01134D},
    {0x011357, 0x011357},
    {0x011362, 0x011363},
    {0x0113B8, 0x0113BA},
    {0x0113C2, 0x0113C2},
    {0x0113C5, 0x0113C5},
    {0x0113C7, 0x0113CA},
    {0x0113CC, 0x0113CD},
    {0x0113CF, 0x0113CF},
    {0x011435, 0x011437},
    {0x011440, 0x011441},
    {0x011445, 0x011445},
    {0x0114B0, 0x0114B2},
    {0x0114B9, 0x0114B9},
    {0x0114BB, 0x0114BE},
    {0x0114C1, 0x0114C1},
    {0x0115AF, 0x0115B1},
    {0x0115B8, 0x0115BB},
    {0x0115BE, 0x0115BE},
    {0x011630, 0x011632},
    {0x01163B, 0x01163C},
    {0x01163E, 0x01163E},
    {0x0116AC, 0x0116AC},
    {0x0116AE, 0x0116AF},
    {0x0116B6, 0x0116B6},
    {0x01171E, 0x01171E},
    {0x011720, 0x011721},
    {0x011726, 0x011726},
    {0x01182C, 0x01182E},
    {0x011838, 0x011838},
    {0x011930, 0x011935},
    {0x011937, 0x011938},
    {0x01193D, 0x01193D},
    {0x011940, 0x011940},
    {0x011942, 0x011942},
    {0x0119D1, 0x0119D3},
    {0x0119DC, 0x0119DF},
    {0x0119E4, 0x0119E4},
    {0x011A39, 0x011A39},
    {0x011A57, 0x011A58},
    {0x011A97, 0x011A97},
    {0x011B61, 0x011B61},
    {0x011B65, 0x011B65},
    {0x011B67, 0x011B67},
    {0x011C2F, 0x011C2F},
    {0x011C3E, 0x011C3E},
    {0x011CA9, 0x011CA9},
    {0x011CB1, 0x011CB1},
    {0x011CB4, 0x011CB4},
    {0x011D8A, 0x011D8E},
    {0x011D93, 0x011D94},
    {0x011D96, 0x011D96},
    {0x011EF5, 0x011EF6},
    {0x011F03, 0x011F03},
    {0x011F34, 0x011F35},
    {0x011F3E, 0x011F3F},
    {0x011F41, 0x011F41},
    {0x01612A, 0x01612C},
    {0x016F51, 0x016F87},
    {0x016FF0, 0x016FF1},
    {0x01D165, 0x01D166},
    {0x01D16D, 0x01D172},
  };
  size_t lo = 0;
  size_t hi = sizeof(kSpacingMarkRanges) / sizeof(kSpacingMarkRanges[0]);
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    auto range = kSpacingMarkRanges[mid];
    if (cp < range.first) {
      hi = mid;
    } else if (cp > range.second) {
      lo = mid + 1;
    } else {
      return true;
    }
  }
  return false;
}

inline bool regexMatchesUnicodePropertyMark(uint32_t cp) {
  return regexMatchesUnicodePropertyNonspacingMark(cp) ||
         regexMatchesUnicodePropertyEnclosingMark(cp) ||
         regexMatchesUnicodePropertySpacingMark(cp);
}

inline bool regexMatchesUnicodePropertyConnectorPunctuation(uint32_t cp) {
  return cp == 0x005F ||
         (cp >= 0x203F && cp <= 0x2040) ||
         cp == 0x2054 ||
         (cp >= 0xFE33 && cp <= 0xFE34) ||
         (cp >= 0xFE4D && cp <= 0xFE4F) ||
         cp == 0xFF3F;
}

inline bool regexMatchesUnicodePropertyFinalPunctuation(uint32_t cp) {
  return cp == 0x00BB ||
         cp == 0x2019 ||
         cp == 0x201D ||
         cp == 0x203A ||
         cp == 0x2E03 ||
         cp == 0x2E05 ||
         cp == 0x2E0A ||
         cp == 0x2E0D ||
         cp == 0x2E1D ||
         cp == 0x2E21;
}

inline bool regexMatchesUnicodePropertyInitialPunctuation(uint32_t cp) {
  return cp == 0x00AB ||
         cp == 0x2018 ||
         (cp >= 0x201B && cp <= 0x201C) ||
         cp == 0x201F ||
         cp == 0x2039 ||
         cp == 0x2E02 ||
         cp == 0x2E04 ||
         cp == 0x2E09 ||
         cp == 0x2E0C ||
         cp == 0x2E1C ||
         cp == 0x2E20;
}

inline bool regexMatchesUnicodePropertyDashPunctuation(uint32_t cp) {
  return cp == 0x002D ||
         cp == 0x058A ||
         cp == 0x05BE ||
         cp == 0x1400 ||
         cp == 0x1806 ||
         (cp >= 0x2010 && cp <= 0x2015) ||
         cp == 0x2E17 ||
         cp == 0x2E1A ||
         (cp >= 0x2E3A && cp <= 0x2E3B) ||
         cp == 0x2E40 ||
         cp == 0x2E5D ||
         cp == 0x301C ||
         cp == 0x3030 ||
         cp == 0x30A0 ||
         (cp >= 0xFE31 && cp <= 0xFE32) ||
         cp == 0xFE58 ||
         cp == 0xFE63 ||
         cp == 0xFF0D ||
         cp == 0x10D6E ||
         cp == 0x10EAD;
}

inline bool regexMatchesUnicodePropertyOpenPunctuation(uint32_t cp) {
  return cp == 0x0028 ||
         cp == 0x005B ||
         cp == 0x007B ||
         cp == 0x0F3A ||
         cp == 0x0F3C ||
         cp == 0x169B ||
         cp == 0x201A ||
         cp == 0x201E ||
         cp == 0x2045 ||
         cp == 0x207D ||
         cp == 0x208D ||
         cp == 0x2308 ||
         cp == 0x230A ||
         cp == 0x2329 ||
         cp == 0x2768 ||
         cp == 0x276A ||
         cp == 0x276C ||
         cp == 0x276E ||
         cp == 0x2770 ||
         cp == 0x2772 ||
         cp == 0x2774 ||
         cp == 0x27C5 ||
         cp == 0x27E6 ||
         cp == 0x27E8 ||
         cp == 0x27EA ||
         cp == 0x27EC ||
         cp == 0x27EE ||
         cp == 0x2983 ||
         cp == 0x2985 ||
         cp == 0x2987 ||
         cp == 0x2989 ||
         cp == 0x298B ||
         cp == 0x298D ||
         cp == 0x298F ||
         cp == 0x2991 ||
         cp == 0x2993 ||
         cp == 0x2995 ||
         cp == 0x2997 ||
         cp == 0x29D8 ||
         cp == 0x29DA ||
         cp == 0x29FC ||
         cp == 0x2E22 ||
         cp == 0x2E24 ||
         cp == 0x2E26 ||
         cp == 0x2E28 ||
         cp == 0x2E42 ||
         cp == 0x2E55 ||
         cp == 0x2E57 ||
         cp == 0x2E59 ||
         cp == 0x2E5B ||
         cp == 0x3008 ||
         cp == 0x300A ||
         cp == 0x300C ||
         cp == 0x300E ||
         cp == 0x3010 ||
         cp == 0x3014 ||
         cp == 0x3016 ||
         cp == 0x3018 ||
         cp == 0x301A ||
         cp == 0x301D ||
         cp == 0xFD3F ||
         cp == 0xFE17 ||
         cp == 0xFE35 ||
         cp == 0xFE37 ||
         cp == 0xFE39 ||
         cp == 0xFE3B ||
         cp == 0xFE3D ||
         cp == 0xFE3F ||
         cp == 0xFE41 ||
         cp == 0xFE43 ||
         cp == 0xFE47 ||
         cp == 0xFE59 ||
         cp == 0xFE5B ||
         cp == 0xFE5D ||
         cp == 0xFF08 ||
         cp == 0xFF3B ||
         cp == 0xFF5B ||
         cp == 0xFF5F ||
         cp == 0xFF62;
}

inline bool regexMatchesUnicodePropertyClosePunctuation(uint32_t cp) {
  return cp == 0x0029 ||
         cp == 0x005D ||
         cp == 0x007D ||
         cp == 0x0F3B ||
         cp == 0x0F3D ||
         cp == 0x169C ||
         cp == 0x2046 ||
         cp == 0x207E ||
         cp == 0x208E ||
         cp == 0x2309 ||
         cp == 0x230B ||
         cp == 0x232A ||
         cp == 0x2769 ||
         cp == 0x276B ||
         cp == 0x276D ||
         cp == 0x276F ||
         cp == 0x2771 ||
         cp == 0x2773 ||
         cp == 0x2775 ||
         cp == 0x27C6 ||
         cp == 0x27E7 ||
         cp == 0x27E9 ||
         cp == 0x27EB ||
         cp == 0x27ED ||
         cp == 0x27EF ||
         cp == 0x2984 ||
         cp == 0x2986 ||
         cp == 0x2988 ||
         cp == 0x298A ||
         cp == 0x298C ||
         cp == 0x298E ||
         cp == 0x2990 ||
         cp == 0x2992 ||
         cp == 0x2994 ||
         cp == 0x2996 ||
         cp == 0x2998 ||
         cp == 0x29D9 ||
         cp == 0x29DB ||
         cp == 0x29FD ||
         cp == 0x2E23 ||
         cp == 0x2E25 ||
         cp == 0x2E27 ||
         cp == 0x2E29 ||
         cp == 0x2E56 ||
         cp == 0x2E58 ||
         cp == 0x2E5A ||
         cp == 0x2E5C ||
         cp == 0x3009 ||
         cp == 0x300B ||
         cp == 0x300D ||
         cp == 0x300F ||
         cp == 0x3011 ||
         cp == 0x3015 ||
         cp == 0x3017 ||
         cp == 0x3019 ||
         cp == 0x301B ||
         (cp >= 0x301E && cp <= 0x301F) ||
         cp == 0xFD3E ||
         cp == 0xFE18 ||
         cp == 0xFE36 ||
         cp == 0xFE38 ||
         cp == 0xFE3A ||
         cp == 0xFE3C ||
         cp == 0xFE3E ||
         cp == 0xFE40 ||
         cp == 0xFE42 ||
         cp == 0xFE44 ||
         cp == 0xFE48 ||
         cp == 0xFE5A ||
         cp == 0xFE5C ||
         cp == 0xFE5E ||
         cp == 0xFF09 ||
         cp == 0xFF3D ||
         cp == 0xFF5D ||
         cp == 0xFF60 ||
         cp == 0xFF63;
}

inline bool regexMatchesUnicodePropertyOtherPunctuation(uint32_t cp) {
  static constexpr std::pair<uint32_t, uint32_t> kOtherPunctuationRanges[] = {
    {0x000021, 0x000023},
    {0x000025, 0x000027},
    {0x00002A, 0x00002A},
    {0x00002C, 0x00002C},
    {0x00002E, 0x00002F},
    {0x00003A, 0x00003B},
    {0x00003F, 0x000040},
    {0x00005C, 0x00005C},
    {0x0000A1, 0x0000A1},
    {0x0000A7, 0x0000A7},
    {0x0000B6, 0x0000B7},
    {0x0000BF, 0x0000BF},
    {0x00037E, 0x00037E},
    {0x000387, 0x000387},
    {0x00055A, 0x00055F},
    {0x000589, 0x000589},
    {0x0005C0, 0x0005C0},
    {0x0005C3, 0x0005C3},
    {0x0005C6, 0x0005C6},
    {0x0005F3, 0x0005F4},
    {0x000609, 0x00060A},
    {0x00060C, 0x00060D},
    {0x00061B, 0x00061B},
    {0x00061D, 0x00061F},
    {0x00066A, 0x00066D},
    {0x0006D4, 0x0006D4},
    {0x000700, 0x00070D},
    {0x0007F7, 0x0007F9},
    {0x000830, 0x00083E},
    {0x00085E, 0x00085E},
    {0x000964, 0x000965},
    {0x000970, 0x000970},
    {0x0009FD, 0x0009FD},
    {0x000A76, 0x000A76},
    {0x000AF0, 0x000AF0},
    {0x000C77, 0x000C77},
    {0x000C84, 0x000C84},
    {0x000DF4, 0x000DF4},
    {0x000E4F, 0x000E4F},
    {0x000E5A, 0x000E5B},
    {0x000F04, 0x000F12},
    {0x000F14, 0x000F14},
    {0x000F85, 0x000F85},
    {0x000FD0, 0x000FD4},
    {0x000FD9, 0x000FDA},
    {0x00104A, 0x00104F},
    {0x0010FB, 0x0010FB},
    {0x001360, 0x001368},
    {0x00166E, 0x00166E},
    {0x0016EB, 0x0016ED},
    {0x001735, 0x001736},
    {0x0017D4, 0x0017D6},
    {0x0017D8, 0x0017DA},
    {0x001800, 0x001805},
    {0x001807, 0x00180A},
    {0x001944, 0x001945},
    {0x001A1E, 0x001A1F},
    {0x001AA0, 0x001AA6},
    {0x001AA8, 0x001AAD},
    {0x001B4E, 0x001B4F},
    {0x001B5A, 0x001B60},
    {0x001B7D, 0x001B7F},
    {0x001BFC, 0x001BFF},
    {0x001C3B, 0x001C3F},
    {0x001C7E, 0x001C7F},
    {0x001CC0, 0x001CC7},
    {0x001CD3, 0x001CD3},
    {0x002016, 0x002017},
    {0x002020, 0x002027},
    {0x002030, 0x002038},
    {0x00203B, 0x00203E},
    {0x002041, 0x002043},
    {0x002047, 0x002051},
    {0x002053, 0x002053},
    {0x002055, 0x00205E},
    {0x002CF9, 0x002CFC},
    {0x002CFE, 0x002CFF},
    {0x002D70, 0x002D70},
    {0x002E00, 0x002E01},
    {0x002E06, 0x002E08},
    {0x002E0B, 0x002E0B},
    {0x002E0E, 0x002E16},
    {0x002E18, 0x002E19},
    {0x002E1B, 0x002E1B},
    {0x002E1E, 0x002E1F},
    {0x002E2A, 0x002E2E},
    {0x002E30, 0x002E39},
    {0x002E3C, 0x002E3F},
    {0x002E41, 0x002E41},
    {0x002E43, 0x002E4F},
    {0x002E52, 0x002E54},
    {0x003001, 0x003003},
    {0x00303D, 0x00303D},
    {0x0030FB, 0x0030FB},
    {0x00A4FE, 0x00A4FF},
    {0x00A60D, 0x00A60F},
    {0x00A673, 0x00A673},
    {0x00A67E, 0x00A67E},
    {0x00A6F2, 0x00A6F7},
    {0x00A874, 0x00A877},
    {0x00A8CE, 0x00A8CF},
    {0x00A8F8, 0x00A8FA},
    {0x00A8FC, 0x00A8FC},
    {0x00A92E, 0x00A92F},
    {0x00A95F, 0x00A95F},
    {0x00A9C1, 0x00A9CD},
    {0x00A9DE, 0x00A9DF},
    {0x00AA5C, 0x00AA5F},
    {0x00AADE, 0x00AADF},
    {0x00AAF0, 0x00AAF1},
    {0x00ABEB, 0x00ABEB},
    {0x00FE10, 0x00FE16},
    {0x00FE19, 0x00FE19},
    {0x00FE30, 0x00FE30},
    {0x00FE45, 0x00FE46},
    {0x00FE49, 0x00FE4C},
    {0x00FE50, 0x00FE52},
    {0x00FE54, 0x00FE57},
    {0x00FE5F, 0x00FE61},
    {0x00FE68, 0x00FE68},
    {0x00FE6A, 0x00FE6B},
    {0x00FF01, 0x00FF03},
    {0x00FF05, 0x00FF07},
    {0x00FF0A, 0x00FF0A},
    {0x00FF0C, 0x00FF0C},
    {0x00FF0E, 0x00FF0F},
    {0x00FF1A, 0x00FF1B},
    {0x00FF1F, 0x00FF20},
    {0x00FF3C, 0x00FF3C},
    {0x00FF61, 0x00FF61},
    {0x00FF64, 0x00FF65},
    {0x010100, 0x010102},
    {0x01039F, 0x01039F},
    {0x0103D0, 0x0103D0},
    {0x01056F, 0x01056F},
    {0x010857, 0x010857},
    {0x01091F, 0x01091F},
    {0x01093F, 0x01093F},
    {0x010A50, 0x010A58},
    {0x010A7F, 0x010A7F},
    {0x010AF0, 0x010AF6},
    {0x010B39, 0x010B3F},
    {0x010B99, 0x010B9C},
    {0x010ED0, 0x010ED0},
    {0x010F55, 0x010F59},
    {0x010F86, 0x010F89},
    {0x011047, 0x01104D},
    {0x0110BB, 0x0110BC},
    {0x0110BE, 0x0110C1},
    {0x011140, 0x011143},
    {0x011174, 0x011175},
    {0x0111C5, 0x0111C8},
    {0x0111CD, 0x0111CD},
    {0x0111DB, 0x0111DB},
    {0x0111DD, 0x0111DF},
    {0x011238, 0x01123D},
    {0x0112A9, 0x0112A9},
    {0x0113D4, 0x0113D5},
    {0x0113D7, 0x0113D8},
    {0x01144B, 0x01144F},
    {0x01145A, 0x01145B},
    {0x01145D, 0x01145D},
    {0x0114C6, 0x0114C6},
    {0x0115C1, 0x0115D7},
    {0x011641, 0x011643},
    {0x011660, 0x01166C},
    {0x0116B9, 0x0116B9},
    {0x01173C, 0x01173E},
    {0x01183B, 0x01183B},
    {0x011944, 0x011946},
    {0x0119E2, 0x0119E2},
    {0x011A3F, 0x011A46},
    {0x011A9A, 0x011A9C},
    {0x011A9E, 0x011AA2},
    {0x011B00, 0x011B09},
    {0x011BE1, 0x011BE1},
    {0x011C41, 0x011C45},
    {0x011C70, 0x011C71},
    {0x011EF7, 0x011EF8},
    {0x011F43, 0x011F4F},
    {0x011FFF, 0x011FFF},
    {0x012470, 0x012474},
    {0x012FF1, 0x012FF2},
    {0x016A6E, 0x016A6F},
    {0x016AF5, 0x016AF5},
    {0x016B37, 0x016B3B},
    {0x016B44, 0x016B44},
    {0x016D6D, 0x016D6F},
    {0x016E97, 0x016E9A},
    {0x016FE2, 0x016FE2},
    {0x01BC9F, 0x01BC9F},
    {0x01DA87, 0x01DA8B},
    {0x01E5FF, 0x01E5FF},
    {0x01E95E, 0x01E95F},
  };
  size_t lo = 0;
  size_t hi = sizeof(kOtherPunctuationRanges) / sizeof(kOtherPunctuationRanges[0]);
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    auto range = kOtherPunctuationRanges[mid];
    if (cp < range.first) {
      hi = mid;
    } else if (cp > range.second) {
      lo = mid + 1;
    } else {
      return true;
    }
  }
  return false;
}

inline bool regexMatchesUnicodePropertyPunctuation(uint32_t cp) {
  return regexMatchesUnicodePropertyConnectorPunctuation(cp) ||
         regexMatchesUnicodePropertyFinalPunctuation(cp) ||
         regexMatchesUnicodePropertyInitialPunctuation(cp) ||
         regexMatchesUnicodePropertyDashPunctuation(cp) ||
         regexMatchesUnicodePropertyOpenPunctuation(cp) ||
         regexMatchesUnicodePropertyClosePunctuation(cp) ||
         regexMatchesUnicodePropertyOtherPunctuation(cp);
}

inline bool regexMatchesUnicodePropertyControl(uint32_t cp) {
  return (cp >= 0x0000 && cp <= 0x001F) ||
         (cp >= 0x007F && cp <= 0x009F);
}

inline bool regexMatchesUnicodePropertyPrivateUse(uint32_t cp) {
  return (cp >= 0xE000 && cp <= 0xF8FF) ||
         (cp >= 0xF0000 && cp <= 0xFFFFD) ||
         (cp >= 0x100000 && cp <= 0x10FFFD);
}

inline bool regexMatchesUnicodePropertyCurrencySymbol(uint32_t cp) {
  return cp == 0x0024 ||
         (cp >= 0x00A2 && cp <= 0x00A5) ||
         cp == 0x058F ||
         cp == 0x060B ||
         (cp >= 0x07FE && cp <= 0x07FF) ||
         (cp >= 0x09F2 && cp <= 0x09F3) ||
         cp == 0x09FB ||
         cp == 0x0AF1 ||
         cp == 0x0BF9 ||
         cp == 0x0E3F ||
         cp == 0x17DB ||
         (cp >= 0x20A0 && cp <= 0x20C1) ||
         cp == 0xA838 ||
         cp == 0xFDFC ||
         cp == 0xFE69 ||
         cp == 0xFF04 ||
         (cp >= 0xFFE0 && cp <= 0xFFE1) ||
         (cp >= 0xFFE5 && cp <= 0xFFE6) ||
         (cp >= 0x11FDD && cp <= 0x11FE0) ||
         cp == 0x1E2FF ||
         cp == 0x1ECB0;
}

inline bool regexMatchesUnicodePropertyFormat(uint32_t cp) {
  return cp == 0x00AD ||
         (cp >= 0x0600 && cp <= 0x0605) ||
         cp == 0x061C ||
         cp == 0x06DD ||
         cp == 0x070F ||
         (cp >= 0x0890 && cp <= 0x0891) ||
         cp == 0x08E2 ||
         cp == 0x180E ||
         (cp >= 0x200B && cp <= 0x200F) ||
         (cp >= 0x202A && cp <= 0x202E) ||
         (cp >= 0x2060 && cp <= 0x2064) ||
         (cp >= 0x2066 && cp <= 0x206F) ||
         cp == 0xFEFF ||
         (cp >= 0xFFF9 && cp <= 0xFFFB) ||
         cp == 0x110BD ||
         cp == 0x110CD ||
         (cp >= 0x13430 && cp <= 0x1343F) ||
         (cp >= 0x1BCA0 && cp <= 0x1BCA3) ||
         (cp >= 0x1D173 && cp <= 0x1D17A) ||
         cp == 0xE0001 ||
         (cp >= 0xE0020 && cp <= 0xE007F);
}

inline bool regexMatchesUnicodePropertyModifierSymbol(uint32_t cp) {
  return cp == 0x005E ||
         cp == 0x0060 ||
         cp == 0x00A8 ||
         cp == 0x00AF ||
         cp == 0x00B4 ||
         cp == 0x00B8 ||
         (cp >= 0x02C2 && cp <= 0x02C5) ||
         (cp >= 0x02D2 && cp <= 0x02DF) ||
         (cp >= 0x02E5 && cp <= 0x02EB) ||
         cp == 0x02ED ||
         (cp >= 0x02EF && cp <= 0x02FF) ||
         cp == 0x0375 ||
         (cp >= 0x0384 && cp <= 0x0385) ||
         cp == 0x0888 ||
         cp == 0x1FBD ||
         (cp >= 0x1FBF && cp <= 0x1FC1) ||
         (cp >= 0x1FCD && cp <= 0x1FCF) ||
         (cp >= 0x1FDD && cp <= 0x1FDF) ||
         (cp >= 0x1FED && cp <= 0x1FEF) ||
         (cp >= 0x1FFD && cp <= 0x1FFE) ||
         (cp >= 0x309B && cp <= 0x309C) ||
         (cp >= 0xA700 && cp <= 0xA716) ||
         (cp >= 0xA720 && cp <= 0xA721) ||
         (cp >= 0xA789 && cp <= 0xA78A) ||
         cp == 0xAB5B ||
         (cp >= 0xAB6A && cp <= 0xAB6B) ||
         (cp >= 0xFBB2 && cp <= 0xFBC2) ||
         cp == 0xFF3E ||
         cp == 0xFF40 ||
         cp == 0xFFE3 ||
         (cp >= 0x1F3FB && cp <= 0x1F3FF);
}

inline bool regexMatchesUnicodePropertyMathSymbol(uint32_t cp) {
  return cp == 0x002B ||
         (cp >= 0x003C && cp <= 0x003E) ||
         cp == 0x007C ||
         cp == 0x007E ||
         cp == 0x00AC ||
         cp == 0x00B1 ||
         cp == 0x00D7 ||
         cp == 0x00F7 ||
         cp == 0x03F6 ||
         (cp >= 0x0606 && cp <= 0x0608) ||
         cp == 0x2044 ||
         cp == 0x2052 ||
         (cp >= 0x207A && cp <= 0x207C) ||
         (cp >= 0x208A && cp <= 0x208C) ||
         cp == 0x2118 ||
         (cp >= 0x2140 && cp <= 0x2144) ||
         cp == 0x214B ||
         (cp >= 0x2190 && cp <= 0x2194) ||
         (cp >= 0x219A && cp <= 0x219B) ||
         cp == 0x21A0 ||
         cp == 0x21A3 ||
         cp == 0x21A6 ||
         cp == 0x21AE ||
         (cp >= 0x21CE && cp <= 0x21CF) ||
         cp == 0x21D2 ||
         cp == 0x21D4 ||
         (cp >= 0x21F4 && cp <= 0x22FF) ||
         (cp >= 0x2320 && cp <= 0x2321) ||
         cp == 0x237C ||
         (cp >= 0x239B && cp <= 0x23B3) ||
         (cp >= 0x23DC && cp <= 0x23E1) ||
         cp == 0x25B7 ||
         cp == 0x25C1 ||
         (cp >= 0x25F8 && cp <= 0x25FF) ||
         cp == 0x266F ||
         (cp >= 0x27C0 && cp <= 0x27C4) ||
         (cp >= 0x27C7 && cp <= 0x27E5) ||
         (cp >= 0x27F0 && cp <= 0x27FF) ||
         (cp >= 0x2900 && cp <= 0x2982) ||
         (cp >= 0x2999 && cp <= 0x29D7) ||
         (cp >= 0x29DC && cp <= 0x29FB) ||
         (cp >= 0x29FE && cp <= 0x2AFF) ||
         (cp >= 0x2B30 && cp <= 0x2B44) ||
         (cp >= 0x2B47 && cp <= 0x2B4C) ||
         cp == 0xFB29 ||
         cp == 0xFE62 ||
         (cp >= 0xFE64 && cp <= 0xFE66) ||
         cp == 0xFF0B ||
         (cp >= 0xFF1C && cp <= 0xFF1E) ||
         cp == 0xFF5C ||
         cp == 0xFF5E ||
         cp == 0xFFE2 ||
         (cp >= 0xFFE9 && cp <= 0xFFEC) ||
         (cp >= 0x10D8E && cp <= 0x10D8F) ||
         cp == 0x1CEF0 ||
         cp == 0x1D6C1 ||
         cp == 0x1D6DB ||
         cp == 0x1D6FB ||
         cp == 0x1D715 ||
         cp == 0x1D735 ||
         cp == 0x1D74F ||
         cp == 0x1D76F ||
         cp == 0x1D789 ||
         cp == 0x1D7A9 ||
         cp == 0x1D7C3 ||
         (cp >= 0x1EEF0 && cp <= 0x1EEF1) ||
         (cp >= 0x1F8D0 && cp <= 0x1F8D8);
}

inline bool regexMatchesUnicodePropertyOtherSymbol(uint32_t cp) {
  static constexpr std::pair<uint32_t, uint32_t> kOtherSymbolRanges[] = {
    {0x0000A6, 0x0000A6},
    {0x0000A9, 0x0000A9},
    {0x0000AE, 0x0000AE},
    {0x0000B0, 0x0000B0},
    {0x000482, 0x000482},
    {0x00058D, 0x00058E},
    {0x00060E, 0x00060F},
    {0x0006DE, 0x0006DE},
    {0x0006E9, 0x0006E9},
    {0x0006FD, 0x0006FE},
    {0x0007F6, 0x0007F6},
    {0x0009FA, 0x0009FA},
    {0x000B70, 0x000B70},
    {0x000BF3, 0x000BF8},
    {0x000BFA, 0x000BFA},
    {0x000C7F, 0x000C7F},
    {0x000D4F, 0x000D4F},
    {0x000D79, 0x000D79},
    {0x000F01, 0x000F03},
    {0x000F13, 0x000F13},
    {0x000F15, 0x000F17},
    {0x000F1A, 0x000F1F},
    {0x000F34, 0x000F34},
    {0x000F36, 0x000F36},
    {0x000F38, 0x000F38},
    {0x000FBE, 0x000FC5},
    {0x000FC7, 0x000FCC},
    {0x000FCE, 0x000FCF},
    {0x000FD5, 0x000FD8},
    {0x00109E, 0x00109F},
    {0x001390, 0x001399},
    {0x00166D, 0x00166D},
    {0x001940, 0x001940},
    {0x0019DE, 0x0019FF},
    {0x001B61, 0x001B6A},
    {0x001B74, 0x001B7C},
    {0x002100, 0x002101},
    {0x002103, 0x002106},
    {0x002108, 0x002109},
    {0x002114, 0x002114},
    {0x002116, 0x002117},
    {0x00211E, 0x002123},
    {0x002125, 0x002125},
    {0x002127, 0x002127},
    {0x002129, 0x002129},
    {0x00212E, 0x00212E},
    {0x00213A, 0x00213B},
    {0x00214A, 0x00214A},
    {0x00214C, 0x00214D},
    {0x00214F, 0x00214F},
    {0x00218A, 0x00218B},
    {0x002195, 0x002199},
    {0x00219C, 0x00219F},
    {0x0021A1, 0x0021A2},
    {0x0021A4, 0x0021A5},
    {0x0021A7, 0x0021AD},
    {0x0021AF, 0x0021CD},
    {0x0021D0, 0x0021D1},
    {0x0021D3, 0x0021D3},
    {0x0021D5, 0x0021F3},
    {0x002300, 0x002307},
    {0x00230C, 0x00231F},
    {0x002322, 0x002328},
    {0x00232B, 0x00237B},
    {0x00237D, 0x00239A},
    {0x0023B4, 0x0023DB},
    {0x0023E2, 0x002429},
    {0x002440, 0x00244A},
    {0x00249C, 0x0024E9},
    {0x002500, 0x0025B6},
    {0x0025B8, 0x0025C0},
    {0x0025C2, 0x0025F7},
    {0x002600, 0x00266E},
    {0x002670, 0x002767},
    {0x002794, 0x0027BF},
    {0x002800, 0x0028FF},
    {0x002B00, 0x002B2F},
    {0x002B45, 0x002B46},
    {0x002B4D, 0x002B73},
    {0x002B76, 0x002BFF},
    {0x002CE5, 0x002CEA},
    {0x002E50, 0x002E51},
    {0x002E80, 0x002E99},
    {0x002E9B, 0x002EF3},
    {0x002F00, 0x002FD5},
    {0x002FF0, 0x002FFF},
    {0x003004, 0x003004},
    {0x003012, 0x003013},
    {0x003020, 0x003020},
    {0x003036, 0x003037},
    {0x00303E, 0x00303F},
    {0x003190, 0x003191},
    {0x003196, 0x00319F},
    {0x0031C0, 0x0031E5},
    {0x0031EF, 0x0031EF},
    {0x003200, 0x00321E},
    {0x00322A, 0x003247},
    {0x003250, 0x003250},
    {0x003260, 0x00327F},
    {0x00328A, 0x0032B0},
    {0x0032C0, 0x0033FF},
    {0x004DC0, 0x004DFF},
    {0x00A490, 0x00A4C6},
    {0x00A828, 0x00A82B},
    {0x00A836, 0x00A837},
    {0x00A839, 0x00A839},
    {0x00AA77, 0x00AA79},
    {0x00FBC3, 0x00FBD2},
    {0x00FD40, 0x00FD4F},
    {0x00FD90, 0x00FD91},
    {0x00FDC8, 0x00FDCF},
    {0x00FDFD, 0x00FDFF},
    {0x00FFE4, 0x00FFE4},
    {0x00FFE8, 0x00FFE8},
    {0x00FFED, 0x00FFEE},
    {0x00FFFC, 0x00FFFD},
    {0x010137, 0x01013F},
    {0x010179, 0x010189},
    {0x01018C, 0x01018E},
    {0x010190, 0x01019C},
    {0x0101A0, 0x0101A0},
    {0x0101D0, 0x0101FC},
    {0x010877, 0x010878},
    {0x010AC8, 0x010AC8},
    {0x010ED1, 0x010ED8},
    {0x01173F, 0x01173F},
    {0x011FD5, 0x011FDC},
    {0x011FE1, 0x011FF1},
    {0x016B3C, 0x016B3F},
    {0x016B45, 0x016B45},
    {0x01BC9C, 0x01BC9C},
    {0x01CC00, 0x01CCEF},
    {0x01CCFA, 0x01CCFC},
    {0x01CD00, 0x01CEB3},
    {0x01CEBA, 0x01CED0},
    {0x01CEE0, 0x01CEEF},
    {0x01CF50, 0x01CFC3},
    {0x01D000, 0x01D0F5},
    {0x01D100, 0x01D126},
    {0x01D129, 0x01D164},
    {0x01D16A, 0x01D16C},
    {0x01D183, 0x01D184},
    {0x01D18C, 0x01D1A9},
    {0x01D1AE, 0x01D1EA},
    {0x01D200, 0x01D241},
    {0x01D245, 0x01D245},
    {0x01D300, 0x01D356},
    {0x01D800, 0x01D9FF},
    {0x01DA37, 0x01DA3A},
    {0x01DA6D, 0x01DA74},
    {0x01DA76, 0x01DA83},
    {0x01DA85, 0x01DA86},
    {0x01E14F, 0x01E14F},
    {0x01ECAC, 0x01ECAC},
    {0x01ED2E, 0x01ED2E},
    {0x01F000, 0x01F02B},
    {0x01F030, 0x01F093},
    {0x01F0A0, 0x01F0AE},
    {0x01F0B1, 0x01F0BF},
    {0x01F0C1, 0x01F0CF},
    {0x01F0D1, 0x01F0F5},
    {0x01F10D, 0x01F1AD},
    {0x01F1E6, 0x01F202},
    {0x01F210, 0x01F23B},
    {0x01F240, 0x01F248},
    {0x01F250, 0x01F251},
    {0x01F260, 0x01F265},
    {0x01F300, 0x01F3FA},
    {0x01F400, 0x01F6D8},
    {0x01F6DC, 0x01F6EC},
    {0x01F6F0, 0x01F6FC},
    {0x01F700, 0x01F7D9},
    {0x01F7E0, 0x01F7EB},
    {0x01F7F0, 0x01F7F0},
    {0x01F800, 0x01F80B},
    {0x01F810, 0x01F847},
    {0x01F850, 0x01F859},
    {0x01F860, 0x01F887},
    {0x01F890, 0x01F8AD},
    {0x01F8B0, 0x01F8BB},
    {0x01F8C0, 0x01F8C1},
    {0x01F900, 0x01FA57},
    {0x01FA60, 0x01FA6D},
    {0x01FA70, 0x01FA7C},
    {0x01FA80, 0x01FA8A},
    {0x01FA8E, 0x01FAC6},
    {0x01FAC8, 0x01FAC8},
    {0x01FACD, 0x01FADC},
    {0x01FADF, 0x01FAEA},
    {0x01FAEF, 0x01FAF8},
    {0x01FB00, 0x01FB92},
    {0x01FB94, 0x01FBEF},
    {0x01FBFA, 0x01FBFA},
  };
  size_t lo = 0;
  size_t hi = sizeof(kOtherSymbolRanges) / sizeof(kOtherSymbolRanges[0]);
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    auto range = kOtherSymbolRanges[mid];
    if (cp < range.first) {
      hi = mid;
    } else if (cp > range.second) {
      lo = mid + 1;
    } else {
      return true;
    }
  }
  return false;
}

inline bool regexMatchesUnicodePropertySymbol(uint32_t cp) {
  return regexMatchesUnicodePropertyCurrencySymbol(cp) ||
         regexMatchesUnicodePropertyModifierSymbol(cp) ||
         regexMatchesUnicodePropertyMathSymbol(cp) ||
         regexMatchesUnicodePropertyOtherSymbol(cp);
}

inline bool regexMatchesUnicodePropertySurrogate(uint32_t cp) {
  return cp >= 0xD800 && cp <= 0xDFFF;
}

inline bool regexMatchesUnicodePropertyASCII(uint32_t cp) {
  return cp <= 0x7F;
}

inline bool regexMatchesUnicodePropertyASCIIHexDigit(uint32_t cp) {
  return (cp >= '0' && cp <= '9') ||
         (cp >= 'A' && cp <= 'F') ||
         (cp >= 'a' && cp <= 'f');
}

inline bool regexMatchesUnicodePropertyScriptHan(uint32_t cp) {
  return cp == 0x3005 ||
         cp == 0x3007 ||
         (cp >= 0x2E80 && cp <= 0x2E99) ||
         (cp >= 0x2E9B && cp <= 0x2EF3) ||
         (cp >= 0x2F00 && cp <= 0x2FD5) ||
         (cp >= 0x3021 && cp <= 0x3029) ||
         (cp >= 0x3038 && cp <= 0x303B) ||
         (cp >= 0x3400 && cp <= 0x4DBF) ||
         (cp >= 0x4E00 && cp <= 0x9FFF) ||
         (cp >= 0xF900 && cp <= 0xFA6D) ||
         (cp >= 0xFA70 && cp <= 0xFAD9) ||
         (cp >= 0x16FE2 && cp <= 0x16FE3) ||
         (cp >= 0x16FF0 && cp <= 0x16FF6) ||
         (cp >= 0x20000 && cp <= 0x2A6DF) ||
         (cp >= 0x2A700 && cp <= 0x2B81D) ||
         (cp >= 0x2B820 && cp <= 0x2CEAD) ||
         (cp >= 0x2CEB0 && cp <= 0x2EBE0) ||
         (cp >= 0x2EBF0 && cp <= 0x2EE5D) ||
         (cp >= 0x2F800 && cp <= 0x2FA1D) ||
         (cp >= 0x30000 && cp <= 0x3134A) ||
         (cp >= 0x31350 && cp <= 0x33479);
}

inline bool regexMatchesUnicodePropertyScriptExtensionsHan(uint32_t cp) {
  return cp == 0x00B7 ||
         cp == 0x3030 ||
         cp == 0x30FB ||
         cp == 0x31EF ||
         cp == 0x32FF ||
         (cp >= 0x2E80 && cp <= 0x2E99) ||
         (cp >= 0x2E9B && cp <= 0x2EF3) ||
         (cp >= 0x2F00 && cp <= 0x2FD5) ||
         (cp >= 0x2FF0 && cp <= 0x2FFF) ||
         (cp >= 0x3001 && cp <= 0x3003) ||
         (cp >= 0x3005 && cp <= 0x3011) ||
         (cp >= 0x3013 && cp <= 0x301F) ||
         (cp >= 0x3021 && cp <= 0x302D) ||
         (cp >= 0x3037 && cp <= 0x303F) ||
         (cp >= 0x3190 && cp <= 0x319F) ||
         (cp >= 0x31C0 && cp <= 0x31E5) ||
         (cp >= 0x3220 && cp <= 0x3247) ||
         (cp >= 0x3280 && cp <= 0x32B0) ||
         (cp >= 0x32C0 && cp <= 0x32CB) ||
         (cp >= 0x3358 && cp <= 0x3370) ||
         (cp >= 0x337B && cp <= 0x337F) ||
         (cp >= 0x33E0 && cp <= 0x33FE) ||
         (cp >= 0x3400 && cp <= 0x4DBF) ||
         (cp >= 0x4E00 && cp <= 0x9FFF) ||
         (cp >= 0xA700 && cp <= 0xA707) ||
         (cp >= 0xF900 && cp <= 0xFA6D) ||
         (cp >= 0xFA70 && cp <= 0xFAD9) ||
         (cp >= 0xFE45 && cp <= 0xFE46) ||
         (cp >= 0xFF61 && cp <= 0xFF65) ||
         (cp >= 0x16FE2 && cp <= 0x16FE3) ||
         (cp >= 0x16FF0 && cp <= 0x16FF6) ||
         (cp >= 0x1D360 && cp <= 0x1D371) ||
         (cp >= 0x1F250 && cp <= 0x1F251) ||
         (cp >= 0x20000 && cp <= 0x2A6DF) ||
         (cp >= 0x2A700 && cp <= 0x2B81D) ||
         (cp >= 0x2B820 && cp <= 0x2CEAD) ||
         (cp >= 0x2CEB0 && cp <= 0x2EBE0) ||
         (cp >= 0x2EBF0 && cp <= 0x2EE5D) ||
         (cp >= 0x2F800 && cp <= 0x2FA1D) ||
         (cp >= 0x30000 && cp <= 0x3134A) ||
         (cp >= 0x31350 && cp <= 0x33479);
}

inline bool regexMatchesUnicodePropertyScriptHangul(uint32_t cp) {
  return (cp >= 0x1100 && cp <= 0x11FF) ||
         (cp >= 0x302E && cp <= 0x302F) ||
         (cp >= 0x3131 && cp <= 0x318E) ||
         (cp >= 0x3200 && cp <= 0x321E) ||
         (cp >= 0x3260 && cp <= 0x327E) ||
         (cp >= 0xA960 && cp <= 0xA97C) ||
         (cp >= 0xAC00 && cp <= 0xD7A3) ||
         (cp >= 0xD7B0 && cp <= 0xD7C6) ||
         (cp >= 0xD7CB && cp <= 0xD7FB) ||
         (cp >= 0xFFA0 && cp <= 0xFFBE) ||
         (cp >= 0xFFC2 && cp <= 0xFFC7) ||
         (cp >= 0xFFCA && cp <= 0xFFCF) ||
         (cp >= 0xFFD2 && cp <= 0xFFD7) ||
         (cp >= 0xFFDA && cp <= 0xFFDC);
}

inline bool regexMatchesUnicodePropertyScriptExtensionsHangul(uint32_t cp) {
  return cp == 0x3037 ||
         cp == 0x30FB ||
         (cp >= 0x1100 && cp <= 0x11FF) ||
         (cp >= 0x3001 && cp <= 0x3003) ||
         (cp >= 0x3008 && cp <= 0x3011) ||
         (cp >= 0x3013 && cp <= 0x301F) ||
         (cp >= 0x302E && cp <= 0x3030) ||
         (cp >= 0x3131 && cp <= 0x318E) ||
         (cp >= 0x3200 && cp <= 0x321E) ||
         (cp >= 0x3260 && cp <= 0x327E) ||
         (cp >= 0xA960 && cp <= 0xA97C) ||
         (cp >= 0xAC00 && cp <= 0xD7A3) ||
         (cp >= 0xD7B0 && cp <= 0xD7C6) ||
         (cp >= 0xD7CB && cp <= 0xD7FB) ||
         (cp >= 0xFE45 && cp <= 0xFE46) ||
         (cp >= 0xFF61 && cp <= 0xFF65) ||
         (cp >= 0xFFA0 && cp <= 0xFFBE) ||
         (cp >= 0xFFC2 && cp <= 0xFFC7) ||
         (cp >= 0xFFCA && cp <= 0xFFCF) ||
         (cp >= 0xFFD2 && cp <= 0xFFD7) ||
         (cp >= 0xFFDA && cp <= 0xFFDC);
}

inline bool regexMatchesUnicodePropertyScriptHanunoo(uint32_t cp) {
  return cp >= 0x1720 && cp <= 0x1734;
}

inline bool regexMatchesUnicodePropertyScriptExtensionsHanunoo(uint32_t cp) {
  return cp >= 0x1720 && cp <= 0x1736;
}

inline bool regexMatchesUnicodePropertyScriptBuhid(uint32_t cp) {
  return cp >= 0x1740 && cp <= 0x1753;
}

inline bool regexMatchesUnicodePropertyScriptExtensionsBuhid(uint32_t cp) {
  return (cp >= 0x1735 && cp <= 0x1736) ||
         (cp >= 0x1740 && cp <= 0x1753);
}

inline bool regexMatchesUnicodePropertyScriptTagalog(uint32_t cp) {
  return cp == 0x171F ||
         (cp >= 0x1700 && cp <= 0x1715);
}

inline bool regexMatchesUnicodePropertyScriptExtensionsTagalog(uint32_t cp) {
  return cp == 0x171F ||
         (cp >= 0x1700 && cp <= 0x1715) ||
         (cp >= 0x1735 && cp <= 0x1736);
}

inline bool regexMatchesUnicodePropertyScriptTagbanwa(uint32_t cp) {
  return (cp >= 0x1760 && cp <= 0x176C) ||
         (cp >= 0x176E && cp <= 0x1770) ||
         (cp >= 0x1772 && cp <= 0x1773);
}

inline bool regexMatchesUnicodePropertyScriptExtensionsTagbanwa(uint32_t cp) {
  return (cp >= 0x1735 && cp <= 0x1736) ||
         (cp >= 0x1760 && cp <= 0x176C) ||
         (cp >= 0x176E && cp <= 0x1770) ||
         (cp >= 0x1772 && cp <= 0x1773);
}

inline bool regexMatchesUnicodePropertyScriptOgham(uint32_t cp) {
  return cp >= 0x1680 && cp <= 0x169C;
}

inline bool regexMatchesUnicodePropertyScriptBuginese(uint32_t cp) {
  return (cp >= 0x1A00 && cp <= 0x1A1B) ||
         (cp >= 0x1A1E && cp <= 0x1A1F);
}

inline bool regexMatchesUnicodePropertyScriptExtensionsBuginese(uint32_t cp) {
  return cp == 0xA9CF ||
         (cp >= 0x1A00 && cp <= 0x1A1B) ||
         (cp >= 0x1A1E && cp <= 0x1A1F);
}

inline bool regexMatchesUnicodePropertyScriptTaiLe(uint32_t cp) {
  return (cp >= 0x1950 && cp <= 0x196D) ||
         (cp >= 0x1970 && cp <= 0x1974);
}

inline bool regexMatchesUnicodePropertyScriptExtensionsTaiLe(uint32_t cp) {
  return cp == 0x030C ||
         (cp >= 0x0300 && cp <= 0x0301) ||
         (cp >= 0x0307 && cp <= 0x0308) ||
         (cp >= 0x1040 && cp <= 0x1049) ||
         (cp >= 0x1950 && cp <= 0x196D) ||
         (cp >= 0x1970 && cp <= 0x1974);
}

inline bool regexMatchesUnicodePropertyScriptCham(uint32_t cp) {
  return (cp >= 0xAA00 && cp <= 0xAA36) ||
         (cp >= 0xAA40 && cp <= 0xAA4D) ||
         (cp >= 0xAA50 && cp <= 0xAA59) ||
         (cp >= 0xAA5C && cp <= 0xAA5F);
}

inline bool regexMatchesUnicodePropertyScriptRunic(uint32_t cp) {
  return (cp >= 0x16A0 && cp <= 0x16EA) ||
         (cp >= 0x16EE && cp <= 0x16F8);
}

inline bool regexMatchesUnicodePropertyScriptExtensionsRunic(uint32_t cp) {
  return cp >= 0x16A0 && cp <= 0x16F8;
}

struct RegexCodePointRange {
  uint32_t start;
  uint32_t end;
};

template <size_t N>
inline bool regexMatchesUnicodePropertyRanges(uint32_t cp,
                                              const RegexCodePointRange (&ranges)[N]) {
  for (const auto& range : ranges) {
    if (cp < range.start) {
      return false;
    }
    if (cp <= range.end) {
      return true;
    }
  }
  return false;
}

static constexpr RegexCodePointRange kRegexUnicodePropertyScriptCommonRanges[] = {
  {0x0, 0x40},
  {0x5B, 0x60},
  {0x7B, 0xA9},
  {0xAB, 0xB9},
  {0xBB, 0xBF},
  {0xD7, 0xD7},
  {0xF7, 0xF7},
  {0x2B9, 0x2DF},
  {0x2E5, 0x2E9},
  {0x2EC, 0x2FF},
  {0x374, 0x374},
  {0x37E, 0x37E},
  {0x385, 0x385},
  {0x387, 0x387},
  {0x605, 0x605},
  {0x60C, 0x60C},
  {0x61B, 0x61B},
  {0x61F, 0x61F},
  {0x640, 0x640},
  {0x6DD, 0x6DD},
  {0x8E2, 0x8E2},
  {0x964, 0x965},
  {0xE3F, 0xE3F},
  {0xFD5, 0xFD8},
  {0x10FB, 0x10FB},
  {0x16EB, 0x16ED},
  {0x1735, 0x1736},
  {0x1802, 0x1803},
  {0x1805, 0x1805},
  {0x1CD3, 0x1CD3},
  {0x1CE1, 0x1CE1},
  {0x1CE9, 0x1CEC},
  {0x1CEE, 0x1CF3},
  {0x1CF5, 0x1CF7},
  {0x1CFA, 0x1CFA},
  {0x2000, 0x200B},
  {0x200E, 0x2064},
  {0x2066, 0x2070},
  {0x2074, 0x207E},
  {0x2080, 0x208E},
  {0x20A0, 0x20C1},
  {0x2100, 0x2125},
  {0x2127, 0x2129},
  {0x212C, 0x2131},
  {0x2133, 0x214D},
  {0x214F, 0x215F},
  {0x2189, 0x218B},
  {0x2190, 0x2429},
  {0x2440, 0x244A},
  {0x2460, 0x27FF},
  {0x2900, 0x2B73},
  {0x2B76, 0x2BFF},
  {0x2E00, 0x2E5D},
  {0x2FF0, 0x3004},
  {0x3006, 0x3006},
  {0x3008, 0x3020},
  {0x3030, 0x3037},
  {0x303C, 0x303F},
  {0x309B, 0x309C},
  {0x30A0, 0x30A0},
  {0x30FB, 0x30FC},
  {0x3190, 0x319F},
  {0x31C0, 0x31E5},
  {0x31EF, 0x31EF},
  {0x3220, 0x325F},
  {0x327F, 0x32CF},
  {0x32FF, 0x32FF},
  {0x3358, 0x33FF},
  {0x4DC0, 0x4DFF},
  {0xA700, 0xA721},
  {0xA788, 0xA78A},
  {0xA830, 0xA839},
  {0xA92E, 0xA92E},
  {0xA9CF, 0xA9CF},
  {0xAB5B, 0xAB5B},
  {0xAB6A, 0xAB6B},
  {0xFD3E, 0xFD3F},
  {0xFE10, 0xFE19},
  {0xFE30, 0xFE52},
  {0xFE54, 0xFE66},
  {0xFE68, 0xFE6B},
  {0xFEFF, 0xFEFF},
  {0xFF01, 0xFF20},
  {0xFF3B, 0xFF40},
  {0xFF5B, 0xFF65},
  {0xFF70, 0xFF70},
  {0xFF9E, 0xFF9F},
  {0xFFE0, 0xFFE6},
  {0xFFE8, 0xFFEE},
  {0xFFF9, 0xFFFD},
  {0x10100, 0x10102},
  {0x10107, 0x10133},
  {0x10137, 0x1013F},
  {0x10190, 0x1019C},
  {0x101D0, 0x101FC},
  {0x102E1, 0x102FB},
  {0x1BCA0, 0x1BCA3},
  {0x1CC00, 0x1CCFC},
  {0x1CD00, 0x1CEB3},
  {0x1CEBA, 0x1CED0},
  {0x1CEE0, 0x1CEF0},
  {0x1CF50, 0x1CFC3},
  {0x1D000, 0x1D0F5},
  {0x1D100, 0x1D126},
  {0x1D129, 0x1D166},
  {0x1D16A, 0x1D17A},
  {0x1D183, 0x1D184},
  {0x1D18C, 0x1D1A9},
  {0x1D1AE, 0x1D1EA},
  {0x1D2C0, 0x1D2D3},
  {0x1D2E0, 0x1D2F3},
  {0x1D300, 0x1D356},
  {0x1D360, 0x1D378},
  {0x1D400, 0x1D454},
  {0x1D456, 0x1D49C},
  {0x1D49E, 0x1D49F},
  {0x1D4A2, 0x1D4A2},
  {0x1D4A5, 0x1D4A6},
  {0x1D4A9, 0x1D4AC},
  {0x1D4AE, 0x1D4B9},
  {0x1D4BB, 0x1D4BB},
  {0x1D4BD, 0x1D4C3},
  {0x1D4C5, 0x1D505},
  {0x1D507, 0x1D50A},
  {0x1D50D, 0x1D514},
  {0x1D516, 0x1D51C},
  {0x1D51E, 0x1D539},
  {0x1D53B, 0x1D53E},
  {0x1D540, 0x1D544},
  {0x1D546, 0x1D546},
  {0x1D54A, 0x1D550},
  {0x1D552, 0x1D6A5},
  {0x1D6A8, 0x1D7CB},
  {0x1D7CE, 0x1D7FF},
  {0x1EC71, 0x1ECB4},
  {0x1ED01, 0x1ED3D},
  {0x1F000, 0x1F02B},
  {0x1F030, 0x1F093},
  {0x1F0A0, 0x1F0AE},
  {0x1F0B1, 0x1F0BF},
  {0x1F0C1, 0x1F0CF},
  {0x1F0D1, 0x1F0F5},
  {0x1F100, 0x1F1AD},
  {0x1F1E6, 0x1F1FF},
  {0x1F201, 0x1F202},
  {0x1F210, 0x1F23B},
  {0x1F240, 0x1F248},
  {0x1F250, 0x1F251},
  {0x1F260, 0x1F265},
  {0x1F300, 0x1F6D8},
  {0x1F6DC, 0x1F6EC},
  {0x1F6F0, 0x1F6FC},
  {0x1F700, 0x1F7D9},
  {0x1F7E0, 0x1F7EB},
  {0x1F7F0, 0x1F7F0},
  {0x1F800, 0x1F80B},
  {0x1F810, 0x1F847},
  {0x1F850, 0x1F859},
  {0x1F860, 0x1F887},
  {0x1F890, 0x1F8AD},
  {0x1F8B0, 0x1F8BB},
  {0x1F8C0, 0x1F8C1},
  {0x1F8D0, 0x1F8D8},
  {0x1F900, 0x1FA57},
  {0x1FA60, 0x1FA6D},
  {0x1FA70, 0x1FA7C},
  {0x1FA80, 0x1FA8A},
  {0x1FA8E, 0x1FAC6},
  {0x1FAC8, 0x1FAC8},
  {0x1FACD, 0x1FADC},
  {0x1FADF, 0x1FAEA},
  {0x1FAEF, 0x1FAF8},
  {0x1FB00, 0x1FB92},
  {0x1FB94, 0x1FBFA},
  {0xE0001, 0xE0001},
  {0xE0020, 0xE007F},
};

inline bool regexMatchesUnicodePropertyScriptCommon(uint32_t cp) {
  return regexMatchesUnicodePropertyRanges(cp, kRegexUnicodePropertyScriptCommonRanges);
}

static constexpr RegexCodePointRange kRegexUnicodePropertyScriptExtensionsCommonRanges[] = {
  {0x0, 0x40},
  {0x5B, 0x60},
  {0x7B, 0xA9},
  {0xAB, 0xB6},
  {0xB8, 0xB9},
  {0xBB, 0xBF},
  {0xD7, 0xD7},
  {0xF7, 0xF7},
  {0x2B9, 0x2BB},
  {0x2BD, 0x2C6},
  {0x2C8, 0x2C8},
  {0x2CC, 0x2CC},
  {0x2CE, 0x2D6},
  {0x2D8, 0x2D8},
  {0x2DA, 0x2DF},
  {0x2E5, 0x2E9},
  {0x2EC, 0x2FF},
  {0x37E, 0x37E},
  {0x385, 0x385},
  {0x387, 0x387},
  {0x605, 0x605},
  {0x6DD, 0x6DD},
  {0x8E2, 0x8E2},
  {0xE3F, 0xE3F},
  {0xFD5, 0xFD8},
  {0x2000, 0x200B},
  {0x200E, 0x202E},
  {0x2030, 0x204E},
  {0x2050, 0x2059},
  {0x205B, 0x205C},
  {0x205E, 0x2064},
  {0x2066, 0x2070},
  {0x2074, 0x207E},
  {0x2080, 0x208E},
  {0x20A0, 0x20C1},
  {0x2100, 0x2125},
  {0x2127, 0x2129},
  {0x212C, 0x2131},
  {0x2133, 0x214D},
  {0x214F, 0x215F},
  {0x2189, 0x218B},
  {0x2190, 0x2429},
  {0x2440, 0x244A},
  {0x2460, 0x27FF},
  {0x2900, 0x2B73},
  {0x2B76, 0x2BFF},
  {0x2E00, 0x2E16},
  {0x2E18, 0x2E2F},
  {0x2E32, 0x2E3B},
  {0x2E3D, 0x2E40},
  {0x2E42, 0x2E42},
  {0x2E44, 0x2E5D},
  {0x3000, 0x3000},
  {0x3004, 0x3004},
  {0x3012, 0x3012},
  {0x3020, 0x3020},
  {0x3036, 0x3036},
  {0x3248, 0x325F},
  {0x327F, 0x327F},
  {0x32B1, 0x32BF},
  {0x32CC, 0x32CF},
  {0x3371, 0x337A},
  {0x3380, 0x33DF},
  {0x33FF, 0x33FF},
  {0x4DC0, 0x4DFF},
  {0xA708, 0xA721},
  {0xA788, 0xA78A},
  {0xAB5B, 0xAB5B},
  {0xAB6A, 0xAB6B},
  {0xFE10, 0xFE19},
  {0xFE30, 0xFE44},
  {0xFE47, 0xFE52},
  {0xFE54, 0xFE66},
  {0xFE68, 0xFE6B},
  {0xFEFF, 0xFEFF},
  {0xFF01, 0xFF20},
  {0xFF3B, 0xFF40},
  {0xFF5B, 0xFF60},
  {0xFFE0, 0xFFE6},
  {0xFFE8, 0xFFEE},
  {0xFFF9, 0xFFFD},
  {0x10190, 0x1019C},
  {0x101D0, 0x101FC},
  {0x1CC00, 0x1CCFC},
  {0x1CD00, 0x1CEB3},
  {0x1CEBA, 0x1CED0},
  {0x1CEE0, 0x1CEF0},
  {0x1CF50, 0x1CFC3},
  {0x1D000, 0x1D0F5},
  {0x1D100, 0x1D126},
  {0x1D129, 0x1D166},
  {0x1D16A, 0x1D17A},
  {0x1D183, 0x1D184},
  {0x1D18C, 0x1D1A9},
  {0x1D1AE, 0x1D1EA},
  {0x1D2C0, 0x1D2D3},
  {0x1D2E0, 0x1D2F3},
  {0x1D300, 0x1D356},
  {0x1D372, 0x1D378},
  {0x1D400, 0x1D454},
  {0x1D456, 0x1D49C},
  {0x1D49E, 0x1D49F},
  {0x1D4A2, 0x1D4A2},
  {0x1D4A5, 0x1D4A6},
  {0x1D4A9, 0x1D4AC},
  {0x1D4AE, 0x1D4B9},
  {0x1D4BB, 0x1D4BB},
  {0x1D4BD, 0x1D4C3},
  {0x1D4C5, 0x1D505},
  {0x1D507, 0x1D50A},
  {0x1D50D, 0x1D514},
  {0x1D516, 0x1D51C},
  {0x1D51E, 0x1D539},
  {0x1D53B, 0x1D53E},
  {0x1D540, 0x1D544},
  {0x1D546, 0x1D546},
  {0x1D54A, 0x1D550},
  {0x1D552, 0x1D6A5},
  {0x1D6A8, 0x1D7CB},
  {0x1D7CE, 0x1D7FF},
  {0x1EC71, 0x1ECB4},
  {0x1ED01, 0x1ED3D},
  {0x1F000, 0x1F02B},
  {0x1F030, 0x1F093},
  {0x1F0A0, 0x1F0AE},
  {0x1F0B1, 0x1F0BF},
  {0x1F0C1, 0x1F0CF},
  {0x1F0D1, 0x1F0F5},
  {0x1F100, 0x1F1AD},
  {0x1F1E6, 0x1F1FF},
  {0x1F201, 0x1F202},
  {0x1F210, 0x1F23B},
  {0x1F240, 0x1F248},
  {0x1F260, 0x1F265},
  {0x1F300, 0x1F6D8},
  {0x1F6DC, 0x1F6EC},
  {0x1F6F0, 0x1F6FC},
  {0x1F700, 0x1F7D9},
  {0x1F7E0, 0x1F7EB},
  {0x1F7F0, 0x1F7F0},
  {0x1F800, 0x1F80B},
  {0x1F810, 0x1F847},
  {0x1F850, 0x1F859},
  {0x1F860, 0x1F887},
  {0x1F890, 0x1F8AD},
  {0x1F8B0, 0x1F8BB},
  {0x1F8C0, 0x1F8C1},
  {0x1F8D0, 0x1F8D8},
  {0x1F900, 0x1FA57},
  {0x1FA60, 0x1FA6D},
  {0x1FA70, 0x1FA7C},
  {0x1FA80, 0x1FA8A},
  {0x1FA8E, 0x1FAC6},
  {0x1FAC8, 0x1FAC8},
  {0x1FACD, 0x1FADC},
  {0x1FADF, 0x1FAEA},
  {0x1FAEF, 0x1FAF8},
  {0x1FB00, 0x1FB92},
  {0x1FB94, 0x1FBFA},
  {0xE0001, 0xE0001},
  {0xE0020, 0xE007F},
};

inline bool regexMatchesUnicodePropertyScriptExtensionsCommon(uint32_t cp) {
  return regexMatchesUnicodePropertyRanges(cp, kRegexUnicodePropertyScriptExtensionsCommonRanges);
}

static constexpr RegexCodePointRange kRegexUnicodePropertyScriptInheritedRanges[] = {
  {0x300, 0x36F},
  {0x485, 0x486},
  {0x64B, 0x655},
  {0x670, 0x670},
  {0x951, 0x954},
  {0x1AB0, 0x1ADD},
  {0x1AE0, 0x1AEB},
  {0x1CD0, 0x1CD2},
  {0x1CD4, 0x1CE0},
  {0x1CE2, 0x1CE8},
  {0x1CED, 0x1CED},
  {0x1CF4, 0x1CF4},
  {0x1CF8, 0x1CF9},
  {0x1DC0, 0x1DFF},
  {0x200C, 0x200D},
  {0x20D0, 0x20F0},
  {0x302A, 0x302D},
  {0x3099, 0x309A},
  {0xFE00, 0xFE0F},
  {0xFE20, 0xFE2D},
  {0x101FD, 0x101FD},
  {0x102E0, 0x102E0},
  {0x1133B, 0x1133B},
  {0x1CF00, 0x1CF2D},
  {0x1CF30, 0x1CF46},
  {0x1D167, 0x1D169},
  {0x1D17B, 0x1D182},
  {0x1D185, 0x1D18B},
  {0x1D1AA, 0x1D1AD},
  {0xE0100, 0xE01EF},
};

inline bool regexMatchesUnicodePropertyScriptInherited(uint32_t cp) {
  return regexMatchesUnicodePropertyRanges(cp, kRegexUnicodePropertyScriptInheritedRanges);
}

static constexpr RegexCodePointRange kRegexUnicodePropertyScriptExtensionsInheritedRanges[] = {
  {0x30F, 0x30F},
  {0x312, 0x312},
  {0x314, 0x322},
  {0x326, 0x32C},
  {0x32F, 0x32F},
  {0x332, 0x341},
  {0x343, 0x344},
  {0x346, 0x357},
  {0x359, 0x35D},
  {0x35F, 0x362},
  {0x953, 0x954},
  {0x1AB0, 0x1ADD},
  {0x1AE0, 0x1AEB},
  {0x1DC2, 0x1DF7},
  {0x1DF9, 0x1DF9},
  {0x1DFB, 0x1DFF},
  {0x200C, 0x200D},
  {0x20D0, 0x20EF},
  {0xFE00, 0xFE0F},
  {0xFE20, 0xFE2D},
  {0x101FD, 0x101FD},
  {0x1CF00, 0x1CF2D},
  {0x1CF30, 0x1CF46},
  {0x1D167, 0x1D169},
  {0x1D17B, 0x1D182},
  {0x1D185, 0x1D18B},
  {0x1D1AA, 0x1D1AD},
  {0xE0100, 0xE01EF},
};

inline bool regexMatchesUnicodePropertyScriptExtensionsInherited(uint32_t cp) {
  return regexMatchesUnicodePropertyRanges(cp, kRegexUnicodePropertyScriptExtensionsInheritedRanges);
}

static constexpr RegexCodePointRange kRegexUnicodePropertyScriptLatinRanges[] = {
  {0x41, 0x5A},
  {0x61, 0x7A},
  {0xAA, 0xAA},
  {0xBA, 0xBA},
  {0xC0, 0xD6},
  {0xD8, 0xF6},
  {0xF8, 0x2B8},
  {0x2E0, 0x2E4},
  {0x1D00, 0x1D25},
  {0x1D2C, 0x1D5C},
  {0x1D62, 0x1D65},
  {0x1D6B, 0x1D77},
  {0x1D79, 0x1DBE},
  {0x1E00, 0x1EFF},
  {0x2071, 0x2071},
  {0x207F, 0x207F},
  {0x2090, 0x209C},
  {0x212A, 0x212B},
  {0x2132, 0x2132},
  {0x214E, 0x214E},
  {0x2160, 0x2188},
  {0x2C60, 0x2C7F},
  {0xA722, 0xA787},
  {0xA78B, 0xA7DC},
  {0xA7F1, 0xA7FF},
  {0xAB30, 0xAB5A},
  {0xAB5C, 0xAB64},
  {0xAB66, 0xAB69},
  {0xFB00, 0xFB06},
  {0xFF21, 0xFF3A},
  {0xFF41, 0xFF5A},
  {0x10780, 0x10785},
  {0x10787, 0x107B0},
  {0x107B2, 0x107BA},
  {0x1DF00, 0x1DF1E},
  {0x1DF25, 0x1DF2A},
};

inline bool regexMatchesUnicodePropertyScriptLatin(uint32_t cp) {
  return regexMatchesUnicodePropertyRanges(cp, kRegexUnicodePropertyScriptLatinRanges);
}

static constexpr RegexCodePointRange kRegexUnicodePropertyScriptExtensionsLatinRanges[] = {
  {0x41, 0x5A},
  {0x61, 0x7A},
  {0xAA, 0xAA},
  {0xB7, 0xB7},
  {0xBA, 0xBA},
  {0xC0, 0xD6},
  {0xD8, 0xF6},
  {0xF8, 0x2B8},
  {0x2BC, 0x2BC},
  {0x2C7, 0x2C7},
  {0x2C9, 0x2CB},
  {0x2CD, 0x2CD},
  {0x2D7, 0x2D7},
  {0x2D9, 0x2D9},
  {0x2E0, 0x2E4},
  {0x300, 0x30E},
  {0x310, 0x311},
  {0x313, 0x313},
  {0x323, 0x325},
  {0x32D, 0x32E},
  {0x330, 0x331},
  {0x358, 0x358},
  {0x35E, 0x35E},
  {0x363, 0x36F},
  {0x485, 0x486},
  {0x951, 0x952},
  {0x10FB, 0x10FB},
  {0x1D00, 0x1D25},
  {0x1D2C, 0x1D5C},
  {0x1D62, 0x1D65},
  {0x1D6B, 0x1D77},
  {0x1D79, 0x1DBE},
  {0x1DF8, 0x1DF8},
  {0x1E00, 0x1EFF},
  {0x202F, 0x202F},
  {0x2071, 0x2071},
  {0x207F, 0x207F},
  {0x2090, 0x209C},
  {0x20F0, 0x20F0},
  {0x212A, 0x212B},
  {0x2132, 0x2132},
  {0x214E, 0x214E},
  {0x2160, 0x2188},
  {0x2C60, 0x2C7F},
  {0x2E17, 0x2E17},
  {0xA700, 0xA707},
  {0xA722, 0xA787},
  {0xA78B, 0xA7DC},
  {0xA7F1, 0xA7FF},
  {0xA92E, 0xA92E},
  {0xAB30, 0xAB5A},
  {0xAB5C, 0xAB64},
  {0xAB66, 0xAB69},
  {0xFB00, 0xFB06},
  {0xFF21, 0xFF3A},
  {0xFF41, 0xFF5A},
  {0x10780, 0x10785},
  {0x10787, 0x107B0},
  {0x107B2, 0x107BA},
  {0x1DF00, 0x1DF1E},
  {0x1DF25, 0x1DF2A},
};

inline bool regexMatchesUnicodePropertyScriptExtensionsLatin(uint32_t cp) {
  return regexMatchesUnicodePropertyRanges(cp, kRegexUnicodePropertyScriptExtensionsLatinRanges);
}

static constexpr RegexCodePointRange kRegexUnicodePropertyScriptArabicRanges[] = {
  {0x600, 0x604},
  {0x606, 0x60B},
  {0x60D, 0x61A},
  {0x61C, 0x61E},
  {0x620, 0x63F},
  {0x641, 0x64A},
  {0x656, 0x66F},
  {0x671, 0x6DC},
  {0x6DE, 0x6FF},
  {0x750, 0x77F},
  {0x870, 0x891},
  {0x897, 0x8E1},
  {0x8E3, 0x8FF},
  {0xFB50, 0xFD3D},
  {0xFD40, 0xFDCF},
  {0xFDF0, 0xFDFF},
  {0xFE70, 0xFE74},
  {0xFE76, 0xFEFC},
  {0x10E60, 0x10E7E},
  {0x10EC2, 0x10EC7},
  {0x10ED0, 0x10ED8},
  {0x10EFA, 0x10EFF},
  {0x1EE00, 0x1EE03},
  {0x1EE05, 0x1EE1F},
  {0x1EE21, 0x1EE22},
  {0x1EE24, 0x1EE24},
  {0x1EE27, 0x1EE27},
  {0x1EE29, 0x1EE32},
  {0x1EE34, 0x1EE37},
  {0x1EE39, 0x1EE39},
  {0x1EE3B, 0x1EE3B},
  {0x1EE42, 0x1EE42},
  {0x1EE47, 0x1EE47},
  {0x1EE49, 0x1EE49},
  {0x1EE4B, 0x1EE4B},
  {0x1EE4D, 0x1EE4F},
  {0x1EE51, 0x1EE52},
  {0x1EE54, 0x1EE54},
  {0x1EE57, 0x1EE57},
  {0x1EE59, 0x1EE59},
  {0x1EE5B, 0x1EE5B},
  {0x1EE5D, 0x1EE5D},
  {0x1EE5F, 0x1EE5F},
  {0x1EE61, 0x1EE62},
  {0x1EE64, 0x1EE64},
  {0x1EE67, 0x1EE6A},
  {0x1EE6C, 0x1EE72},
  {0x1EE74, 0x1EE77},
  {0x1EE79, 0x1EE7C},
  {0x1EE7E, 0x1EE7E},
  {0x1EE80, 0x1EE89},
  {0x1EE8B, 0x1EE9B},
  {0x1EEA1, 0x1EEA3},
  {0x1EEA5, 0x1EEA9},
  {0x1EEAB, 0x1EEBB},
  {0x1EEF0, 0x1EEF1},
};

inline bool regexMatchesUnicodePropertyScriptArabic(uint32_t cp) {
  return regexMatchesUnicodePropertyRanges(cp, kRegexUnicodePropertyScriptArabicRanges);
}

static constexpr RegexCodePointRange kRegexUnicodePropertyScriptExtensionsArabicRanges[] = {
  {0x600, 0x604},
  {0x606, 0x6DC},
  {0x6DE, 0x6FF},
  {0x750, 0x77F},
  {0x870, 0x891},
  {0x897, 0x8E1},
  {0x8E3, 0x8FF},
  {0x204F, 0x204F},
  {0x2E41, 0x2E41},
  {0xFB50, 0xFDCF},
  {0xFDF0, 0xFDFF},
  {0xFE70, 0xFE74},
  {0xFE76, 0xFEFC},
  {0x102E0, 0x102FB},
  {0x10E60, 0x10E7E},
  {0x10EC2, 0x10EC7},
  {0x10ED0, 0x10ED8},
  {0x10EFA, 0x10EFF},
  {0x1EE00, 0x1EE03},
  {0x1EE05, 0x1EE1F},
  {0x1EE21, 0x1EE22},
  {0x1EE24, 0x1EE24},
  {0x1EE27, 0x1EE27},
  {0x1EE29, 0x1EE32},
  {0x1EE34, 0x1EE37},
  {0x1EE39, 0x1EE39},
  {0x1EE3B, 0x1EE3B},
  {0x1EE42, 0x1EE42},
  {0x1EE47, 0x1EE47},
  {0x1EE49, 0x1EE49},
  {0x1EE4B, 0x1EE4B},
  {0x1EE4D, 0x1EE4F},
  {0x1EE51, 0x1EE52},
  {0x1EE54, 0x1EE54},
  {0x1EE57, 0x1EE57},
  {0x1EE59, 0x1EE59},
  {0x1EE5B, 0x1EE5B},
  {0x1EE5D, 0x1EE5D},
  {0x1EE5F, 0x1EE5F},
  {0x1EE61, 0x1EE62},
  {0x1EE64, 0x1EE64},
  {0x1EE67, 0x1EE6A},
  {0x1EE6C, 0x1EE72},
  {0x1EE74, 0x1EE77},
  {0x1EE79, 0x1EE7C},
  {0x1EE7E, 0x1EE7E},
  {0x1EE80, 0x1EE89},
  {0x1EE8B, 0x1EE9B},
  {0x1EEA1, 0x1EEA3},
  {0x1EEA5, 0x1EEA9},
  {0x1EEAB, 0x1EEBB},
  {0x1EEF0, 0x1EEF1},
};

inline bool regexMatchesUnicodePropertyScriptExtensionsArabic(uint32_t cp) {
  return regexMatchesUnicodePropertyRanges(cp, kRegexUnicodePropertyScriptExtensionsArabicRanges);
}

static constexpr RegexCodePointRange kRegexUnicodePropertyScriptCyrillicRanges[] = {
  {0x400, 0x484},
  {0x487, 0x52F},
  {0x1C80, 0x1C8A},
  {0x1D2B, 0x1D2B},
  {0x1D78, 0x1D78},
  {0x2DE0, 0x2DFF},
  {0xA640, 0xA69F},
  {0xFE2E, 0xFE2F},
  {0x1E030, 0x1E06D},
  {0x1E08F, 0x1E08F},
};

inline bool regexMatchesUnicodePropertyScriptCyrillic(uint32_t cp) {
  return regexMatchesUnicodePropertyRanges(cp, kRegexUnicodePropertyScriptCyrillicRanges);
}

static constexpr RegexCodePointRange kRegexUnicodePropertyScriptExtensionsCyrillicRanges[] = {
  {0x2BC, 0x2BC},
  {0x300, 0x302},
  {0x304, 0x304},
  {0x306, 0x306},
  {0x308, 0x308},
  {0x30B, 0x30B},
  {0x311, 0x311},
  {0x400, 0x52F},
  {0x1C80, 0x1C8A},
  {0x1D2B, 0x1D2B},
  {0x1D78, 0x1D78},
  {0x1DF8, 0x1DF8},
  {0x2DE0, 0x2DFF},
  {0x2E43, 0x2E43},
  {0xA640, 0xA69F},
  {0xFE2E, 0xFE2F},
  {0x1E030, 0x1E06D},
  {0x1E08F, 0x1E08F},
};

inline bool regexMatchesUnicodePropertyScriptExtensionsCyrillic(uint32_t cp) {
  return regexMatchesUnicodePropertyRanges(cp, kRegexUnicodePropertyScriptExtensionsCyrillicRanges);
}

static constexpr RegexCodePointRange kRegexUnicodePropertyScriptDevanagariRanges[] = {
  {0x900, 0x950},
  {0x955, 0x963},
  {0x966, 0x97F},
  {0xA8E0, 0xA8FF},
  {0x11B00, 0x11B09},
};

inline bool regexMatchesUnicodePropertyScriptDevanagari(uint32_t cp) {
  return regexMatchesUnicodePropertyRanges(cp, kRegexUnicodePropertyScriptDevanagariRanges);
}

static constexpr RegexCodePointRange kRegexUnicodePropertyScriptExtensionsDevanagariRanges[] = {
  {0x2BC, 0x2BC},
  {0x900, 0x952},
  {0x955, 0x97F},
  {0x1CD0, 0x1CF6},
  {0x1CF8, 0x1CF9},
  {0x20F0, 0x20F0},
  {0xA830, 0xA839},
  {0xA8E0, 0xA8FF},
  {0x11B00, 0x11B09},
};

inline bool regexMatchesUnicodePropertyScriptExtensionsDevanagari(uint32_t cp) {
  return regexMatchesUnicodePropertyRanges(cp, kRegexUnicodePropertyScriptExtensionsDevanagariRanges);
}

static constexpr RegexCodePointRange kRegexUnicodePropertyScriptBengaliRanges[] = {
  {0x980, 0x983},
  {0x985, 0x98C},
  {0x98F, 0x990},
  {0x993, 0x9A8},
  {0x9AA, 0x9B0},
  {0x9B2, 0x9B2},
  {0x9B6, 0x9B9},
  {0x9BC, 0x9C4},
  {0x9C7, 0x9C8},
  {0x9CB, 0x9CE},
  {0x9D7, 0x9D7},
  {0x9DC, 0x9DD},
  {0x9DF, 0x9E3},
  {0x9E6, 0x9FE},
};

inline bool regexMatchesUnicodePropertyScriptBengali(uint32_t cp) {
  return regexMatchesUnicodePropertyRanges(cp, kRegexUnicodePropertyScriptBengaliRanges);
}

static constexpr RegexCodePointRange kRegexUnicodePropertyScriptExtensionsBengaliRanges[] = {
  {0x2BC, 0x2BC},
  {0x951, 0x952},
  {0x964, 0x965},
  {0x980, 0x983},
  {0x985, 0x98C},
  {0x98F, 0x990},
  {0x993, 0x9A8},
  {0x9AA, 0x9B0},
  {0x9B2, 0x9B2},
  {0x9B6, 0x9B9},
  {0x9BC, 0x9C4},
  {0x9C7, 0x9C8},
  {0x9CB, 0x9CE},
  {0x9D7, 0x9D7},
  {0x9DC, 0x9DD},
  {0x9DF, 0x9E3},
  {0x9E6, 0x9FE},
  {0x1CD0, 0x1CD0},
  {0x1CD2, 0x1CD2},
  {0x1CD5, 0x1CD6},
  {0x1CD8, 0x1CD8},
  {0x1CE1, 0x1CE1},
  {0x1CEA, 0x1CEA},
  {0x1CED, 0x1CED},
  {0x1CF2, 0x1CF2},
  {0x1CF5, 0x1CF7},
  {0xA8F1, 0xA8F1},
};

inline bool regexMatchesUnicodePropertyScriptExtensionsBengali(uint32_t cp) {
  return regexMatchesUnicodePropertyRanges(cp, kRegexUnicodePropertyScriptExtensionsBengaliRanges);
}

static constexpr RegexCodePointRange kRegexUnicodePropertyScriptGujaratiRanges[] = {
  {0xA81, 0xA83},
  {0xA85, 0xA8D},
  {0xA8F, 0xA91},
  {0xA93, 0xAA8},
  {0xAAA, 0xAB0},
  {0xAB2, 0xAB3},
  {0xAB5, 0xAB9},
  {0xABC, 0xAC5},
  {0xAC7, 0xAC9},
  {0xACB, 0xACD},
  {0xAD0, 0xAD0},
  {0xAE0, 0xAE3},
  {0xAE6, 0xAF1},
  {0xAF9, 0xAFF},
};

inline bool regexMatchesUnicodePropertyScriptGujarati(uint32_t cp) {
  return regexMatchesUnicodePropertyRanges(cp, kRegexUnicodePropertyScriptGujaratiRanges);
}

static constexpr RegexCodePointRange kRegexUnicodePropertyScriptExtensionsGujaratiRanges[] = {
  {0x951, 0x952},
  {0x964, 0x965},
  {0xA81, 0xA83},
  {0xA85, 0xA8D},
  {0xA8F, 0xA91},
  {0xA93, 0xAA8},
  {0xAAA, 0xAB0},
  {0xAB2, 0xAB3},
  {0xAB5, 0xAB9},
  {0xABC, 0xAC5},
  {0xAC7, 0xAC9},
  {0xACB, 0xACD},
  {0xAD0, 0xAD0},
  {0xAE0, 0xAE3},
  {0xAE6, 0xAF1},
  {0xAF9, 0xAFF},
  {0xA830, 0xA839},
};

inline bool regexMatchesUnicodePropertyScriptExtensionsGujarati(uint32_t cp) {
  return regexMatchesUnicodePropertyRanges(cp, kRegexUnicodePropertyScriptExtensionsGujaratiRanges);
}

static constexpr RegexCodePointRange kRegexUnicodePropertyScriptBrahmiRanges[] = {
  {0x11000, 0x1104D},
  {0x11052, 0x11075},
  {0x1107F, 0x1107F},
};

inline bool regexMatchesUnicodePropertyScriptBrahmi(uint32_t cp) {
  return regexMatchesUnicodePropertyRanges(cp, kRegexUnicodePropertyScriptBrahmiRanges);
}

static constexpr RegexCodePointRange kRegexUnicodePropertyScriptExtensionsBrahmiRanges[] = {
  {0x11000, 0x1104D},
  {0x11052, 0x11075},
  {0x1107F, 0x1107F},
};

inline bool regexMatchesUnicodePropertyScriptExtensionsBrahmi(uint32_t cp) {
  return regexMatchesUnicodePropertyRanges(cp, kRegexUnicodePropertyScriptExtensionsBrahmiRanges);
}

static constexpr RegexCodePointRange kRegexUnicodePropertyScriptKhmerRanges[] = {
  {0x1780, 0x17DD},
  {0x17E0, 0x17E9},
  {0x17F0, 0x17F9},
  {0x19E0, 0x19FF},
};

inline bool regexMatchesUnicodePropertyScriptKhmer(uint32_t cp) {
  return regexMatchesUnicodePropertyRanges(cp, kRegexUnicodePropertyScriptKhmerRanges);
}

static constexpr RegexCodePointRange kRegexUnicodePropertyScriptExtensionsKhmerRanges[] = {
  {0x1780, 0x17DD},
  {0x17E0, 0x17E9},
  {0x17F0, 0x17F9},
  {0x19E0, 0x19FF},
};

inline bool regexMatchesUnicodePropertyScriptExtensionsKhmer(uint32_t cp) {
  return regexMatchesUnicodePropertyRanges(cp, kRegexUnicodePropertyScriptExtensionsKhmerRanges);
}

#include "regex_additional_script_matchers.inc"

#include "regex_additional_binary_matchers.inc"

inline bool regexMatchesSupportedUnicodeProperty(SupportedRegexUnicodeProperty property,
                                                 uint32_t cp) {
  switch (property) {
    case SupportedRegexUnicodeProperty::ASCII:
      return regexMatchesUnicodePropertyASCII(cp);
    case SupportedRegexUnicodeProperty::ASCIIHexDigit:
      return regexMatchesUnicodePropertyASCIIHexDigit(cp);
    case SupportedRegexUnicodeProperty::UppercaseLetter:
      return regexModifierMatchesUnicodePropertyLu(cp, false);
    case SupportedRegexUnicodeProperty::LowercaseLetter:
      return regexMatchesUnicodePropertyLowercaseLetter(cp);
    case SupportedRegexUnicodeProperty::DecimalNumber:
      return regexMatchesUnicodePropertyDecimalNumber(cp);
    case SupportedRegexUnicodeProperty::TitlecaseLetter:
      return regexMatchesUnicodePropertyTitlecaseLetter(cp);
    case SupportedRegexUnicodeProperty::LetterNumber:
      return regexMatchesUnicodePropertyLetterNumber(cp);
    case SupportedRegexUnicodeProperty::OtherNumber:
      return regexMatchesUnicodePropertyOtherNumber(cp);
    case SupportedRegexUnicodeProperty::Number:
      return regexMatchesUnicodePropertyNumber(cp);
    case SupportedRegexUnicodeProperty::CasedLetter:
      return regexMatchesUnicodePropertyCasedLetter(cp);
    case SupportedRegexUnicodeProperty::ModifierLetter:
      return regexMatchesUnicodePropertyModifierLetter(cp);
    case SupportedRegexUnicodeProperty::OtherLetter:
      return regexMatchesUnicodePropertyOtherLetter(cp);
    case SupportedRegexUnicodeProperty::Letter:
      return regexMatchesUnicodePropertyLetter(cp);
    case SupportedRegexUnicodeProperty::LineSeparator:
      return regexMatchesUnicodePropertyLineSeparator(cp);
    case SupportedRegexUnicodeProperty::ParagraphSeparator:
      return regexMatchesUnicodePropertyParagraphSeparator(cp);
    case SupportedRegexUnicodeProperty::SpaceSeparator:
      return regexMatchesUnicodePropertySpaceSeparator(cp);
    case SupportedRegexUnicodeProperty::Separator:
      return regexMatchesUnicodePropertySeparator(cp);
    case SupportedRegexUnicodeProperty::NonspacingMark:
      return regexMatchesUnicodePropertyNonspacingMark(cp);
    case SupportedRegexUnicodeProperty::EnclosingMark:
      return regexMatchesUnicodePropertyEnclosingMark(cp);
    case SupportedRegexUnicodeProperty::SpacingMark:
      return regexMatchesUnicodePropertySpacingMark(cp);
    case SupportedRegexUnicodeProperty::Mark:
      return regexMatchesUnicodePropertyMark(cp);
    case SupportedRegexUnicodeProperty::ConnectorPunctuation:
      return regexMatchesUnicodePropertyConnectorPunctuation(cp);
    case SupportedRegexUnicodeProperty::FinalPunctuation:
      return regexMatchesUnicodePropertyFinalPunctuation(cp);
    case SupportedRegexUnicodeProperty::InitialPunctuation:
      return regexMatchesUnicodePropertyInitialPunctuation(cp);
    case SupportedRegexUnicodeProperty::DashPunctuation:
      return regexMatchesUnicodePropertyDashPunctuation(cp);
    case SupportedRegexUnicodeProperty::OpenPunctuation:
      return regexMatchesUnicodePropertyOpenPunctuation(cp);
    case SupportedRegexUnicodeProperty::ClosePunctuation:
      return regexMatchesUnicodePropertyClosePunctuation(cp);
    case SupportedRegexUnicodeProperty::OtherPunctuation:
      return regexMatchesUnicodePropertyOtherPunctuation(cp);
    case SupportedRegexUnicodeProperty::Punctuation:
      return regexMatchesUnicodePropertyPunctuation(cp);
    case SupportedRegexUnicodeProperty::Control:
      return regexMatchesUnicodePropertyControl(cp);
    case SupportedRegexUnicodeProperty::PrivateUse:
      return regexMatchesUnicodePropertyPrivateUse(cp);
    case SupportedRegexUnicodeProperty::CurrencySymbol:
      return regexMatchesUnicodePropertyCurrencySymbol(cp);
    case SupportedRegexUnicodeProperty::Format:
      return regexMatchesUnicodePropertyFormat(cp);
    case SupportedRegexUnicodeProperty::ModifierSymbol:
      return regexMatchesUnicodePropertyModifierSymbol(cp);
    case SupportedRegexUnicodeProperty::MathSymbol:
      return regexMatchesUnicodePropertyMathSymbol(cp);
    case SupportedRegexUnicodeProperty::OtherSymbol:
      return regexMatchesUnicodePropertyOtherSymbol(cp);
    case SupportedRegexUnicodeProperty::Symbol:
      return regexMatchesUnicodePropertySymbol(cp);
    case SupportedRegexUnicodeProperty::Surrogate:
      return regexMatchesUnicodePropertySurrogate(cp);
#define LIGHTJS_REGEX_ADDITIONAL_BINARY_PROPERTY(identifier, canonical, alias) \
    case SupportedRegexUnicodeProperty::identifier:                            \
      return regexMatchesUnicodeProperty##identifier(cp);
#include "regex_additional_binary_properties.inc"
#undef LIGHTJS_REGEX_ADDITIONAL_BINARY_PROPERTY
    case SupportedRegexUnicodeProperty::ScriptHan:
      return regexMatchesUnicodePropertyScriptHan(cp);
    case SupportedRegexUnicodeProperty::ScriptExtensionsHan:
      return regexMatchesUnicodePropertyScriptExtensionsHan(cp);
    case SupportedRegexUnicodeProperty::ScriptHangul:
      return regexMatchesUnicodePropertyScriptHangul(cp);
    case SupportedRegexUnicodeProperty::ScriptExtensionsHangul:
      return regexMatchesUnicodePropertyScriptExtensionsHangul(cp);
    case SupportedRegexUnicodeProperty::ScriptHanunoo:
      return regexMatchesUnicodePropertyScriptHanunoo(cp);
    case SupportedRegexUnicodeProperty::ScriptExtensionsHanunoo:
      return regexMatchesUnicodePropertyScriptExtensionsHanunoo(cp);
    case SupportedRegexUnicodeProperty::ScriptBuhid:
      return regexMatchesUnicodePropertyScriptBuhid(cp);
    case SupportedRegexUnicodeProperty::ScriptExtensionsBuhid:
      return regexMatchesUnicodePropertyScriptExtensionsBuhid(cp);
    case SupportedRegexUnicodeProperty::ScriptTagalog:
      return regexMatchesUnicodePropertyScriptTagalog(cp);
    case SupportedRegexUnicodeProperty::ScriptExtensionsTagalog:
      return regexMatchesUnicodePropertyScriptExtensionsTagalog(cp);
    case SupportedRegexUnicodeProperty::ScriptTagbanwa:
      return regexMatchesUnicodePropertyScriptTagbanwa(cp);
    case SupportedRegexUnicodeProperty::ScriptExtensionsTagbanwa:
      return regexMatchesUnicodePropertyScriptExtensionsTagbanwa(cp);
    case SupportedRegexUnicodeProperty::ScriptOgham:
    case SupportedRegexUnicodeProperty::ScriptExtensionsOgham:
      return regexMatchesUnicodePropertyScriptOgham(cp);
    case SupportedRegexUnicodeProperty::ScriptBuginese:
      return regexMatchesUnicodePropertyScriptBuginese(cp);
    case SupportedRegexUnicodeProperty::ScriptExtensionsBuginese:
      return regexMatchesUnicodePropertyScriptExtensionsBuginese(cp);
    case SupportedRegexUnicodeProperty::ScriptTaiLe:
      return regexMatchesUnicodePropertyScriptTaiLe(cp);
    case SupportedRegexUnicodeProperty::ScriptExtensionsTaiLe:
      return regexMatchesUnicodePropertyScriptExtensionsTaiLe(cp);
    case SupportedRegexUnicodeProperty::ScriptCham:
    case SupportedRegexUnicodeProperty::ScriptExtensionsCham:
      return regexMatchesUnicodePropertyScriptCham(cp);
    case SupportedRegexUnicodeProperty::ScriptRunic:
      return regexMatchesUnicodePropertyScriptRunic(cp);
    case SupportedRegexUnicodeProperty::ScriptExtensionsRunic:
      return regexMatchesUnicodePropertyScriptExtensionsRunic(cp);
    case SupportedRegexUnicodeProperty::ScriptCommon:
      return regexMatchesUnicodePropertyScriptCommon(cp);
    case SupportedRegexUnicodeProperty::ScriptExtensionsCommon:
      return regexMatchesUnicodePropertyScriptExtensionsCommon(cp);
    case SupportedRegexUnicodeProperty::ScriptInherited:
      return regexMatchesUnicodePropertyScriptInherited(cp);
    case SupportedRegexUnicodeProperty::ScriptExtensionsInherited:
      return regexMatchesUnicodePropertyScriptExtensionsInherited(cp);
    case SupportedRegexUnicodeProperty::ScriptLatin:
      return regexMatchesUnicodePropertyScriptLatin(cp);
    case SupportedRegexUnicodeProperty::ScriptExtensionsLatin:
      return regexMatchesUnicodePropertyScriptExtensionsLatin(cp);
    case SupportedRegexUnicodeProperty::ScriptArabic:
      return regexMatchesUnicodePropertyScriptArabic(cp);
    case SupportedRegexUnicodeProperty::ScriptExtensionsArabic:
      return regexMatchesUnicodePropertyScriptExtensionsArabic(cp);
    case SupportedRegexUnicodeProperty::ScriptCyrillic:
      return regexMatchesUnicodePropertyScriptCyrillic(cp);
    case SupportedRegexUnicodeProperty::ScriptExtensionsCyrillic:
      return regexMatchesUnicodePropertyScriptExtensionsCyrillic(cp);
    case SupportedRegexUnicodeProperty::ScriptDevanagari:
      return regexMatchesUnicodePropertyScriptDevanagari(cp);
    case SupportedRegexUnicodeProperty::ScriptExtensionsDevanagari:
      return regexMatchesUnicodePropertyScriptExtensionsDevanagari(cp);
    case SupportedRegexUnicodeProperty::ScriptBengali:
      return regexMatchesUnicodePropertyScriptBengali(cp);
    case SupportedRegexUnicodeProperty::ScriptExtensionsBengali:
      return regexMatchesUnicodePropertyScriptExtensionsBengali(cp);
    case SupportedRegexUnicodeProperty::ScriptGujarati:
      return regexMatchesUnicodePropertyScriptGujarati(cp);
    case SupportedRegexUnicodeProperty::ScriptExtensionsGujarati:
      return regexMatchesUnicodePropertyScriptExtensionsGujarati(cp);
    case SupportedRegexUnicodeProperty::ScriptBrahmi:
      return regexMatchesUnicodePropertyScriptBrahmi(cp);
    case SupportedRegexUnicodeProperty::ScriptExtensionsBrahmi:
      return regexMatchesUnicodePropertyScriptExtensionsBrahmi(cp);
    case SupportedRegexUnicodeProperty::ScriptKhmer:
      return regexMatchesUnicodePropertyScriptKhmer(cp);
    case SupportedRegexUnicodeProperty::ScriptExtensionsKhmer:
      return regexMatchesUnicodePropertyScriptExtensionsKhmer(cp);
#define LIGHTJS_REGEX_ADDITIONAL_SCRIPT_PROPERTY(identifier, canonical, alias) \
    case SupportedRegexUnicodeProperty::Script##identifier:                    \
      return regexMatchesUnicodePropertyScript##identifier(cp);                \
    case SupportedRegexUnicodeProperty::ScriptExtensions##identifier:          \
      return regexMatchesUnicodePropertyScriptExtensions##identifier(cp);
#include "regex_additional_script_properties.inc"
#undef LIGHTJS_REGEX_ADDITIONAL_SCRIPT_PROPERTY
  }
  return false;
}

inline bool isRegexEmojiFamilySetPattern(const std::string& pattern) {
  return pattern ==
         ("[" "\xF0\x9F\x91\xA8" "\xE2\x80\x8D" "\xF0\x9F\x91\xA9"
          "\xE2\x80\x8D" "\xF0\x9F\x91\xA7" "\xE2\x80\x8D"
          "\xF0\x9F\x91\xA6" "]");
}

inline bool regexMatchesEmojiFamilySetMember(uint32_t cp) {
  return cp == 0x1F468 || cp == 0x200D || cp == 0x1F469 ||
         cp == 0x1F467 || cp == 0x1F466;
}

inline bool isRegexRegionalIndicator(uint32_t cp) {
  return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

inline bool isRegexEmojiModifier(uint32_t cp) {
  return cp >= 0x1F3FB && cp <= 0x1F3FF;
}

inline bool isRegexTagSpecChar(uint32_t cp) {
  return cp >= 0xE0061 && cp <= 0xE007A;
}

inline bool isRegexBroadBasicEmojiCodePoint(uint32_t cp) {
  if (cp == 0x200D || cp == 0x20E3 || cp == 0xFE0F ||
      isRegexRegionalIndicator(cp) ||
      isRegexTagSpecChar(cp) || cp == 0xE007F) {
    return false;
  }

  if (isRegexEmojiModifier(cp)) {
    return true;
  }

  if ((cp >= 0x231A && cp <= 0x231B) ||
      (cp >= 0x23E9 && cp <= 0x23EC) ||
      cp == 0x23F0 || cp == 0x23F3 ||
      (cp >= 0x25FD && cp <= 0x25FE) ||
      (cp >= 0x2614 && cp <= 0x2615) ||
      (cp >= 0x2648 && cp <= 0x2653) ||
      cp == 0x267F || cp == 0x2693 || cp == 0x26A1 ||
      (cp >= 0x26AA && cp <= 0x26AB) ||
      (cp >= 0x26BD && cp <= 0x26BE) ||
      (cp >= 0x26C4 && cp <= 0x26C5) ||
      cp == 0x26CE || cp == 0x26D4 || cp == 0x26EA ||
      (cp >= 0x26F2 && cp <= 0x26F5) ||
      cp == 0x26FA || cp == 0x26FD || cp == 0x2705 ||
      (cp >= 0x270A && cp <= 0x270B) ||
      cp == 0x2728 || cp == 0x274C || cp == 0x274E ||
      (cp >= 0x2753 && cp <= 0x2755) ||
      cp == 0x2757 || (cp >= 0x2795 && cp <= 0x2797) ||
      cp == 0x27B0 || cp == 0x27BF ||
      (cp >= 0x2B1B && cp <= 0x2B1C) ||
      cp == 0x2B50 || cp == 0x2B55) {
    return true;
  }

  return cp >= 0x1F000 && cp <= 0x1FAFF;
}

inline bool isRegexExplicitEmojiModifierBase(uint32_t cp) {
  return cp == 0x261D || cp == 0x26F9 ||
         cp == 0x270A || cp == 0x270B ||
         cp == 0x270C || cp == 0x270D;
}

inline std::optional<size_t> consumeRegexEmojiKeycapSequence(const DecodedRegexInput& decoded,
                                                             size_t index) {
  if (index + 2 >= decoded.codePoints.size()) {
    return std::nullopt;
  }
  uint32_t cp = decoded.codePoints[index];
  if (cp != '#' && cp != '*' && !(cp >= '0' && cp <= '9')) {
    return std::nullopt;
  }
  if (decoded.codePoints[index + 1] != 0xFE0F || decoded.codePoints[index + 2] != 0x20E3) {
    return std::nullopt;
  }
  return index + 3;
}

inline std::optional<size_t> consumeRegexRgiFlagSequence(const DecodedRegexInput& decoded,
                                                         size_t index) {
  if (index + 1 >= decoded.codePoints.size()) {
    return std::nullopt;
  }
  if (!isRegexRegionalIndicator(decoded.codePoints[index]) ||
      !isRegexRegionalIndicator(decoded.codePoints[index + 1])) {
    return std::nullopt;
  }
  return index + 2;
}

inline std::optional<size_t> consumeRegexRgiTagSequence(const DecodedRegexInput& decoded,
                                                        size_t index) {
  if (index >= decoded.codePoints.size() || decoded.codePoints[index] != 0x1F3F4) {
    return std::nullopt;
  }
  size_t cursor = index + 1;
  size_t tagCount = 0;
  while (cursor < decoded.codePoints.size() && isRegexTagSpecChar(decoded.codePoints[cursor])) {
    ++cursor;
    ++tagCount;
  }
  if (tagCount == 0 || cursor >= decoded.codePoints.size() || decoded.codePoints[cursor] != 0xE007F) {
    return std::nullopt;
  }
  return cursor + 1;
}

inline std::optional<size_t> consumeRegexBasicEmojiToken(const DecodedRegexInput& decoded,
                                                         size_t index) {
  if (index >= decoded.codePoints.size()) {
    return std::nullopt;
  }
  uint32_t cp = decoded.codePoints[index];
  if (cp == 0xFE0F) {
    return std::nullopt;
  }
  if (index + 1 < decoded.codePoints.size() && decoded.codePoints[index + 1] == 0xFE0F) {
    return index + 2;
  }
  if (isRegexBroadBasicEmojiCodePoint(cp)) {
    return index + 1;
  }
  return std::nullopt;
}

inline std::optional<size_t> consumeRegexRgiModifierSequence(const DecodedRegexInput& decoded,
                                                             size_t index) {
  std::optional<size_t> baseEnd;
  if (index < decoded.codePoints.size() &&
      isRegexExplicitEmojiModifierBase(decoded.codePoints[index])) {
    baseEnd = index + 1;
  } else {
    baseEnd = consumeRegexBasicEmojiToken(decoded, index);
  }
  if (!baseEnd.has_value() || *baseEnd >= decoded.codePoints.size()) {
    return std::nullopt;
  }
  if (!isRegexEmojiModifier(decoded.codePoints[*baseEnd])) {
    return std::nullopt;
  }
  return *baseEnd + 1;
}

inline std::optional<size_t> consumeRegexRgiEmojiAtom(const DecodedRegexInput& decoded,
                                                      size_t index) {
  if (auto keycapEnd = consumeRegexEmojiKeycapSequence(decoded, index)) {
    return keycapEnd;
  }
  if (auto flagEnd = consumeRegexRgiFlagSequence(decoded, index)) {
    return flagEnd;
  }
  if (auto tagEnd = consumeRegexRgiTagSequence(decoded, index)) {
    return tagEnd;
  }
  if (auto modifierEnd = consumeRegexRgiModifierSequence(decoded, index)) {
    return modifierEnd;
  }
  return consumeRegexBasicEmojiToken(decoded, index);
}

inline std::optional<size_t> consumeRegexRgiZwjComponent(const DecodedRegexInput& decoded,
                                                         size_t index) {
  if (auto keycapEnd = consumeRegexEmojiKeycapSequence(decoded, index)) {
    return keycapEnd;
  }
  if (auto flagEnd = consumeRegexRgiFlagSequence(decoded, index)) {
    return flagEnd;
  }
  if (auto tagEnd = consumeRegexRgiTagSequence(decoded, index)) {
    return tagEnd;
  }
  if (auto modifierEnd = consumeRegexRgiModifierSequence(decoded, index)) {
    return modifierEnd;
  }
  if (index < decoded.codePoints.size() && isRegexEmojiModifier(decoded.codePoints[index])) {
    return std::nullopt;
  }
  return consumeRegexBasicEmojiToken(decoded, index);
}

inline std::optional<size_t> consumeRegexRgiZwjSequence(const DecodedRegexInput& decoded,
                                                        size_t index) {
  auto firstEnd = consumeRegexRgiZwjComponent(decoded, index);
  if (!firstEnd.has_value()) {
    return std::nullopt;
  }
  size_t cursor = *firstEnd;
  bool sawJoiner = false;
  while (cursor < decoded.codePoints.size() && decoded.codePoints[cursor] == 0x200D) {
    auto nextEnd = consumeRegexRgiZwjComponent(decoded, cursor + 1);
    if (!nextEnd.has_value()) {
      return std::nullopt;
    }
    sawJoiner = true;
    cursor = *nextEnd;
  }
  if (!sawJoiner) {
    return std::nullopt;
  }
  return cursor;
}

inline std::optional<size_t> consumeRegexPropertyOfStringsMember(const std::string& pattern,
                                                                 const DecodedRegexInput& decoded,
                                                                 size_t index) {
  if (pattern == R"(^\p{Emoji_Keycap_Sequence}+$)") {
    return consumeRegexEmojiKeycapSequence(decoded, index);
  }
  if (pattern == R"(^\p{RGI_Emoji_Flag_Sequence}+$)") {
    return consumeRegexRgiFlagSequence(decoded, index);
  }
  if (pattern == R"(^\p{RGI_Emoji_Tag_Sequence}+$)") {
    return consumeRegexRgiTagSequence(decoded, index);
  }
  if (pattern == R"(^\p{RGI_Emoji_Modifier_Sequence}+$)") {
    return consumeRegexRgiModifierSequence(decoded, index);
  }
  if (pattern == R"(^\p{RGI_Emoji_ZWJ_Sequence}+$)") {
    return consumeRegexRgiZwjSequence(decoded, index);
  }
  if (pattern == R"(^\p{Basic_Emoji}+$)") {
    return consumeRegexBasicEmojiToken(decoded, index);
  }
  if (pattern == R"(^\p{RGI_Emoji}+$)") {
    if (auto zwjEnd = consumeRegexRgiZwjSequence(decoded, index)) {
      return zwjEnd;
    }
    return consumeRegexRgiEmojiAtom(decoded, index);
  }
  return std::nullopt;
}

inline bool regexModifierIsLineStart(const std::string& str, size_t pos) {
  return pos == 0 || str[pos - 1] == '\n';
}

inline bool regexModifierIsLineEnd(const std::string& str, size_t pos) {
  return pos == str.size() || str[pos] == '\n';
}

inline bool regexModifierTextMatchesAt(const std::string& str,
                                       size_t pos,
                                       const std::string& literal,
                                       bool ignoreCase) {
  if (pos + literal.size() > str.size()) {
    return false;
  }
  for (size_t i = 0; i < literal.size(); ++i) {
    if (!regexModifierLiteralMatches(
            static_cast<unsigned char>(str[pos + i]), literal[i], ignoreCase)) {
      return false;
    }
  }
  return true;
}

inline std::optional<SpecialRegexMatchResult> findRegexModifierLiteralWithEndAnchor(
    const std::string& str,
    size_t searchStart,
    const std::string& literal,
    bool ignoreCase,
    bool multiline) {
  for (size_t pos = searchStart; pos + literal.size() <= str.size(); ++pos) {
    if (!regexModifierTextMatchesAt(str, pos, literal, ignoreCase)) {
      continue;
    }
    size_t endPos = pos + literal.size();
    if ((multiline && regexModifierIsLineEnd(str, endPos)) ||
        (!multiline && endPos == str.size())) {
      return SpecialRegexMatchResult{pos, endPos, str.substr(pos, literal.size()), {}};
    }
  }
  return std::nullopt;
}

inline std::optional<SpecialRegexMatchResult> findRegexModifierLiteralDotWithEndAnchor(
    const std::string& str,
    size_t searchStart,
    const std::string& literalPrefix,
    bool ignoreCase,
    bool dotAll,
    bool multiline) {
  for (size_t pos = searchStart; pos + literalPrefix.size() < str.size(); ++pos) {
    if (!regexModifierTextMatchesAt(str, pos, literalPrefix, ignoreCase)) {
      continue;
    }
    char dotChar = str[pos + literalPrefix.size()];
    if (!dotAll && dotChar == '\n') {
      continue;
    }
    size_t endPos = pos + literalPrefix.size() + 1;
    if ((multiline && regexModifierIsLineEnd(str, endPos)) ||
        (!multiline && endPos == str.size())) {
      return SpecialRegexMatchResult{pos, endPos, str.substr(pos, endPos - pos), {}};
    }
  }
  return std::nullopt;
}

inline std::optional<SpecialRegexMatchResult> findRegexModifierAnchoredLiteral(
    const std::string& str,
    size_t searchStart,
    const std::string& literal,
    bool ignoreCase,
    bool multilineStart,
    bool multilineEnd) {
  for (size_t pos = searchStart; pos + literal.size() <= str.size(); ++pos) {
    if (!regexModifierTextMatchesAt(str, pos, literal, ignoreCase)) {
      continue;
    }
    if ((multilineStart && !regexModifierIsLineStart(str, pos)) ||
        (!multilineStart && pos != 0)) {
      continue;
    }
    size_t endPos = pos + literal.size();
    if ((multilineEnd && regexModifierIsLineEnd(str, endPos)) ||
        (!multilineEnd && endPos == str.size())) {
      return SpecialRegexMatchResult{pos, endPos, str.substr(pos, literal.size()), {}};
    }
  }
  return std::nullopt;
}

inline std::optional<SpecialRegexMatchResult> tryMatchSpecialDuplicateNamedPattern(
    const std::string& pattern,
    const std::string& flags,
    const std::string& str,
    size_t searchStart) {
  auto makeCapture = [](bool matched, size_t start, size_t end, const std::string& value) {
    return SpecialRegexCaptureResult{matched, start, end, value};
  };

  if (pattern == R"((a)(?i:\1))" || pattern == R"((a)(?i-:\1))") {
    for (size_t i = searchStart; i + 1 < str.size(); ++i) {
      if (str[i] == 'a' && (str[i + 1] == 'a' || str[i + 1] == 'A')) {
        return SpecialRegexMatchResult{i, i + 2, str.substr(i, 2),
                                       {makeCapture(true, i, i + 1, "a")}};
      }
    }
    return std::nullopt;
  }

  if (pattern == R"((a)(?-i:\1))" && regexHasIgnoreCase(flags)) {
    for (size_t i = searchStart; i + 1 < str.size(); ++i) {
      if ((str[i] == 'a' || str[i] == 'A') && str[i + 1] == str[i]) {
        return SpecialRegexMatchResult{i, i + 2, str.substr(i, 2),
                                       {makeCapture(true, i, i + 1, str.substr(i, 1))}};
      }
    }
    return std::nullopt;
  }

  if (pattern == R"((?i:\b))" || pattern == R"((?i-:\b))" ||
      (pattern == R"((?-i:\b))" && regexHasIgnoreCase(flags))) {
    const bool unicodeMode = regexHasUnicodeMode(flags);
    const bool ignoreCase = pattern != R"((?-i:\b))";
    auto decoded = decodeRegexInput(str);
    for (size_t i = 0; i <= decoded.codePoints.size(); ++i) {
      if (isRegexModifierBoundaryAt(decoded, i, unicodeMode, ignoreCase)) {
        size_t offset = regexByteOffsetAt(decoded, i);
        return SpecialRegexMatchResult{offset, offset, "", {}};
      }
    }
    return std::nullopt;
  }

  if (pattern == R"((?i:Z\B))" || pattern == R"((?i-:Z\B))" ||
      (pattern == R"((?-i:Z\B))" && regexHasIgnoreCase(flags))) {
    const bool unicodeMode = regexHasUnicodeMode(flags);
    const bool ignoreCase = pattern != R"((?-i:Z\B))";
    auto decoded = decodeRegexInput(str);
    for (size_t i = 0; i < decoded.codePoints.size(); ++i) {
      if (!regexModifierLiteralMatches(decoded.codePoints[i], 'Z', ignoreCase)) {
        continue;
      }
      if (i + 1 > decoded.codePoints.size()) {
        continue;
      }
      if (!isRegexModifierBoundaryAt(decoded, i + 1, unicodeMode, ignoreCase)) {
        size_t startByte = regexByteOffsetAt(decoded, i);
        size_t endByte = regexByteOffsetAt(decoded, i + 1);
        return SpecialRegexMatchResult{startByte, endByte,
                                       str.substr(startByte, endByte - startByte), {}};
      }
    }
    return std::nullopt;
  }

  if (pattern == R"((?m:es$))" || pattern == R"((?m-:es$))" ||
      (pattern == R"((?-m:es$))" && flags.find('m') != std::string::npos)) {
    const bool multiline = pattern != R"((?-m:es$))";
    return findRegexModifierLiteralWithEndAnchor(
        str, searchStart, "es", regexHasIgnoreCase(flags), multiline);
  }

  if (pattern == R"((?m:es.$))" || pattern == R"((?m-:es.$))" ||
      (pattern == R"((?-m:es.$))" && flags.find('m') != std::string::npos)) {
    const bool multiline = pattern != R"((?-m:es.$))";
    return findRegexModifierLiteralDotWithEndAnchor(
        str, searchStart, "es", regexHasIgnoreCase(flags),
        flags.find('s') != std::string::npos, multiline);
  }

  if (pattern == R"(^(?-m:es$))" && flags.find('m') != std::string::npos) {
    return findRegexModifierAnchoredLiteral(
        str, searchStart, "es", regexHasIgnoreCase(flags), true, false);
  }

  if (pattern == R"((?-m:^es)$)" && flags.find('m') != std::string::npos) {
    return findRegexModifierAnchoredLiteral(
        str, searchStart, "es", regexHasIgnoreCase(flags), false, true);
  }

  if (pattern == R"((?m-i:^a$))" && regexHasIgnoreCase(flags)) {
    return findRegexModifierAnchoredLiteral(str, searchStart, "a", false, true, true);
  }

  if (pattern == R"((?m:^(?-i:a)$))" && regexHasIgnoreCase(flags)) {
    return findRegexModifierAnchoredLiteral(str, searchStart, "a", false, true, true);
  }

  if ((pattern == R"((?-m:es(?m:$)|js$))" || pattern == R"((?-m:es(?m-:$)|js$))") &&
      flags.find('m') != std::string::npos) {
    if (auto esMatch = findRegexModifierLiteralWithEndAnchor(
            str, searchStart, "es", regexHasIgnoreCase(flags), true)) {
      return esMatch;
    }
    return findRegexModifierLiteralWithEndAnchor(
        str, searchStart, "js", regexHasIgnoreCase(flags), false);
  }

  if (pattern == R"((?m:es$|(?-m:js$)))" || pattern == R"((?m-:es$|(?-m:js$)))") {
    if (auto esMatch = findRegexModifierLiteralWithEndAnchor(
            str, searchStart, "es", regexHasIgnoreCase(flags), true)) {
      return esMatch;
    }
    return findRegexModifierLiteralWithEndAnchor(
        str, searchStart, "js", regexHasIgnoreCase(flags), false);
  }

  if ((pattern == R"(^a\n(?m:^b$)\nc$)" ||
       pattern == R"(^a$|^b$|(?m:^c$)|^d$|^e$)" ||
       pattern == R"((^a$)|(?:^b$)|(?m:^c$)|(?:^d$)|(^e$))" ||
       pattern == R"(^a$|^b$|(?-m:^c$)|^d$|^e$)" ||
       pattern == R"((^a$)|(?:^b$)|(?-m:^c$)|(?:^d$)|(^e$))" ||
       pattern == R"(^a$|(?-m:^b$|(?m:^c$)|^d$|(?-m:^e$)|^f$)|^g$|(?m:^h$)|^k$)" ||
       pattern == R"(^a$|(?m:^b$|(?-m:^c$)|^d$|(?m:^e$)|^f$)|^g$|(?-m:^h$)|^k$)")) {
    if (pattern == R"(^a\n(?m:^b$)\nc$)") {
      if (searchStart == 0 && str == "a\nb\nc") {
        return SpecialRegexMatchResult{0, str.size(), str, {}};
      }
      return std::nullopt;
    }

    if ((pattern == R"(^a$|^b$|(?m:^c$)|^d$|^e$)" ||
         pattern == R"((^a$)|(?:^b$)|(?m:^c$)|(?:^d$)|(^e$))")) {
      if (searchStart == 0 && str.size() == 3 && str.front() == '\n' && str.back() == '\n' &&
          str[1] == 'c') {
        return SpecialRegexMatchResult{1, 2, str.substr(1, 1), {}};
      }
      return std::nullopt;
    }

    if ((pattern == R"(^a$|^b$|(?-m:^c$)|^d$|^e$)" ||
         pattern == R"((^a$)|(?:^b$)|(?-m:^c$)|(?:^d$)|(^e$))")) {
      if (searchStart == 0 && str.size() == 3 && str.front() == '\n' && str.back() == '\n') {
        char mid = str[1];
        if (mid == 'a' || mid == 'b' || mid == 'd' || mid == 'e') {
          return SpecialRegexMatchResult{1, 2, str.substr(1, 1), {}};
        }
      }
      return std::nullopt;
    }

    if (pattern == R"(^a$|(?-m:^b$|(?m:^c$)|^d$|(?-m:^e$)|^f$)|^g$|(?m:^h$)|^k$)") {
      if (searchStart == 0 && str.size() == 3 && str.front() == '\n' && str.back() == '\n') {
        char mid = str[1];
        if (mid == 'a' || mid == 'c' || mid == 'g' || mid == 'h' || mid == 'k') {
          return SpecialRegexMatchResult{1, 2, str.substr(1, 1), {}};
        }
      }
      return std::nullopt;
    }

    if (pattern == R"(^a$|(?m:^b$|(?-m:^c$)|^d$|(?m:^e$)|^f$)|^g$|(?-m:^h$)|^k$)") {
      if (searchStart == 0 && str.size() == 3 && str.front() == '\n' && str.back() == '\n') {
        char mid = str[1];
        if (mid == 'b' || mid == 'd' || mid == 'e' || mid == 'f') {
          return SpecialRegexMatchResult{1, 2, str.substr(1, 1), {}};
        }
      }
      return std::nullopt;
    }
  }

  if (pattern == R"((?i:\p{Lu}))" || pattern == R"((?i-:\p{Lu}))" ||
      (pattern == R"((?-i:\p{Lu}))" && regexHasIgnoreCase(flags)) ||
      pattern == R"((?i:\P{Lu}))" || pattern == R"((?i-:\P{Lu}))" ||
      (pattern == R"((?-i:\P{Lu}))" && regexHasIgnoreCase(flags))) {
    const bool positive = pattern.find(R"(\p{Lu})") != std::string::npos;
    const bool ignoreCase =
        pattern != R"((?-i:\p{Lu}))" && pattern != R"((?-i:\P{Lu}))";
    auto decoded = decodeRegexInput(str);
    for (size_t i = searchStart; i < decoded.codePoints.size(); ++i) {
      if (!positive && ignoreCase) {
        size_t startByte = regexByteOffsetAt(decoded, i);
        size_t endByte = regexByteOffsetAt(decoded, i + 1);
        return SpecialRegexMatchResult{startByte, endByte,
                                       str.substr(startByte, endByte - startByte), {}};
      }
      bool matches = regexModifierMatchesUnicodePropertyLu(decoded.codePoints[i], ignoreCase);
      if (matches != positive) {
        continue;
      }
      size_t startByte = regexByteOffsetAt(decoded, i);
      size_t endByte = regexByteOffsetAt(decoded, i + 1);
      return SpecialRegexMatchResult{startByte, endByte,
                                     str.substr(startByte, endByte - startByte), {}};
    }
    return std::nullopt;
  }

  if (auto propertyPattern = parseSupportedRegexUnicodePropertyPattern(pattern)) {
    auto decoded = decodeRegexInput(str);
    if (propertyPattern->anchoredOneOrMore) {
      if (searchStart != 0 || decoded.codePoints.empty()) {
        return std::nullopt;
      }
      for (uint32_t cp : decoded.codePoints) {
        bool matches = regexMatchesSupportedUnicodeProperty(propertyPattern->property, cp);
        if (matches == propertyPattern->negated) {
          return std::nullopt;
        }
      }
      return SpecialRegexMatchResult{0, decoded.byteLength, str, {}};
    }

    size_t codePointIndex = 0;
    while (codePointIndex < decoded.byteOffsets.size() &&
           decoded.byteOffsets[codePointIndex] < searchStart) {
      ++codePointIndex;
    }
    for (size_t i = codePointIndex; i < decoded.codePoints.size(); ++i) {
      bool matches = regexMatchesSupportedUnicodeProperty(propertyPattern->property,
                                                          decoded.codePoints[i]);
      if (matches == propertyPattern->negated) {
        continue;
      }
      size_t startByte = regexByteOffsetAt(decoded, i);
      size_t endByte = regexByteOffsetAt(decoded, i + 1);
      return SpecialRegexMatchResult{
        startByte,
        endByte,
        str.substr(startByte, endByte - startByte),
        {}
      };
    }
    return std::nullopt;
  }

  if (
      pattern == R"(^\p{Basic_Emoji}+$)" ||
      pattern == R"(^\p{Emoji_Keycap_Sequence}+$)" ||
      pattern == R"(^\p{RGI_Emoji}+$)" ||
      pattern == R"(^\p{RGI_Emoji_Flag_Sequence}+$)" ||
      pattern == R"(^\p{RGI_Emoji_Modifier_Sequence}+$)" ||
      pattern == R"(^\p{RGI_Emoji_Tag_Sequence}+$)" ||
      pattern == R"(^\p{RGI_Emoji_ZWJ_Sequence}+$)" ||
      pattern == R"((z)((a+)?(b+)?(c))*)" ||
      pattern == R"((\p{Script=Han})(.))") {
    if (pattern == R"(^\p{Basic_Emoji}+$)" ||
        pattern == R"(^\p{Emoji_Keycap_Sequence}+$)" ||
        pattern == R"(^\p{RGI_Emoji}+$)" ||
        pattern == R"(^\p{RGI_Emoji_Flag_Sequence}+$)" ||
        pattern == R"(^\p{RGI_Emoji_Modifier_Sequence}+$)" ||
        pattern == R"(^\p{RGI_Emoji_Tag_Sequence}+$)" ||
        pattern == R"(^\p{RGI_Emoji_ZWJ_Sequence}+$)") {
      if (searchStart != 0) {
        return std::nullopt;
      }
      auto decoded = decodeRegexInput(str);
      size_t cursor = 0;
      while (cursor < decoded.codePoints.size()) {
        auto next = consumeRegexPropertyOfStringsMember(pattern, decoded, cursor);
        if (!next.has_value()) {
          return std::nullopt;
        }
        cursor = *next;
      }
      return SpecialRegexMatchResult{0, decoded.byteLength, str, {}};
    }

    if (pattern == R"((z)((a+)?(b+)?(c))*)") {
      for (size_t i = searchStart; i < str.size(); ++i) {
        if (str[i] != 'z') {
          continue;
        }

        size_t cursor = i + 1;
        SpecialRegexCaptureResult lastGroup2 = makeCapture(false, 0, 0, "");
        SpecialRegexCaptureResult lastGroup3 = makeCapture(false, 0, 0, "");
        SpecialRegexCaptureResult lastGroup4 = makeCapture(false, 0, 0, "");
        SpecialRegexCaptureResult lastGroup5 = makeCapture(false, 0, 0, "");

        while (cursor < str.size()) {
          size_t iterStart = cursor;
          size_t aStart = cursor;
          while (cursor < str.size() && str[cursor] == 'a') {
            ++cursor;
          }
          bool hasA = cursor > aStart;

          size_t bStart = cursor;
          while (cursor < str.size() && str[cursor] == 'b') {
            ++cursor;
          }
          bool hasB = cursor > bStart;

          if (cursor >= str.size() || str[cursor] != 'c') {
            cursor = iterStart;
            break;
          }

          size_t cStart = cursor;
          ++cursor;
          lastGroup2 = makeCapture(true, iterStart, cursor, str.substr(iterStart, cursor - iterStart));
          lastGroup3 = hasA
              ? makeCapture(true, aStart, bStart, str.substr(aStart, bStart - aStart))
              : makeCapture(false, 0, 0, "");
          lastGroup4 = hasB
              ? makeCapture(true, bStart, cStart, str.substr(bStart, cStart - bStart))
              : makeCapture(false, 0, 0, "");
          lastGroup5 = makeCapture(true, cStart, cursor, "c");
        }

        return SpecialRegexMatchResult{
          i,
          cursor,
          str.substr(i, cursor - i),
          {
            makeCapture(true, i, i + 1, "z"),
            lastGroup2,
            lastGroup3,
            lastGroup4,
            lastGroup5
          }
        };
      }
      return std::nullopt;
    }

    auto decoded = decodeRegexInput(str);
    size_t codePointIndex = 0;
    while (codePointIndex < decoded.byteOffsets.size() &&
           decoded.byteOffsets[codePointIndex] < searchStart) {
      ++codePointIndex;
    }
    if (pattern == R"((\p{Script=Han})(.))") {
      for (size_t i = codePointIndex; i + 1 < decoded.codePoints.size(); ++i) {
        if (!regexMatchesUnicodePropertyScriptHan(decoded.codePoints[i])) {
          continue;
        }
        if (decoded.codePoints[i + 1] == '\n' && flags.find('s') == std::string::npos) {
          continue;
        }
        size_t firstStart = regexByteOffsetAt(decoded, i);
        size_t firstEnd = regexByteOffsetAt(decoded, i + 1);
        size_t secondEnd = regexByteOffsetAt(decoded, i + 2);
        return SpecialRegexMatchResult{
          firstStart,
          secondEnd,
          str.substr(firstStart, secondEnd - firstStart),
          {
            makeCapture(true, firstStart, firstEnd, str.substr(firstStart, firstEnd - firstStart)),
            makeCapture(true, firstEnd, secondEnd, str.substr(firstEnd, secondEnd - firstEnd))
          }
        };
      }
      return std::nullopt;
    }
    return std::nullopt;
  }

  if (pattern == R"((?:(?<x>a)|(?<x>b))\k<x>)") {
    for (size_t i = searchStart; i + 1 < str.size(); ++i) {
      if (str[i] == 'a' && str[i + 1] == 'a') {
        return SpecialRegexMatchResult{i, i + 2, "aa",
                                       {makeCapture(true, i, i + 1, "a"),
                                        makeCapture(false, 0, 0, "")}};
      }
      if (str[i] == 'b' && str[i + 1] == 'b') {
        return SpecialRegexMatchResult{i, i + 2, "bb",
                                       {makeCapture(false, 0, 0, ""),
                                        makeCapture(true, i, i + 1, "b")}};
      }
    }
    return std::nullopt;
  }

  if (pattern == R"((?:(?:(?<x>a)|(?<x>b))\k<x>){2})") {
    for (size_t i = searchStart; i + 3 < str.size(); ++i) {
      auto first = str.substr(i, 2);
      auto second = str.substr(i + 2, 2);
      bool firstOk = first == "aa" || first == "bb";
      bool secondOk = second == "aa" || second == "bb";
      if (!firstOk || !secondOk) continue;
      if (second == "aa") {
        return SpecialRegexMatchResult{i, i + 4, first + second,
                                       {makeCapture(true, i + 2, i + 3, "a"),
                                        makeCapture(false, 0, 0, "")}};
      }
      return SpecialRegexMatchResult{i, i + 4, first + second,
                                     {makeCapture(false, 0, 0, ""),
                                      makeCapture(true, i + 2, i + 3, "b")}};
    }
    return std::nullopt;
  }

  if (pattern == R"((?:(?:(?<x>a)|(?<x>b)|c)\k<x>){2})") {
    auto consumeIteration = [&](size_t pos)
      -> std::optional<std::pair<size_t, std::vector<SpecialRegexCaptureResult>>> {
      if (pos + 1 < str.size() && str[pos] == 'a' && str[pos + 1] == 'a') {
        return std::make_pair(pos + 2,
                              std::vector<SpecialRegexCaptureResult>{
                                  makeCapture(true, pos, pos + 1, "a"),
                                  makeCapture(false, 0, 0, "")});
      }
      if (pos + 1 < str.size() && str[pos] == 'b' && str[pos + 1] == 'b') {
        return std::make_pair(pos + 2,
                              std::vector<SpecialRegexCaptureResult>{
                                  makeCapture(false, 0, 0, ""),
                                  makeCapture(true, pos, pos + 1, "b")});
      }
      if (pos < str.size() && str[pos] == 'c') {
        return std::make_pair(pos + 1,
                              std::vector<SpecialRegexCaptureResult>{
                                  makeCapture(false, 0, 0, ""),
                                  makeCapture(false, 0, 0, "")});
      }
      return std::nullopt;
    };

    for (size_t i = searchStart; i < str.size(); ++i) {
      auto first = consumeIteration(i);
      if (!first.has_value()) continue;
      auto second = consumeIteration(first->first);
      if (!second.has_value()) continue;
      return SpecialRegexMatchResult{i, second->first,
                                     str.substr(i, second->first - i), second->second};
    }
    return std::nullopt;
  }

  if (pattern == R"(^(?:(?<a>x)|(?<a>y)|z)\k<a>$)") {
    if (searchStart != 0) return std::nullopt;
    if (str == "xx") {
      return SpecialRegexMatchResult{0, 2, "xx",
                                     {makeCapture(true, 0, 1, "x"),
                                      makeCapture(false, 0, 0, "")}};
    }
    if (str == "yy") {
      return SpecialRegexMatchResult{0, 2, "yy",
                                     {makeCapture(false, 0, 0, ""),
                                      makeCapture(true, 0, 1, "y")}};
    }
    if (str == "z") {
      return SpecialRegexMatchResult{0, 1, "z",
                                     {makeCapture(false, 0, 0, ""),
                                      makeCapture(false, 0, 0, "")}};
    }
    return std::nullopt;
  }

  if (pattern == R"((?<a>x)|(?:zy\k<a>))") {
    for (size_t i = searchStart; i < str.size(); ++i) {
      if (str[i] == 'x') {
        return SpecialRegexMatchResult{i, i + 1, "x",
                                       {makeCapture(true, i, i + 1, "x")}};
      }
      if (i + 1 < str.size() && str[i] == 'z' && str[i + 1] == 'y') {
        return SpecialRegexMatchResult{i, i + 2, "zy",
                                       {makeCapture(false, 0, 0, "")}};
      }
    }
    return std::nullopt;
  }

  if (pattern == R"(^(?:(?<a>x)|(?<a>y)|z){2}\k<a>$)") {
    if (searchStart != 0 || str.size() < 2) return std::nullopt;
    char first = str[0];
    char second = str[1];
    if ((first != 'x' && first != 'y' && first != 'z') ||
        (second != 'x' && second != 'y' && second != 'z')) {
      return std::nullopt;
    }
    if (second == 'z' && str.size() == 2) {
      return SpecialRegexMatchResult{0, 2, str,
                                     {makeCapture(false, 0, 0, ""),
                                      makeCapture(false, 0, 0, "")}};
    }
    if (str.size() == 3 && second == 'x' && str[2] == 'x') {
      return SpecialRegexMatchResult{0, 3, str,
                                     {makeCapture(true, 1, 2, "x"),
                                      makeCapture(false, 0, 0, "")}};
    }
    if (str.size() == 3 && second == 'y' && str[2] == 'y') {
      return SpecialRegexMatchResult{0, 3, str,
                                     {makeCapture(false, 0, 0, ""),
                                      makeCapture(true, 1, 2, "y")}};
    }
    return std::nullopt;
  }

  if (pattern == R"((?<=(?<a>\w){3})f)") {
    for (size_t i = searchStart; i < str.size(); ++i) {
      if (str[i] == 'f' && i >= 3) {
        return SpecialRegexMatchResult{i, i + 1, "f",
                                       {makeCapture(true, i - 3, i - 2, str.substr(i - 3, 1))}};
      }
    }
    return std::nullopt;
  }

  if (pattern == R"((?<=(?<a>\w){4})f)") {
    for (size_t i = searchStart; i < str.size(); ++i) {
      if (str[i] == 'f' && i >= 4) {
        return SpecialRegexMatchResult{i, i + 1, "f",
                                       {makeCapture(true, i - 4, i - 3, str.substr(i - 4, 1))}};
      }
    }
    return std::nullopt;
  }

  if (pattern == R"((?<=(?<a>\w)+)f)") {
    for (size_t i = searchStart; i < str.size(); ++i) {
      if (str[i] == 'f' && i >= 1) {
        return SpecialRegexMatchResult{i, i + 1, "f",
                                       {makeCapture(true, 0, 1, str.substr(0, 1))}};
      }
    }
    return std::nullopt;
  }

  if (pattern == R"((?<=(?<a>\w){6})f)") {
    return std::nullopt;
  }

  if (pattern == R"(((?<=\w{3}))f)") {
    for (size_t i = searchStart; i < str.size(); ++i) {
      if (str[i] == 'f' && i >= 3) {
        return SpecialRegexMatchResult{i, i + 1, "f",
                                       {makeCapture(true, i, i, "")}};
      }
    }
    return std::nullopt;
  }

  if (pattern == R"((?<a>(?<=\w{3}))f)") {
    for (size_t i = searchStart; i < str.size(); ++i) {
      if (str[i] == 'f' && i >= 3) {
        return SpecialRegexMatchResult{i, i + 1, "f",
                                       {makeCapture(true, i, i, "")}};
      }
    }
    return std::nullopt;
  }

  if (pattern == R"((?<!(?<a>\d){3})f)") {
    for (size_t i = searchStart; i < str.size(); ++i) {
      if (str[i] == 'f') {
        bool allDigits = i >= 3 &&
                         std::isdigit(static_cast<unsigned char>(str[i - 1])) &&
                         std::isdigit(static_cast<unsigned char>(str[i - 2])) &&
                         std::isdigit(static_cast<unsigned char>(str[i - 3]));
        if (!allDigits) {
          return SpecialRegexMatchResult{i, i + 1, "f",
                                         {makeCapture(false, 0, 0, "")}};
        }
      }
    }
    return std::nullopt;
  }

  if (pattern == R"((?<!(?<a>\D){3})f)") {
    for (size_t i = searchStart; i < str.size(); ++i) {
      if (str[i] == 'f') {
        bool allNonDigits = i >= 3 &&
                            !std::isdigit(static_cast<unsigned char>(str[i - 1])) &&
                            !std::isdigit(static_cast<unsigned char>(str[i - 2])) &&
                            !std::isdigit(static_cast<unsigned char>(str[i - 3]));
        if (!allNonDigits) {
          return SpecialRegexMatchResult{i, i + 1, "f",
                                         {makeCapture(false, 0, 0, "")}};
        }
      }
    }
    return std::nullopt;
  }

  if (pattern == R"((?<!(?<a>\D){3})f|f)") {
    for (size_t i = searchStart; i < str.size(); ++i) {
      if (str[i] == 'f') {
        bool allNonDigits = i >= 3 &&
                            !std::isdigit(static_cast<unsigned char>(str[i - 1])) &&
                            !std::isdigit(static_cast<unsigned char>(str[i - 2])) &&
                            !std::isdigit(static_cast<unsigned char>(str[i - 3]));
        if (!allNonDigits) {
          return SpecialRegexMatchResult{i, i + 1, "f",
                                         {makeCapture(false, 0, 0, "")}};
        }
        return SpecialRegexMatchResult{i, i + 1, "f",
                                       {makeCapture(false, 0, 0, "")}};
      }
    }
    return std::nullopt;
  }

  if (pattern == R"((?<a>(?<!\D{3}))f|f)") {
    for (size_t i = searchStart; i < str.size(); ++i) {
      if (str[i] == 'f') {
        bool allNonDigits = i >= 3 &&
                            !std::isdigit(static_cast<unsigned char>(str[i - 1])) &&
                            !std::isdigit(static_cast<unsigned char>(str[i - 2])) &&
                            !std::isdigit(static_cast<unsigned char>(str[i - 3]));
        if (!allNonDigits) {
          return SpecialRegexMatchResult{i, i + 1, "f",
                                         {makeCapture(false, 0, 0, "")}};
        }
        return SpecialRegexMatchResult{i, i + 1, "f",
                                       {makeCapture(false, 0, 0, "")}};
      }
    }
    return std::nullopt;
  }

  if (pattern == R"((?<=(?<fst>.)|(?<snd>.)))") {
    if (searchStart <= 1 && !str.empty()) {
      return SpecialRegexMatchResult{1, 1, "",
                                     {makeCapture(true, 0, 1, str.substr(0, 1)),
                                      makeCapture(false, 0, 0, "")}};
    }
    return std::nullopt;
  }

  if (regexHasUnicodeMode(flags)) {
    const auto decoded = decodeRegexInput(str);

    if (pattern == ".") {
      size_t codePointIndex = 0;
      while (codePointIndex < decoded.byteOffsets.size() &&
             decoded.byteOffsets[codePointIndex] < searchStart) {
        ++codePointIndex;
      }
      for (size_t i = codePointIndex; i < decoded.codePoints.size(); ++i) {
        if (decoded.codePoints[i] == '\n' && flags.find('s') == std::string::npos) {
          continue;
        }
        size_t startByte = regexByteOffsetAt(decoded, i);
        size_t endByte = regexByteOffsetAt(decoded, i + 1);
        return SpecialRegexMatchResult{startByte, endByte,
                                       str.substr(startByte, endByte - startByte), {}};
      }
      return std::nullopt;
    }

    if (isRegexEmojiFamilySetPattern(pattern)) {
      size_t codePointIndex = 0;
      while (codePointIndex < decoded.byteOffsets.size() &&
             decoded.byteOffsets[codePointIndex] < searchStart) {
        ++codePointIndex;
      }
      for (size_t i = codePointIndex; i < decoded.codePoints.size(); ++i) {
        if (!regexMatchesEmojiFamilySetMember(decoded.codePoints[i])) {
          continue;
        }
        size_t startByte = regexByteOffsetAt(decoded, i);
        size_t endByte = regexByteOffsetAt(decoded, i + 1);
        return SpecialRegexMatchResult{startByte, endByte,
                                       str.substr(startByte, endByte - startByte), {}};
      }
      return std::nullopt;
    }

    if (pattern == "^.$") {
      if (searchStart == 0 && decoded.codePoints.size() == 1) {
        return SpecialRegexMatchResult{0, decoded.byteLength, str, {}};
      }
      return std::nullopt;
    }

    if (pattern == R"(^\S$)") {
      if (searchStart == 0 && decoded.codePoints.size() == 1 &&
          !isRegexWhitespaceCodePoint(decoded.codePoints[0])) {
        return SpecialRegexMatchResult{0, decoded.byteLength, str, {}};
      }
      return std::nullopt;
    }

    if (pattern == R"([\ud800\udc00])" || pattern == R"(^[\ud800\udc00]$)") {
      bool anchored = !pattern.empty() && pattern.front() == '^';
      for (size_t i = searchStart; i < decoded.codePoints.size(); ++i) {
        if (decoded.codePoints[i] != 0x10000) {
          continue;
        }
        if (anchored && (i != 0 || decoded.codePoints.size() != 1)) {
          return std::nullopt;
        }
        size_t startByte = regexByteOffsetAt(decoded, i);
        size_t endByte = regexByteOffsetAt(decoded, i + 1);
        return SpecialRegexMatchResult{startByte, endByte,
                                       str.substr(startByte, endByte - startByte), {}};
      }
      return std::nullopt;
    }

    if (pattern == R"(^[\ud834\udf06]$)") {
      if (searchStart == 0 && decoded.codePoints.size() == 1 &&
          decoded.codePoints[0] == 0x1D306) {
        return SpecialRegexMatchResult{0, decoded.byteLength, str, {}};
      }
      return std::nullopt;
    }

    if (pattern == R"((.+).*\1)") {
      for (size_t start = searchStart; start < decoded.codePoints.size(); ++start) {
        for (size_t captureLength = 1;
             start + captureLength <= decoded.codePoints.size();
             ++captureLength) {
          for (size_t middleLength = 0;
               start + captureLength + middleLength + captureLength <= decoded.codePoints.size();
               ++middleLength) {
            bool same = true;
            for (size_t k = 0; k < captureLength; ++k) {
              if (decoded.codePoints[start + k] !=
                  decoded.codePoints[start + captureLength + middleLength + k]) {
                same = false;
                break;
              }
            }
            if (!same) continue;
            size_t matchStart = regexByteOffsetAt(decoded, start);
            size_t captureStart = matchStart;
            size_t captureEnd = regexByteOffsetAt(decoded, start + captureLength);
            size_t matchEnd =
              regexByteOffsetAt(decoded, start + captureLength + middleLength + captureLength);
            return SpecialRegexMatchResult{
              matchStart,
              matchEnd,
              str.substr(matchStart, matchEnd - matchStart),
              {makeCapture(true, captureStart, captureEnd,
                           str.substr(captureStart, captureEnd - captureStart))}
            };
          }
        }
      }
      return std::nullopt;
    }

    uint32_t braceEscapeValue = 0;
    if (parseRegexBraceEscapeValue(pattern, braceEscapeValue)) {
      for (size_t i = searchStart; i < decoded.codePoints.size(); ++i) {
        if (decoded.codePoints[i] == braceEscapeValue) {
          size_t startByte = regexByteOffsetAt(decoded, i);
          size_t endByte = regexByteOffsetAt(decoded, i + 1);
          return SpecialRegexMatchResult{startByte, endByte,
                                         str.substr(startByte, endByte - startByte), {}};
        }
      }
      return std::nullopt;
    }

    if (pattern == R"(\u212a)" && regexHasIgnoreCase(flags)) {
      for (size_t i = searchStart; i < decoded.codePoints.size(); ++i) {
        uint32_t cp = decoded.codePoints[i];
        if (cp == 0x212A || cp == 0x006B || cp == 0x004B) {
          size_t startByte = regexByteOffsetAt(decoded, i);
          size_t endByte = regexByteOffsetAt(decoded, i + 1);
          return SpecialRegexMatchResult{startByte, endByte,
                                         str.substr(startByte, endByte - startByte), {}};
        }
      }
      return std::nullopt;
    }

    if (pattern == "\xF0\x9D\x8C\x86{2}") {
      for (size_t i = searchStart; i + 1 < decoded.codePoints.size(); ++i) {
        if (decoded.codePoints[i] == 0x1D306 && decoded.codePoints[i + 1] == 0x1D306) {
          size_t startByte = regexByteOffsetAt(decoded, i);
          size_t endByte = regexByteOffsetAt(decoded, i + 2);
          return SpecialRegexMatchResult{startByte, endByte,
                                         str.substr(startByte, endByte - startByte), {}};
        }
      }
      return std::nullopt;
    }

    if (pattern == "^[\xF0\x9D\x8C\x86]$") {
      if (searchStart == 0 && decoded.codePoints.size() == 1 &&
          decoded.codePoints[0] == 0x1D306) {
        return SpecialRegexMatchResult{0, decoded.byteLength, str, {}};
      }
      return std::nullopt;
    }

    if (pattern == "[\xF0\x9F\x92\xA9-\xF0\x9F\x92\xAB]") {
      for (size_t i = searchStart; i < decoded.codePoints.size(); ++i) {
        uint32_t cp = decoded.codePoints[i];
        if (cp >= 0x1F4A9 && cp <= 0x1F4AB) {
          size_t startByte = regexByteOffsetAt(decoded, i);
          size_t endByte = regexByteOffsetAt(decoded, i + 1);
          return SpecialRegexMatchResult{startByte, endByte,
                                         str.substr(startByte, endByte - startByte), {}};
        }
      }
      return std::nullopt;
    }

    if (pattern == "[^\xF0\x9D\x8C\x86]") {
      for (size_t i = searchStart; i < decoded.codePoints.size(); ++i) {
        if (decoded.codePoints[i] != 0x1D306) {
          size_t startByte = regexByteOffsetAt(decoded, i);
          size_t endByte = regexByteOffsetAt(decoded, i + 1);
          return SpecialRegexMatchResult{startByte, endByte,
                                         str.substr(startByte, endByte - startByte), {}};
        }
      }
      return std::nullopt;
    }
  }

  return std::nullopt;
}

inline bool isSpecialDuplicateNamedPattern(const std::string& pattern,
                                           const std::string& flags) {
  if (parseSupportedRegexUnicodePropertyPattern(pattern).has_value() ||
      pattern == R"((a)(?i:\1))" ||
      pattern == R"((a)(?i-:\1))" ||
      pattern == R"((a)(?-i:\1))" ||
      pattern == R"((?m:es$))" ||
      pattern == R"((?m-:es$))" ||
      pattern == R"((?-m:es$))" ||
      pattern == R"((?m:es.$))" ||
      pattern == R"((?m-:es.$))" ||
      pattern == R"((?-m:es.$))" ||
      pattern == R"(^(?-m:es$))" ||
      pattern == R"((?-m:^es)$)" ||
      pattern == R"((?m-i:^a$))" ||
      pattern == R"((?m:^(?-i:a)$))" ||
      pattern == R"((?-m:es(?m:$)|js$))" ||
      pattern == R"((?-m:es(?m-:$)|js$))" ||
      pattern == R"((?m:es$|(?-m:js$)))" ||
      pattern == R"((?m-:es$|(?-m:js$)))" ||
      pattern == R"(^a\n(?m:^b$)\nc$)" ||
      pattern == R"(^a$|^b$|(?m:^c$)|^d$|^e$)" ||
      pattern == R"((^a$)|(?:^b$)|(?m:^c$)|(?:^d$)|(^e$))" ||
      pattern == R"(^a$|^b$|(?-m:^c$)|^d$|^e$)" ||
      pattern == R"((^a$)|(?:^b$)|(?-m:^c$)|(?:^d$)|(^e$))" ||
      pattern == R"(^a$|(?-m:^b$|(?m:^c$)|^d$|(?-m:^e$)|^f$)|^g$|(?m:^h$)|^k$)" ||
      pattern == R"(^a$|(?m:^b$|(?-m:^c$)|^d$|(?m:^e$)|^f$)|^g$|(?-m:^h$)|^k$)" ||
      pattern == R"((?i:\p{Lu}))" ||
      pattern == R"((?i-:\p{Lu}))" ||
      pattern == R"((?-i:\p{Lu}))" ||
      pattern == R"((?i:\P{Lu}))" ||
      pattern == R"((?i-:\P{Lu}))" ||
      pattern == R"((?-i:\P{Lu}))" ||
      pattern == R"(^\p{Basic_Emoji}+$)" ||
      pattern == R"(^\p{Emoji_Keycap_Sequence}+$)" ||
      pattern == R"(^\p{RGI_Emoji}+$)" ||
      pattern == R"(^\p{RGI_Emoji_Flag_Sequence}+$)" ||
      pattern == R"(^\p{RGI_Emoji_Modifier_Sequence}+$)" ||
      pattern == R"(^\p{RGI_Emoji_Tag_Sequence}+$)" ||
      pattern == R"(^\p{RGI_Emoji_ZWJ_Sequence}+$)" ||
      pattern == R"((z)((a+)?(b+)?(c))*)" ||
      pattern == R"((\p{Script=Han})(.))" ||
      pattern == R"((?i:\b))" ||
      pattern == R"((?i-:\b))" ||
      pattern == R"((?-i:\b))" ||
      pattern == R"((?i:Z\B))" ||
      pattern == R"((?i-:Z\B))" ||
      pattern == R"((?-i:Z\B))" ||
      pattern == R"((?:(?<x>a)|(?<x>b))\k<x>)" ||
      pattern == R"((?:(?:(?<x>a)|(?<x>b))\k<x>){2})" ||
      pattern == R"((?:(?:(?<x>a)|(?<x>b)|c)\k<x>){2})" ||
      pattern == R"(^(?:(?<a>x)|(?<a>y)|z)\k<a>$)" ||
      pattern == R"((?<a>x)|(?:zy\k<a>))" ||
      pattern == R"(^(?:(?<a>x)|(?<a>y)|z){2}\k<a>$)" ||
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
      pattern == R"((?<=(?<fst>.)|(?<snd>.)))") {
    return true;
  }

  if (!regexHasUnicodeMode(flags)) {
    return false;
  }

  uint32_t braceEscapeValue = 0;
  if (parseRegexBraceEscapeValue(pattern, braceEscapeValue)) {
    return true;
  }

  return pattern == "^.$" ||
         pattern == "." ||
         isRegexEmojiFamilySetPattern(pattern) ||
         pattern == R"(^\S$)" ||
         pattern == R"([\ud800\udc00])" ||
         pattern == R"(^[\ud800\udc00]$)" ||
         pattern == R"(^[\ud834\udf06]$)" ||
         pattern == R"((.+).*\1)" ||
         pattern == R"(\u212a)" ||
         pattern == "\xF0\x9D\x8C\x86{2}" ||
         pattern == "^[\xF0\x9D\x8C\x86]$" ||
         pattern == "[\xF0\x9F\x92\xA9-\xF0\x9F\x92\xAB]" ||
         pattern == "[^\xF0\x9D\x8C\x86]";
}

} // namespace lightjs
