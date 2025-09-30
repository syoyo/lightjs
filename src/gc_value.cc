#include "value.h"
#include "gc.h"

namespace tinyjs {

// Helper to extract GCObject pointers from Values
static void addValueReferences(const Value& value, std::vector<GCObject*>& refs) {
    if (auto* func = std::get_if<std::shared_ptr<Function>>(&value.data)) {
        if (*func) refs.push_back(func->get());
    } else if (auto* arr = std::get_if<std::shared_ptr<Array>>(&value.data)) {
        if (*arr) refs.push_back(arr->get());
    } else if (auto* obj = std::get_if<std::shared_ptr<Object>>(&value.data)) {
        if (*obj) refs.push_back(obj->get());
    } else if (auto* typed = std::get_if<std::shared_ptr<TypedArray>>(&value.data)) {
        if (*typed) refs.push_back(typed->get());
    } else if (auto* promise = std::get_if<std::shared_ptr<Promise>>(&value.data)) {
        if (*promise) refs.push_back(promise->get());
    } else if (auto* regex = std::get_if<std::shared_ptr<Regex>>(&value.data)) {
        if (*regex) refs.push_back(regex->get());
    }
}

void Function::getReferences(std::vector<GCObject*>& refs) const {
    // Functions may reference other objects through closures
    // This would need to be implemented based on closure structure
}

void Array::getReferences(std::vector<GCObject*>& refs) const {
    for (const auto& element : elements) {
        addValueReferences(element, refs);
    }
}

void Object::getReferences(std::vector<GCObject*>& refs) const {
    for (const auto& [key, value] : properties) {
        addValueReferences(value, refs);
    }
}

void Promise::getReferences(std::vector<GCObject*>& refs) const {
    addValueReferences(result, refs);
}

} // namespace tinyjs