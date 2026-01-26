#pragma once

#include "value.h"
#include "wasm/wasm_runtime.h"
#include <memory>

namespace lightjs {

// Forward declarations
struct Environment;

// WASM instance wrapper for JavaScript exposure
struct WasmInstanceJS : public GCObject {
    std::shared_ptr<wasm::WasmInstance> instance;
    std::unique_ptr<wasm::WasmRuntime> runtime;

    explicit WasmInstanceJS(std::shared_ptr<wasm::WasmInstance> inst, std::unique_ptr<wasm::WasmRuntime> rt)
        : instance(std::move(inst)), runtime(std::move(rt)) {}

    // GCObject interface
    const char* typeName() const override { return "WasmInstance"; }
    void getReferences(std::vector<GCObject*>& refs) const override {}
};

// WASM Memory wrapper for JavaScript exposure
struct WasmMemoryJS : public GCObject {
    std::shared_ptr<wasm::WasmMemory> memory;

    explicit WasmMemoryJS(std::shared_ptr<wasm::WasmMemory> mem)
        : memory(std::move(mem)) {}

    // GCObject interface
    const char* typeName() const override { return "WasmMemory"; }
    void getReferences(std::vector<GCObject*>& refs) const override {}
};

// Helper functions to create WebAssembly global object
namespace wasm_js {
    // Create the WebAssembly global object with instantiate, compile, etc.
    Value createWebAssemblyGlobal();

    // Convert Value to WASM value
    std::optional<wasm::WasmValue> valueToWasm(const Value& val);

    // Convert WASM value to JavaScript value
    Value wasmToValue(const wasm::WasmValue& val);
}

} // namespace lightjs
