#include "value.h"
#include "gc.h"
#include <stdexcept>
#include <algorithm>

namespace lightjs {

// Array.prototype.push
Value Array_push(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isArray()) {
        throw std::runtime_error("Array.push called on non-array");
    }

    auto arr = std::get<std::shared_ptr<Array>>(args[0].data);

    // Push all arguments to the array
    for (size_t i = 1; i < args.size(); ++i) {
        arr->elements.push_back(args[i]);
    }

    return Value(static_cast<double>(arr->elements.size()));
}

// Array.prototype.pop
Value Array_pop(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isArray()) {
        throw std::runtime_error("Array.pop called on non-array");
    }

    auto arr = std::get<std::shared_ptr<Array>>(args[0].data);

    if (arr->elements.empty()) {
        return Value(Undefined{});
    }

    Value result = arr->elements.back();
    arr->elements.pop_back();
    return result;
}

// Array.prototype.shift
Value Array_shift(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isArray()) {
        throw std::runtime_error("Array.shift called on non-array");
    }

    auto arr = std::get<std::shared_ptr<Array>>(args[0].data);

    if (arr->elements.empty()) {
        return Value(Undefined{});
    }

    Value result = arr->elements.front();
    arr->elements.erase(arr->elements.begin());
    return result;
}

// Array.prototype.unshift
Value Array_unshift(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isArray()) {
        throw std::runtime_error("Array.unshift called on non-array");
    }

    auto arr = std::get<std::shared_ptr<Array>>(args[0].data);

    // Insert all arguments at the beginning
    for (size_t i = 1; i < args.size(); ++i) {
        arr->elements.insert(arr->elements.begin() + (i - 1), args[i]);
    }

    return Value(static_cast<double>(arr->elements.size()));
}

// Array.prototype.slice
Value Array_slice(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isArray()) {
        throw std::runtime_error("Array.slice called on non-array");
    }

    auto arr = std::get<std::shared_ptr<Array>>(args[0].data);
    auto result = std::make_shared<Array>();
    GarbageCollector::instance().reportAllocation(sizeof(Array));

    size_t len = arr->elements.size();
    int start = 0;
    int end = static_cast<int>(len);

    if (args.size() > 1 && args[1].isNumber()) {
        start = static_cast<int>(std::get<double>(args[1].data));
        if (start < 0) start = std::max(0, static_cast<int>(len) + start);
        if (start > static_cast<int>(len)) start = len;
    }

    if (args.size() > 2 && args[2].isNumber()) {
        end = static_cast<int>(std::get<double>(args[2].data));
        if (end < 0) end = std::max(0, static_cast<int>(len) + end);
        if (end > static_cast<int>(len)) end = len;
    }

    for (int i = start; i < end; ++i) {
        result->elements.push_back(arr->elements[i]);
    }

    return Value(result);
}

// Array.prototype.splice
Value Array_splice(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isArray()) {
        throw std::runtime_error("Array.splice called on non-array");
    }

    auto arr = std::get<std::shared_ptr<Array>>(args[0].data);
    auto removed = std::make_shared<Array>();
    GarbageCollector::instance().reportAllocation(sizeof(Array));

    size_t len = arr->elements.size();
    int start = 0;
    int deleteCount = len;

    if (args.size() > 1 && args[1].isNumber()) {
        start = static_cast<int>(std::get<double>(args[1].data));
        if (start < 0) start = std::max(0, static_cast<int>(len) + start);
        if (start > static_cast<int>(len)) start = len;
    }

    if (args.size() > 2 && args[2].isNumber()) {
        deleteCount = std::max(0, static_cast<int>(std::get<double>(args[2].data)));
        deleteCount = std::min(deleteCount, static_cast<int>(len) - start);
    }

    // Remove elements and store them
    for (int i = 0; i < deleteCount; ++i) {
        removed->elements.push_back(arr->elements[start]);
        arr->elements.erase(arr->elements.begin() + start);
    }

    // Insert new elements
    for (size_t i = 3; i < args.size(); ++i) {
        arr->elements.insert(arr->elements.begin() + start + (i - 3), args[i]);
    }

    return Value(removed);
}

// Array.prototype.join
Value Array_join(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isArray()) {
        throw std::runtime_error("Array.join called on non-array");
    }

    auto arr = std::get<std::shared_ptr<Array>>(args[0].data);
    std::string separator = ",";

    if (args.size() > 1 && args[1].isString()) {
        separator = std::get<std::string>(args[1].data);
    }

    std::string result;
    for (size_t i = 0; i < arr->elements.size(); ++i) {
        if (i > 0) result += separator;
        result += arr->elements[i].toString();
    }

    return Value(result);
}

// Array.prototype.indexOf
Value Array_indexOf(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isArray()) {
        throw std::runtime_error("Array.indexOf called on non-array");
    }

    auto arr = std::get<std::shared_ptr<Array>>(args[0].data);

    if (args.size() < 2) {
        return Value(-1.0);
    }

    Value searchElement = args[1];
    int fromIndex = 0;

    if (args.size() > 2 && args[2].isNumber()) {
        fromIndex = static_cast<int>(std::get<double>(args[2].data));
        if (fromIndex < 0) fromIndex = std::max(0, static_cast<int>(arr->elements.size()) + fromIndex);
    }

    for (size_t i = fromIndex; i < arr->elements.size(); ++i) {
        // Simple equality check - should use SameValueZero
        if (arr->elements[i].toString() == searchElement.toString()) {
            return Value(static_cast<double>(i));
        }
    }

    return Value(-1.0);
}

// Array.prototype.includes
Value Array_includes(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isArray()) {
        throw std::runtime_error("Array.includes called on non-array");
    }

    auto arr = std::get<std::shared_ptr<Array>>(args[0].data);

    if (args.size() < 2) {
        return Value(false);
    }

    Value searchElement = args[1];
    int fromIndex = 0;

    if (args.size() > 2 && args[2].isNumber()) {
        fromIndex = static_cast<int>(std::get<double>(args[2].data));
        if (fromIndex < 0) fromIndex = std::max(0, static_cast<int>(arr->elements.size()) + fromIndex);
    }

    for (size_t i = fromIndex; i < arr->elements.size(); ++i) {
        // Simple equality check
        if (arr->elements[i].toString() == searchElement.toString()) {
            return Value(true);
        }
    }

    return Value(false);
}

// Array.prototype.reverse
Value Array_reverse(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isArray()) {
        throw std::runtime_error("Array.reverse called on non-array");
    }

    auto arr = std::get<std::shared_ptr<Array>>(args[0].data);
    std::reverse(arr->elements.begin(), arr->elements.end());
    return args[0]; // Return the array itself
}

// Array.prototype.concat
Value Array_concat(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isArray()) {
        throw std::runtime_error("Array.concat called on non-array");
    }

    auto arr = std::get<std::shared_ptr<Array>>(args[0].data);
    auto result = std::make_shared<Array>();
    GarbageCollector::instance().reportAllocation(sizeof(Array));

    // Copy original array elements
    result->elements = arr->elements;

    // Add all arguments
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i].isArray()) {
            auto otherArr = std::get<std::shared_ptr<Array>>(args[i].data);
            result->elements.insert(result->elements.end(),
                                  otherArr->elements.begin(),
                                  otherArr->elements.end());
        } else {
            result->elements.push_back(args[i]);
        }
    }

    return Value(result);
}

// Array.prototype.map
Value Array_map(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isArray()) {
        throw std::runtime_error("Array.map called on non-array");
    }
    if (args.size() < 2 || !args[1].isFunction()) {
        throw std::runtime_error("Array.map requires a callback function");
    }

    auto arr = std::get<std::shared_ptr<Array>>(args[0].data);
    auto callback = std::get<std::shared_ptr<Function>>(args[1].data);
    auto result = std::make_shared<Array>();
    GarbageCollector::instance().reportAllocation(sizeof(Array));

    for (size_t i = 0; i < arr->elements.size(); ++i) {
        std::vector<Value> callArgs = {arr->elements[i], Value(static_cast<double>(i)), args[0]};
        Value mapped = callback->nativeFunc ? callback->nativeFunc(callArgs) : Value(Undefined{});
        result->elements.push_back(mapped);
    }

    return Value(result);
}

// Array.prototype.filter
Value Array_filter(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isArray()) {
        throw std::runtime_error("Array.filter called on non-array");
    }
    if (args.size() < 2 || !args[1].isFunction()) {
        throw std::runtime_error("Array.filter requires a callback function");
    }

    auto arr = std::get<std::shared_ptr<Array>>(args[0].data);
    auto callback = std::get<std::shared_ptr<Function>>(args[1].data);
    auto result = std::make_shared<Array>();
    GarbageCollector::instance().reportAllocation(sizeof(Array));

    for (size_t i = 0; i < arr->elements.size(); ++i) {
        std::vector<Value> callArgs = {arr->elements[i], Value(static_cast<double>(i)), args[0]};
        Value keep = callback->nativeFunc ? callback->nativeFunc(callArgs) : Value(Undefined{});
        if (keep.toBool()) {
            result->elements.push_back(arr->elements[i]);
        }
    }

    return Value(result);
}

// Array.prototype.reduce
Value Array_reduce(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isArray()) {
        throw std::runtime_error("Array.reduce called on non-array");
    }
    if (args.size() < 2 || !args[1].isFunction()) {
        throw std::runtime_error("Array.reduce requires a callback function");
    }

    auto arr = std::get<std::shared_ptr<Array>>(args[0].data);
    auto callback = std::get<std::shared_ptr<Function>>(args[1].data);

    if (arr->elements.empty()) {
        return args.size() > 2 ? args[2] : Value(Undefined{});
    }

    Value accumulator = args.size() > 2 ? args[2] : arr->elements[0];
    size_t start = args.size() > 2 ? 0 : 1;

    for (size_t i = start; i < arr->elements.size(); ++i) {
        std::vector<Value> callArgs = {accumulator, arr->elements[i], Value(static_cast<double>(i)), args[0]};
        accumulator = callback->nativeFunc ? callback->nativeFunc(callArgs) : Value(Undefined{});
    }

    return accumulator;
}

// Array.prototype.forEach
Value Array_forEach(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isArray()) {
        throw std::runtime_error("Array.forEach called on non-array");
    }
    if (args.size() < 2 || !args[1].isFunction()) {
        throw std::runtime_error("Array.forEach requires a callback function");
    }

    auto arr = std::get<std::shared_ptr<Array>>(args[0].data);
    auto callback = std::get<std::shared_ptr<Function>>(args[1].data);

    for (size_t i = 0; i < arr->elements.size(); ++i) {
        std::vector<Value> callArgs = {arr->elements[i], Value(static_cast<double>(i)), args[0]};
        if (callback->nativeFunc) {
            callback->nativeFunc(callArgs);
        }
    }

    return Value(Undefined{});
}

} // namespace lightjs