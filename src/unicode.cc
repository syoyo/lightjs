#include "unicode.h"
#include <stdexcept>

namespace lightjs {
namespace unicode {

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

} // namespace unicode
} // namespace lightjs
