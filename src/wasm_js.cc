#include "wasm_js.h"
#include "value.h"
#include "streams.h"
#include "wasm_js.h"
#include "wasm/wasm_runtime.h"
#include "wasm/wasm_decoder.h"
#include <fstream>
#include <vector>

namespace lightjs {

// Convert JavaScript Value to WASM Value
std::optional<wasm::WasmValue> wasm_js::valueToWasm(const Value& val) {
    if (std::holds_alternative<double>(val.data)) {
        double d = std::get<double>(val.data);
        // Check if it's an integer
        if (d == static_cast<int32_t>(d)) {
            return wasm::WasmValue(static_cast<int32_t>(d));
        }
        return wasm::WasmValue(d);
    } else if (std::holds_alternative<BigInt>(val.data)) {
        return wasm::WasmValue(bigint::toInt64Trunc(std::get<BigInt>(val.data).value));
    } else if (std::holds_alternative<bool>(val.data)) {
        return wasm::WasmValue(static_cast<int32_t>(std::get<bool>(val.data)));
    }
    return std::nullopt;
}

// Convert WASM Value to JavaScript Value
Value wasm_js::wasmToValue(const wasm::WasmValue& val) {
    if (std::holds_alternative<int32_t>(val.data)) {
        return Value(static_cast<double>(std::get<int32_t>(val.data)));
    } else if (std::holds_alternative<int64_t>(val.data)) {
        return Value(BigInt(std::get<int64_t>(val.data)));
    } else if (std::holds_alternative<float>(val.data)) {
        return Value(static_cast<double>(std::get<float>(val.data)));
    } else if (std::holds_alternative<double>(val.data)) {
        return Value(std::get<double>(val.data));
    }
    return Value(Undefined{});
}

// Create WebAssembly global object
Value wasm_js::createWebAssemblyGlobal() {
    auto wasmObj = GarbageCollector::makeGC<Object>();

    // WebAssembly.instantiate(bufferSource, importObject?)
    auto instantiate = GarbageCollector::makeGC<Function>();
    instantiate->isNative = true;
    instantiate->nativeFunc = [](const std::vector<Value>& args) -> Value {
        if (args.empty()) {
            return Value(Undefined{});
        }

        // Get the buffer source (Uint8Array or ArrayBuffer)
        std::vector<uint8_t> wasmBytes;

        const auto& bufferArg = args[0];
        if (std::holds_alternative<GCPtr<TypedArray>>(bufferArg.data)) {
            auto typedArray = bufferArg.getGC<TypedArray>();
            const auto& bytes = typedArray->storage();
            wasmBytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(typedArray->byteOffset),
                             bytes.begin() + static_cast<std::ptrdiff_t>(typedArray->byteOffset + typedArray->currentByteLength()));
        } else if (std::holds_alternative<GCPtr<ArrayBuffer>>(bufferArg.data)) {
            auto arrayBuffer = bufferArg.getGC<ArrayBuffer>();
            wasmBytes = arrayBuffer->data;
        } else if (std::holds_alternative<std::string>(bufferArg.data)) {
            // Allow reading from file path (extension for convenience)
            const auto& path = std::get<std::string>(bufferArg.data);
            std::ifstream file(path, std::ios::binary);
            if (!file.is_open()) {
                return Value(Undefined{});
            }
            wasmBytes.assign(std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>());
        } else {
            return Value(Undefined{});
        }

        // Create runtime and instantiate
        auto runtime = wasm::WasmRuntimeFactory::createInterpreter();

        // Parse import object if provided
        wasm::ImportResolver importResolver = nullptr;
        if (args.size() > 1 && std::holds_alternative<GCPtr<Object>>(args[1].data)) {
            auto importObj = args[1].getGC<Object>();

            // Create import resolver that captures importObj
            importResolver = [importObj](const std::string& module, const std::string& name)
                -> std::optional<std::function<std::vector<wasm::WasmValue>(const std::vector<wasm::WasmValue>&)>> {

                // Look up module.name in import object
                if (importObj->properties.find(module) != importObj->properties.end()) {
                    const auto& moduleVal = importObj->properties.at(module);
                    if (std::holds_alternative<GCPtr<Object>>(moduleVal.data)) {
                        auto moduleObj = moduleVal.getGC<Object>();
                        if (moduleObj->properties.find(name) != moduleObj->properties.end()) {
                            const auto& funcVal = moduleObj->properties.at(name);
                            if (std::holds_alternative<GCPtr<Function>>(funcVal.data)) {
                                auto func = funcVal.getGC<Function>();

                                // Create a wrapper that converts between WASM and JS values
                                return [func](const std::vector<wasm::WasmValue>& wasmArgs)
                                    -> std::vector<wasm::WasmValue> {

                                    // Convert WASM args to JS values
                                    std::vector<Value> jsArgs;
                                    for (const auto& wasmArg : wasmArgs) {
                                        jsArgs.push_back(wasmToValue(wasmArg));
                                    }

                                    // Call the JS function
                                    Value result;
                                    if (func->isNative) {
                                        result = func->nativeFunc(jsArgs);
                                    } else {
                                        // For non-native functions, we'd need interpreter access
                                        // For now, just return undefined
                                        result = Value(Undefined{});
                                    }

                                    // Convert result back to WASM value
                                    std::vector<wasm::WasmValue> wasmResults;
                                    auto wasmResult = valueToWasm(result);
                                    if (wasmResult.has_value()) {
                                        wasmResults.push_back(wasmResult.value());
                                    }
                                    return wasmResults;
                                };
                            }
                        }
                    }
                }
                return std::nullopt;
            };
        }

        auto instance = runtime->instantiate(wasmBytes, importResolver);
        if (!instance.has_value()) {
            return Value(Undefined{});
        }

        // Create a wrapper object with the instance
        auto wasmInstance = std::make_shared<WasmInstanceJS>(instance.value(), std::move(runtime));

        // Create result object with instance and exports
        auto resultObj = GarbageCollector::makeGC<Object>();

        // Add instance property (for future module access)
        auto instanceObj = GarbageCollector::makeGC<Object>();

        // Create exports object
        auto exportsObj = GarbageCollector::makeGC<Object>();
        auto exportNames = wasmInstance->runtime->getExports(wasmInstance->instance);

        for (const auto& exportName : exportNames) {
            // Create a function wrapper for each export
            auto exportFunc = GarbageCollector::makeGC<Function>();
            exportFunc->isNative = true;

            // Capture the instance and function name
            exportFunc->nativeFunc = [wasmInstance, exportName](const std::vector<Value>& args) -> Value {
                // Convert JS args to WASM args
                std::vector<wasm::WasmValue> wasmArgs;
                for (const auto& arg : args) {
                    auto wasmArg = valueToWasm(arg);
                    if (wasmArg.has_value()) {
                        wasmArgs.push_back(wasmArg.value());
                    }
                }

                // Invoke the WASM function
                auto result = wasmInstance->runtime->invoke(wasmInstance->instance, exportName, wasmArgs);

                if (!result.success) {
                    return Value(Undefined{});
                }

                // Convert result back to JS value
                if (!result.values.empty()) {
                    return wasmToValue(result.values[0]);
                }

                return Value(Undefined{});
            };

            exportsObj->properties[exportName] = Value(exportFunc);
        }

        instanceObj->properties["exports"] = Value(exportsObj);
        resultObj->properties["instance"] = Value(instanceObj);

        return Value(resultObj);
    };

    wasmObj->properties["instantiate"] = Value(instantiate);

    // WebAssembly.compile(bufferSource)
    auto compile = GarbageCollector::makeGC<Function>();
    compile->isNative = true;
    compile->nativeFunc = [](const std::vector<Value>& args) -> Value {
        // For now, compile just validates the WASM binary
        if (args.empty()) {
            return Value(Undefined{});
        }

        std::vector<uint8_t> wasmBytes;

        const auto& bufferArg = args[0];
        if (std::holds_alternative<GCPtr<TypedArray>>(bufferArg.data)) {
            auto typedArray = bufferArg.getGC<TypedArray>();
            const auto& bytes = typedArray->storage();
            wasmBytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(typedArray->byteOffset),
                             bytes.begin() + static_cast<std::ptrdiff_t>(typedArray->byteOffset + typedArray->currentByteLength()));
        } else if (std::holds_alternative<GCPtr<ArrayBuffer>>(bufferArg.data)) {
            auto arrayBuffer = bufferArg.getGC<ArrayBuffer>();
            wasmBytes = arrayBuffer->data;
        } else {
            return Value(Undefined{});
        }

        // Decode the module to validate it
        wasm::WasmDecoder decoder(wasmBytes);
        auto module = decoder.decode();

        if (!module.has_value()) {
            return Value(Undefined{});
        }

        // Return a module object (simplified - just return true for now)
        return Value(true);
    };

    wasmObj->properties["compile"] = Value(compile);

    // WebAssembly.validate(bufferSource)
    auto validate = GarbageCollector::makeGC<Function>();
    validate->isNative = true;
    validate->nativeFunc = [](const std::vector<Value>& args) -> Value {
        if (args.empty()) {
            return Value(false);
        }

        std::vector<uint8_t> wasmBytes;

        const auto& bufferArg = args[0];
        if (std::holds_alternative<GCPtr<TypedArray>>(bufferArg.data)) {
            auto typedArray = bufferArg.getGC<TypedArray>();
            const auto& bytes = typedArray->storage();
            wasmBytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(typedArray->byteOffset),
                             bytes.begin() + static_cast<std::ptrdiff_t>(typedArray->byteOffset + typedArray->currentByteLength()));
        } else if (std::holds_alternative<GCPtr<ArrayBuffer>>(bufferArg.data)) {
            auto arrayBuffer = bufferArg.getGC<ArrayBuffer>();
            wasmBytes = arrayBuffer->data;
        } else {
            return Value(false);
        }

        // Try to decode the module
        wasm::WasmDecoder decoder(wasmBytes);
        auto module = decoder.decode();

        return Value(module.has_value());
    };

    wasmObj->properties["validate"] = Value(validate);

    return Value(wasmObj);
}

} // namespace lightjs
