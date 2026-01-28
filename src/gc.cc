#include "gc.h"
#include "value.h"
#include <algorithm>
#include <iostream>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace lightjs {

// ============================================================================
// MemoryLimits implementation
// ============================================================================

size_t MemoryLimits::getSystemMemory() {
#ifdef _WIN32
    MEMORYSTATUSEX memStatus;
    memStatus.dwLength = sizeof(memStatus);
    if (GlobalMemoryStatusEx(&memStatus)) {
        return static_cast<size_t>(memStatus.ullTotalPhys);
    }
    return 0;
#elif defined(__APPLE__)
    int64_t memsize = 0;
    size_t len = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &len, nullptr, 0) == 0) {
        return static_cast<size_t>(memsize);
    }
    return 0;
#elif defined(__linux__)
    // Try /proc/meminfo first (more accurate)
    std::ifstream meminfo("/proc/meminfo");
    if (meminfo.is_open()) {
        std::string line;
        while (std::getline(meminfo, line)) {
            if (line.find("MemTotal:") == 0) {
                size_t kb = 0;
                // Parse "MemTotal:       xxxxx kB"
                size_t pos = line.find(':');
                if (pos != std::string::npos) {
                    std::string numPart = line.substr(pos + 1);
                    // Skip whitespace
                    size_t start = numPart.find_first_not_of(" \t");
                    if (start != std::string::npos) {
                        kb = std::stoull(numPart.substr(start));
                        return kb * 1024;  // Convert KB to bytes
                    }
                }
            }
        }
    }
    // Fallback to sysconf
    long pages = sysconf(_SC_PHYS_PAGES);
    long pageSize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && pageSize > 0) {
        return static_cast<size_t>(pages) * static_cast<size_t>(pageSize);
    }
    return 0;
#else
    return 0;
#endif
}

size_t MemoryLimits::getDefaultHeapLimit() {
    size_t systemMem = getSystemMemory();

    // Node.js behavior: 4GB for systems with 16GB+ RAM, otherwise 2GB
    if (systemMem >= EXTENDED_LIMIT_THRESHOLD) {
        return EXTENDED_HEAP_LIMIT;
    }
    return DEFAULT_HEAP_LIMIT;
}

thread_local bool GarbageCollector::gcDisabled_ = false;

// GCObject implementation
GCObject::GCObject() {
    GarbageCollector::instance().registerObject(this);
}

GCObject::~GCObject() {
    GarbageCollector::instance().unregisterObject(this);
}

void GCObject::addRef() {
    refCount_.fetch_add(1, std::memory_order_relaxed);
}

void GCObject::release() {
    if (refCount_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete this;
    }
}

void GCObject::mark() {
    if (marked_) return;
    marked_ = true;

    // Mark all referenced objects
    std::vector<GCObject*> refs;
    getReferences(refs);
    for (auto* ref : refs) {
        if (ref) ref->mark();
    }
}

// GarbageCollector implementation
GarbageCollector& GarbageCollector::instance() {
    static GarbageCollector instance;
    return instance;
}

GarbageCollector::GarbageCollector() {}

void GarbageCollector::registerObject(GCObject* obj) {
    if (!obj || gcDisabled_) return;
    objects_.insert(obj);
    stats_.objectCount++;
    stats_.totalAllocated += sizeof(GCObject);  // Base object size
    stats_.currentlyAllocated += sizeof(GCObject);
    if (stats_.objectCount > stats_.peakObjectCount) {
        stats_.peakObjectCount = stats_.objectCount;
    }
    if (stats_.currentlyAllocated > stats_.peakAllocated) {
        stats_.peakAllocated = stats_.currentlyAllocated;
    }
}

void GarbageCollector::unregisterObject(GCObject* obj) {
    if (!obj) return;
    objects_.erase(obj);
    stats_.objectCount--;
    if (stats_.currentlyAllocated >= sizeof(GCObject)) {
        stats_.currentlyAllocated -= sizeof(GCObject);
    }
    stats_.totalFreed += sizeof(GCObject);
}

void GarbageCollector::collect() {
    if (collectInProgress_ || !autoCollectEnabled_) return;

    collectInProgress_ = true;
    auto startTime = std::chrono::high_resolution_clock::now();

    // Phase 1: Clear all marks
    for (auto* obj : objects_) {
        obj->clearMark();
    }

    // Phase 2: Mark phase - find and mark all reachable objects
    markPhase();

    // Phase 3: Detect and handle cycles
    detectCycles();

    // Phase 4: Sweep phase - collect unmarked objects
    sweepPhase();

    // Update statistics
    auto endTime = std::chrono::high_resolution_clock::now();
    stats_.lastGCTime = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
    stats_.totalGCTime += stats_.lastGCTime;
    stats_.collectionsTriggered++;

    bytesAllocatedSinceGC_ = 0;
    collectInProgress_ = false;
}

void GarbageCollector::collectIfNeeded() {
    if (bytesAllocatedSinceGC_ >= allocationThreshold_) {
        collect();
    }
}

void GarbageCollector::markPhase() {
    // Find root objects (those with external references beyond GC control)
    std::unordered_set<GCObject*> roots;
    findRoots(roots);

    // Mark all objects reachable from roots
    for (auto* root : roots) {
        root->mark();
    }
}

void GarbageCollector::sweepPhase() {
    std::vector<GCObject*> toDelete;

    for (auto* obj : objects_) {
        if (!obj->isMarked() && obj->refCount() == 0) {
            toDelete.push_back(obj);
        }
    }

    // Delete unmarked objects with zero references
    for (auto* obj : toDelete) {
        delete obj;
    }
}

void GarbageCollector::findRoots(std::unordered_set<GCObject*>& roots) {
    // Objects with ref count > 0 are roots
    // (They have references from outside the GC system)
    for (auto* obj : objects_) {
        if (obj->refCount() > 0) {
            roots.insert(obj);
        }
    }
}

void GarbageCollector::detectCycles() {
    // Use Tarjan's strongly connected components algorithm
    std::unordered_map<GCObject*, int> indices;
    std::unordered_map<GCObject*, int> lowlinks;
    std::vector<GCObject*> stack;
    std::unordered_set<GCObject*> onStack;
    std::vector<std::vector<GCObject*>> sccs;
    int index = 0;

    for (auto* obj : objects_) {
        if (indices.find(obj) == indices.end()) {
            strongConnect(obj, index, indices, lowlinks, stack, onStack, sccs);
        }
    }

    // Process strongly connected components (cycles)
    for (const auto& scc : sccs) {
        if (scc.size() > 1) {
            // This is a cycle
            bool hasExternalRef = false;
            for (auto* obj : scc) {
                // Check if any object in the cycle has external references
                if (obj->refCount() > 0) {
                    hasExternalRef = true;
                    break;
                }
            }

            if (!hasExternalRef) {
                // Cycle with no external references - mark for collection
                for (auto* obj : scc) {
                    obj->clearMark();
                }
                stats_.cyclesDetected++;
            }
        }
    }
}

void GarbageCollector::strongConnect(
    GCObject* obj,
    int& index,
    std::unordered_map<GCObject*, int>& indices,
    std::unordered_map<GCObject*, int>& lowlinks,
    std::vector<GCObject*>& stack,
    std::unordered_set<GCObject*>& onStack,
    std::vector<std::vector<GCObject*>>& sccs)
{
    indices[obj] = index;
    lowlinks[obj] = index;
    index++;
    stack.push_back(obj);
    onStack.insert(obj);

    // Get all references from this object
    std::vector<GCObject*> refs;
    obj->getReferences(refs);

    for (auto* ref : refs) {
        if (!ref) continue;

        if (indices.find(ref) == indices.end()) {
            // Successor has not been visited
            strongConnect(ref, index, indices, lowlinks, stack, onStack, sccs);
            lowlinks[obj] = std::min(lowlinks[obj], lowlinks[ref]);
        } else if (onStack.find(ref) != onStack.end()) {
            // Successor is in stack and hence in current SCC
            lowlinks[obj] = std::min(lowlinks[obj], indices[ref]);
        }
    }

    // If obj is a root node, pop the stack and generate an SCC
    if (lowlinks[obj] == indices[obj]) {
        std::vector<GCObject*> scc;
        GCObject* w;
        do {
            w = stack.back();
            stack.pop_back();
            onStack.erase(w);
            scc.push_back(w);
        } while (w != obj);
        sccs.push_back(scc);
    }
}

size_t GarbageCollector::getCurrentMemoryUsage() const {
    return stats_.currentlyAllocated;
}

bool GarbageCollector::checkHeapLimit(size_t additionalBytes) const {
    if (!heapLimitEnabled_) return true;
    return (stats_.currentlyAllocated + additionalBytes) <= heapLimit_;
}

void GarbageCollector::reportAllocation(size_t bytes) {
    // Track the allocation
    stats_.currentlyAllocated += bytes;
    stats_.totalAllocated += bytes;
    bytesAllocatedSinceGC_ += bytes;

    if (stats_.currentlyAllocated > stats_.peakAllocated) {
        stats_.peakAllocated = stats_.currentlyAllocated;
    }

    // Track if we exceeded the heap limit
    if (heapLimitEnabled_ && stats_.currentlyAllocated > heapLimit_) {
        stats_.heapLimitExceeded++;
    }

    collectIfNeeded();
}

void GarbageCollector::reportDeallocation(size_t bytes) {
    if (stats_.currentlyAllocated >= bytes) {
        stats_.currentlyAllocated -= bytes;
    }
    stats_.totalFreed += bytes;
}

} // namespace lightjs