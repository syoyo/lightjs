#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>

namespace lightjs {

/**
 * ObjectShape - Hidden class for optimized property storage
 *
 * Objects with the same property names in the same order share a shape.
 * This enables:
 * - Flat array storage (faster than hash map)
 * - Inline caching (cache property offsets)
 * - Memory reduction (shared shapes)
 *
 * Example:
 *   let obj1 = {x: 1, y: 2};
 *   let obj2 = {x: 10, y: 20};
 *   // obj1 and obj2 share the same shape
 */
class ObjectShape : public std::enable_shared_from_this<ObjectShape> {
public:
  // Shape ID for fast comparison
  using ShapeId = uint64_t;

  ObjectShape();
  explicit ObjectShape(std::shared_ptr<ObjectShape> parent);

  // Get unique shape ID
  ShapeId getId() const { return id_; }

  // Get property offset (-1 if not found)
  int getPropertyOffset(const std::string& name) const;

  // Check if shape has property
  bool hasProperty(const std::string& name) const;

  // Get all property names in order
  const std::vector<std::string>& getPropertyNames() const { return properties_; }

  // Get property count
  size_t getPropertyCount() const { return properties_.size(); }

  // Transition: add a new property, returns new shape
  std::shared_ptr<ObjectShape> addProperty(const std::string& name);

  // Get parent shape (for prototype chain)
  std::shared_ptr<ObjectShape> getParent() const { return parent_; }

  // Check if this is the root shape (empty object)
  bool isRoot() const { return properties_.empty() && !parent_; }

  // Get or create a shape with specific properties
  static std::shared_ptr<ObjectShape> getShape(const std::vector<std::string>& properties);

  // Create root shape (empty object)
  static std::shared_ptr<ObjectShape> createRootShape();

  // Statistics
  static size_t getTotalShapeCount();
  static void clearShapeCache();

private:
  // Hash function for vector<string> (must be defined before use)
  struct VectorHash {
    size_t operator()(const std::vector<std::string>& vec) const {
      size_t hash = 0;
      for (const auto& str : vec) {
        // Simple hash combining
        hash ^= std::hash<std::string>{}(str) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
      }
      return hash;
    }
  };

  ShapeId id_;
  std::vector<std::string> properties_;  // Property names in order
  std::unordered_map<std::string, size_t> propertyMap_;  // Name -> offset
  std::shared_ptr<ObjectShape> parent_;  // Parent shape

  // Shape transitions: propertyName -> new shape
  std::unordered_map<std::string, std::shared_ptr<ObjectShape>> transitions_;

  // Global shape cache and ID counter
  static ShapeId nextShapeId_;
  static std::unordered_map<std::vector<std::string>, std::shared_ptr<ObjectShape>, VectorHash> shapeCache_;
};

// Note: ShapedObject is defined in value.h after Value is complete
// This avoids circular dependency issues

/**
 * Polymorphic inline cache for property access
 *
 * Caches multiple shape/offset pairs for fast property lookup
 * at polymorphic call sites (where multiple object types are used)
 */
struct PropertyCache {
  static constexpr size_t MAX_ENTRIES = 4;  // Maximum cache entries (polymorphic)

  struct CacheEntry {
    ObjectShape::ShapeId shapeId = 0;
    int offset = -1;
  };

  CacheEntry entries[MAX_ENTRIES];
  size_t entryCount = 0;
  size_t hitCount = 0;
  size_t missCount = 0;

  PropertyCache() = default;

  // Try to use cache - check all entries
  bool tryGet(ObjectShape::ShapeId currentShapeId, int& outOffset) {
    // Check all cached entries
    for (size_t i = 0; i < entryCount; ++i) {
      if (entries[i].shapeId == currentShapeId && entries[i].offset >= 0) {
        hitCount++;
        outOffset = entries[i].offset;
        // Move to front for better locality (most recently used first)
        if (i > 0) {
          CacheEntry temp = entries[i];
          for (size_t j = i; j > 0; --j) {
            entries[j] = entries[j - 1];
          }
          entries[0] = temp;
        }
        return true;
      }
    }
    missCount++;
    return false;
  }

  // Update cache - add new entry or update existing
  void update(ObjectShape::ShapeId newShapeId, int newOffset) {
    // Check if already in cache
    for (size_t i = 0; i < entryCount; ++i) {
      if (entries[i].shapeId == newShapeId) {
        entries[i].offset = newOffset;
        return;
      }
    }

    // Add new entry
    if (entryCount < MAX_ENTRIES) {
      // Shift entries down to make room at front
      for (size_t i = entryCount; i > 0; --i) {
        entries[i] = entries[i - 1];
      }
      entries[0] = {newShapeId, newOffset};
      entryCount++;
    } else {
      // Cache full - replace oldest entry (last one)
      for (size_t i = MAX_ENTRIES - 1; i > 0; --i) {
        entries[i] = entries[i - 1];
      }
      entries[0] = {newShapeId, newOffset};
    }
  }

  // Get hit rate
  double getHitRate() const {
    size_t total = hitCount + missCount;
    return total > 0 ? static_cast<double>(hitCount) / total : 0.0;
  }

  // Check if cache is megamorphic (too many different shapes)
  bool isMegamorphic() const {
    return entryCount >= MAX_ENTRIES && missCount > hitCount * 2;
  }
};

} // namespace lightjs
