#pragma once

#include "value.h"
#include <unordered_map>
#include <memory>
#include <optional>

namespace tinyjs {

class Environment : public std::enable_shared_from_this<Environment> {
public:
  Environment() = default;
  explicit Environment(std::shared_ptr<Environment> parent);

  void define(const std::string& name, const Value& value, bool isConst = false);
  std::optional<Value> get(const std::string& name) const;
  bool set(const std::string& name, const Value& value);
  bool has(const std::string& name) const;

  static std::shared_ptr<Environment> createGlobal();
  std::shared_ptr<Environment> createChild();

private:
  std::shared_ptr<Environment> parent_;
  std::unordered_map<std::string, Value> bindings_;
  std::unordered_map<std::string, bool> constants_;
};

}