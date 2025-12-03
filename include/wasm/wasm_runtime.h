#pragma once

#include "wasm_types.h"
#include <memory>
#include <optional>
#include <functional>

namespace lightjs {
namespace wasm {

// Forward declarations
class WasmModule;
class WasmInstance;

// Memory interface (supports memory64)
class WasmMemory {
public:
    virtual ~WasmMemory() = default;

    // Memory operations
    virtual uint64_t size() const = 0;  // Size in bytes
    virtual uint64_t pages() const = 0;  // Size in pages (64KB each)
    virtual bool grow(uint64_t deltaPages) = 0;
    virtual bool is64() const = 0;  // Returns true if memory64

    // Byte access
    virtual std::optional<uint8_t> readByte(uint64_t addr) const = 0;
    virtual bool writeByte(uint64_t addr, uint8_t value) = 0;

    // Multi-byte read/write (with bounds checking)
    virtual std::optional<std::vector<uint8_t>> read(uint64_t addr, uint64_t length) const = 0;
    virtual bool write(uint64_t addr, const std::vector<uint8_t>& data) = 0;

    // Typed reads (little-endian)
    virtual std::optional<int32_t> readI32(uint64_t addr) const = 0;
    virtual std::optional<int64_t> readI64(uint64_t addr) const = 0;
    virtual std::optional<float> readF32(uint64_t addr) const = 0;
    virtual std::optional<double> readF64(uint64_t addr) const = 0;

    // Typed writes (little-endian)
    virtual bool writeI32(uint64_t addr, int32_t value) = 0;
    virtual bool writeI64(uint64_t addr, int64_t value) = 0;
    virtual bool writeF32(uint64_t addr, float value) = 0;
    virtual bool writeF64(uint64_t addr, double value) = 0;
};

// Import resolver - allows host to provide imported functions
using ImportResolver = std::function<std::optional<std::function<std::vector<WasmValue>(const std::vector<WasmValue>&)>>(
    const std::string& module,
    const std::string& name
)>;

// Execution result
struct ExecutionResult {
    bool success;
    std::string error;
    std::vector<WasmValue> values;  // Return values

    static ExecutionResult ok(const std::vector<WasmValue>& vals = {}) {
        return ExecutionResult{true, "", vals};
    }

    static ExecutionResult err(const std::string& msg) {
        return ExecutionResult{false, msg, {}};
    }
};

// Abstract WASM runtime interface
// This allows us to swap implementations (interpreter, JIT, etc.)
class WasmRuntime {
public:
    virtual ~WasmRuntime() = default;

    // Load and instantiate a module from binary
    virtual std::optional<std::shared_ptr<WasmInstance>> instantiate(
        const std::vector<uint8_t>& wasmBinary,
        ImportResolver importResolver = nullptr
    ) = 0;

    // Execute a function by name
    virtual ExecutionResult invoke(
        std::shared_ptr<WasmInstance> instance,
        const std::string& funcName,
        const std::vector<WasmValue>& args
    ) = 0;

    // Execute a function by index
    virtual ExecutionResult invokeByIndex(
        std::shared_ptr<WasmInstance> instance,
        uint32_t funcIdx,
        const std::vector<WasmValue>& args
    ) = 0;

    // Get exported function names
    virtual std::vector<std::string> getExports(std::shared_ptr<WasmInstance> instance) = 0;

    // Access instance memory
    virtual std::shared_ptr<WasmMemory> getMemory(std::shared_ptr<WasmInstance> instance) = 0;

    // Get/set global variables
    virtual std::optional<WasmValue> getGlobal(std::shared_ptr<WasmInstance> instance, const std::string& name) = 0;
    virtual bool setGlobal(std::shared_ptr<WasmInstance> instance, const std::string& name, const WasmValue& value) = 0;
};

// WASM module (parsed but not instantiated)
class WasmModule {
public:
    std::vector<FuncType> types;
    std::vector<Import> imports;
    std::vector<uint32_t> functionTypeIndices;
    std::vector<Table> tables;
    std::vector<Limits> memories;
    std::vector<Global> globals;
    std::vector<Export> exports;
    std::vector<Function> functions;
    std::optional<uint32_t> startFunction;

    // Binary sections (for lazy parsing if needed)
    std::vector<uint8_t> customSections;
    std::vector<uint8_t> dataSections;
};

// WASM instance (instantiated module with state)
class WasmInstance {
public:
    std::shared_ptr<WasmModule> module;
    std::shared_ptr<WasmMemory> memory;
    std::vector<Global> globals;
    std::vector<std::function<std::vector<WasmValue>(const std::vector<WasmValue>&)>> functions;

    // Execution state for interpreter
    struct ExecutionContext {
        std::vector<WasmValue> stack;
        std::vector<WasmValue> locals;
        uint32_t pc;  // Program counter
        std::vector<uint32_t> callStack;
    };

    std::unique_ptr<ExecutionContext> context;
};

// Factory for creating runtime implementations
class WasmRuntimeFactory {
public:
    // Create interpreter-based runtime
    static std::unique_ptr<WasmRuntime> createInterpreter();

    // Future: Create JIT-based runtime
    // static std::unique_ptr<WasmRuntime> createJIT();

    // Future: Create LLVM-based runtime
    // static std::unique_ptr<WasmRuntime> createLLVM();
};

} // namespace wasm
} // namespace lightjs
