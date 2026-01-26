#include "url.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace lightjs {

// Percent encoding/decoding helpers

static bool shouldEncode(char c, bool encodeSlash) {
  // Unreserved characters: A-Z a-z 0-9 - _ . ~
  if (std::isalnum(static_cast<unsigned char>(c))) return false;
  if (c == '-' || c == '_' || c == '.' || c == '~') return false;
  if (!encodeSlash && c == '/') return false;
  return true;
}

static char hexDigit(int value) {
  return value < 10 ? '0' + value : 'A' + (value - 10);
}

static int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

std::string percentEncode(const std::string& str, bool encodeSlash) {
  std::string result;
  result.reserve(str.size() * 3); // Worst case

  for (char c : str) {
    if (shouldEncode(c, encodeSlash)) {
      result += '%';
      result += hexDigit((static_cast<unsigned char>(c) >> 4) & 0xF);
      result += hexDigit(static_cast<unsigned char>(c) & 0xF);
    } else {
      result += c;
    }
  }

  return result;
}

std::string percentDecode(const std::string& str) {
  std::string result;
  result.reserve(str.size());

  for (size_t i = 0; i < str.size(); i++) {
    if (str[i] == '%' && i + 2 < str.size()) {
      int hi = hexValue(str[i + 1]);
      int lo = hexValue(str[i + 2]);
      if (hi >= 0 && lo >= 0) {
        result += static_cast<char>((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    result += str[i];
  }

  return result;
}

// URLSearchParams implementation

URLSearchParams::URLSearchParams(const std::string& query) {
  if (query.empty()) return;

  // Skip leading '?'
  std::string q = query[0] == '?' ? query.substr(1) : query;

  // Parse query string: key1=value1&key2=value2
  size_t start = 0;
  while (start < q.size()) {
    size_t ampPos = q.find('&', start);
    std::string pair = (ampPos == std::string::npos) ?
                       q.substr(start) :
                       q.substr(start, ampPos - start);

    size_t eqPos = pair.find('=');
    if (eqPos != std::string::npos) {
      std::string key = percentDecode(pair.substr(0, eqPos));
      std::string value = percentDecode(pair.substr(eqPos + 1));
      params.emplace_back(key, value);
    } else {
      // Key without value
      params.emplace_back(percentDecode(pair), "");
    }

    if (ampPos == std::string::npos) break;
    start = ampPos + 1;
  }
}

void URLSearchParams::append(const std::string& name, const std::string& value) {
  params.emplace_back(name, value);
}

void URLSearchParams::deleteKey(const std::string& name) {
  params.erase(
    std::remove_if(params.begin(), params.end(),
      [&name](const auto& p) { return p.first == name; }),
    params.end()
  );
}

std::string URLSearchParams::get(const std::string& name) const {
  for (const auto& [key, value] : params) {
    if (key == name) return value;
  }
  return "";
}

std::vector<std::string> URLSearchParams::getAll(const std::string& name) const {
  std::vector<std::string> result;
  for (const auto& [key, value] : params) {
    if (key == name) result.push_back(value);
  }
  return result;
}

bool URLSearchParams::has(const std::string& name) const {
  for (const auto& [key, value] : params) {
    if (key == name) return true;
  }
  return false;
}

void URLSearchParams::set(const std::string& name, const std::string& value) {
  bool found = false;
  params.erase(
    std::remove_if(params.begin(), params.end(),
      [&](const auto& p) {
        if (p.first == name) {
          if (!found) {
            // Keep the first one but update value
            const_cast<std::string&>(p.second) = value;
            found = true;
            return false;
          }
          return true; // Remove duplicates
        }
        return false;
      }),
    params.end()
  );

  if (!found) {
    params.emplace_back(name, value);
  }
}

void URLSearchParams::sort() {
  std::sort(params.begin(), params.end(),
    [](const auto& a, const auto& b) { return a.first < b.first; });
}

std::string URLSearchParams::toString() const {
  if (params.empty()) return "";

  std::ostringstream oss;
  bool first = true;

  for (const auto& [key, value] : params) {
    if (!first) oss << '&';
    oss << percentEncode(key) << '=' << percentEncode(value);
    first = false;
  }

  return oss.str();
}

// URL implementation

URL::URL(const std::string& url, const std::string& base) {
  // Simple URL parsing (not fully spec-compliant, but functional)
  parse(url);
  searchParams = std::make_shared<URLSearchParams>(search);
}

void URL::parse(const std::string& url) {
  href = url;
  size_t pos = 0;

  // Parse protocol
  size_t colonPos = url.find("://");
  if (colonPos != std::string::npos) {
    protocol = url.substr(0, colonPos + 1); // Include ':'
    pos = colonPos + 3; // Skip "://"
  } else {
    // No protocol, might be relative
    protocol = "";
  }

  // Find end of authority (before path, query, or hash)
  size_t pathStart = url.find('/', pos);
  size_t queryStart = url.find('?', pos);
  size_t hashStart = url.find('#', pos);

  size_t authorityEnd = std::string::npos;
  if (pathStart != std::string::npos) authorityEnd = pathStart;
  if (queryStart != std::string::npos && (authorityEnd == std::string::npos || queryStart < authorityEnd))
    authorityEnd = queryStart;
  if (hashStart != std::string::npos && (authorityEnd == std::string::npos || hashStart < authorityEnd))
    authorityEnd = hashStart;

  // Parse authority (username:password@hostname:port)
  std::string authority = (authorityEnd == std::string::npos) ?
                          url.substr(pos) :
                          url.substr(pos, authorityEnd - pos);

  // Check for userinfo (username:password@)
  size_t atPos = authority.find('@');
  if (atPos != std::string::npos) {
    std::string userinfo = authority.substr(0, atPos);
    size_t colonPos = userinfo.find(':');
    if (colonPos != std::string::npos) {
      username = userinfo.substr(0, colonPos);
      password = userinfo.substr(colonPos + 1);
    } else {
      username = userinfo;
    }
    authority = authority.substr(atPos + 1);
  }

  // Parse hostname and port
  size_t portPos = authority.rfind(':');
  if (portPos != std::string::npos && portPos > authority.rfind(']')) {
    // Has port (and not an IPv6 address)
    hostname = authority.substr(0, portPos);
    port = authority.substr(portPos + 1);
  } else {
    hostname = authority;
    port = "";
  }

  // Parse pathname
  if (authorityEnd == std::string::npos || authorityEnd >= url.size()) {
    pathname = "/";
    search = "";
    hash = "";
    return;
  }

  pos = authorityEnd;

  // Find query and hash
  queryStart = url.find('?', pos);
  hashStart = url.find('#', pos);

  if (queryStart != std::string::npos) {
    pathname = url.substr(pos, queryStart - pos);
    if (hashStart != std::string::npos) {
      search = url.substr(queryStart, hashStart - queryStart);
      hash = url.substr(hashStart);
    } else {
      search = url.substr(queryStart);
      hash = "";
    }
  } else if (hashStart != std::string::npos) {
    pathname = url.substr(pos, hashStart - pos);
    search = "";
    hash = url.substr(hashStart);
  } else {
    pathname = url.substr(pos);
    search = "";
    hash = "";
  }

  if (pathname.empty()) pathname = "/";
}

std::string URL::getHost() const {
  return port.empty() ? hostname : (hostname + ":" + port);
}

std::string URL::getOrigin() const {
  return protocol + "//" + getHost();
}

void URL::updateHref() {
  std::ostringstream oss;

  if (!protocol.empty()) {
    oss << protocol << "//";
  }

  if (!username.empty()) {
    oss << username;
    if (!password.empty()) {
      oss << ":" << password;
    }
    oss << "@";
  }

  oss << hostname;

  if (!port.empty()) {
    oss << ":" << port;
  }

  oss << pathname;

  if (searchParams && !searchParams->params.empty()) {
    oss << "?" << searchParams->toString();
  } else if (!search.empty()) {
    oss << search;
  }

  if (!hash.empty()) {
    oss << hash;
  }

  href = oss.str();
}

// Constructor functions

std::shared_ptr<Function> createURLSearchParamsConstructor() {
  auto constructor = std::make_shared<Function>();
  constructor->isNative = true;
  constructor->isConstructor = true;

  constructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    std::string query;
    if (!args.empty()) {
      query = args[0].toString();
    }

    auto params = std::make_shared<URLSearchParams>(query);

    // Create wrapper object with methods
    auto obj = std::make_shared<Object>();

    // append method
    auto appendFn = std::make_shared<Function>();
    appendFn->isNative = true;
    appendFn->nativeFunc = [params](const std::vector<Value>& args) -> Value {
      if (args.size() >= 2) {
        params->append(args[0].toString(), args[1].toString());
      }
      return Value(Undefined{});
    };
    obj->properties["append"] = Value(appendFn);

    // delete method
    auto deleteFn = std::make_shared<Function>();
    deleteFn->isNative = true;
    deleteFn->nativeFunc = [params](const std::vector<Value>& args) -> Value {
      if (!args.empty()) {
        params->deleteKey(args[0].toString());
      }
      return Value(Undefined{});
    };
    obj->properties["delete"] = Value(deleteFn);

    // get method
    auto getFn = std::make_shared<Function>();
    getFn->isNative = true;
    getFn->nativeFunc = [params](const std::vector<Value>& args) -> Value {
      if (args.empty()) return Value(Null{});
      std::string result = params->get(args[0].toString());
      return result.empty() ? Value(Null{}) : Value(result);
    };
    obj->properties["get"] = Value(getFn);

    // getAll method
    auto getAllFn = std::make_shared<Function>();
    getAllFn->isNative = true;
    getAllFn->nativeFunc = [params](const std::vector<Value>& args) -> Value {
      if (args.empty()) {
        return Value(std::make_shared<Array>());
      }
      auto values = params->getAll(args[0].toString());
      auto arr = std::make_shared<Array>();
      for (const auto& v : values) {
        arr->elements.push_back(Value(v));
      }
      return Value(arr);
    };
    obj->properties["getAll"] = Value(getAllFn);

    // has method
    auto hasFn = std::make_shared<Function>();
    hasFn->isNative = true;
    hasFn->nativeFunc = [params](const std::vector<Value>& args) -> Value {
      if (args.empty()) return Value(false);
      return Value(params->has(args[0].toString()));
    };
    obj->properties["has"] = Value(hasFn);

    // set method
    auto setFn = std::make_shared<Function>();
    setFn->isNative = true;
    setFn->nativeFunc = [params](const std::vector<Value>& args) -> Value {
      if (args.size() >= 2) {
        params->set(args[0].toString(), args[1].toString());
      }
      return Value(Undefined{});
    };
    obj->properties["set"] = Value(setFn);

    // sort method
    auto sortFn = std::make_shared<Function>();
    sortFn->isNative = true;
    sortFn->nativeFunc = [params](const std::vector<Value>& args) -> Value {
      params->sort();
      return Value(Undefined{});
    };
    obj->properties["sort"] = Value(sortFn);

    // toString method
    auto toStringFn = std::make_shared<Function>();
    toStringFn->isNative = true;
    toStringFn->nativeFunc = [params](const std::vector<Value>& args) -> Value {
      return Value(params->toString());
    };
    obj->properties["toString"] = Value(toStringFn);

    return Value(obj);
  };

  return constructor;
}

std::shared_ptr<Function> createURLConstructor() {
  auto constructor = std::make_shared<Function>();
  constructor->isNative = true;
  constructor->isConstructor = true;

  constructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      throw std::runtime_error("URL constructor requires at least 1 argument");
    }

    std::string urlStr = args[0].toString();
    std::string base = args.size() > 1 ? args[1].toString() : "";

    auto url = std::make_shared<URL>(urlStr, base);

    // Create wrapper object with properties
    auto obj = std::make_shared<Object>();

    obj->properties["href"] = Value(url->href);
    obj->properties["protocol"] = Value(url->protocol);
    obj->properties["username"] = Value(url->username);
    obj->properties["password"] = Value(url->password);
    obj->properties["hostname"] = Value(url->hostname);
    obj->properties["port"] = Value(url->port);
    obj->properties["pathname"] = Value(url->pathname);
    obj->properties["search"] = Value(url->search);
    obj->properties["hash"] = Value(url->hash);
    obj->properties["host"] = Value(url->getHost());
    obj->properties["origin"] = Value(url->getOrigin());

    // Add searchParams
    auto searchParamsConstructor = createURLSearchParamsConstructor();
    auto searchParamsObj = searchParamsConstructor->nativeFunc({Value(url->search)});
    obj->properties["searchParams"] = searchParamsObj;

    // toString method
    auto toStringFn = std::make_shared<Function>();
    toStringFn->isNative = true;
    toStringFn->nativeFunc = [url](const std::vector<Value>& args) -> Value {
      return Value(url->toString());
    };
    obj->properties["toString"] = Value(toStringFn);

    return Value(obj);
  };

  return constructor;
}

} // namespace lightjs
