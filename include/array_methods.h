#pragma once

#include "value.h"
#include <vector>

namespace tinyjs {

// Array prototype methods
Value Array_push(const std::vector<Value>& args);
Value Array_pop(const std::vector<Value>& args);
Value Array_shift(const std::vector<Value>& args);
Value Array_unshift(const std::vector<Value>& args);
Value Array_slice(const std::vector<Value>& args);
Value Array_splice(const std::vector<Value>& args);
Value Array_join(const std::vector<Value>& args);
Value Array_indexOf(const std::vector<Value>& args);
Value Array_includes(const std::vector<Value>& args);
Value Array_reverse(const std::vector<Value>& args);
Value Array_concat(const std::vector<Value>& args);

} // namespace tinyjs