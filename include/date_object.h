#pragma once

#include "value.h"
#include <vector>

namespace tinyjs {

// Date constructor and static methods
Value Date_constructor(const std::vector<Value>& args);
Value Date_now(const std::vector<Value>& args);
Value Date_parse(const std::vector<Value>& args);

// Date prototype methods
Value Date_getTime(const std::vector<Value>& args);
Value Date_getFullYear(const std::vector<Value>& args);
Value Date_getMonth(const std::vector<Value>& args);
Value Date_getDate(const std::vector<Value>& args);
Value Date_getDay(const std::vector<Value>& args);
Value Date_getHours(const std::vector<Value>& args);
Value Date_getMinutes(const std::vector<Value>& args);
Value Date_getSeconds(const std::vector<Value>& args);
Value Date_toString(const std::vector<Value>& args);
Value Date_toISOString(const std::vector<Value>& args);

} // namespace tinyjs