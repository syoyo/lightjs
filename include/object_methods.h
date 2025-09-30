#pragma once

#include "value.h"
#include <vector>

namespace tinyjs {

// Object static methods
Value Object_keys(const std::vector<Value>& args);
Value Object_values(const std::vector<Value>& args);
Value Object_entries(const std::vector<Value>& args);
Value Object_assign(const std::vector<Value>& args);
Value Object_hasOwnProperty(const std::vector<Value>& args);
Value Object_getOwnPropertyNames(const std::vector<Value>& args);
Value Object_create(const std::vector<Value>& args);

} // namespace tinyjs