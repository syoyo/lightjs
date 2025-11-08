#pragma once

#include "value.h"
#include <vector>

namespace lightjs {

// String prototype methods
Value String_charAt(const std::vector<Value>& args);
Value String_charCodeAt(const std::vector<Value>& args);
Value String_codePointAt(const std::vector<Value>& args);
Value String_indexOf(const std::vector<Value>& args);
Value String_lastIndexOf(const std::vector<Value>& args);
Value String_substring(const std::vector<Value>& args);
Value String_substr(const std::vector<Value>& args);
Value String_slice(const std::vector<Value>& args);
Value String_split(const std::vector<Value>& args);
Value String_replace(const std::vector<Value>& args);
Value String_toLowerCase(const std::vector<Value>& args);
Value String_toUpperCase(const std::vector<Value>& args);
Value String_trim(const std::vector<Value>& args);

// String static methods
Value String_fromCharCode(const std::vector<Value>& args);
Value String_fromCodePoint(const std::vector<Value>& args);

} // namespace lightjs