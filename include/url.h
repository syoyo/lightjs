#pragma once

#include "value.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace lightjs {

/**
 * URLSearchParams - Query string manipulation
 *
 * Web API compatible implementation
 * https://developer.mozilla.org/en-US/docs/Web/API/URLSearchParams
 */
struct URLSearchParams : public GCObject {
  // Store as vector to maintain insertion order
  std::vector<std::pair<std::string, std::string>> params;

  URLSearchParams() = default;
  explicit URLSearchParams(const std::string& query);

  // Add a new key-value pair
  void append(const std::string& name, const std::string& value);

  // Delete all entries with the given name
  void deleteKey(const std::string& name);

  // Get first value for a name
  std::string get(const std::string& name) const;

  // Get all values for a name
  std::vector<std::string> getAll(const std::string& name) const;

  // Check if name exists
  bool has(const std::string& name) const;

  // Set value (replaces all existing)
  void set(const std::string& name, const std::string& value);

  // Sort all key-value pairs by key
  void sort();

  // Convert to query string
  std::string toString() const;

  // Get number of parameters
  size_t size() const { return params.size(); }

  // GCObject interface
  const char* typeName() const override { return "URLSearchParams"; }
  void getReferences(std::vector<GCObject*>& refs) const override {}
};

/**
 * URL - URL parsing and manipulation
 *
 * Web API compatible implementation
 * https://developer.mozilla.org/en-US/docs/Web/API/URL
 */
struct URL : public GCObject {
  std::string href;           // Full URL
  std::string protocol;       // e.g., "https:"
  std::string username;       // Username (if any)
  std::string password;       // Password (if any)
  std::string hostname;       // e.g., "example.com"
  std::string port;           // Port number (empty if default)
  std::string pathname;       // e.g., "/path/to/page"
  std::string search;         // Query string including '?'
  std::string hash;           // Fragment including '#'

  std::shared_ptr<URLSearchParams> searchParams;

  URL() = default;
  explicit URL(const std::string& url, const std::string& base = "");

  // Parse a URL string
  void parse(const std::string& url);

  // Get host (hostname:port)
  std::string getHost() const;

  // Get origin (protocol://host)
  std::string getOrigin() const;

  // Convert to string
  std::string toString() const { return href; }

  // Update href when components change
  void updateHref();

  // GCObject interface
  const char* typeName() const override { return "URL"; }
  void getReferences(std::vector<GCObject*>& refs) const override {
    if (searchParams) refs.push_back(searchParams.get());
  }
};

/**
 * Percent-encode a string for URL
 */
std::string percentEncode(const std::string& str, bool encodeSlash = true);

/**
 * Percent-decode a string from URL
 */
std::string percentDecode(const std::string& str);

/**
 * Create URL constructor for JavaScript
 */
std::shared_ptr<Function> createURLConstructor();

/**
 * Create URLSearchParams constructor for JavaScript
 */
std::shared_ptr<Function> createURLSearchParamsConstructor();

} // namespace lightjs
