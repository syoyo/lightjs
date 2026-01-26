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

    // Section decoding for Element and DataCount
    bool decodeElementSection(WasmModule& module);
    bool decodeDataCountSection(WasmModule& module);

    // Error handling with context
    const char* sectionName(uint8_t sectionId) const {
        switch (sectionId) {
            case 0: return "Custom";
            case 1: return "Type";
            case 2: return "Import";
            case 3: return "Function";
            case 4: return "Table";
            case 5: return "Memory";
            case 6: return "Global";
            case 7: return "Export";
            case 8: return "Start";
            case 9: return "Element";
            case 10: return "Code";
            case 11: return "Data";
            case 12: return "DataCount";
            default: return "Unknown";
        }
    }

    void setError(const std::string& msg) {
        if (error_.empty()) {
            std::string context = msg + " at offset " + std::to_string(pos_);
            if (currentSection_ != 255) {
                context += " in " + std::string(sectionName(currentSection_)) + " section";
            }
            error_ = context;
        }
    }

    const std::vector<uint8_t>& data_;
    size_t pos_;
    std::string error_;
    uint8_t currentSection_ = 255;  // Track current section for better error messages
};

} // namespace wasm
} // namespace lightjs
