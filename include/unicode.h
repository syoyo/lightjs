#pragma once

#include <string>
#include <cstdint>
#include <vector>

namespace lightjs {
namespace unicode {

// UTF-8 utilities

// Get the length of a UTF-8 string in code points (not bytes)
size_t utf8Length(const std::string& str);

// Get the byte index of the nth code point
size_t utf8ByteIndex(const std::string& str, size_t codePointIndex);

// Get the code point at a specific code point index
uint32_t codePointAt(const std::string& str, size_t codePointIndex);

// Get character (as string) at a specific code point index
std::string charAt(const std::string& str, size_t codePointIndex);

// Decode a single UTF-8 character starting at byteIndex
// Returns the code point and advances byteIndex
uint32_t decodeUTF8(const std::string& str, size_t& byteIndex);

// Encode a code point to UTF-8
std::string encodeUTF8(uint32_t codePoint);

// Check if a byte is a UTF-8 continuation byte
inline bool isContinuationByte(uint8_t byte) {
    return (byte & 0xC0) == 0x80;
}

// Get the number of bytes in a UTF-8 sequence from the first byte
inline size_t utf8SequenceLength(uint8_t firstByte) {
    if ((firstByte & 0x80) == 0x00) return 1;  // 0xxxxxxx
    if ((firstByte & 0xE0) == 0xC0) return 2;  // 110xxxxx
    if ((firstByte & 0xF0) == 0xE0) return 3;  // 1110xxxx
    if ((firstByte & 0xF8) == 0xF0) return 4;  // 11110xxx
    return 1; // Invalid, treat as single byte
}

// Convert code points array to UTF-8 string
std::string fromCodePoints(const std::vector<uint32_t>& codePoints);

// Convert UTF-8 string to code points array
std::vector<uint32_t> toCodePoints(const std::string& str);

// Validate UTF-8 string
bool isValidUTF8(const std::string& str);

} // namespace unicode
} // namespace lightjs
