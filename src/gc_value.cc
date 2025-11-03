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
    } else if (auto* map = std::get_if<std::shared_ptr<Map>>(&value.data)) {
        if (*map) refs.push_back(map->get());
    } else if (auto* set = std::get_if<std::shared_ptr<Set>>(&value.data)) {
        if (*set) refs.push_back(set->get());
    } else if (auto* err = std::get_if<std::shared_ptr<Error>>(&value.data)) {
        if (*err) refs.push_back(err->get());
    } else if (auto* gen = std::get_if<std::shared_ptr<Generator>>(&value.data)) {
        if (*gen) refs.push_back(gen->get());
    } else if (auto* proxy = std::get_if<std::shared_ptr<Proxy>>(&value.data)) {
        if (*proxy) refs.push_back(proxy->get());
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
    for (const auto& chainedPromise : chainedPromises) {
        if (chainedPromise) refs.push_back(chainedPromise.get());
    }
}

void Map::getReferences(std::vector<GCObject*>& refs) const {
    for (const auto& [key, value] : entries) {
        addValueReferences(key, refs);
        addValueReferences(value, refs);
    }
}

void Set::getReferences(std::vector<GCObject*>& refs) const {
    for (const auto& value : values) {
        addValueReferences(value, refs);
    }
}

void Proxy::getReferences(std::vector<GCObject*>& refs) const {
    if (target) addValueReferences(*target, refs);
    if (handler) addValueReferences(*handler, refs);
}

} // namespace tinyjs