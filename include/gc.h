#pragma once

#include <memory>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <functional>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <stdexcept>

namespace lightjs {

// Memory limit configuration (Node.js-like behavior)
struct MemoryLimits {
    // Default heap limit: 2GB
    static constexpr size_t DEFAULT_HEAP_LIMIT = 2ULL * 1024 * 1024 * 1024;
    // Extended heap limit for systems with 16GB+ RAM: 4GB
    static constexpr size_t EXTENDED_HEAP_LIMIT = 4ULL * 1024 * 1024 * 1024;
    // Threshold for extended limit (16GB system RAM)
    static constexpr size_t EXTENDED_LIMIT_THRESHOLD = 16ULL * 1024 * 1024 * 1024;

    // Detect system memory and return appropriate limit
    static size_t getDefaultHeapLimit();
    // Get total system memory in bytes
    static size_t getSystemMemory();
};

// Forward declarations
class Value;
class Object;
class Array;
class Function;
class TypedArray;
class Promise;
class Regex;
class GCObject;
class Module;

// Garbage Collection Statistics
struct GCStats {
    size_t totalAllocated = 0;       // Total bytes allocated over time
    size_t totalFreed = 0;           // Total bytes freed over time
    size_t currentlyAllocated = 0;   // Current bytes in use
    size_t peakAllocated = 0;        // Peak bytes ever allocated
    size_t objectCount = 0;          // Current number of GC objects
    size_t peakObjectCount = 0;      // Peak number of GC objects
    size_t collectionsTriggered = 0;
    size_t cyclesDetected = 0;
    size_t heapLimitExceeded = 0;    // Number of times heap limit was hit
    std::chrono::microseconds totalGCTime{0};
    std::chrono::microseconds lastGCTime{0};
};

// Base class for garbage-collected objects
class GCObject {
public:
    GCObject();
    virtual ~GCObject();

    // Reference counting
    void addRef();
    void release();
    size_t refCount() const { return refCount_; }

    // Mark-and-sweep support
    void mark();
    bool isMarked() const { return marked_; }
    void clearMark() { marked_ = false; }

    // Get all referenced GC objects (for cycle detection)
    virtual void getReferences(std::vector<GCObject*>& refs) const {}

    // Type identification for debugging
    virtual const char* typeName() const = 0;

protected:
    std::atomic<size_t> refCount_{1};
    bool marked_ = false;
    bool inCycleCheck_ = false;

    friend class GarbageCollector;
};

// Smart pointer for GC objects with automatic reference counting
template<typename T>
class GCPtr {
public:
    GCPtr() : ptr_(nullptr) {}

    explicit GCPtr(T* ptr) : ptr_(ptr) {
        if (ptr_) ptr_->addRef();
    }

    GCPtr(const GCPtr& other) : ptr_(other.ptr_) {
        if (ptr_) ptr_->addRef();
    }

    GCPtr(GCPtr&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }

    ~GCPtr() {
        if (ptr_) ptr_->release();
    }

    GCPtr& operator=(const GCPtr& other) {
        if (this != &other) {
            if (ptr_) ptr_->release();
            ptr_ = other.ptr_;
            if (ptr_) ptr_->addRef();
        }
        return *this;
    }

    GCPtr& operator=(GCPtr&& other) noexcept {
        if (this != &other) {
            if (ptr_) ptr_->release();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    T& operator*() const { return *ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    bool operator==(const GCPtr& other) const { return ptr_ == other.ptr_; }
    bool operator!=(const GCPtr& other) const { return ptr_ != other.ptr_; }

private:
    T* ptr_;
};

// Garbage Collector
class GarbageCollector {
public:
    static GarbageCollector& instance();

    // Register/unregister objects
    void registerObject(GCObject* obj);
    void unregisterObject(GCObject* obj);

    // Manual collection triggers
    void collect();
    void collectIfNeeded();

    // Configuration
    void setThreshold(size_t threshold) { allocationThreshold_ = threshold; }
    size_t getThreshold() const { return allocationThreshold_; }

    void setAutoCollect(bool enabled) { autoCollectEnabled_ = enabled; }
    bool isAutoCollectEnabled() const { return autoCollectEnabled_; }

    // Heap limit configuration (Node.js-like behavior)
    // Default: 2GB, or 4GB on systems with 16GB+ RAM
    void setHeapLimit(size_t bytes) { heapLimit_ = bytes; }
    size_t getHeapLimit() const { return heapLimit_; }
    bool isHeapLimitEnabled() const { return heapLimitEnabled_; }
    void setHeapLimitEnabled(bool enabled) { heapLimitEnabled_ = enabled; }

    // Statistics
    const GCStats& getStats() const { return stats_; }
    void resetStats() { stats_ = GCStats{}; }

    // Memory pressure
    size_t getCurrentMemoryUsage() const;
    void reportAllocation(size_t bytes);
    void reportDeallocation(size_t bytes);

    // Check if heap limit would be exceeded by an allocation
    // Returns true if allocation is OK, false if limit exceeded
    bool checkHeapLimit(size_t additionalBytes = 0) const;

    // Exception thrown when heap limit is exceeded
    class HeapLimitExceededException : public std::exception {
    public:
        HeapLimitExceededException(size_t current, size_t limit, size_t requested)
            : current_(current), limit_(limit), requested_(requested) {
            message_ = "FATAL ERROR: CALL_AND_RETRY_LAST Allocation failed - "
                       "JavaScript heap out of memory";
        }
        const char* what() const noexcept override { return message_.c_str(); }
        size_t currentUsage() const { return current_; }
        size_t heapLimit() const { return limit_; }
        size_t requestedSize() const { return requested_; }
    private:
        size_t current_;
        size_t limit_;
        size_t requested_;
        std::string message_;
    };

private:
    GarbageCollector();
    ~GarbageCollector() = default;

    // Prevent copying
    GarbageCollector(const GarbageCollector&) = delete;
    GarbageCollector& operator=(const GarbageCollector&) = delete;

    // Mark-and-sweep implementation
    void markPhase();
    void sweepPhase();
    void detectCycles();

    // Find root objects (those with external references)
    void findRoots(std::unordered_set<GCObject*>& roots);

    // Cycle detection using Tarjan's algorithm
    void strongConnect(GCObject* obj,
                       int& index,
                       std::unordered_map<GCObject*, int>& indices,
                       std::unordered_map<GCObject*, int>& lowlinks,
                       std::vector<GCObject*>& stack,
                       std::unordered_set<GCObject*>& onStack,
                       std::vector<std::vector<GCObject*>>& sccs);

private:
    std::unordered_set<GCObject*> objects_;
    std::unordered_set<GCObject*> roots_;

    size_t allocationThreshold_ = 1024 * 1024;  // 1MB default threshold
    size_t bytesAllocatedSinceGC_ = 0;
    bool autoCollectEnabled_ = true;
    bool collectInProgress_ = false;

    // Heap limit settings (Node.js-like behavior)
    size_t heapLimit_ = MemoryLimits::getDefaultHeapLimit();
    bool heapLimitEnabled_ = true;

    GCStats stats_;

    static thread_local bool gcDisabled_;  // Disable GC during certain operations
};

// RAII helper to temporarily disable GC
class GCDisableScope {
public:
    GCDisableScope() {
        wasEnabled_ = GarbageCollector::instance().isAutoCollectEnabled();
        GarbageCollector::instance().setAutoCollect(false);
    }

    ~GCDisableScope() {
        GarbageCollector::instance().setAutoCollect(wasEnabled_);
    }

private:
    bool wasEnabled_;
};

} // namespace lightjs