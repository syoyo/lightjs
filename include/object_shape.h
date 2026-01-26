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

/**
 * Shape-based object with optimized property storage
 */
struct ShapedObject {
  std::shared_ptr<ObjectShape> shape;
  std::vector<class Value> slots;  // Dense array indexed by property offset

  // For properties added after shape creation (rare)
  std::unordered_map<std::string, class Value> dynamicProperties;

  ShapedObject() : shape(ObjectShape::createRootShape()) {}
  explicit ShapedObject(std::shared_ptr<ObjectShape> s) : shape(s) {
    if (shape) {
      slots.resize(shape->getPropertyCount());
    }
  }

  // Get property by name
  const Value* getProperty(const std::string& name) const;
  Value* getProperty(const std::string& name);

  // Set property by name (may transition shape)
  void setProperty(const std::string& name, const Value& value);

  // Check if property exists
  bool hasProperty(const std::string& name) const;

  // Delete property (converts to dynamic)
  bool deleteProperty(const std::string& name);

  // Get all property names
  std::vector<std::string> getPropertyNames() const;
};

/**
 * Inline cache for property access
 *
 * Caches the shape and offset for fast property lookup
 */
struct PropertyCache {
  ObjectShape::ShapeId shapeId;
  int offset;
  size_t hitCount;
  size_t missCount;

  PropertyCache() : shapeId(0), offset(-1), hitCount(0), missCount(0) {}

  // Try to use cache
  bool tryGet(ObjectShape::ShapeId currentShapeId, int& outOffset) {
    if (currentShapeId == shapeId && offset >= 0) {
      hitCount++;
      outOffset = offset;
      return true;
    }
    missCount++;
    return false;
  }

  // Update cache
  void update(ObjectShape::ShapeId newShapeId, int newOffset) {
    shapeId = newShapeId;
    offset = newOffset;
  }

  // Get hit rate
  double getHitRate() const {
    size_t total = hitCount + missCount;
    return total > 0 ? static_cast<double>(hitCount) / total : 0.0;
  }
};

} // namespace lightjs
