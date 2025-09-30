#include "value.h"
#include "gc.h"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <limits>

namespace tinyjs {

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
    auto date = std::make_shared<Date>();
    GarbageCollector::instance().reportAllocation(sizeof(Date));

    if (args.size() == 1 && args[0].isNumber()) {
        // Date(milliseconds)
        int64_t ms = static_cast<int64_t>(std::get<double>(args[0].data));
        date->setTime(ms);
    } else if (args.size() >= 2) {
        // Date(year, month, day?, hour?, minute?, second?, millisecond?)
        int year = args[0].isNumber() ? static_cast<int>(std::get<double>(args[0].data)) : 1970;
        int month = args[1].isNumber() ? static_cast<int>(std::get<double>(args[1].data)) : 0;
        int day = args.size() > 2 && args[2].isNumber() ? static_cast<int>(std::get<double>(args[2].data)) : 1;
        int hour = args.size() > 3 && args[3].isNumber() ? static_cast<int>(std::get<double>(args[3].data)) : 0;
        int minute = args.size() > 4 && args[4].isNumber() ? static_cast<int>(std::get<double>(args[4].data)) : 0;
        int second = args.size() > 5 && args[5].isNumber() ? static_cast<int>(std::get<double>(args[5].data)) : 0;
        int ms = args.size() > 6 && args[6].isNumber() ? static_cast<int>(std::get<double>(args[6].data)) : 0;

        date = std::make_shared<Date>(year, month, day, hour, minute, second, ms);
        GarbageCollector::instance().reportAllocation(sizeof(Date));
    }

    return Value(date);
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

} // namespace tinyjs