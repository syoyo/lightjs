#pragma once

#include "wasm_types.h"
#include "wasm_runtime.h"
#include <optional>
#include <string>

namespace lightjs {
namespace wasm {

// WASM binary decoder
class WasmDecoder {
public:
    explicit WasmDecoder(const std::vector<uint8_t>& binary)
        : data_(binary), pos_(0) {}

    // Decode entire module
    std::optional<std::shared_ptr<WasmModule>> decode();

    // Get last error message
    const std::string& error() const { return error_; }

private:
    // Binary reading primitives
    bool hasMore() const { return pos_ < data_.size(); }
    uint8_t readByte();
    uint32_t readU32();
    uint64_t readU64();
    int32_t readI32();
    int64_t readI64();
    float readF32();
    double readF64();
    std::string readString();
    std::vector<uint8_t> readBytes(size_t count);

    // LEB128 decoding
    uint32_t readVarU32();
    uint64_t readVarU64();
    int32_t readVarI32();
    int64_t readVarI64();

    // Type decoding
    std::optional<ValueType> readValueType();
    std::optional<FuncType> readFuncType();
    std::optional<Limits> readLimits();

    // Section decoding
    bool readMagicAndVersion();
    bool decodeTypeSection(WasmModule& module);
    bool decodeImportSection(WasmModule& module);
    bool decodeFunctionSection(WasmModule& module);
    bool decodeTableSection(WasmModule& module);
    bool decodeMemorySection(WasmModule& module);
    bool decodeGlobalSection(WasmModule& module);
    bool decodeExportSection(WasmModule& module);
    bool decodeStartSection(WasmModule& module);
    bool decodeCodeSection(WasmModule& module);
    bool decodeDataSection(WasmModule& module);

    // Instruction decoding
    std::optional<Instruction> decodeInstruction();
    std::vector<Instruction> decodeExpression();

    // Error handling
    void setError(const std::string& msg) {
        if (error_.empty()) {
            error_ = msg + " at offset " + std::to_string(pos_);
        }
    }

    const std::vector<uint8_t>& data_;
    size_t pos_;
    std::string error_;
};

} // namespace wasm
} // namespace lightjs
