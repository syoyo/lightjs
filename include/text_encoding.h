#pragma once

#include "value.h"
#include <string>
#include <vector>
#include <cstdint>

namespace lightjs {

/**
 * TextEncoder - Encodes strings to UTF-8 bytes
 *
 * Web API compatible implementation
 * https://developer.mozilla.org/en-US/docs/Web/API/TextEncoder
 */
struct TextEncoder : public GCObject {
  // Encoding is always UTF-8
  std::string encoding = "utf-8";

  /**
   * Encode a string to UTF-8 bytes
   * Returns a Uint8Array
   */
  Value encode(const std::string& input);

  /**
   * Encode into an existing Uint8Array
   * Returns object with 'read' and 'written' properties
   */
  Value encodeInto(const std::string& source, std::shared_ptr<TypedArray> dest);

  // GCObject interface
  const char* typeName() const override { return "TextEncoder"; }
  void getReferences(std::vector<GCObject*>& refs) const override {}
};

/**
 * TextDecoder - Decodes UTF-8 bytes to strings
 *
 * Web API compatible implementation
 * https://developer.mozilla.org/en-US/docs/Web/API/TextDecoder
 */
struct TextDecoder : public GCObject {
  std::string encoding = "utf-8";
  bool fatal = false;           // Throw on invalid sequences
  bool ignoreBOM = false;       // Ignore byte order mark

  TextDecoder() = default;
  TextDecoder(const std::string& enc, bool fatal_ = false, bool ignoreBOM_ = false)
    : encoding(enc), fatal(fatal_), ignoreBOM(ignoreBOM_) {}

  /**
   * Decode bytes to string
   * Input can be ArrayBuffer, TypedArray, or DataView
   */
  Value decode(const Value& input);

  /**
   * Decode from byte array
   */
  std::string decodeBytes(const uint8_t* bytes, size_t length);

  // GCObject interface
  const char* typeName() const override { return "TextDecoder"; }
  void getReferences(std::vector<GCObject*>& refs) const override {}
};

/**
 * Create TextEncoder constructor for JavaScript
 */
std::shared_ptr<Function> createTextEncoderConstructor();

/**
 * Create TextDecoder constructor for JavaScript
 */
std::shared_ptr<Function> createTextDecoderConstructor();

} // namespace lightjs
