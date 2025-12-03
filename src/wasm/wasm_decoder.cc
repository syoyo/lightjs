#include "wasm/wasm_decoder.h"
#include <cstring>

namespace lightjs {
namespace wasm {

// WASM magic number and version
constexpr uint32_t WASM_MAGIC = 0x6D736100;  // "\0asm"
constexpr uint32_t WASM_VERSION = 1;

// Section IDs
enum class SectionId : uint8_t {
    Custom = 0,
    Type = 1,
    Import = 2,
    Function = 3,
    Table = 4,
    Memory = 5,
    Global = 6,
    Export = 7,
    Start = 8,
    Element = 9,
    Code = 10,
    Data = 11,
    DataCount = 12
};

// Binary reading primitives
uint8_t WasmDecoder::readByte() {
    if (pos_ >= data_.size()) {
        setError("Unexpected end of input");
        return 0;
    }
    return data_[pos_++];
}

uint32_t WasmDecoder::readU32() {
    if (pos_ + 4 > data_.size()) {
        setError("Unexpected end of input reading u32");
        return 0;
    }
    uint32_t value;
    std::memcpy(&value, &data_[pos_], 4);
    pos_ += 4;
    return value;
}

uint64_t WasmDecoder::readU64() {
    if (pos_ + 8 > data_.size()) {
        setError("Unexpected end of input reading u64");
        return 0;
    }
    uint64_t value;
    std::memcpy(&value, &data_[pos_], 8);
    pos_ += 8;
    return value;
}

float WasmDecoder::readF32() {
    if (pos_ + 4 > data_.size()) {
        setError("Unexpected end of input reading f32");
        return 0.0f;
    }
    float value;
    std::memcpy(&value, &data_[pos_], 4);
    pos_ += 4;
    return value;
}

double WasmDecoder::readF64() {
    if (pos_ + 8 > data_.size()) {
        setError("Unexpected end of input reading f64");
        return 0.0;
    }
    double value;
    std::memcpy(&value, &data_[pos_], 8);
    pos_ += 8;
    return value;
}

// LEB128 decoding (unsigned)
uint32_t WasmDecoder::readVarU32() {
    uint32_t result = 0;
    uint32_t shift = 0;

    while (true) {
        uint8_t byte = readByte();
        result |= static_cast<uint32_t>(byte & 0x7F) << shift;

        if ((byte & 0x80) == 0) {
            break;
        }

        shift += 7;
        if (shift >= 35) {
            setError("VarU32 too long");
            return 0;
        }
    }

    return result;
}

uint64_t WasmDecoder::readVarU64() {
    uint64_t result = 0;
    uint32_t shift = 0;

    while (true) {
        uint8_t byte = readByte();
        result |= static_cast<uint64_t>(byte & 0x7F) << shift;

        if ((byte & 0x80) == 0) {
            break;
        }

        shift += 7;
        if (shift >= 70) {
            setError("VarU64 too long");
            return 0;
        }
    }

    return result;
}

// LEB128 decoding (signed)
int32_t WasmDecoder::readVarI32() {
    int32_t result = 0;
    uint32_t shift = 0;
    uint8_t byte;

    do {
        byte = readByte();
        result |= static_cast<int32_t>(byte & 0x7F) << shift;
        shift += 7;
    } while ((byte & 0x80) != 0);

    // Sign extend
    if ((shift < 32) && (byte & 0x40)) {
        result |= (~0 << shift);
    }

    return result;
}

int64_t WasmDecoder::readVarI64() {
    int64_t result = 0;
    uint32_t shift = 0;
    uint8_t byte;

    do {
        byte = readByte();
        result |= static_cast<int64_t>(byte & 0x7F) << shift;
        shift += 7;
    } while ((byte & 0x80) != 0);

    // Sign extend
    if ((shift < 64) && (byte & 0x40)) {
        result |= (~0LL << shift);
    }

    return result;
}

std::string WasmDecoder::readString() {
    uint32_t length = readVarU32();
    if (pos_ + length > data_.size()) {
        setError("Unexpected end of input reading string");
        return "";
    }

    std::string result(reinterpret_cast<const char*>(&data_[pos_]), length);
    pos_ += length;
    return result;
}

std::vector<uint8_t> WasmDecoder::readBytes(size_t count) {
    if (pos_ + count > data_.size()) {
        setError("Unexpected end of input reading bytes");
        return {};
    }

    std::vector<uint8_t> result(data_.begin() + pos_, data_.begin() + pos_ + count);
    pos_ += count;
    return result;
}

// Type decoding
std::optional<ValueType> WasmDecoder::readValueType() {
    uint8_t byte = readByte();
    switch (byte) {
        case 0x7F: return ValueType::I32;
        case 0x7E: return ValueType::I64;
        case 0x7D: return ValueType::F32;
        case 0x7C: return ValueType::F64;
        case 0x7B: return ValueType::V128;
        case 0x70: return ValueType::FuncRef;
        case 0x6F: return ValueType::ExternRef;
        default:
            setError("Invalid value type: " + std::to_string(byte));
            return std::nullopt;
    }
}

std::optional<FuncType> WasmDecoder::readFuncType() {
    uint8_t form = readByte();
    if (form != 0x60) {
        setError("Expected function type form 0x60");
        return std::nullopt;
    }

    FuncType funcType;

    // Parameters
    uint32_t paramCount = readVarU32();
    for (uint32_t i = 0; i < paramCount; ++i) {
        auto type = readValueType();
        if (!type) return std::nullopt;
        funcType.params.push_back(*type);
    }

    // Results
    uint32_t resultCount = readVarU32();
    for (uint32_t i = 0; i < resultCount; ++i) {
        auto type = readValueType();
        if (!type) return std::nullopt;
        funcType.results.push_back(*type);
    }

    return funcType;
}

std::optional<Limits> WasmDecoder::readLimits() {
    Limits limits;
    uint8_t flags = readByte();

    limits.hasMax = (flags & 0x01) != 0;
    limits.is64 = (flags & 0x04) != 0;  // memory64 extension

    if (limits.is64) {
        limits.min = readVarU64();
        if (limits.hasMax) {
            limits.max = readVarU64();
        }
    } else {
        limits.min = readVarU32();
        if (limits.hasMax) {
            limits.max = readVarU32();
        }
    }

    return limits;
}

// Magic and version
bool WasmDecoder::readMagicAndVersion() {
    if (readU32() != WASM_MAGIC) {
        setError("Invalid WASM magic number");
        return false;
    }

    if (readU32() != WASM_VERSION) {
        setError("Unsupported WASM version");
        return false;
    }

    return true;
}

// Type section
bool WasmDecoder::decodeTypeSection(WasmModule& module) {
    uint32_t count = readVarU32();

    for (uint32_t i = 0; i < count; ++i) {
        auto funcType = readFuncType();
        if (!funcType) return false;
        module.types.push_back(*funcType);
    }

    return true;
}

// Import section
bool WasmDecoder::decodeImportSection(WasmModule& module) {
    uint32_t count = readVarU32();

    for (uint32_t i = 0; i < count; ++i) {
        Import import;
        import.module = readString();
        import.name = readString();

        uint8_t kind = readByte();
        import.kind = static_cast<Import::Kind>(kind);

        switch (import.kind) {
            case Import::Kind::Function:
                import.typeIdx = readVarU32();
                break;
            case Import::Kind::Table:
                // Skip table import details for now
                readValueType();
                readLimits();
                break;
            case Import::Kind::Memory:
                readLimits();
                break;
            case Import::Kind::Global:
                readValueType();
                readByte();  // mutability
                break;
        }

        module.imports.push_back(import);
    }

    return true;
}

// Function section
bool WasmDecoder::decodeFunctionSection(WasmModule& module) {
    uint32_t count = readVarU32();

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t typeIdx = readVarU32();
        module.functionTypeIndices.push_back(typeIdx);
    }

    return true;
}

// Table section
bool WasmDecoder::decodeTableSection(WasmModule& module) {
    uint32_t count = readVarU32();

    for (uint32_t i = 0; i < count; ++i) {
        Table table;
        auto elemType = readValueType();
        if (!elemType) return false;
        table.elemType = *elemType;

        auto limits = readLimits();
        if (!limits) return false;
        table.limits = *limits;

        module.tables.push_back(table);
    }

    return true;
}

// Memory section
bool WasmDecoder::decodeMemorySection(WasmModule& module) {
    uint32_t count = readVarU32();

    for (uint32_t i = 0; i < count; ++i) {
        auto limits = readLimits();
        if (!limits) return false;
        module.memories.push_back(*limits);
    }

    return true;
}

// Global section
bool WasmDecoder::decodeGlobalSection(WasmModule& module) {
    uint32_t count = readVarU32();

    for (uint32_t i = 0; i < count; ++i) {
        Global global;
        auto type = readValueType();
        if (!type) return false;
        global.type = *type;

        global.mutable_ = readByte() != 0;

        // Read init expression
        auto initExpr = decodeExpression();
        if (!error_.empty()) return false;

        // For now, just store a default value
        global.value = WasmValue(int32_t(0));

        module.globals.push_back(global);
    }

    return true;
}

// Export section
bool WasmDecoder::decodeExportSection(WasmModule& module) {
    uint32_t count = readVarU32();

    for (uint32_t i = 0; i < count; ++i) {
        Export exp;
        exp.name = readString();

        uint8_t kind = readByte();
        exp.kind = static_cast<Export::Kind>(kind);
        exp.idx = readVarU32();

        module.exports.push_back(exp);
    }

    return true;
}

// Start section
bool WasmDecoder::decodeStartSection(WasmModule& module) {
    module.startFunction = readVarU32();
    return true;
}

// Code section (function bodies)
bool WasmDecoder::decodeCodeSection(WasmModule& module) {
    uint32_t count = readVarU32();

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t bodySize = readVarU32();
        size_t bodyStart = pos_;

        Function func;
        if (i < module.functionTypeIndices.size()) {
            func.typeIdx = module.functionTypeIndices[i];
        }

        // Read locals
        uint32_t localGroupCount = readVarU32();
        for (uint32_t j = 0; j < localGroupCount; ++j) {
            uint32_t count = readVarU32();
            auto type = readValueType();
            if (!type) return false;

            for (uint32_t k = 0; k < count; ++k) {
                func.locals.push_back(*type);
            }
        }

        // Read function body
        func.body = decodeExpression();
        if (!error_.empty()) return false;

        module.functions.push_back(func);

        // Verify we read exactly bodySize bytes
        if (pos_ != bodyStart + bodySize) {
            pos_ = bodyStart + bodySize;
        }
    }

    return true;
}

// Data section (memory initialization)
bool WasmDecoder::decodeDataSection(WasmModule& module) {
    uint32_t count = readVarU32();

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t flags = readVarU32();

        if (flags == 0) {
            // Active data segment
            auto offsetExpr = decodeExpression();
            uint32_t size = readVarU32();
            readBytes(size);
        } else if (flags == 1) {
            // Passive data segment
            uint32_t size = readVarU32();
            readBytes(size);
        } else if (flags == 2) {
            // Active with memory index
            uint32_t memIdx = readVarU32();
            auto offsetExpr = decodeExpression();
            uint32_t size = readVarU32();
            readBytes(size);
        }
    }

    return true;
}

// Expression decoding
std::vector<Instruction> WasmDecoder::decodeExpression() {
    std::vector<Instruction> instructions;

    while (true) {
        auto instr = decodeInstruction();
        if (!instr) break;

        bool isEnd = (instr->opcode == Opcode::End);
        instructions.push_back(*instr);

        if (isEnd) break;
    }

    return instructions;
}

// Instruction decoding
std::optional<Instruction> WasmDecoder::decodeInstruction() {
    if (!hasMore()) {
        setError("Unexpected end of input in instruction");
        return std::nullopt;
    }

    uint8_t opcodeByte = readByte();
    Opcode opcode = static_cast<Opcode>(opcodeByte);
    Instruction instr(opcode);

    switch (opcode) {
        // Control flow
        case Opcode::Block:
        case Opcode::Loop:
        case Opcode::If:
            instr.immediate = readByte();  // block type
            break;

        case Opcode::Br:
        case Opcode::BrIf:
            instr.immediate = readVarU32();
            break;

        case Opcode::BrTable: {
            uint32_t count = readVarU32();
            std::vector<uint32_t> targets;
            for (uint32_t i = 0; i < count; ++i) {
                targets.push_back(readVarU32());
            }
            uint32_t defaultTarget = readVarU32();
            targets.push_back(defaultTarget);
            instr.immediate = targets;
            break;
        }

        case Opcode::Call:
        case Opcode::CallIndirect:
            instr.immediate = readVarU32();
            if (opcode == Opcode::CallIndirect) {
                readVarU32();  // table index
            }
            break;

        // Variable access
        case Opcode::LocalGet:
        case Opcode::LocalSet:
        case Opcode::LocalTee:
        case Opcode::GlobalGet:
        case Opcode::GlobalSet:
            instr.immediate = readVarU32();
            break;

        // Memory access
        case Opcode::I32Load:
        case Opcode::I64Load:
        case Opcode::F32Load:
        case Opcode::F64Load:
        case Opcode::I32Load8S:
        case Opcode::I32Load8U:
        case Opcode::I32Load16S:
        case Opcode::I32Load16U:
        case Opcode::I64Load8S:
        case Opcode::I64Load8U:
        case Opcode::I64Load16S:
        case Opcode::I64Load16U:
        case Opcode::I64Load32S:
        case Opcode::I64Load32U:
        case Opcode::I32Store:
        case Opcode::I64Store:
        case Opcode::F32Store:
        case Opcode::F64Store:
        case Opcode::I32Store8:
        case Opcode::I32Store16:
        case Opcode::I64Store8:
        case Opcode::I64Store16:
        case Opcode::I64Store32: {
            uint32_t align = readVarU32();
            uint32_t offset = readVarU32();
            instr.immediate = std::make_pair(align, offset);
            break;
        }

        case Opcode::MemorySize:
        case Opcode::MemoryGrow:
            readByte();  // reserved (must be 0)
            break;

        // Constants
        case Opcode::I32Const:
            instr.immediate = readVarI32();
            break;

        case Opcode::I64Const:
            instr.immediate = readVarI64();
            break;

        case Opcode::F32Const:
            instr.immediate = readF32();
            break;

        case Opcode::F64Const:
            instr.immediate = readF64();
            break;

        default:
            // Most instructions have no immediate
            break;
    }

    return instr;
}

// Main decode function
std::optional<std::shared_ptr<WasmModule>> WasmDecoder::decode() {
    auto module = std::make_shared<WasmModule>();

    if (!readMagicAndVersion()) {
        return std::nullopt;
    }

    while (hasMore() && error_.empty()) {
        uint8_t sectionId = readByte();
        uint32_t sectionSize = readVarU32();
        size_t sectionStart = pos_;

        switch (static_cast<SectionId>(sectionId)) {
            case SectionId::Type:
                if (!decodeTypeSection(*module)) return std::nullopt;
                break;
            case SectionId::Import:
                if (!decodeImportSection(*module)) return std::nullopt;
                break;
            case SectionId::Function:
                if (!decodeFunctionSection(*module)) return std::nullopt;
                break;
            case SectionId::Table:
                if (!decodeTableSection(*module)) return std::nullopt;
                break;
            case SectionId::Memory:
                if (!decodeMemorySection(*module)) return std::nullopt;
                break;
            case SectionId::Global:
                if (!decodeGlobalSection(*module)) return std::nullopt;
                break;
            case SectionId::Export:
                if (!decodeExportSection(*module)) return std::nullopt;
                break;
            case SectionId::Start:
                if (!decodeStartSection(*module)) return std::nullopt;
                break;
            case SectionId::Code:
                if (!decodeCodeSection(*module)) return std::nullopt;
                break;
            case SectionId::Data:
                if (!decodeDataSection(*module)) return std::nullopt;
                break;
            case SectionId::Custom:
            default:
                // Skip unknown sections
                pos_ = sectionStart + sectionSize;
                break;
        }

        // Ensure we're at the end of the section
        if (pos_ != sectionStart + sectionSize) {
            pos_ = sectionStart + sectionSize;
        }
    }

    if (!error_.empty()) {
        return std::nullopt;
    }

    return module;
}

} // namespace wasm
} // namespace lightjs
