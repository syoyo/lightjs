#include "wasm/wasm_runtime.h"
#include "checked_arithmetic.h"
#include <cstring>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace lightjs {
namespace wasm {

namespace {

constexpr uint64_t kWasmPageSize = 65536;
constexpr uint64_t kMaxWasmMemoryBytes = 256ull * 1024ull * 1024ull;
constexpr uint64_t kMaxWasmMemoryPages = kMaxWasmMemoryBytes / kWasmPageSize;

}  // namespace

// Concrete memory implementation with memory64 support
class WasmMemoryImpl : public WasmMemory {
public:
    explicit WasmMemoryImpl(const Limits& limits)
        : limits_(limits), is64_(limits.is64) {
        if (limits_.hasMax && limits_.max < limits_.min) {
            throw std::runtime_error("Invalid WASM memory limits");
        }
        if ((!is64_ && (limits_.min > 65536 || (limits_.hasMax && limits_.max > 65536))) ||
            limits_.min > kMaxWasmMemoryPages ||
            (limits_.hasMax && limits_.max > kMaxWasmMemoryPages)) {
            throw std::runtime_error("WASM memory exceeds implementation limit");
        }

        uint64_t initialBytes = 0;
        size_t initialSize = 0;
        if (!checked::mul(limits_.min, kWasmPageSize, initialBytes) ||
            initialBytes > kMaxWasmMemoryBytes ||
            !checked::narrow(initialBytes, initialSize)) {
            throw std::runtime_error("WASM memory exceeds implementation limit");
        }
        data_.resize(initialSize, 0);
    }

    uint64_t size() const override {
        return data_.size();
    }

    uint64_t pages() const override {
        return data_.size() / kWasmPageSize;
    }

    bool is64() const override {
        return is64_;
    }

    bool grow(uint64_t deltaPages) override {
        uint64_t currentPages = pages();
        uint64_t newPages = 0;
        if (!checked::add(currentPages, deltaPages, newPages)) {
            return false;
        }

        // Check if exceeds max (if specified)
        if (limits_.hasMax && newPages > limits_.max) {
            return false;
        }

        // Check memory64 limits
        if (!is64_ && newPages > 65536) {
            // 32-bit addressing maxes out at 4GB (65536 pages)
            return false;
        }

        if (newPages > kMaxWasmMemoryPages) {
            return false;
        }

        uint64_t newSize = 0;
        size_t resizedBytes = 0;
        if (!checked::mul(newPages, kWasmPageSize, newSize) ||
            newSize > kMaxWasmMemoryBytes ||
            !checked::narrow(newSize, resizedBytes)) {
            return false;
        }

        data_.resize(resizedBytes, 0);
        return true;
    }

    std::optional<uint8_t> readByte(uint64_t addr) const override {
        size_t offset = 0;
        if (!resolveRange(addr, 1, offset)) {
            return std::nullopt;
        }
        return data_[offset];
    }

    bool writeByte(uint64_t addr, uint8_t value) override {
        size_t offset = 0;
        if (!resolveRange(addr, 1, offset)) {
            return false;
        }
        data_[offset] = value;
        return true;
    }

    std::optional<std::vector<uint8_t>> read(uint64_t addr, uint64_t length) const override {
        size_t offset = 0;
        size_t count = 0;
        if (!resolveRange(addr, length, offset, &count)) {
            return std::nullopt;
        }
        return std::vector<uint8_t>(data_.begin() + offset, data_.begin() + offset + count);
    }

    bool write(uint64_t addr, const std::vector<uint8_t>& bytes) override {
        size_t offset = 0;
        size_t count = 0;
        if (!resolveRange(addr, static_cast<uint64_t>(bytes.size()), offset, &count)) {
            return false;
        }
        std::copy(bytes.begin(), bytes.end(), data_.begin() + offset);
        return true;
    }

    std::optional<int32_t> readI32(uint64_t addr) const override {
        size_t offset = 0;
        if (!resolveRange(addr, sizeof(int32_t), offset)) {
            return std::nullopt;
        }
        int32_t value;
        std::memcpy(&value, &data_[offset], sizeof(int32_t));
        return value;
    }

    std::optional<int64_t> readI64(uint64_t addr) const override {
        size_t offset = 0;
        if (!resolveRange(addr, sizeof(int64_t), offset)) {
            return std::nullopt;
        }
        int64_t value;
        std::memcpy(&value, &data_[offset], sizeof(int64_t));
        return value;
    }

    std::optional<float> readF32(uint64_t addr) const override {
        size_t offset = 0;
        if (!resolveRange(addr, sizeof(float), offset)) {
            return std::nullopt;
        }
        float value;
        std::memcpy(&value, &data_[offset], sizeof(float));
        return value;
    }

    std::optional<double> readF64(uint64_t addr) const override {
        size_t offset = 0;
        if (!resolveRange(addr, sizeof(double), offset)) {
            return std::nullopt;
        }
        double value;
        std::memcpy(&value, &data_[offset], sizeof(double));
        return value;
    }

    bool writeI32(uint64_t addr, int32_t value) override {
        size_t offset = 0;
        if (!resolveRange(addr, sizeof(int32_t), offset)) {
            return false;
        }
        std::memcpy(&data_[offset], &value, sizeof(int32_t));
        return true;
    }

    bool writeI64(uint64_t addr, int64_t value) override {
        size_t offset = 0;
        if (!resolveRange(addr, sizeof(int64_t), offset)) {
            return false;
        }
        std::memcpy(&data_[offset], &value, sizeof(int64_t));
        return true;
    }

    bool writeF32(uint64_t addr, float value) override {
        size_t offset = 0;
        if (!resolveRange(addr, sizeof(float), offset)) {
            return false;
        }
        std::memcpy(&data_[offset], &value, sizeof(float));
        return true;
    }

    bool writeF64(uint64_t addr, double value) override {
        size_t offset = 0;
        if (!resolveRange(addr, sizeof(double), offset)) {
            return false;
        }
        std::memcpy(&data_[offset], &value, sizeof(double));
        return true;
    }

private:
    bool resolveRange(uint64_t addr,
                      uint64_t length,
                      size_t& offset,
                      size_t* count = nullptr) const {
        uint64_t available = static_cast<uint64_t>(data_.size());
        if (!checked::rangeWithin(addr, length, available) ||
            !checked::narrow(addr, offset)) {
            return false;
        }
        if (count && !checked::narrow(length, *count)) {
            return false;
        }
        return true;
    }

    Limits limits_;
    bool is64_;
    std::vector<uint8_t> data_;
};

// Factory function to create memory
std::shared_ptr<WasmMemory> createMemory(const Limits& limits) {
    return std::make_shared<WasmMemoryImpl>(limits);
}

} // namespace wasm
} // namespace lightjs
