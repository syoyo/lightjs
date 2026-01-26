#include "wasm/wasm_runtime.h"
#include <cstring>
#include <algorithm>

namespace lightjs {
namespace wasm {

// Concrete memory implementation with memory64 support
class WasmMemoryImpl : public WasmMemory {
public:
    explicit WasmMemoryImpl(const Limits& limits)
        : limits_(limits), is64_(limits.is64) {
        // Each page is 64KB
        uint64_t initialBytes = limits.min * 65536;
        data_.resize(initialBytes, 0);
    }

    uint64_t size() const override {
        return data_.size();
    }

    uint64_t pages() const override {
        return data_.size() / 65536;
    }

    bool is64() const override {
        return is64_;
    }

    bool grow(uint64_t deltaPages) override {
        uint64_t currentPages = pages();
        uint64_t newPages = currentPages + deltaPages;

        // Check if exceeds max (if specified)
        if (limits_.hasMax && newPages > limits_.max) {
            return false;
        }

        // Check memory64 limits
        if (!is64_ && newPages > 65536) {
            // 32-bit addressing maxes out at 4GB (65536 pages)
            return false;
        }

        uint64_t newSize = newPages * 65536;
        data_.resize(newSize, 0);
        return true;
    }

    std::optional<uint8_t> readByte(uint64_t addr) const override {
        if (addr >= data_.size()) {
            return std::nullopt;
        }
        return data_[addr];
    }

    bool writeByte(uint64_t addr, uint8_t value) override {
        if (addr >= data_.size()) {
            return false;
        }
        data_[addr] = value;
        return true;
    }

    std::optional<std::vector<uint8_t>> read(uint64_t addr, uint64_t length) const override {
        if (addr + length > data_.size() || addr + length < addr) {
            return std::nullopt;
        }
        return std::vector<uint8_t>(data_.begin() + addr, data_.begin() + addr + length);
    }

    bool write(uint64_t addr, const std::vector<uint8_t>& bytes) override {
        if (addr + bytes.size() > data_.size() || addr + bytes.size() < addr) {
            return false;
        }
        std::copy(bytes.begin(), bytes.end(), data_.begin() + addr);
        return true;
    }

    std::optional<int32_t> readI32(uint64_t addr) const override {
        if (addr + 4 > data_.size()) {
            return std::nullopt;
        }
        int32_t value;
        std::memcpy(&value, &data_[addr], 4);
        return value;
    }

    std::optional<int64_t> readI64(uint64_t addr) const override {
        if (addr + 8 > data_.size()) {
            return std::nullopt;
        }
        int64_t value;
        std::memcpy(&value, &data_[addr], 8);
        return value;
    }

    std::optional<float> readF32(uint64_t addr) const override {
        if (addr + 4 > data_.size()) {
            return std::nullopt;
        }
        float value;
        std::memcpy(&value, &data_[addr], 4);
        return value;
    }

    std::optional<double> readF64(uint64_t addr) const override {
        if (addr + 8 > data_.size()) {
            return std::nullopt;
        }
        double value;
        std::memcpy(&value, &data_[addr], 8);
        return value;
    }

    bool writeI32(uint64_t addr, int32_t value) override {
        if (addr + 4 > data_.size()) {
            return false;
        }
        std::memcpy(&data_[addr], &value, 4);
        return true;
    }

    bool writeI64(uint64_t addr, int64_t value) override {
        if (addr + 8 > data_.size()) {
            return false;
        }
        std::memcpy(&data_[addr], &value, 8);
        return true;
    }

    bool writeF32(uint64_t addr, float value) override {
        if (addr + 4 > data_.size()) {
            return false;
        }
        std::memcpy(&data_[addr], &value, 4);
        return true;
    }

    bool writeF64(uint64_t addr, double value) override {
        if (addr + 8 > data_.size()) {
            return false;
        }
        std::memcpy(&data_[addr], &value, 8);
        return true;
    }

private:
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
