#include "object_shape.h"
#include "value.h"
#include <algorithm>

namespace lightjs {

// Static members
ObjectShape::ShapeId ObjectShape::nextShapeId_ = 1;
std::unordered_map<std::vector<std::string>, std::shared_ptr<ObjectShape>, ObjectShape::VectorHash>
    ObjectShape::shapeCache_;

ObjectShape::ObjectShape() : id_(nextShapeId_++), parent_(nullptr) {}

ObjectShape::ObjectShape(std::shared_ptr<ObjectShape> parent)
    : id_(nextShapeId_++), parent_(parent) {
  if (parent) {
    // Inherit properties from parent
    properties_ = parent->properties_;
    propertyMap_ = parent->propertyMap_;
  }
}

int ObjectShape::getPropertyOffset(const std::string& name) const {
  auto it = propertyMap_.find(name);
  if (it != propertyMap_.end()) {
    return static_cast<int>(it->second);
  }
  return -1;
}

bool ObjectShape::hasProperty(const std::string& name) const {
  return propertyMap_.find(name) != propertyMap_.end();
}

std::shared_ptr<ObjectShape> ObjectShape::addProperty(const std::string& name) {
  // Check if we already have a transition for this property
  auto it = transitions_.find(name);
  if (it != transitions_.end()) {
    return it->second;
  }

  // Create new shape with this property added
  auto newShape = std::make_shared<ObjectShape>(shared_from_this());
  newShape->properties_.push_back(name);
  newShape->propertyMap_[name] = newShape->properties_.size() - 1;

  // Cache the transition
  transitions_[name] = newShape;

  return newShape;
}

std::shared_ptr<ObjectShape> ObjectShape::getShape(const std::vector<std::string>& properties) {
  // Check cache first
  auto it = shapeCache_.find(properties);
  if (it != shapeCache_.end()) {
    return it->second;
  }

  // Create new shape
  auto shape = std::make_shared<ObjectShape>();
  shape->properties_ = properties;
  for (size_t i = 0; i < properties.size(); i++) {
    shape->propertyMap_[properties[i]] = i;
  }

  // Cache it
  shapeCache_[properties] = shape;

  return shape;
}

std::shared_ptr<ObjectShape> ObjectShape::createRootShape() {
  static auto rootShape = std::make_shared<ObjectShape>();
  return rootShape;
}

size_t ObjectShape::getTotalShapeCount() {
  return nextShapeId_ - 1;
}

void ObjectShape::clearShapeCache() {
  shapeCache_.clear();
  nextShapeId_ = 1;
}

// ShapedObject implementation is currently unused - Object uses direct hash map
// with inline caching on shape IDs for optimization. ShapedObject can be
// enabled later if slot-based storage is desired.

} // namespace lightjs
