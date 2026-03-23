#pragma once

#include "value.h"
#include <vector>

namespace lightjs {

// Date constructor and static methods
Value Date_constructor(const std::vector<Value>& args);
Value Date_now(const std::vector<Value>& args);
Value Date_parse(const std::vector<Value>& args);
Value Date_UTC(const std::vector<Value>& args);

// Date prototype methods
Value Date_getTime(const std::vector<Value>& args);
Value Date_getFullYear(const std::vector<Value>& args);
Value Date_getMonth(const std::vector<Value>& args);
Value Date_getDate(const std::vector<Value>& args);
Value Date_getDay(const std::vector<Value>& args);
Value Date_getHours(const std::vector<Value>& args);
Value Date_getMinutes(const std::vector<Value>& args);
Value Date_getSeconds(const std::vector<Value>& args);
Value Date_getUTCFullYear(const std::vector<Value>& args);
Value Date_getUTCMonth(const std::vector<Value>& args);
Value Date_getUTCDate(const std::vector<Value>& args);
Value Date_getUTCDay(const std::vector<Value>& args);
Value Date_getUTCHours(const std::vector<Value>& args);
Value Date_getUTCMinutes(const std::vector<Value>& args);
Value Date_getUTCSeconds(const std::vector<Value>& args);
Value Date_getMilliseconds(const std::vector<Value>& args);
Value Date_getUTCMilliseconds(const std::vector<Value>& args);
Value Date_getTimezoneOffset(const std::vector<Value>& args);
Value Date_setTime(const std::vector<Value>& args);
Value Date_setMilliseconds(const std::vector<Value>& args);
Value Date_setUTCMilliseconds(const std::vector<Value>& args);
Value Date_setSeconds(const std::vector<Value>& args);
Value Date_setUTCSeconds(const std::vector<Value>& args);
Value Date_setMinutes(const std::vector<Value>& args);
Value Date_setUTCMinutes(const std::vector<Value>& args);
Value Date_setHours(const std::vector<Value>& args);
Value Date_setUTCHours(const std::vector<Value>& args);
Value Date_setDate(const std::vector<Value>& args);
Value Date_setUTCDate(const std::vector<Value>& args);
Value Date_setMonth(const std::vector<Value>& args);
Value Date_setUTCMonth(const std::vector<Value>& args);
Value Date_setFullYear(const std::vector<Value>& args);
Value Date_setUTCFullYear(const std::vector<Value>& args);
Value Date_toDateString(const std::vector<Value>& args);
Value Date_toTimeString(const std::vector<Value>& args);
Value Date_toUTCString(const std::vector<Value>& args);
Value Date_toJSON(const std::vector<Value>& args);
Value Date_toTemporalInstant(const std::vector<Value>& args);
Value Date_toString(const std::vector<Value>& args);
Value Date_toISOString(const std::vector<Value>& args);

} // namespace lightjs
