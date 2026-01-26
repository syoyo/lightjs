#include "text_encoding.h"
#include "unicode.h"
#include <algorithm>
#include <stdexcept>

namespace lightjs {

// TextEncoder implementation

Value TextEncoder::encode(const std::string& input) {
  // Convert string to UTF-8 bytes (already UTF-8, just copy bytes)
  auto array = std::make_shared<TypedArray>(TypedArrayType::Uint8Array, input.size());

  for (size_t i = 0; i < input.size(); i++) {
    array->setElement(i, static_cast<uint8_t>(input[i]));
  }

  return Value(array);
}

Value TextEncoder::encodeInto(const std::string& source, std::shared_ptr<TypedArray> dest) {
  if (!dest || dest->type != TypedArrayType::Uint8Array) {
    throw std::runtime_error("TextEncoder.encodeInto: destination must be Uint8Array");
  }

  size_t written = 0;
  size_t read = 0;

  // Copy bytes until source exhausted or dest full
  size_t toCopy = std::min(source.size(), dest->length);
  for (size_t i = 0; i < toCopy; i++) {
    dest->setElement(i, static_cast<uint8_t>(source[i]));
    written++;
  }

  // Count code points read
  size_t byteIndex = 0;
  while (byteIndex < toCopy) {
    unicode::decodeUTF8(source, byteIndex);
    read++;
  }

  // Return result object
  auto result = std::make_shared<Object>();
  result->properties["read"] = Value(static_cast<double>(read));
  result->properties["written"] = Value(static_cast<double>(written));

  return Value(result);
}

// TextDecoder implementation

std::string TextDecoder::decodeBytes(const uint8_t* bytes, size_t length) {
  if (encoding != "utf-8" && encoding != "utf8") {
    throw std::runtime_error("TextDecoder: only UTF-8 encoding is supported");
  }

  std::string result;
  result.reserve(length);

  // Copy bytes to string
  for (size_t i = 0; i < length; i++) {
    result += static_cast<char>(bytes[i]);
  }

  // Validate UTF-8 if fatal mode
  if (fatal && !unicode::isValidUTF8(result)) {
    throw std::runtime_error("TextDecoder: invalid UTF-8 sequence");
  }

  // Strip BOM if not ignored
  if (!ignoreBOM && result.size() >= 3) {
    if (static_cast<uint8_t>(result[0]) == 0xEF &&
        static_cast<uint8_t>(result[1]) == 0xBB &&
        static_cast<uint8_t>(result[2]) == 0xBF) {
      result = result.substr(3);
    }
  }

  return result;
}

Value TextDecoder::decode(const Value& input) {
  // Extract bytes from input
  const uint8_t* bytes = nullptr;
  size_t length = 0;

  if (auto* typedArray = std::get_if<std::shared_ptr<TypedArray>>(&input.value)) {
    bytes = (*typedArray)->data.data();
    length = (*typedArray)->length * (*typedArray)->elementSize();
  } else if (auto* arrayBuffer = std::get_if<std::shared_ptr<ArrayBuffer>>(&input.value)) {
    bytes = (*arrayBuffer)->data.data();
    length = (*arrayBuffer)->byteLength;
  } else if (auto* dataView = std::get_if<std::shared_ptr<DataView>>(&input.value)) {
    if ((*dataView)->buffer) {
      bytes = (*dataView)->buffer->data.data() + (*dataView)->byteOffset;
      length = (*dataView)->byteLength;
    }
  } else {
    throw std::runtime_error("TextDecoder.decode: input must be ArrayBuffer, TypedArray, or DataView");
  }

  std::string result = decodeBytes(bytes, length);
  return Value(result);
}

// Constructor functions

std::shared_ptr<Function> createTextEncoderConstructor() {
  auto constructor = std::make_shared<Function>();
  constructor->isNative = true;
  constructor->isConstructor = true;

  constructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto encoder = std::make_shared<TextEncoder>();

    // Add encode method
    auto encodeMethod = std::make_shared<Function>();
    encodeMethod->isNative = true;
    encodeMethod->nativeFunc = [encoder](const std::vector<Value>& args) -> Value {
      if (args.empty()) {
        return encoder->encode("");
      }
      return encoder->encode(args[0].toString());
    };

    auto encoderObj = std::make_shared<Object>();
    encoderObj->properties["encode"] = Value(encodeMethod);
    encoderObj->properties["encoding"] = Value(encoder->encoding);

    // Add encodeInto method
    auto encodeIntoMethod = std::make_shared<Function>();
    encodeIntoMethod->isNative = true;
    encodeIntoMethod->nativeFunc = [encoder](const std::vector<Value>& args) -> Value {
      if (args.size() < 2) {
        throw std::runtime_error("TextEncoder.encodeInto: requires 2 arguments");
      }

      std::string source = args[0].toString();

      auto* typedArray = std::get_if<std::shared_ptr<TypedArray>>(&args[1].value);
      if (!typedArray) {
        throw std::runtime_error("TextEncoder.encodeInto: second argument must be Uint8Array");
      }

      return encoder->encodeInto(source, *typedArray);
    };
    encoderObj->properties["encodeInto"] = Value(encodeIntoMethod);

    return Value(encoderObj);
  };

  return constructor;
}

std::shared_ptr<Function> createTextDecoderConstructor() {
  auto constructor = std::make_shared<Function>();
  constructor->isNative = true;
  constructor->isConstructor = true;

  constructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    std::string encoding = "utf-8";
    bool fatal = false;
    bool ignoreBOM = false;

    // Parse label argument
    if (!args.empty()) {
      encoding = args[0].toString();
      // Normalize encoding name
      std::transform(encoding.begin(), encoding.end(), encoding.begin(), ::tolower);
    }

    // Parse options argument
    if (args.size() > 1) {
      auto* optionsObj = std::get_if<std::shared_ptr<Object>>(&args[1].value);
      if (optionsObj) {
        auto fatalIt = (*optionsObj)->properties.find("fatal");
        if (fatalIt != (*optionsObj)->properties.end()) {
          fatal = fatalIt->second.isTruthy();
        }

        auto ignoreBOMIt = (*optionsObj)->properties.find("ignoreBOM");
        if (ignoreBOMIt != (*optionsObj)->properties.end()) {
          ignoreBOM = ignoreBOMIt->second.isTruthy();
        }
      }
    }

    auto decoder = std::make_shared<TextDecoder>(encoding, fatal, ignoreBOM);

    // Add decode method
    auto decodeMethod = std::make_shared<Function>();
    decodeMethod->isNative = true;
    decodeMethod->nativeFunc = [decoder](const std::vector<Value>& args) -> Value {
      if (args.empty()) {
        return Value("");
      }
      return decoder->decode(args[0]);
    };

    auto decoderObj = std::make_shared<Object>();
    decoderObj->properties["decode"] = Value(decodeMethod);
    decoderObj->properties["encoding"] = Value(decoder->encoding);
    decoderObj->properties["fatal"] = Value(decoder->fatal);
    decoderObj->properties["ignoreBOM"] = Value(decoder->ignoreBOM);

    return Value(decoderObj);
  };

  return constructor;
}

} // namespace lightjs
