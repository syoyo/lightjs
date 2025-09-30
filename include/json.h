#pragma once

#include "value.h"
#include <vector>

namespace tinyjs {

// JSON object methods
Value JSON_parse(const std::vector<Value>& args);
Value JSON_stringify(const std::vector<Value>& args);

} // namespace tinyjs