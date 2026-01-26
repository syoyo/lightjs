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

// ShapedObject implementation

const Value* ShapedObject::getProperty(const std::string& name) const {
  // Try shape-based lookup first
  int offset = shape->getPropertyOffset(name);
  if (offset >= 0 && offset < static_cast<int>(slots.size())) {
    return &slots[offset];
  }

  // Fall back to dynamic properties
  auto it = dynamicProperties.find(name);
  if (it != dynamicProperties.end()) {
    return &it->second;
  }

  return nullptr;
}

Value* ShapedObject::getProperty(const std::string& name) {
  // Try shape-based lookup first
  int offset = shape->getPropertyOffset(name);
  if (offset >= 0 && offset < static_cast<int>(slots.size())) {
    return &slots[offset];
  }

  // Fall back to dynamic properties
  auto it = dynamicProperties.find(name);
  if (it != dynamicProperties.end()) {
    return &it->second;
  }

  return nullptr;
}

void ShapedObject::setProperty(const std::string& name, const Value& value) {
  // Check if property already exists in shape
  int offset = shape->getPropertyOffset(name);
  if (offset >= 0 && offset < static_cast<int>(slots.size())) {
    slots[offset] = value;
    return;
  }

  // Check if property exists in dynamic properties
  auto it = dynamicProperties.find(name);
  if (it != dynamicProperties.end()) {
    it->second = value;
    return;
  }

  // New property: try to transition shape
  // Only transition if we don't have too many dynamic properties
  if (dynamicProperties.empty() && slots.size() < 32) {
    // Transition to new shape
    shape = shape->addProperty(name);
    slots.resize(shape->getPropertyCount());
    offset = shape->getPropertyOffset(name);
    slots[offset] = value;
  } else {
    // Too many properties or already have dynamic properties
    // Just add to dynamic properties
    dynamicProperties[name] = value;
  }
}

bool ShapedObject::hasProperty(const std::string& name) const {
  return shape->hasProperty(name) || dynamicProperties.find(name) != dynamicProperties.end();
}

bool ShapedObject::deleteProperty(const std::string& name) {
  // Check dynamic properties first
  auto it = dynamicProperties.find(name);
  if (it != dynamicProperties.end()) {
    dynamicProperties.erase(it);
    return true;
  }

  // If property is in shape, we need to convert to dynamic
  int offset = shape->getPropertyOffset(name);
  if (offset >= 0 && offset < static_cast<int>(slots.size())) {
    // Copy all properties to dynamic
    for (size_t i = 0; i < slots.size(); i++) {
      const auto& propName = shape->getPropertyNames()[i];
      if (propName != name) {
        dynamicProperties[propName] = slots[i];
      }
    }

    // Clear shape and slots
    slots.clear();
    shape = ObjectShape::createRootShape();
    return true;
  }

  return false;
}

std::vector<std::string> ShapedObject::getPropertyNames() const {
  std::vector<std::string> names;

  // Add shape properties
  const auto& shapeProps = shape->getPropertyNames();
  names.insert(names.end(), shapeProps.begin(), shapeProps.end());

  // Add dynamic properties
  for (const auto& [name, _] : dynamicProperties) {
    names.push_back(name);
  }

  return names;
}

} // namespace lightjs
