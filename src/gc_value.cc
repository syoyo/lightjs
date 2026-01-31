#include "value.h"
#include "gc.h"
#include "streams.h"

namespace lightjs {

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
    } else if (auto* weakmap = std::get_if<std::shared_ptr<WeakMap>>(&value.data)) {
        if (*weakmap) refs.push_back(weakmap->get());
    } else if (auto* weakset = std::get_if<std::shared_ptr<WeakSet>>(&value.data)) {
        if (*weakset) refs.push_back(weakset->get());
    } else if (auto* readable = std::get_if<std::shared_ptr<ReadableStream>>(&value.data)) {
        if (*readable) refs.push_back(readable->get());
    } else if (auto* writable = std::get_if<std::shared_ptr<WritableStream>>(&value.data)) {
        if (*writable) refs.push_back(writable->get());
    } else if (auto* transform = std::get_if<std::shared_ptr<TransformStream>>(&value.data)) {
        if (*transform) refs.push_back(transform->get());
    }
}

void Function::getReferences(std::vector<GCObject*>& refs) const {
    for (const auto& [key, value] : properties) {
        (void)key;
        addValueReferences(value, refs);
    }
    // Closures would add references here when implemented
}

void Array::getReferences(std::vector<GCObject*>& refs) const {
    for (const auto& element : elements) {
        addValueReferences(element, refs);
    }
}

bool Object::getSlot(int offset, Value& out) const {
    if (useSlots && offset >= 0 && static_cast<size_t>(offset) < slots.size()) {
        out = slots[offset];
        return true;
    }
    return false;
}

void Object::setSlot(int offset, const Value& value) {
    if (offset >= 0) {
        if (static_cast<size_t>(offset) >= slots.size()) {
            slots.resize(offset + 1);
        }
        slots[offset] = value;
        const_cast<Object*>(this)->useSlots = true;
    }
}

void Object::getReferences(std::vector<GCObject*>& refs) const {
    for (const auto& [key, value] : properties) {
        addValueReferences(value, refs);
    }
    // Also add references from slots
    for (const auto& value : slots) {
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

// WeakMap implementation
void WeakMap::set(const Value& key, const Value& value) {
    // WeakMap only accepts objects as keys
    GCObject* keyObj = nullptr;
    if (key.isObject()) {
        keyObj = std::get<std::shared_ptr<Object>>(key.data).get();
    } else if (key.isArray()) {
        keyObj = std::get<std::shared_ptr<Array>>(key.data).get();
    } else if (key.isFunction()) {
        keyObj = std::get<std::shared_ptr<Function>>(key.data).get();
    }

    if (keyObj) {
        entries[keyObj] = value;
    }
}

bool WeakMap::has(const Value& key) const {
    GCObject* keyObj = nullptr;
    if (key.isObject()) {
        keyObj = std::get<std::shared_ptr<Object>>(key.data).get();
    } else if (key.isArray()) {
        keyObj = std::get<std::shared_ptr<Array>>(key.data).get();
    } else if (key.isFunction()) {
        keyObj = std::get<std::shared_ptr<Function>>(key.data).get();
    }

    return keyObj && entries.find(keyObj) != entries.end();
}

Value WeakMap::get(const Value& key) const {
    GCObject* keyObj = nullptr;
    if (key.isObject()) {
        keyObj = std::get<std::shared_ptr<Object>>(key.data).get();
    } else if (key.isArray()) {
        keyObj = std::get<std::shared_ptr<Array>>(key.data).get();
    } else if (key.isFunction()) {
        keyObj = std::get<std::shared_ptr<Function>>(key.data).get();
    }

    if (keyObj) {
        auto it = entries.find(keyObj);
        if (it != entries.end()) {
            return it->second;
        }
    }
    return Value(Undefined{});
}

bool WeakMap::deleteKey(const Value& key) {
    GCObject* keyObj = nullptr;
    if (key.isObject()) {
        keyObj = std::get<std::shared_ptr<Object>>(key.data).get();
    } else if (key.isArray()) {
        keyObj = std::get<std::shared_ptr<Array>>(key.data).get();
    } else if (key.isFunction()) {
        keyObj = std::get<std::shared_ptr<Function>>(key.data).get();
    }

    if (keyObj) {
        return entries.erase(keyObj) > 0;
    }
    return false;
}

void WeakMap::getReferences(std::vector<GCObject*>& refs) const {
    // Note: WeakMap uses weak references for keys, so we don't add them
    // We only add the values
    for (const auto& [key, value] : entries) {
        addValueReferences(value, refs);
    }
}

// WeakSet implementation
bool WeakSet::add(const Value& value) {
    GCObject* obj = nullptr;
    if (value.isObject()) {
        obj = std::get<std::shared_ptr<Object>>(value.data).get();
    } else if (value.isArray()) {
        obj = std::get<std::shared_ptr<Array>>(value.data).get();
    } else if (value.isFunction()) {
        obj = std::get<std::shared_ptr<Function>>(value.data).get();
    }

    if (obj) {
        values.insert(obj);
        return true;
    }
    return false;
}

bool WeakSet::has(const Value& value) const {
    GCObject* obj = nullptr;
    if (value.isObject()) {
        obj = std::get<std::shared_ptr<Object>>(value.data).get();
    } else if (value.isArray()) {
        obj = std::get<std::shared_ptr<Array>>(value.data).get();
    } else if (value.isFunction()) {
        obj = std::get<std::shared_ptr<Function>>(value.data).get();
    }

    return obj && values.find(obj) != values.end();
}

bool WeakSet::deleteValue(const Value& value) {
    GCObject* obj = nullptr;
    if (value.isObject()) {
        obj = std::get<std::shared_ptr<Object>>(value.data).get();
    } else if (value.isArray()) {
        obj = std::get<std::shared_ptr<Array>>(value.data).get();
    } else if (value.isFunction()) {
        obj = std::get<std::shared_ptr<Function>>(value.data).get();
    }

    if (obj) {
        return values.erase(obj) > 0;
    }
    return false;
}

void WeakSet::getReferences(std::vector<GCObject*>& refs) const {
    // WeakSet uses weak references, so we don't add them to refs
    // This allows the GC to collect objects in the WeakSet
}

} // namespace lightjs
