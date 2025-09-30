#include "gc.h"
#include "value.h"
#include <algorithm>
#include <iostream>

namespace tinyjs {

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
    stats_.currentlyAllocated++;
    stats_.totalAllocated++;
    if (stats_.currentlyAllocated > stats_.peakAllocated) {
        stats_.peakAllocated = stats_.currentlyAllocated;
    }
}

void GarbageCollector::unregisterObject(GCObject* obj) {
    if (!obj) return;
    objects_.erase(obj);
    stats_.currentlyAllocated--;
    stats_.totalFreed++;
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
    // This is a simple approximation
    // In a real implementation, you'd track actual memory usage
    return stats_.currentlyAllocated * sizeof(GCObject);
}

void GarbageCollector::reportAllocation(size_t bytes) {
    bytesAllocatedSinceGC_ += bytes;
    collectIfNeeded();
}

void GarbageCollector::reportDeallocation(size_t bytes) {
    // Track deallocations if needed
}

} // namespace tinyjs