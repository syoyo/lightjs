#include "environment.h"
#include <iostream>

namespace tinyjs {

Environment::Environment(std::shared_ptr<Environment> parent)
  : parent_(parent) {}

void Environment::define(const std::string& name, const Value& value, bool isConst) {
  bindings_[name] = value;
  if (isConst) {
    constants_[name] = true;
  }
}

std::optional<Value> Environment::get(const std::string& name) const {
  auto it = bindings_.find(name);
  if (it != bindings_.end()) {
    return it->second;
  }
  if (parent_) {
    return parent_->get(name);
  }
  return std::nullopt;
}

bool Environment::set(const std::string& name, const Value& value) {
  auto it = bindings_.find(name);
  if (it != bindings_.end()) {
    if (constants_.find(name) != constants_.end()) {
      return false;
    }
    bindings_[name] = value;
    return true;
  }
  if (parent_) {
    return parent_->set(name, value);
  }
  return false;
}

bool Environment::has(const std::string& name) const {
  if (bindings_.find(name) != bindings_.end()) {
    return true;
  }
  if (parent_) {
    return parent_->has(name);
  }
  return false;
}

std::shared_ptr<Environment> Environment::createChild() {
  return std::make_shared<Environment>(shared_from_this());
}

std::shared_ptr<Environment> Environment::createGlobal() {
  auto env = std::make_shared<Environment>();

  auto consoleFn = std::make_shared<Function>();
  consoleFn->isNative = true;
  consoleFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    for (const auto& arg : args) {
      std::cout << arg.toString() << " ";
    }
    std::cout << std::endl;
    return Value(Undefined{});
  };

  auto consoleObj = std::make_shared<Object>();
  consoleObj->properties["log"] = Value(consoleFn);

  env->define("console", Value(consoleObj));
  env->define("undefined", Value(Undefined{}));

  return env;
}

}