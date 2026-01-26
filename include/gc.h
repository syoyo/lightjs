#pragma once

#include <memory>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <functional>
#include <atomic>
#include <chrono>

namespace lightjs {

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
    size_t totalAllocated = 0;
    size_t totalFreed = 0;
    size_t currentlyAllocated = 0;
    size_t peakAllocated = 0;
    size_t collectionsTriggered = 0;
    size_t cyclesDetected = 0;
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

    // Statistics
    const GCStats& getStats() const { return stats_; }
    void resetStats() { stats_ = GCStats{}; }

    // Memory pressure
    size_t getCurrentMemoryUsage() const;
    void reportAllocation(size_t bytes);
    void reportDeallocation(size_t bytes);

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