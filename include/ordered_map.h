#pragma once

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace lightjs {

// An unordered_map wrapper that also tracks key insertion order.
// Provides the same API surface as std::unordered_map for the operations
// used in this codebase, plus orderedKeys() for iteration in insertion order.
// This is needed for ES spec-compliant property enumeration order.
template <typename K, typename V>
class OrderedMap {
 public:
  using map_type = std::unordered_map<K, V>;
  using iterator = typename map_type::iterator;
  using const_iterator = typename map_type::const_iterator;
  using value_type = typename map_type::value_type;
  using size_type = typename map_type::size_type;

  OrderedMap() = default;

  // operator[] - inserts key into order vector if new
  V& operator[](const K& key) {
    auto it = map_.find(key);
    if (it == map_.end()) {
      order_.push_back(key);
      return map_[key];
    }
    return it->second;
  }

  V& operator[](K&& key) {
    auto it = map_.find(key);
    if (it == map_.end()) {
      order_.push_back(key);
      return map_[std::move(key)];
    }
    return it->second;
  }

  iterator find(const K& key) { return map_.find(key); }
  const_iterator find(const K& key) const { return map_.find(key); }

  // Unordered iteration (for code that doesn't care about order)
  iterator begin() { return map_.begin(); }
  iterator end() { return map_.end(); }
  const_iterator begin() const { return map_.begin(); }
  const_iterator end() const { return map_.end(); }

  size_type count(const K& key) const { return map_.count(key); }
  size_type size() const { return map_.size(); }
  bool empty() const { return map_.empty(); }

  V& at(const K& key) { return map_.at(key); }
  const V& at(const K& key) const { return map_.at(key); }

  size_type erase(const K& key) {
    auto result = map_.erase(key);
    if (result > 0) {
      order_.erase(std::remove(order_.begin(), order_.end(), key),
                   order_.end());
    }
    return result;
  }

  void clear() {
    map_.clear();
    order_.clear();
  }

  // Keys in insertion order (re-inserted keys appear at new position)
  const std::vector<K>& orderedKeys() const { return order_; }

 private:
  map_type map_;
  std::vector<K> order_;
};

}  // namespace lightjs
