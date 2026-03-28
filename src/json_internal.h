#pragma once

#include "environment.h"
#include "interpreter.h"
#include "json.h"
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace lightjs::detail {

bool isJSONObjectLike(const Value& value);
bool parseJSONArrayIndex(const std::string& key, size_t& index);
[[noreturn]] void throwInterpreterError(Interpreter* interp);
Value jsonGet(Interpreter* interp, const Value& receiver, const std::string& key);
Value jsonGetMethodForBigInt(Interpreter* interp, const Value& value, const std::string& key);
bool jsonIsArrayValue(Interpreter* interp, const Value& value);
double jsonToLength(Interpreter* interp, Value value);
double jsonLengthOfArrayLike(Interpreter* interp, const Value& value);
bool jsonDeleteProperty(Interpreter* interp, const Value& receiver, const std::string& key);
bool jsonCreateDataProperty(Interpreter* interp,
                            const Value& receiver,
                            const std::string& key,
                            const Value& value);
std::vector<std::string> jsonEnumerableOwnKeys(Interpreter* interp, const Value& value);

class JSONParser {
public:
    explicit JSONParser(const std::string& str);

    Value parse(std::string* outSource = nullptr);

private:
    void skipWhitespace();
    Value parseValue(std::string* outSource = nullptr);
    Value parseString();
    Value parseNumber();
    Value parseTrue();
    Value parseFalse();
    Value parseNull();
    Value parseObject();
    Value parseArray();

    const std::string& str_;
    size_t pos_;
};

class JSONStringifier {
public:
    void setGap(const std::string& g);
    void setReplacer(const Value& fn);
    void setPropertyList(const std::vector<std::string>& list);
    std::string stringify(const Value& value);

private:
    const void* getPointer(const Value& v) const;
    void checkCircular(const Value& v);
    Value callToJSON(const Value& value, const std::string& key);
    Value applyReplacer(const Value& holder, const std::string& key, Value value);
    Value unwrapPrimitive(const Value& value);
    bool serializeValue(const Value& holder, const std::string& key, Value value);
    static int32_t decodeUTF8(const std::string& str, size_t& pos);
    void stringifyString(const std::string& str);
    void serializeArray(const Value& arrayVal);
    void serializeObject(const Value& objVal);
    void stringifyStringTo(std::ostringstream& os, const std::string& str);

    std::ostringstream out_;
    std::string gap_;
    std::string indent_;
    std::vector<const void*> stack_;
    Value replacerFn_ = Value(Undefined{});
    std::vector<std::string> propertyList_;
    bool hasPropertyList_ = false;
};

}  // namespace lightjs::detail
