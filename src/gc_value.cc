#include "value.h"
#include "streams.h"
#include "wasm_js.h"
#include "gc.h"
#include "streams.h"
#include "wasm_js.h"
#include "interpreter.h"

namespace lightjs {

// Helper to extract GCObject pointers from Values
static void addValueReferences(const Value& value, std::vector<GCObject*>& refs) {
    if (auto* func = std::get_if<GCPtr<Function>>(&value.data)) {
        if (*func) refs.push_back(func->get());
    } else if (auto* arr = std::get_if<GCPtr<Array>>(&value.data)) {
        if (*arr) refs.push_back(arr->get());
    } else if (auto* obj = std::get_if<GCPtr<Object>>(&value.data)) {
        if (*obj) refs.push_back(obj->get());
    } else if (auto* typed = std::get_if<GCPtr<TypedArray>>(&value.data)) {
        if (*typed) refs.push_back(typed->get());
    } else if (auto* promise = std::get_if<GCPtr<Promise>>(&value.data)) {
        if (*promise) refs.push_back(promise->get());
    } else if (auto* regex = std::get_if<GCPtr<Regex>>(&value.data)) {
        if (*regex) refs.push_back(regex->get());
    } else if (auto* map = std::get_if<GCPtr<Map>>(&value.data)) {
        if (*map) refs.push_back(map->get());
    } else if (auto* set = std::get_if<GCPtr<Set>>(&value.data)) {
        if (*set) refs.push_back(set->get());
    } else if (auto* err = std::get_if<GCPtr<Error>>(&value.data)) {
        if (*err) refs.push_back(err->get());
    } else if (auto* gen = std::get_if<GCPtr<Generator>>(&value.data)) {
        if (*gen) refs.push_back(gen->get());
    } else if (auto* proxy = std::get_if<GCPtr<Proxy>>(&value.data)) {
        if (*proxy) refs.push_back(proxy->get());
    } else if (auto* weakmap = std::get_if<GCPtr<WeakMap>>(&value.data)) {
        if (*weakmap) refs.push_back(weakmap->get());
    } else if (auto* weakset = std::get_if<GCPtr<WeakSet>>(&value.data)) {
        if (*weakset) refs.push_back(weakset->get());
    } else if (auto* readable = std::get_if<GCPtr<ReadableStream>>(&value.data)) {
        if (*readable) refs.push_back(readable->get());
    } else if (auto* writable = std::get_if<GCPtr<WritableStream>>(&value.data)) {
        if (*writable) refs.push_back(writable->get());
    } else if (auto* transform = std::get_if<GCPtr<TransformStream>>(&value.data)) {
        if (*transform) refs.push_back(transform->get());
    } else if (auto* env = std::get_if<GCPtr<Environment>>(&value.data)) {
        if (*env) refs.push_back(env->get());
    }
}

void Value::getReferences(std::vector<GCObject*>& refs) const {
    addValueReferences(*this, refs);
}

void Environment::getReferences(std::vector<GCObject*>& refs) const {
    if (parent_) refs.push_back(parent_.get());
    for (const auto& [name, value] : bindings_) {
        (void)name;
        addValueReferences(value, refs);
    }
}

void Function::getReferences(std::vector<GCObject*>& refs) const {
    if (closure) refs.push_back(closure.get());
    for (const auto& [key, value] : properties) {
        (void)key;
        addValueReferences(value, refs);
    }
}

void Array::getReferences(std::vector<GCObject*>& refs) const {
    for (const auto& element : elements) {
        addValueReferences(element, refs);
    }
    for (const auto& [key, value] : properties) {
        (void)key;
        addValueReferences(value, refs);
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
    for (const auto& [key, value] : properties) {
        (void)key;
        addValueReferences(value, refs);
    }
}

void Regex::getReferences(std::vector<GCObject*>& refs) const {
    for (const auto& [key, value] : properties) {
        (void)key;
        addValueReferences(value, refs);
    }
}

void Map::getReferences(std::vector<GCObject*>& refs) const {
    for (const auto& [key, value] : entries) {
        addValueReferences(key, refs);
        addValueReferences(value, refs);
    }
    for (const auto& [key, value] : properties) {
        (void)key;
        addValueReferences(value, refs);
    }
}

void Set::getReferences(std::vector<GCObject*>& refs) const {
    for (const auto& value : values) {
        addValueReferences(value, refs);
    }
    for (const auto& [key, value] : properties) {
        (void)key;
        addValueReferences(value, refs);
    }
}

void Generator::getReferences(std::vector<GCObject*>& refs) const {
    if (function) refs.push_back(function.get());
    if (context) refs.push_back(context.get());
    for (const auto& [key, value] : properties) {
        (void)key;
        addValueReferences(value, refs);
    }
    if (suspendedTask) {
        static_cast<Task*>(suspendedTask.get())->getReferences(refs);
    }
}

void Proxy::getReferences(std::vector<GCObject*>& refs) const {
    if (target) addValueReferences(*target, refs);
    if (handler) addValueReferences(*handler, refs);
}

// WeakMap implementation
// Helper to extract GCObject* from any GC-held value type
static GCObject* extractGCObject(const Value& key) {
    if (key.isObject()) return key.getGC<Object>().get();
    if (key.isArray()) return key.getGC<Array>().get();
    if (key.isFunction()) return key.getGC<Function>().get();
    if (key.isClass()) return key.getGC<Class>().get();
    if (key.isMap()) return key.getGC<Map>().get();
    if (key.isSet()) return key.getGC<Set>().get();
    if (key.isError()) return key.getGC<Error>().get();
    if (key.isRegex()) return key.getGC<Regex>().get();
    if (key.isPromise()) return key.getGC<Promise>().get();
    if (key.isGenerator()) return key.getGC<Generator>().get();
    if (key.isTypedArray()) return key.getGC<TypedArray>().get();
    if (key.isArrayBuffer()) return key.getGC<ArrayBuffer>().get();
    if (key.isDataView()) return key.getGC<DataView>().get();
    if (key.isWeakMap()) return key.getGC<WeakMap>().get();
    if (key.isWeakSet()) return key.getGC<WeakSet>().get();
    return nullptr;
}

void WeakMap::set(const Value& key, const Value& value) {
    if (key.isSymbol()) {
        symbolEntries[std::get<Symbol>(key.data).id] = value;
        return;
    }
    GCObject* keyObj = extractGCObject(key);
    if (keyObj) {
        entries[keyObj] = value;
    }
}

bool WeakMap::has(const Value& key) const {
    if (key.isSymbol()) {
        return symbolEntries.find(std::get<Symbol>(key.data).id) != symbolEntries.end();
    }
    GCObject* keyObj = extractGCObject(key);
    return keyObj && entries.find(keyObj) != entries.end();
}

Value WeakMap::get(const Value& key) const {
    if (key.isSymbol()) {
        auto it = symbolEntries.find(std::get<Symbol>(key.data).id);
        if (it != symbolEntries.end()) return it->second;
        return Value(Undefined{});
    }
    GCObject* keyObj = extractGCObject(key);
    if (keyObj) {
        auto it = entries.find(keyObj);
        if (it != entries.end()) return it->second;
    }
    return Value(Undefined{});
}

bool WeakMap::deleteKey(const Value& key) {
    if (key.isSymbol()) {
        return symbolEntries.erase(std::get<Symbol>(key.data).id) > 0;
    }
    GCObject* keyObj = extractGCObject(key);
    if (keyObj) {
        return entries.erase(keyObj) > 0;
    }
    return false;
}

void WeakMap::getReferences(std::vector<GCObject*>& refs) const {
    for (const auto& [key, value] : entries) {
        addValueReferences(value, refs);
    }
    for (const auto& [key, value] : symbolEntries) {
        addValueReferences(value, refs);
    }
    for (const auto& [key, value] : properties) {
        (void)key;
        addValueReferences(value, refs);
    }
}

// WeakSet implementation
bool WeakSet::add(const Value& value) {
    if (value.isSymbol()) {
        symbolValues.insert(std::get<Symbol>(value.data).id);
        return true;
    }
    GCObject* obj = extractGCObject(value);
    if (obj) {
        values.insert(obj);
        return true;
    }
    return false;
}

bool WeakSet::has(const Value& value) const {
    if (value.isSymbol()) {
        return symbolValues.find(std::get<Symbol>(value.data).id) != symbolValues.end();
    }
    GCObject* obj = extractGCObject(value);
    return obj && values.find(obj) != values.end();
}

bool WeakSet::deleteValue(const Value& value) {
    if (value.isSymbol()) {
        return symbolValues.erase(std::get<Symbol>(value.data).id) > 0;
    }
    GCObject* obj = extractGCObject(value);
    if (obj) {
        return values.erase(obj) > 0;
    }
    return false;
}

void WeakSet::getReferences(std::vector<GCObject*>& refs) const {
    for (const auto& [key, value] : properties) {
        (void)key;
        addValueReferences(value, refs);
    }
}

void Error::getReferences(std::vector<GCObject*>& refs) const {
    for (const auto& [key, value] : properties) {
        (void)key;
        addValueReferences(value, refs);
    }
}

void ArrayBuffer::getReferences(std::vector<GCObject*>& refs) const {
    for (const auto& view : views) {
        if (view) refs.push_back(view.get());
    }
    for (const auto& [key, value] : properties) {
        (void)key;
        addValueReferences(value, refs);
    }
}

void DataView::getReferences(std::vector<GCObject*>& refs) const {
    if (buffer) refs.push_back(buffer.get());
    for (const auto& [key, value] : properties) {
        (void)key;
        addValueReferences(value, refs);
    }
}

void TypedArray::getReferences(std::vector<GCObject*>& refs) const {
    if (viewedBuffer) refs.push_back(viewedBuffer.get());
    for (const auto& [key, value] : properties) {
        (void)key;
        addValueReferences(value, refs);
    }
}

} // namespace lightjs
