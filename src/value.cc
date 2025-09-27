#include "value.h"
#include <sstream>
#include <cmath>

namespace tinyjs {

bool Value::toBool() const {
  return std::visit([](auto&& arg) -> bool {
    using T = std::decay_t<decltype(arg)>;
    if constexpr (std::is_same_v<T, Undefined>) {
      return false;
    } else if constexpr (std::is_same_v<T, Null>) {
      return false;
    } else if constexpr (std::is_same_v<T, bool>) {
      return arg;
    } else if constexpr (std::is_same_v<T, double>) {
      return arg != 0.0 && !std::isnan(arg);
    } else if constexpr (std::is_same_v<T, std::string>) {
      return !arg.empty();
    } else {
      return true;
    }
  }, data);
}

double Value::toNumber() const {
  return std::visit([](auto&& arg) -> double {
    using T = std::decay_t<decltype(arg)>;
    if constexpr (std::is_same_v<T, Undefined>) {
      return std::nan("");
    } else if constexpr (std::is_same_v<T, Null>) {
      return 0.0;
    } else if constexpr (std::is_same_v<T, bool>) {
      return arg ? 1.0 : 0.0;
    } else if constexpr (std::is_same_v<T, double>) {
      return arg;
    } else if constexpr (std::is_same_v<T, std::string>) {
      try {
        return std::stod(arg);
      } catch (...) {
        return std::nan("");
      }
    } else {
      return std::nan("");
    }
  }, data);
}

std::string Value::toString() const {
  return std::visit([](auto&& arg) -> std::string {
    using T = std::decay_t<decltype(arg)>;
    if constexpr (std::is_same_v<T, Undefined>) {
      return "undefined";
    } else if constexpr (std::is_same_v<T, Null>) {
      return "null";
    } else if constexpr (std::is_same_v<T, bool>) {
      return arg ? "true" : "false";
    } else if constexpr (std::is_same_v<T, double>) {
      std::ostringstream oss;
      oss << arg;
      return oss.str();
    } else if constexpr (std::is_same_v<T, std::string>) {
      return arg;
    } else if constexpr (std::is_same_v<T, std::shared_ptr<Function>>) {
      return "[Function]";
    } else if constexpr (std::is_same_v<T, std::shared_ptr<Array>>) {
      return "[Array]";
    } else if constexpr (std::is_same_v<T, std::shared_ptr<Object>>) {
      return "[Object]";
    } else {
      return "";
    }
  }, data);
}

}