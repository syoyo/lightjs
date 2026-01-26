#include "string_table.h"
#include <numeric>

namespace lightjs {

StringTable& StringTable::instance() {
  static StringTable instance;
  return instance;
}

std::shared_ptr<std::string> StringTable::intern(std::string_view str) {
  std::lock_guard<std::mutex> lock(mutex_);

  stats_.totalInterns++;

  // Fast path: check if already interned
  std::string key(str);
  auto it = table_.find(key);

  if (it != table_.end()) {
    stats_.hitCount++;
    return it->second;
  }

  // Slow path: create new interned string
  stats_.missCount++;
  auto interned = std::make_shared<std::string>(std::move(key));
  table_[*interned] = interned;
  stats_.uniqueStrings = table_.size();
  stats_.totalBytes += interned->size();

  return interned;
}

bool StringTable::contains(std::string_view str) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return table_.find(std::string(str)) != table_.end();
}

size_t StringTable::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return table_.size();
}

size_t StringTable::memoryUsage() const {
  std::lock_guard<std::mutex> lock(mutex_);

  // Calculate total memory: strings + map overhead
  size_t stringBytes = 0;
  for (const auto& [key, value] : table_) {
    stringBytes += value->capacity();
  }

  // Estimate map overhead (rough approximation)
  size_t mapOverhead = table_.size() * (sizeof(std::string) + sizeof(std::shared_ptr<std::string>) + 16);

  return stringBytes + mapOverhead;
}

void StringTable::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  table_.clear();
  resetStats();
}

StringTable::Stats StringTable::getStats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Stats current = stats_;
  current.uniqueStrings = table_.size();
  return current;
}

void StringTable::resetStats() {
  stats_ = Stats();
}

} // namespace lightjs
