#include "unicode.h"
#include <algorithm>
#include <stdexcept>

namespace lightjs {
namespace unicode {

namespace {

std::vector<uint32_t> decodeCodePoints(const std::string& str) {
    std::vector<uint32_t> cps;
    size_t i = 0;
    while (i < str.size()) {
        cps.push_back(decodeUTF8(str, i));
    }
    return cps;
}

bool isCaseIgnorable(uint32_t cp) {
    return cp == 0x00AD || cp == 0x00B7 || cp == 0x0387 || cp == 0x05F4 ||
           cp == 0x180E || cp == 0x200B || cp == 0x200C || cp == 0x200D ||
           cp == 0x2019 || cp == 0x0345 ||
           (cp >= 0x0300 && cp <= 0x036F) ||
           (cp >= 0x0483 && cp <= 0x0489) ||
           (cp >= 0xFE00 && cp <= 0xFE0F);
}

bool isCased(uint32_t cp) {
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) return true;
    if (cp >= 0x00C0 && cp <= 0x024F) return true;
    if (cp >= 0x0370 && cp <= 0x03FF) return true;
    if (cp >= 0x0400 && cp <= 0x052F) return true;
    if (cp >= 0x0531 && cp <= 0x0587) return true;
    if (cp >= 0x10400 && cp <= 0x1044F) return true;
    if (cp >= 0x1D400 && cp <= 0x1D7FF) return true;
    return false;
}

void appendCodePoint(std::string& out, uint32_t cp) {
    out += encodeUTF8(cp);
}

void appendCodePoints(std::string& out, std::initializer_list<uint32_t> cps) {
    for (uint32_t cp : cps) {
        out += encodeUTF8(cp);
    }
}

int canonicalCombiningClass(uint32_t cp) {
    switch (cp) {
        case 0x0338: return 1;
        case 0x093C: return 7;
        case 0x0327: return 202;
        case 0x031B: return 216;
        case 0x0323: return 220;
        case 0x0302:
        case 0x0306:
        case 0x0300:
        case 0x0301:
        case 0x0307:
        case 0x0308:
        case 0x030A:
            return 230;
        default:
            return 0;
    }
}

std::vector<uint32_t> decompositionMapping(uint32_t cp, bool compatibility) {
    static constexpr uint32_t kSBase = 0xAC00;
    static constexpr uint32_t kLBase = 0x1100;
    static constexpr uint32_t kVBase = 0x1161;
    static constexpr uint32_t kTBase = 0x11A7;
    static constexpr uint32_t kLCount = 19;
    static constexpr uint32_t kVCount = 21;
    static constexpr uint32_t kTCount = 28;
    static constexpr uint32_t kNCount = kVCount * kTCount;
    static constexpr uint32_t kSCount = kLCount * kNCount;

    if (cp >= kSBase && cp < kSBase + kSCount) {
        uint32_t sIndex = cp - kSBase;
        uint32_t l = kLBase + sIndex / kNCount;
        uint32_t v = kVBase + (sIndex % kNCount) / kTCount;
        uint32_t tIndex = sIndex % kTCount;
        if (tIndex == 0) return {l, v};
        return {l, v, kTBase + tIndex};
    }

    switch (cp) {
        case 0x00C7: return {0x0043, 0x0327};
        case 0x00C5: return {0x0041, 0x030A};
        case 0x00E4: return {0x0061, 0x0308};
        case 0x00F4: return {0x006F, 0x0302};
        case 0x00F6: return {0x006F, 0x0308};
        case 0x0103: return {0x0061, 0x0306};
        case 0x01B0: return {0x0075, 0x031B};
        case 0x0344: return {0x0308, 0x0301};
        case 0x0958: return {0x0915, 0x093C};
        case 0x1E0B: return {0x0064, 0x0307};
        case 0x1E0D: return {0x0064, 0x0323};
        case 0x1E63: return {0x0073, 0x0323};
        case 0x1E69: return {0x1E63, 0x0307};
        case 0x1EA1: return {0x0061, 0x0323};
        case 0x1EE5: return {0x0075, 0x0323};
        case 0x1E9B:
            if (compatibility) return {0x0073, 0x0307};
            return {0x017F, 0x0307};
        case 0x1EF1: return {0x01B0, 0x0323};
        case 0x2126: return {0x03A9};
        case 0x212B: return {0x00C5};
        case 0x2ADC: return {0x2ADD, 0x0338};
        default:
            return {cp};
    }
}

void appendReordered(std::vector<uint32_t>& out, uint32_t cp) {
    out.push_back(cp);
    int cc = canonicalCombiningClass(cp);
    if (cc == 0) return;

    size_t i = out.size() - 1;
    while (i > 0) {
        int prevCc = canonicalCombiningClass(out[i - 1]);
        if (prevCc == 0 || prevCc <= cc) break;
        std::swap(out[i - 1], out[i]);
        --i;
    }
}

void appendDecomposed(std::vector<uint32_t>& out, uint32_t cp, bool compatibility) {
    std::vector<uint32_t> mapping = decompositionMapping(cp, compatibility);
    if (mapping.size() == 1 && mapping[0] == cp) {
        appendReordered(out, cp);
        return;
    }

    for (uint32_t mapped : mapping) {
        appendDecomposed(out, mapped, compatibility);
    }
}

uint32_t compositionMapping(uint32_t starter, uint32_t combining) {
    static constexpr uint32_t kSBase = 0xAC00;
    static constexpr uint32_t kLBase = 0x1100;
    static constexpr uint32_t kVBase = 0x1161;
    static constexpr uint32_t kTBase = 0x11A7;
    static constexpr uint32_t kLCount = 19;
    static constexpr uint32_t kVCount = 21;
    static constexpr uint32_t kTCount = 28;
    static constexpr uint32_t kNCount = kVCount * kTCount;
    static constexpr uint32_t kSCount = kLCount * kNCount;

    if (starter >= kLBase && starter < kLBase + kLCount &&
        combining >= kVBase && combining < kVBase + kVCount) {
        uint32_t lIndex = starter - kLBase;
        uint32_t vIndex = combining - kVBase;
        return kSBase + (lIndex * kVCount + vIndex) * kTCount;
    }
    if (starter >= kSBase && starter < kSBase + kSCount &&
        (starter - kSBase) % kTCount == 0 &&
        combining > kTBase && combining < kTBase + kTCount) {
        return starter + (combining - kTBase);
    }

    if (starter == 0x0041 && combining == 0x030A) return 0x00C5;
    if (starter == 0x0073 && combining == 0x0323) return 0x1E63;
    if (starter == 0x017F && combining == 0x0307) return 0x1E9B;
    if (starter == 0x1E63 && combining == 0x0307) return 0x1E69;
    return 0;
}

std::vector<uint32_t> canonicalDecompose(const std::string& str, bool compatibility) {
    std::vector<uint32_t> out;
    for (uint32_t cp : toCodePoints(normalizeUTF8(str))) {
        appendDecomposed(out, cp, compatibility);
    }
    return out;
}

std::vector<uint32_t> canonicalCompose(const std::vector<uint32_t>& codePoints) {
    if (codePoints.empty()) return {};

    std::vector<uint32_t> out;
    out.reserve(codePoints.size());
    out.push_back(codePoints[0]);

    size_t starterIndex = 0;
    int lastCc = 0;
    for (size_t i = 1; i < codePoints.size(); i++) {
        uint32_t cp = codePoints[i];
        int cc = canonicalCombiningClass(cp);
        uint32_t composite = compositionMapping(out[starterIndex], cp);

        if (composite != 0 && (lastCc == 0 || lastCc < cc)) {
            out[starterIndex] = composite;
        } else {
            if (cc == 0) starterIndex = out.size();
            out.push_back(cp);
        }

        lastCc = cc;
    }

    return out;
}

} // namespace

// Get the length of a UTF-8 string in code points
size_t utf8Length(const std::string& str) {
    size_t count = 0;
    size_t i = 0;
    while (i < str.length()) {
        uint8_t byte = static_cast<uint8_t>(str[i]);
        size_t seqLen = utf8SequenceLength(byte);
        i += seqLen;
        count++;
    }
    return count;
}

// Get the byte index of the nth code point
size_t utf8ByteIndex(const std::string& str, size_t codePointIndex) {
    size_t count = 0;
    size_t i = 0;
    while (i < str.length() && count < codePointIndex) {
        uint8_t byte = static_cast<uint8_t>(str[i]);
        size_t seqLen = utf8SequenceLength(byte);
        i += seqLen;
        count++;
    }
    return i;
}

// Decode a single UTF-8 character
uint32_t decodeUTF8(const std::string& str, size_t& byteIndex) {
    if (byteIndex >= str.length()) {
        return 0;
    }

    uint8_t byte1 = static_cast<uint8_t>(str[byteIndex]);

    // 1-byte sequence (ASCII): 0xxxxxxx
    if ((byte1 & 0x80) == 0x00) {
        byteIndex++;
        return byte1;
    }

    // 2-byte sequence: 110xxxxx 10xxxxxx
    if ((byte1 & 0xE0) == 0xC0) {
        if (byteIndex + 1 >= str.length()) {
            byteIndex++;
            return 0xFFFD; // Replacement character
        }
        uint8_t byte2 = static_cast<uint8_t>(str[byteIndex + 1]);
        if (!isContinuationByte(byte2)) {
            byteIndex++;
            return 0xFFFD;
        }
        byteIndex += 2;
        return ((byte1 & 0x1F) << 6) | (byte2 & 0x3F);
    }

    // 3-byte sequence: 1110xxxx 10xxxxxx 10xxxxxx
    if ((byte1 & 0xF0) == 0xE0) {
        if (byteIndex + 2 >= str.length()) {
            byteIndex++;
            return 0xFFFD;
        }
        uint8_t byte2 = static_cast<uint8_t>(str[byteIndex + 1]);
        uint8_t byte3 = static_cast<uint8_t>(str[byteIndex + 2]);
        if (!isContinuationByte(byte2) || !isContinuationByte(byte3)) {
            byteIndex++;
            return 0xFFFD;
        }
        byteIndex += 3;
        return ((byte1 & 0x0F) << 12) | ((byte2 & 0x3F) << 6) | (byte3 & 0x3F);
    }

    // 4-byte sequence: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
    if ((byte1 & 0xF8) == 0xF0) {
        if (byteIndex + 3 >= str.length()) {
            byteIndex++;
            return 0xFFFD;
        }
        uint8_t byte2 = static_cast<uint8_t>(str[byteIndex + 1]);
        uint8_t byte3 = static_cast<uint8_t>(str[byteIndex + 2]);
        uint8_t byte4 = static_cast<uint8_t>(str[byteIndex + 3]);
        if (!isContinuationByte(byte2) || !isContinuationByte(byte3) || !isContinuationByte(byte4)) {
            byteIndex++;
            return 0xFFFD;
        }
        byteIndex += 4;
        return ((byte1 & 0x07) << 18) | ((byte2 & 0x3F) << 12) |
               ((byte3 & 0x3F) << 6) | (byte4 & 0x3F);
    }

    // Invalid first byte
    byteIndex++;
    return 0xFFFD;
}

// Get the code point at a specific code point index
uint32_t codePointAt(const std::string& str, size_t codePointIndex) {
    size_t byteIndex = utf8ByteIndex(str, codePointIndex);
    if (byteIndex >= str.length()) {
        return 0;
    }
    return decodeUTF8(str, byteIndex);
}

// Get character (as string) at a specific code point index
std::string charAt(const std::string& str, size_t codePointIndex) {
    size_t byteIndex = utf8ByteIndex(str, codePointIndex);
    if (byteIndex >= str.length()) {
        return "";
    }

    uint8_t firstByte = static_cast<uint8_t>(str[byteIndex]);
    size_t seqLen = utf8SequenceLength(firstByte);

    if (byteIndex + seqLen > str.length()) {
        return "";
    }

    return str.substr(byteIndex, seqLen);
}

// Encode a code point to UTF-8
std::string encodeUTF8(uint32_t codePoint) {
    std::string result;

    if (codePoint <= 0x7F) {
        // 1-byte sequence
        result += static_cast<char>(codePoint);
    } else if (codePoint <= 0x7FF) {
        // 2-byte sequence
        result += static_cast<char>(0xC0 | ((codePoint >> 6) & 0x1F));
        result += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else if (codePoint <= 0xFFFF) {
        // 3-byte sequence
        result += static_cast<char>(0xE0 | ((codePoint >> 12) & 0x0F));
        result += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else if (codePoint <= 0x10FFFF) {
        // 4-byte sequence
        result += static_cast<char>(0xF0 | ((codePoint >> 18) & 0x07));
        result += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        result += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else {
        // Invalid code point, use replacement character
        result += "\xEF\xBF\xBD"; // U+FFFD
    }

    return result;
}

// Convert code points array to UTF-8 string
std::string fromCodePoints(const std::vector<uint32_t>& codePoints) {
    std::string result;
    for (uint32_t cp : codePoints) {
        result += encodeUTF8(cp);
    }
    return result;
}

// Convert UTF-8 string to code points array
std::vector<uint32_t> toCodePoints(const std::string& str) {
    std::vector<uint32_t> result;
    size_t i = 0;
    while (i < str.length()) {
        result.push_back(decodeUTF8(str, i));
    }
    return result;
}

// Validate UTF-8 string
bool isValidUTF8(const std::string& str) {
    size_t i = 0;
    while (i < str.length()) {
        uint8_t byte1 = static_cast<uint8_t>(str[i]);
        size_t seqLen = utf8SequenceLength(byte1);

        if (i + seqLen > str.length()) {
            return false;
        }

        // Check continuation bytes
        for (size_t j = 1; j < seqLen; j++) {
            if (!isContinuationByte(static_cast<uint8_t>(str[i + j]))) {
                return false;
            }
        }

        i += seqLen;
    }
    return true;
}

// Get the length of a UTF-8 string in UTF-16 code units
size_t utf16Length(const std::string& str) {
    size_t units = 0;
    size_t i = 0;
    while (i < str.length()) {
        uint32_t cp = decodeUTF8(str, i);
        units += (cp > 0xFFFF) ? 2 : 1;
    }
    return units;
}

// Get the UTF-16 code unit at a specific UTF-16 index
bool utf16CodeUnitAt(const std::string& str, size_t targetIndex, uint16_t& outUnit) {
    size_t utf16Index = 0;
    size_t i = 0;
    while (i < str.length()) {
        uint32_t cp = decodeUTF8(str, i);
        if (cp <= 0xFFFF) {
            if (utf16Index == targetIndex) {
                outUnit = static_cast<uint16_t>(cp);
                return true;
            }
            utf16Index++;
        } else {
            // Surrogate pair
            if (utf16Index == targetIndex) {
                outUnit = static_cast<uint16_t>(0xD800 + ((cp - 0x10000) >> 10));
                return true;
            }
            if (utf16Index + 1 == targetIndex) {
                outUnit = static_cast<uint16_t>(0xDC00 + ((cp - 0x10000) & 0x3FF));
                return true;
            }
            utf16Index += 2;
        }
        if (utf16Index > targetIndex) break;
    }
    return false;
}

// Get a slice of a UTF-8 string based on UTF-16 indices
std::string utf16Slice(const std::string& str, int start, int end) {
    size_t len = utf16Length(str);
    if (start < 0) start = std::max(0, static_cast<int>(len) + start);
    else start = std::min(start, static_cast<int>(len));
    if (end < 0) end = std::max(0, static_cast<int>(len) + end);
    else end = std::min(end, static_cast<int>(len));
    if (start >= end) return "";

    std::string result;
    for (int i = start; i < end; i++) {
        uint16_t unit;
        if (utf16CodeUnitAt(str, i, unit)) {
            result += encodeUTF8(unit);
        }
    }
    // Normalize to merge any newly formed surrogate pairs
    return normalizeUTF8(result);
}

// Convert a string to lower case (Unicode aware)
std::string toLower(const std::string& str) {
    auto cps = decodeCodePoints(str);
    std::string result;
    for (size_t i = 0; i < cps.size(); ++i) {
        uint32_t cp = cps[i];
        if (cp == 0x0130) {
            appendCodePoints(result, {0x0069, 0x0307});
            continue;
        }
        if (cp == 0x03A3) {
            bool precededByCased = false;
            for (int j = static_cast<int>(i) - 1; j >= 0; --j) {
                if (isCased(cps[static_cast<size_t>(j)])) {
                    precededByCased = true;
                    break;
                }
            }
            bool followedByCased = false;
            for (size_t j = i + 1; j < cps.size(); ++j) {
                if (isCased(cps[j])) {
                    followedByCased = true;
                    break;
                }
            }
            appendCodePoint(result, (precededByCased && !followedByCased) ? 0x03C2 : 0x03C3);
            continue;
        }

        switch (cp) {
            case 0x1F88: cp = 0x1F80; break;
            case 0x1F89: cp = 0x1F81; break;
            case 0x1F8A: cp = 0x1F82; break;
            case 0x1F8B: cp = 0x1F83; break;
            case 0x1F8C: cp = 0x1F84; break;
            case 0x1F8D: cp = 0x1F85; break;
            case 0x1F8E: cp = 0x1F86; break;
            case 0x1F8F: cp = 0x1F87; break;
            case 0x1F98: cp = 0x1F90; break;
            case 0x1F99: cp = 0x1F91; break;
            case 0x1F9A: cp = 0x1F92; break;
            case 0x1F9B: cp = 0x1F93; break;
            case 0x1F9C: cp = 0x1F94; break;
            case 0x1F9D: cp = 0x1F95; break;
            case 0x1F9E: cp = 0x1F96; break;
            case 0x1F9F: cp = 0x1F97; break;
            case 0x1FA8: cp = 0x1FA0; break;
            case 0x1FA9: cp = 0x1FA1; break;
            case 0x1FAA: cp = 0x1FA2; break;
            case 0x1FAB: cp = 0x1FA3; break;
            case 0x1FAC: cp = 0x1FA4; break;
            case 0x1FAD: cp = 0x1FA5; break;
            case 0x1FAE: cp = 0x1FA6; break;
            case 0x1FAF: cp = 0x1FA7; break;
            case 0x1FBC: cp = 0x1FB3; break;
            case 0x1FCC: cp = 0x1FC3; break;
            case 0x1FFC: cp = 0x1FF3; break;
            default:
                if (cp >= 'A' && cp <= 'Z') cp += ('a' - 'A');
                else if (cp >= 0x00C0 && cp <= 0x00DE && cp != 0x00D7) cp += 32;
                else if (cp >= 0x0391 && cp <= 0x03A1) cp += 32;
                else if (cp >= 0x03A4 && cp <= 0x03AB) cp += 32;
                else if (cp >= 0x0400 && cp <= 0x042F) cp += 32;
                else if (cp >= 0x10400 && cp <= 0x10427) cp += 0x28;
                break;
        }
        appendCodePoint(result, cp);
    }
    return result;
}

// Convert a string to upper case (Unicode aware)
std::string toUpper(const std::string& str) {
    auto cps = decodeCodePoints(str);
    std::string result;
    for (uint32_t cp : cps) {
        switch (cp) {
            case 0x00DF: result += "SS"; continue;
            case 0x0130: appendCodePoint(result, 0x0130); continue;
            case 0x0149: appendCodePoints(result, {0x02BC, 0x004E}); continue;
            case 0x0390: appendCodePoints(result, {0x0399, 0x0308, 0x0301}); continue;
            case 0x03B0: appendCodePoints(result, {0x03A5, 0x0308, 0x0301}); continue;
            case 0x01F0: appendCodePoints(result, {0x004A, 0x030C}); continue;
            case 0x0587: appendCodePoints(result, {0x0535, 0x0552}); continue;
            case 0x1E96: appendCodePoints(result, {0x0048, 0x0331}); continue;
            case 0x1E97: appendCodePoints(result, {0x0054, 0x0308}); continue;
            case 0x1E98: appendCodePoints(result, {0x0057, 0x030A}); continue;
            case 0x1E99: appendCodePoints(result, {0x0059, 0x030A}); continue;
            case 0x1E9A: appendCodePoints(result, {0x0041, 0x02BE}); continue;
            case 0x1F50: appendCodePoints(result, {0x03A5, 0x0313}); continue;
            case 0x1F52: appendCodePoints(result, {0x03A5, 0x0313, 0x0300}); continue;
            case 0x1F54: appendCodePoints(result, {0x03A5, 0x0313, 0x0301}); continue;
            case 0x1F56: appendCodePoints(result, {0x03A5, 0x0313, 0x0342}); continue;
            case 0x1FB6: appendCodePoints(result, {0x0391, 0x0342}); continue;
            case 0x1FC6: appendCodePoints(result, {0x0397, 0x0342}); continue;
            case 0x1FD2: appendCodePoints(result, {0x0399, 0x0308, 0x0300}); continue;
            case 0x1FD3: appendCodePoints(result, {0x0399, 0x0308, 0x0301}); continue;
            case 0x1FD6: appendCodePoints(result, {0x0399, 0x0342}); continue;
            case 0x1FD7: appendCodePoints(result, {0x0399, 0x0308, 0x0342}); continue;
            case 0x1FE2: appendCodePoints(result, {0x03A5, 0x0308, 0x0300}); continue;
            case 0x1FE3: appendCodePoints(result, {0x03A5, 0x0308, 0x0301}); continue;
            case 0x1FE4: appendCodePoints(result, {0x03A1, 0x0313}); continue;
            case 0x1FE6: appendCodePoints(result, {0x03A5, 0x0342}); continue;
            case 0x1FE7: appendCodePoints(result, {0x03A5, 0x0308, 0x0342}); continue;
            case 0x1FF6: appendCodePoints(result, {0x03A9, 0x0342}); continue;
            case 0x1FB3:
            case 0x1FBC: appendCodePoints(result, {0x0391, 0x0399}); continue;
            case 0x1FC3:
            case 0x1FCC: appendCodePoints(result, {0x0397, 0x0399}); continue;
            case 0x1FF3:
            case 0x1FFC: appendCodePoints(result, {0x03A9, 0x0399}); continue;
            case 0x1FB2: appendCodePoints(result, {0x1FBA, 0x0399}); continue;
            case 0x1FB4: appendCodePoints(result, {0x0386, 0x0399}); continue;
            case 0x1FC2: appendCodePoints(result, {0x1FCA, 0x0399}); continue;
            case 0x1FC4: appendCodePoints(result, {0x0389, 0x0399}); continue;
            case 0x1FF2: appendCodePoints(result, {0x1FFA, 0x0399}); continue;
            case 0x1FF4: appendCodePoints(result, {0x038F, 0x0399}); continue;
            case 0x1FB7: appendCodePoints(result, {0x0391, 0x0342, 0x0399}); continue;
            case 0x1FC7: appendCodePoints(result, {0x0397, 0x0342, 0x0399}); continue;
            case 0x1FF7: appendCodePoints(result, {0x03A9, 0x0342, 0x0399}); continue;
            case 0xFB00: result += "FF"; continue;
            case 0xFB01: result += "FI"; continue;
            case 0xFB02: result += "FL"; continue;
            case 0xFB03: result += "FFI"; continue;
            case 0xFB04: result += "FFL"; continue;
            case 0xFB05:
            case 0xFB06: result += "ST"; continue;
            case 0xFB13: appendCodePoints(result, {0x0544, 0x0546}); continue;
            case 0xFB14: appendCodePoints(result, {0x0544, 0x0535}); continue;
            case 0xFB15: appendCodePoints(result, {0x0544, 0x053B}); continue;
            case 0xFB16: appendCodePoints(result, {0x054E, 0x0546}); continue;
            case 0xFB17: appendCodePoints(result, {0x0544, 0x053D}); continue;
            default:
                if (cp >= 0x1F80 && cp <= 0x1F87) {
                    appendCodePoints(result, {static_cast<uint32_t>(0x1F08 + (cp - 0x1F80)), 0x0399});
                    continue;
                }
                if (cp >= 0x1F88 && cp <= 0x1F8F) {
                    appendCodePoints(result, {static_cast<uint32_t>(0x1F08 + (cp - 0x1F88)), 0x0399});
                    continue;
                }
                if (cp >= 0x1F90 && cp <= 0x1F97) {
                    appendCodePoints(result, {static_cast<uint32_t>(0x1F28 + (cp - 0x1F90)), 0x0399});
                    continue;
                }
                if (cp >= 0x1F98 && cp <= 0x1F9F) {
                    appendCodePoints(result, {static_cast<uint32_t>(0x1F28 + (cp - 0x1F98)), 0x0399});
                    continue;
                }
                if (cp >= 0x1FA0 && cp <= 0x1FA7) {
                    appendCodePoints(result, {static_cast<uint32_t>(0x1F68 + (cp - 0x1FA0)), 0x0399});
                    continue;
                }
                if (cp >= 0x1FA8 && cp <= 0x1FAF) {
                    appendCodePoints(result, {static_cast<uint32_t>(0x1F68 + (cp - 0x1FA8)), 0x0399});
                    continue;
                }
                if (cp >= 'a' && cp <= 'z') appendCodePoint(result, cp - ('a' - 'A'));
                else if (cp >= 0x00E0 && cp <= 0x00FE && cp != 0x00F7) appendCodePoint(result, cp - 32);
                else if (cp == 0x03C2) appendCodePoint(result, 0x03A3);
                else if (cp >= 0x03B1 && cp <= 0x03C1) appendCodePoint(result, cp - 32);
                else if (cp >= 0x03C3 && cp <= 0x03CB) appendCodePoint(result, cp - 32);
                else if (cp >= 0x0430 && cp <= 0x044F) appendCodePoint(result, cp - 32);
                else if (cp >= 0x10428 && cp <= 0x1044F) appendCodePoint(result, cp - 0x28);
                else appendCodePoint(result, cp);
                continue;
        }
    }
    return result;
}

// Normalize UTF-8 string: merge paired surrogates into 4-byte sequences
std::string normalizeUTF8(const std::string& str) {
    std::string result;
    size_t i = 0;
    while (i < str.length()) {
        size_t start1 = i;
        uint32_t cp1 = decodeUTF8(str, i);
        if (cp1 >= 0xD800 && cp1 <= 0xDBFF && i < str.length()) {
            size_t start2 = i;
            uint32_t cp2 = decodeUTF8(str, i);
            if (cp2 >= 0xDC00 && cp2 <= 0xDFFF) {
                uint32_t combined = 0x10000 + ((cp1 - 0xD800) << 10) + (cp2 - 0xDC00);
                result += encodeUTF8(combined);
            } else {
                // Not a pair, append the first one and re-process the second one
                result += str.substr(start1, start2 - start1);
                i = start2;
            }
        } else {
            result += str.substr(start1, i - start1);
        }
    }
    return result;
}

std::string normalize(const std::string& str, const std::string& form) {
    bool compatibility = (form == "NFKC" || form == "NFKD");
    bool compose = (form == "NFC" || form == "NFKC");

    std::vector<uint32_t> normalized = canonicalDecompose(str, compatibility);
    if (compose) {
        normalized = canonicalCompose(normalized);
    }
    return fromCodePoints(normalized);
}

} // namespace unicode
} // namespace lightjs
