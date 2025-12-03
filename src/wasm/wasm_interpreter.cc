#include "wasm/wasm_runtime.h"
#include "wasm/wasm_decoder.h"
#include <algorithm>
#include <cmath>

namespace lightjs {
namespace wasm {

// Forward declaration of memory factory
std::shared_ptr<WasmMemory> createMemory(const Limits& limits);

// Interpreter-based WASM runtime
class WasmInterpreter : public WasmRuntime {
public:
    std::optional<std::shared_ptr<WasmInstance>> instantiate(
        const std::vector<uint8_t>& wasmBinary,
        ImportResolver importResolver) override;

    ExecutionResult invoke(
        std::shared_ptr<WasmInstance> instance,
        const std::string& funcName,
        const std::vector<WasmValue>& args) override;

    ExecutionResult invokeByIndex(
        std::shared_ptr<WasmInstance> instance,
        uint32_t funcIdx,
        const std::vector<WasmValue>& args) override;

    std::vector<std::string> getExports(std::shared_ptr<WasmInstance> instance) override;
    std::shared_ptr<WasmMemory> getMemory(std::shared_ptr<WasmInstance> instance) override;
    std::optional<WasmValue> getGlobal(std::shared_ptr<WasmInstance> instance, const std::string& name) override;
    bool setGlobal(std::shared_ptr<WasmInstance> instance, const std::string& name, const WasmValue& value) override;

private:
    ExecutionResult executeFunction(
        std::shared_ptr<WasmInstance> instance,
        uint32_t funcIdx,
        const std::vector<WasmValue>& args);

    ExecutionResult executeInstruction(
        std::shared_ptr<WasmInstance> instance,
        const Instruction& instr,
        std::vector<WasmValue>& stack,
        std::vector<WasmValue>& locals);

    // Arithmetic operations
    WasmValue executeI32Add(const WasmValue& a, const WasmValue& b);
    WasmValue executeI32Sub(const WasmValue& a, const WasmValue& b);
    WasmValue executeI32Mul(const WasmValue& a, const WasmValue& b);
    WasmValue executeI64Add(const WasmValue& a, const WasmValue& b);
    WasmValue executeI64Sub(const WasmValue& a, const WasmValue& b);
    WasmValue executeI64Mul(const WasmValue& a, const WasmValue& b);
    WasmValue executeF32Add(const WasmValue& a, const WasmValue& b);
    WasmValue executeF32Sub(const WasmValue& a, const WasmValue& b);
    WasmValue executeF32Mul(const WasmValue& a, const WasmValue& b);
    WasmValue executeF32Div(const WasmValue& a, const WasmValue& b);
    WasmValue executeF64Add(const WasmValue& a, const WasmValue& b);
    WasmValue executeF64Sub(const WasmValue& a, const WasmValue& b);
    WasmValue executeF64Mul(const WasmValue& a, const WasmValue& b);
    WasmValue executeF64Div(const WasmValue& a, const WasmValue& b);

    // Comparison operations
    bool executeI32Eq(const WasmValue& a, const WasmValue& b);
    bool executeI32Lt(const WasmValue& a, const WasmValue& b);
    bool executeI32Gt(const WasmValue& a, const WasmValue& b);
    bool executeI64Eq(const WasmValue& a, const WasmValue& b);
    bool executeF32Eq(const WasmValue& a, const WasmValue& b);
    bool executeF32Lt(const WasmValue& a, const WasmValue& b);
    bool executeF64Eq(const WasmValue& a, const WasmValue& b);
    bool executeF64Lt(const WasmValue& a, const WasmValue& b);
};

// Instantiate module
std::optional<std::shared_ptr<WasmInstance>> WasmInterpreter::instantiate(
    const std::vector<uint8_t>& wasmBinary,
    ImportResolver importResolver) {

    // Decode the binary
    WasmDecoder decoder(wasmBinary);
    auto module = decoder.decode();

    if (!module) {
        return std::nullopt;
    }

    // Create instance
    auto instance = std::make_shared<WasmInstance>();
    instance->module = *module;
    instance->context = std::make_unique<WasmInstance::ExecutionContext>();

    // Create memory if module has memory section
    if (!(*module)->memories.empty()) {
        instance->memory = createMemory((*module)->memories[0]);
    }

    // Initialize globals
    for (const auto& global : (*module)->globals) {
        instance->globals.push_back(global);
    }

    // Resolve imports
    if (importResolver) {
        for (const auto& import : (*module)->imports) {
            if (import.kind == Import::Kind::Function) {
                auto hostFunc = importResolver(import.module, import.name);
                if (hostFunc) {
                    instance->functions.push_back(*hostFunc);
                }
            }
        }
    }

    return instance;
}

// Execute function by name
ExecutionResult WasmInterpreter::invoke(
    std::shared_ptr<WasmInstance> instance,
    const std::string& funcName,
    const std::vector<WasmValue>& args) {

    // Find function in exports
    for (const auto& exp : instance->module->exports) {
        if (exp.name == funcName && exp.kind == Export::Kind::Function) {
            return invokeByIndex(instance, exp.idx, args);
        }
    }

    return ExecutionResult::err("Function not found: " + funcName);
}

// Execute function by index
ExecutionResult WasmInterpreter::invokeByIndex(
    std::shared_ptr<WasmInstance> instance,
    uint32_t funcIdx,
    const std::vector<WasmValue>& args) {

    return executeFunction(instance, funcIdx, args);
}

// Get exported function names
std::vector<std::string> WasmInterpreter::getExports(std::shared_ptr<WasmInstance> instance) {
    std::vector<std::string> exports;
    for (const auto& exp : instance->module->exports) {
        if (exp.kind == Export::Kind::Function) {
            exports.push_back(exp.name);
        }
    }
    return exports;
}

// Get memory
std::shared_ptr<WasmMemory> WasmInterpreter::getMemory(std::shared_ptr<WasmInstance> instance) {
    return instance->memory;
}

// Get global
std::optional<WasmValue> WasmInterpreter::getGlobal(
    std::shared_ptr<WasmInstance> instance,
    const std::string& name) {

    for (const auto& exp : instance->module->exports) {
        if (exp.name == name && exp.kind == Export::Kind::Global) {
            if (exp.idx < instance->globals.size()) {
                return instance->globals[exp.idx].value;
            }
        }
    }
    return std::nullopt;
}

// Set global
bool WasmInterpreter::setGlobal(
    std::shared_ptr<WasmInstance> instance,
    const std::string& name,
    const WasmValue& value) {

    for (const auto& exp : instance->module->exports) {
        if (exp.name == name && exp.kind == Export::Kind::Global) {
            if (exp.idx < instance->globals.size()) {
                if (instance->globals[exp.idx].mutable_) {
                    instance->globals[exp.idx].value = value;
                    return true;
                }
            }
        }
    }
    return false;
}

// Execute function
ExecutionResult WasmInterpreter::executeFunction(
    std::shared_ptr<WasmInstance> instance,
    uint32_t funcIdx,
    const std::vector<WasmValue>& args) {

    // Check if this is an imported function
    uint32_t numImports = static_cast<uint32_t>(instance->module->imports.size());
    if (funcIdx < numImports) {
        // Call host function
        if (funcIdx < instance->functions.size()) {
            auto result = instance->functions[funcIdx](args);
            return ExecutionResult::ok(result);
        }
        return ExecutionResult::err("Imported function not found");
    }

    // Adjust index for module functions
    uint32_t moduleFuncIdx = funcIdx - numImports;
    if (moduleFuncIdx >= instance->module->functions.size()) {
        return ExecutionResult::err("Function index out of bounds");
    }

    const Function& func = instance->module->functions[moduleFuncIdx];

    // Initialize locals with parameters
    std::vector<WasmValue> locals;
    locals.insert(locals.end(), args.begin(), args.end());

    // Add local variables (initialized to zero)
    for (const auto& localType : func.locals) {
        switch (localType) {
            case ValueType::I32: locals.push_back(WasmValue(int32_t(0))); break;
            case ValueType::I64: locals.push_back(WasmValue(int64_t(0))); break;
            case ValueType::F32: locals.push_back(WasmValue(0.0f)); break;
            case ValueType::F64: locals.push_back(WasmValue(0.0)); break;
            default: break;
        }
    }

    // Execute instructions
    std::vector<WasmValue> stack;

    for (const auto& instr : func.body) {
        auto result = executeInstruction(instance, instr, stack, locals);
        if (!result.success) {
            return result;
        }

        // Check for return (End instruction)
        if (instr.opcode == Opcode::End || instr.opcode == Opcode::Return) {
            break;
        }
    }

    // Get result from stack
    std::vector<WasmValue> results;
    if (!stack.empty()) {
        results.push_back(stack.back());
    }

    return ExecutionResult::ok(results);
}

// Execute single instruction
ExecutionResult WasmInterpreter::executeInstruction(
    std::shared_ptr<WasmInstance> instance,
    const Instruction& instr,
    std::vector<WasmValue>& stack,
    std::vector<WasmValue>& locals) {

    switch (instr.opcode) {
        // Constants
        case Opcode::I32Const:
            stack.push_back(WasmValue(std::get<int32_t>(instr.immediate)));
            break;

        case Opcode::I64Const:
            stack.push_back(WasmValue(std::get<int64_t>(instr.immediate)));
            break;

        case Opcode::F32Const:
            stack.push_back(WasmValue(std::get<float>(instr.immediate)));
            break;

        case Opcode::F64Const:
            stack.push_back(WasmValue(std::get<double>(instr.immediate)));
            break;

        // Local variables
        case Opcode::LocalGet: {
            uint32_t idx = std::get<uint32_t>(instr.immediate);
            if (idx >= locals.size()) {
                return ExecutionResult::err("Local index out of bounds");
            }
            stack.push_back(locals[idx]);
            break;
        }

        case Opcode::LocalSet: {
            uint32_t idx = std::get<uint32_t>(instr.immediate);
            if (idx >= locals.size() || stack.empty()) {
                return ExecutionResult::err("Local set error");
            }
            locals[idx] = stack.back();
            stack.pop_back();
            break;
        }

        case Opcode::LocalTee: {
            uint32_t idx = std::get<uint32_t>(instr.immediate);
            if (idx >= locals.size() || stack.empty()) {
                return ExecutionResult::err("Local tee error");
            }
            locals[idx] = stack.back();
            break;
        }

        // Global variables
        case Opcode::GlobalGet: {
            uint32_t idx = std::get<uint32_t>(instr.immediate);
            if (idx >= instance->globals.size()) {
                return ExecutionResult::err("Global index out of bounds");
            }
            stack.push_back(instance->globals[idx].value);
            break;
        }

        case Opcode::GlobalSet: {
            uint32_t idx = std::get<uint32_t>(instr.immediate);
            if (idx >= instance->globals.size() || stack.empty()) {
                return ExecutionResult::err("Global set error");
            }
            if (!instance->globals[idx].mutable_) {
                return ExecutionResult::err("Cannot set immutable global");
            }
            instance->globals[idx].value = stack.back();
            stack.pop_back();
            break;
        }

        // Memory operations
        case Opcode::I32Load: {
            if (!instance->memory || stack.empty()) {
                return ExecutionResult::err("Memory load error");
            }
            auto pair = std::get<std::pair<uint32_t, uint32_t>>(instr.immediate);
            uint64_t addr = static_cast<uint64_t>(stack.back().asI32()) + pair.second;
            stack.pop_back();

            auto val = instance->memory->readI32(addr);
            if (!val) {
                return ExecutionResult::err("Memory access out of bounds");
            }
            stack.push_back(WasmValue(*val));
            break;
        }

        case Opcode::I32Store: {
            if (!instance->memory || stack.size() < 2) {
                return ExecutionResult::err("Memory store error");
            }
            auto pair = std::get<std::pair<uint32_t, uint32_t>>(instr.immediate);
            WasmValue value = stack.back(); stack.pop_back();
            uint64_t addr = static_cast<uint64_t>(stack.back().asI32()) + pair.second;
            stack.pop_back();

            if (!instance->memory->writeI32(addr, value.asI32())) {
                return ExecutionResult::err("Memory write out of bounds");
            }
            break;
        }

        // Arithmetic - i32
        case Opcode::I32Add: {
            if (stack.size() < 2) return ExecutionResult::err("Stack underflow");
            WasmValue b = stack.back(); stack.pop_back();
            WasmValue a = stack.back(); stack.pop_back();
            stack.push_back(executeI32Add(a, b));
            break;
        }

        case Opcode::I32Sub: {
            if (stack.size() < 2) return ExecutionResult::err("Stack underflow");
            WasmValue b = stack.back(); stack.pop_back();
            WasmValue a = stack.back(); stack.pop_back();
            stack.push_back(executeI32Sub(a, b));
            break;
        }

        case Opcode::I32Mul: {
            if (stack.size() < 2) return ExecutionResult::err("Stack underflow");
            WasmValue b = stack.back(); stack.pop_back();
            WasmValue a = stack.back(); stack.pop_back();
            stack.push_back(executeI32Mul(a, b));
            break;
        }

        // Arithmetic - i64
        case Opcode::I64Add: {
            if (stack.size() < 2) return ExecutionResult::err("Stack underflow");
            WasmValue b = stack.back(); stack.pop_back();
            WasmValue a = stack.back(); stack.pop_back();
            stack.push_back(executeI64Add(a, b));
            break;
        }

        // Arithmetic - f32
        case Opcode::F32Add: {
            if (stack.size() < 2) return ExecutionResult::err("Stack underflow");
            WasmValue b = stack.back(); stack.pop_back();
            WasmValue a = stack.back(); stack.pop_back();
            stack.push_back(executeF32Add(a, b));
            break;
        }

        case Opcode::F32Mul: {
            if (stack.size() < 2) return ExecutionResult::err("Stack underflow");
            WasmValue b = stack.back(); stack.pop_back();
            WasmValue a = stack.back(); stack.pop_back();
            stack.push_back(executeF32Mul(a, b));
            break;
        }

        // Arithmetic - f64
        case Opcode::F64Add: {
            if (stack.size() < 2) return ExecutionResult::err("Stack underflow");
            WasmValue b = stack.back(); stack.pop_back();
            WasmValue a = stack.back(); stack.pop_back();
            stack.push_back(executeF64Add(a, b));
            break;
        }

        case Opcode::F64Mul: {
            if (stack.size() < 2) return ExecutionResult::err("Stack underflow");
            WasmValue b = stack.back(); stack.pop_back();
            WasmValue a = stack.back(); stack.pop_back();
            stack.push_back(executeF64Mul(a, b));
            break;
        }

        // Comparisons
        case Opcode::I32Eq: {
            if (stack.size() < 2) return ExecutionResult::err("Stack underflow");
            WasmValue b = stack.back(); stack.pop_back();
            WasmValue a = stack.back(); stack.pop_back();
            stack.push_back(WasmValue(executeI32Eq(a, b) ? int32_t(1) : int32_t(0)));
            break;
        }

        case Opcode::I32LtS: {
            if (stack.size() < 2) return ExecutionResult::err("Stack underflow");
            WasmValue b = stack.back(); stack.pop_back();
            WasmValue a = stack.back(); stack.pop_back();
            stack.push_back(WasmValue(executeI32Lt(a, b) ? int32_t(1) : int32_t(0)));
            break;
        }

        // Control flow
        case Opcode::Drop:
            if (stack.empty()) return ExecutionResult::err("Stack underflow");
            stack.pop_back();
            break;

        case Opcode::Nop:
        case Opcode::End:
        case Opcode::Return:
            // No operation
            break;

        default:
            return ExecutionResult::err("Unsupported opcode: " +
                std::to_string(static_cast<uint8_t>(instr.opcode)));
    }

    return ExecutionResult::ok();
}

// Arithmetic implementations
WasmValue WasmInterpreter::executeI32Add(const WasmValue& a, const WasmValue& b) {
    return WasmValue(a.asI32() + b.asI32());
}

WasmValue WasmInterpreter::executeI32Sub(const WasmValue& a, const WasmValue& b) {
    return WasmValue(a.asI32() - b.asI32());
}

WasmValue WasmInterpreter::executeI32Mul(const WasmValue& a, const WasmValue& b) {
    return WasmValue(a.asI32() * b.asI32());
}

WasmValue WasmInterpreter::executeI64Add(const WasmValue& a, const WasmValue& b) {
    return WasmValue(a.asI64() + b.asI64());
}

WasmValue WasmInterpreter::executeI64Sub(const WasmValue& a, const WasmValue& b) {
    return WasmValue(a.asI64() - b.asI64());
}

WasmValue WasmInterpreter::executeI64Mul(const WasmValue& a, const WasmValue& b) {
    return WasmValue(a.asI64() * b.asI64());
}

WasmValue WasmInterpreter::executeF32Add(const WasmValue& a, const WasmValue& b) {
    return WasmValue(a.asF32() + b.asF32());
}

WasmValue WasmInterpreter::executeF32Sub(const WasmValue& a, const WasmValue& b) {
    return WasmValue(a.asF32() - b.asF32());
}

WasmValue WasmInterpreter::executeF32Mul(const WasmValue& a, const WasmValue& b) {
    return WasmValue(a.asF32() * b.asF32());
}

WasmValue WasmInterpreter::executeF32Div(const WasmValue& a, const WasmValue& b) {
    return WasmValue(a.asF32() / b.asF32());
}

WasmValue WasmInterpreter::executeF64Add(const WasmValue& a, const WasmValue& b) {
    return WasmValue(a.asF64() + b.asF64());
}

WasmValue WasmInterpreter::executeF64Sub(const WasmValue& a, const WasmValue& b) {
    return WasmValue(a.asF64() - b.asF64());
}

WasmValue WasmInterpreter::executeF64Mul(const WasmValue& a, const WasmValue& b) {
    return WasmValue(a.asF64() * b.asF64());
}

WasmValue WasmInterpreter::executeF64Div(const WasmValue& a, const WasmValue& b) {
    return WasmValue(a.asF64() / b.asF64());
}

// Comparison implementations
bool WasmInterpreter::executeI32Eq(const WasmValue& a, const WasmValue& b) {
    return a.asI32() == b.asI32();
}

bool WasmInterpreter::executeI32Lt(const WasmValue& a, const WasmValue& b) {
    return a.asI32() < b.asI32();
}

bool WasmInterpreter::executeI32Gt(const WasmValue& a, const WasmValue& b) {
    return a.asI32() > b.asI32();
}

bool WasmInterpreter::executeI64Eq(const WasmValue& a, const WasmValue& b) {
    return a.asI64() == b.asI64();
}

bool WasmInterpreter::executeF32Eq(const WasmValue& a, const WasmValue& b) {
    return a.asF32() == b.asF32();
}

bool WasmInterpreter::executeF32Lt(const WasmValue& a, const WasmValue& b) {
    return a.asF32() < b.asF32();
}

bool WasmInterpreter::executeF64Eq(const WasmValue& a, const WasmValue& b) {
    return a.asF64() == b.asF64();
}

bool WasmInterpreter::executeF64Lt(const WasmValue& a, const WasmValue& b) {
    return a.asF64() < b.asF64();
}

// Factory function
std::unique_ptr<WasmRuntime> WasmRuntimeFactory::createInterpreter() {
    return std::make_unique<WasmInterpreter>();
}

} // namespace wasm
} // namespace lightjs
