#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <memory>
#include <mutex>

namespace lightjs {

/**
 * Global string interning table for deduplication
 *
 * Provides:
 * - Memory reduction (20-40% for typical programs)
 * - O(1) string equality via pointer comparison
 * - Cache-friendly property lookup
 *
 * Thread-safe for concurrent access.
 */
class StringTable {
public:
  /**
   * Get the global string table instance
   */
  static StringTable& instance();

  /**
   * Intern a string - returns shared pointer to deduplicated string
   * If the string already exists in the table, returns existing pointer
   * Otherwise, creates new entry and returns pointer
   */
  std::shared_ptr<std::string> intern(std::string_view str);

  /**
   * Check if a string is already interned
   */
  bool contains(std::string_view str) const;

  /**
   * Get statistics about the string table
   */
  size_t size() const;
  size_t memoryUsage() const;

  /**
   * Clear the table (useful for testing)
   */
  void clear();

  /**
   * Get interning statistics
   */
  struct Stats {
    size_t totalInterns = 0;      // Total intern() calls
    size_t uniqueStrings = 0;     // Unique strings stored
    size_t hitCount = 0;          // Cache hits (string already interned)
    size_t missCount = 0;         // Cache misses (new string added)
    size_t totalBytes = 0;        // Total bytes stored

    double hitRate() const {
      return totalInterns > 0 ? (double)hitCount / totalInterns : 0.0;
    }
  };

  Stats getStats() const;
  void resetStats();

private:
  StringTable() = default;
  ~StringTable() = default;

  // Non-copyable, non-movable (singleton)
  StringTable(const StringTable&) = delete;
  StringTable& operator=(const StringTable&) = delete;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<std::string>> table_;

  // Statistics (mutable for const getStats)
  mutable Stats stats_;
};

/**
 * Helper to create interned string from string literal
 */
inline std::shared_ptr<std::string> intern(std::string_view str) {
  return StringTable::instance().intern(str);
}

} // namespace lightjs
