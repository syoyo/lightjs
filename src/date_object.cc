#include "value.h"
#include "streams.h"
#include "wasm_js.h"
#include "gc.h"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <limits>

namespace lightjs {

// Date object implementation
struct Date : public Object {
    std::chrono::system_clock::time_point timePoint;

    Date() : timePoint(std::chrono::system_clock::now()) {}

    Date(int64_t milliseconds) {
        auto duration = std::chrono::milliseconds(milliseconds);
        timePoint = std::chrono::system_clock::time_point(duration);
    }

    Date(int year, int month, int day = 1, int hour = 0, int minute = 0, int second = 0, int millisecond = 0) {
        std::tm tm = {};
        tm.tm_year = year - 1900;
        tm.tm_mon = month;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = minute;
        tm.tm_sec = second;

        auto timeT = std::mktime(&tm);
        timePoint = std::chrono::system_clock::from_time_t(timeT);
        timePoint += std::chrono::milliseconds(millisecond);
    }

    int64_t getTime() const {
        auto duration = timePoint.time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    }

    void setTime(int64_t milliseconds) {
        auto duration = std::chrono::milliseconds(milliseconds);
        timePoint = std::chrono::system_clock::time_point(duration);
    }

    std::tm toTm() const {
        auto timeT = std::chrono::system_clock::to_time_t(timePoint);
        std::tm tm;
        #ifdef _WIN32
        localtime_s(&tm, &timeT);
        #else
        localtime_r(&timeT, &tm);
        #endif
        return tm;
    }

    std::tm toUtcTm() const {
        auto timeT = std::chrono::system_clock::to_time_t(timePoint);
        std::tm tm;
        #ifdef _WIN32
        gmtime_s(&tm, &timeT);
        #else
        gmtime_r(&timeT, &tm);
        #endif
        return tm;
    }

    std::string toString() const {
        auto tm = toTm();
        std::ostringstream oss;
        oss << std::put_time(&tm, "%a %b %d %Y %H:%M:%S");
        return oss.str();
    }

    std::string toISOString() const {
        auto tm = toUtcTm();
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");

        // Add milliseconds
        auto ms = getTime() % 1000;
        oss << "." << std::setfill('0') << std::setw(3) << ms << "Z";

        return oss.str();
    }

    // GCObject interface
    const char* typeName() const override { return "Date"; }
    void getReferences(std::vector<GCObject*>& refs) const override {}
};

// Date constructor
Value Date_constructor(const std::vector<Value>& args) {
    auto date = GarbageCollector::makeGC<Date>();

    if (args.size() == 1 && args[0].isNumber()) {
        // Date(milliseconds)
        int64_t ms = static_cast<int64_t>(std::get<double>(args[0].data));
        date->setTime(ms);
    } else if (args.size() >= 2) {
        // Date(year, month, day?, hour?, minute?, second?, millisecond?)
        int year = args[0].isUndefined() ? 1970 : static_cast<int>(args[0].toNumber());
        int month = args[1].isUndefined() ? 0 : static_cast<int>(args[1].toNumber());
        int day = args.size() > 2 && !args[2].isUndefined() ? static_cast<int>(args[2].toNumber()) : 1;
        int hour = args.size() > 3 && !args[3].isUndefined() ? static_cast<int>(args[3].toNumber()) : 0;
        int minute = args.size() > 4 && !args[4].isUndefined() ? static_cast<int>(args[4].toNumber()) : 0;
        int second = args.size() > 5 && !args[5].isUndefined() ? static_cast<int>(args[5].toNumber()) : 0;
        int ms = args.size() > 6 && !args[6].isUndefined() ? static_cast<int>(args[6].toNumber()) : 0;

        date = GarbageCollector::makeGC<Date>(year, month, day, hour, minute, second, ms);
    }

    // Store time value for prototype/instance methods.
    date->properties["__date_ms__"] = Value(static_cast<double>(date->getTime()));

    auto readThisMs = [](const std::vector<Value>& args) -> int64_t {
      if (args.empty() || !args[0].isObject()) {
        throw std::runtime_error("TypeError: Date method called on non-object");
      }
      auto obj = args[0].getGC<Object>();
      auto it = obj->properties.find("__date_ms__");
      if (it == obj->properties.end() || !it->second.isNumber()) {
        throw std::runtime_error("TypeError: Date method called on incompatible receiver");
      }
      return static_cast<int64_t>(it->second.toNumber());
    };

    auto install = [&](const std::string& name, std::function<Value(const std::vector<Value>&)> fn) {
      auto f = GarbageCollector::makeGC<Function>();
      f->isNative = true;
      f->properties["__uses_this_arg__"] = Value(true);
      f->properties["__throw_on_new__"] = Value(true);
      f->nativeFunc = fn;
      date->properties[name] = Value(f);
      date->properties["__non_enum_" + name] = Value(true);
    };

    install("getFullYear", [readThisMs](const std::vector<Value>& args) -> Value {
      int64_t ms = readThisMs(args);
      Date tmp(ms);
      auto tm = tmp.toTm();
      return Value(static_cast<double>(tm.tm_year + 1900));
    });
    install("getMonth", [readThisMs](const std::vector<Value>& args) -> Value {
      int64_t ms = readThisMs(args);
      Date tmp(ms);
      auto tm = tmp.toTm();
      return Value(static_cast<double>(tm.tm_mon));
    });
    install("getDate", [readThisMs](const std::vector<Value>& args) -> Value {
      int64_t ms = readThisMs(args);
      Date tmp(ms);
      auto tm = tmp.toTm();
      return Value(static_cast<double>(tm.tm_mday));
    });
    install("getUTCFullYear", [readThisMs](const std::vector<Value>& args) -> Value {
      int64_t ms = readThisMs(args);
      Date tmp(ms);
      auto tm = tmp.toUtcTm();
      return Value(static_cast<double>(tm.tm_year + 1900));
    });
    install("getUTCMonth", [readThisMs](const std::vector<Value>& args) -> Value {
      int64_t ms = readThisMs(args);
      Date tmp(ms);
      auto tm = tmp.toUtcTm();
      return Value(static_cast<double>(tm.tm_mon));
    });
    install("getUTCDate", [readThisMs](const std::vector<Value>& args) -> Value {
      int64_t ms = readThisMs(args);
      Date tmp(ms);
      auto tm = tmp.toUtcTm();
      return Value(static_cast<double>(tm.tm_mday));
    });

    auto toStringFn = GarbageCollector::makeGC<Function>();
    toStringFn->isNative = true;
    toStringFn->nativeFunc = [](const std::vector<Value>&) -> Value {
        return Value(std::string("[object Date]"));
    };
    date->properties["toString"] = Value(toStringFn);
    date->properties["__is_date__"] = Value(true);
    return Value(GCPtr<Object>(static_cast<Object*>(date.get())));
}

// Date.now
Value Date_now(const std::vector<Value>& args) {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    return Value(static_cast<double>(milliseconds));
}

// Date.parse (simplified)
Value Date_parse(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isString()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    // Very simplified date parsing - would need proper ISO 8601 parsing
    return Value(static_cast<double>(Date().getTime()));
}

// Date.prototype.getTime
Value Date_getTime(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    // This is a simplified check - in real implementation we'd check if it's a Date object
    if (args[0].isObject()) {
        // For now, return current time
        auto now = std::chrono::system_clock::now();
        auto duration = now.time_since_epoch();
        auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
        return Value(static_cast<double>(milliseconds));
    }

    return Value(std::numeric_limits<double>::quiet_NaN());
}

// Date.prototype.getFullYear
Value Date_getFullYear(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isObject()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    Date date;
    auto tm = date.toTm();
    return Value(static_cast<double>(tm.tm_year + 1900));
}

// Date.prototype.getMonth
Value Date_getMonth(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isObject()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    Date date;
    auto tm = date.toTm();
    return Value(static_cast<double>(tm.tm_mon));
}

// Date.prototype.getDate
Value Date_getDate(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isObject()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    Date date;
    auto tm = date.toTm();
    return Value(static_cast<double>(tm.tm_mday));
}

// Date.prototype.getDay
Value Date_getDay(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isObject()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    Date date;
    auto tm = date.toTm();
    return Value(static_cast<double>(tm.tm_wday));
}

// Date.prototype.getHours
Value Date_getHours(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isObject()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    Date date;
    auto tm = date.toTm();
    return Value(static_cast<double>(tm.tm_hour));
}

// Date.prototype.getMinutes
Value Date_getMinutes(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isObject()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    Date date;
    auto tm = date.toTm();
    return Value(static_cast<double>(tm.tm_min));
}

// Date.prototype.getSeconds
Value Date_getSeconds(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isObject()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    Date date;
    auto tm = date.toTm();
    return Value(static_cast<double>(tm.tm_sec));
}

// Date.prototype.toString
Value Date_toString(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isObject()) {
        return Value(std::string("Invalid Date"));
    }

    Date date;
    return Value(date.toString());
}

// Date.prototype.toISOString
Value Date_toISOString(const std::vector<Value>& args) {
    if (args.empty() || !args[0].isObject()) {
        return Value(std::string("Invalid Date"));
    }

    Date date;
    return Value(date.toISOString());
}

} // namespace lightjs
