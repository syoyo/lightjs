#include "value.h"
#include "interpreter.h"
#include "symbols.h"
#include "streams.h"
#include "wasm_js.h"
#include "gc.h"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <limits>

namespace lightjs {

Value Date_parse(const std::vector<Value>& args);

namespace {

bool isObjectLike(const Value& value) {
    return value.isObject() || value.isArray() || value.isFunction();
}

OrderedMap<std::string, Value>* propertiesForValue(const Value& value) {
    if (value.isObject()) return &value.getGC<Object>()->properties;
    if (value.isFunction()) return &value.getGC<Function>()->properties;
    if (value.isArray()) return &value.getGC<Array>()->properties;
    return nullptr;
}

bool isDateObject(const Value& value) {
    auto* props = propertiesForValue(value);
    if (!props) {
        return false;
    }
    auto isDateIt = props->find("__is_date__");
    auto msIt = props->find("__date_ms__");
    return isDateIt != props->end() &&
           isDateIt->second.isBool() &&
           std::get<bool>(isDateIt->second.data) &&
           msIt != props->end() &&
           msIt->second.isNumber();
}

double dateValueFromReceiver(const Value& value) {
    if (!isDateObject(value)) {
        throw std::runtime_error("TypeError: Date method called on incompatible receiver");
    }
    auto* props = propertiesForValue(value);
    return props->at("__date_ms__").toNumber();
}

double receiverTimeValue(const std::vector<Value>& args) {
    if (args.empty()) {
        throw std::runtime_error("TypeError: Date method called on incompatible receiver");
    }
    return dateValueFromReceiver(args[0]);
}

Value toPrimitiveForDate(const Value& value, const std::string& hint) {
    if (!isObjectLike(value)) {
        return value;
    }

    auto* interp = getGlobalInterpreter();
    if (!interp) {
        throw std::runtime_error("TypeError: Cannot convert object to primitive value");
    }

    auto [hasExotic, exotic] = interp->getPropertyForExternal(value, WellKnownSymbols::toPrimitiveKey());
    if (interp->hasError()) {
        Value err = interp->getError();
        interp->clearError();
        throw JsValueException(err);
    }
    if (hasExotic && !exotic.isUndefined() && !exotic.isNull()) {
        if (!exotic.isFunction()) {
            throw std::runtime_error("TypeError: @@toPrimitive is not callable");
        }
        Value result = interp->callForHarness(exotic, {Value(hint)}, value);
        if (interp->hasError()) {
            Value err = interp->getError();
            interp->clearError();
            throw JsValueException(err);
        }
        if (isObjectLike(result)) {
            throw std::runtime_error("TypeError: @@toPrimitive must return a primitive value");
        }
        return result;
    }

    const char* first = hint == "string" ? "toString" : "valueOf";
    const char* second = hint == "string" ? "valueOf" : "toString";
    for (const char* methodName : {first, second}) {
        auto [found, method] = interp->getPropertyForExternal(value, methodName);
        if (interp->hasError()) {
            Value err = interp->getError();
            interp->clearError();
            throw JsValueException(err);
        }
        if (found && method.isFunction()) {
            Value result = interp->callForHarness(method, {}, value);
            if (interp->hasError()) {
                Value err = interp->getError();
                interp->clearError();
                throw JsValueException(err);
            }
            if (!isObjectLike(result)) {
                return result;
            }
        }
    }

    throw std::runtime_error("TypeError: Cannot convert object to primitive value");
}

double toNumberForDateOperation(const Value& arg) {
    Value primitive = toPrimitiveForDate(arg, "number");
    if (primitive.isSymbol() || primitive.isBigInt()) {
        throw std::runtime_error("TypeError: Cannot convert to number");
    }
    return primitive.toNumber();
}

double timeClip(double time) {
    if (!std::isfinite(time) || std::fabs(time) > 8.64e15) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return std::trunc(time) + 0.0;
}

double roundToEcmaDouble(double value) {
    volatile double rounded = value;
    return rounded;
}

int64_t floorDiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    int64_t r = a % b;
    if (r != 0 && ((r > 0) != (b > 0))) {
        --q;
    }
    return q;
}

int64_t daysFromCivil(int64_t year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

double makeUtcDateValue(double yearValue,
                        double monthValue,
                        double dayValue,
                        double hourValue,
                        double minuteValue,
                        double secondValue,
                        double millisecondValue,
                        bool adjustTwoDigitYear = true) {
    if (!std::isfinite(yearValue) || !std::isfinite(monthValue) || !std::isfinite(dayValue) ||
        !std::isfinite(hourValue) || !std::isfinite(minuteValue) || !std::isfinite(secondValue) ||
        !std::isfinite(millisecondValue)) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    double integerYear = std::trunc(yearValue);
    if (adjustTwoDigitYear && integerYear >= 0.0 && integerYear <= 99.0) {
        integerYear += 1900.0;
    }

    const int64_t year = static_cast<int64_t>(integerYear);
    const int64_t month = static_cast<int64_t>(std::trunc(monthValue));
    const int64_t day = static_cast<int64_t>(std::trunc(dayValue));
    const double hour = std::trunc(hourValue);
    const double minute = std::trunc(minuteValue);
    const double second = std::trunc(secondValue);
    const double millisecond = std::trunc(millisecondValue);

    const int64_t normalizedYear = year + floorDiv(month, 12);
    const int64_t normalizedMonth = month - floorDiv(month, 12) * 12;
    const int64_t dayIndex = daysFromCivil(normalizedYear, static_cast<unsigned>(normalizedMonth + 1), 1) + (day - 1);
    double timeWithinDay = roundToEcmaDouble(static_cast<double>(hour) * 3600000.0);
    timeWithinDay = roundToEcmaDouble(timeWithinDay + roundToEcmaDouble(static_cast<double>(minute) * 60000.0));
    timeWithinDay = roundToEcmaDouble(timeWithinDay + roundToEcmaDouble(static_cast<double>(second) * 1000.0));
    timeWithinDay = roundToEcmaDouble(timeWithinDay + static_cast<double>(millisecond));

    return roundToEcmaDouble(roundToEcmaDouble(static_cast<double>(dayIndex) * 86400000.0) + timeWithinDay);
}

double makeLocalDateValue(double yearValue,
                          double monthValue,
                          double dayValue,
                          double hourValue,
                          double minuteValue,
                          double secondValue,
                          double millisecondValue,
                          bool adjustTwoDigitYear = true) {
    if (!std::isfinite(yearValue) || !std::isfinite(monthValue) || !std::isfinite(dayValue) ||
        !std::isfinite(hourValue) || !std::isfinite(minuteValue) || !std::isfinite(secondValue) ||
        !std::isfinite(millisecondValue)) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    double integerYear = std::trunc(yearValue);
    if (adjustTwoDigitYear && integerYear >= 0.0 && integerYear <= 99.0) {
        integerYear += 1900.0;
    }

    std::tm tm = {};
    tm.tm_year = static_cast<int>(integerYear) - 1900;
    tm.tm_mon = static_cast<int>(std::trunc(monthValue));
    tm.tm_mday = static_cast<int>(std::trunc(dayValue));
    tm.tm_hour = static_cast<int>(std::trunc(hourValue));
    tm.tm_min = static_cast<int>(std::trunc(minuteValue));
    tm.tm_sec = static_cast<int>(std::trunc(secondValue));
    tm.tm_isdst = -1;

    std::time_t local = std::mktime(&tm);
    double base = static_cast<double>(local) * 1000.0 + std::trunc(millisecondValue);
    return timeClip(base);
}

int64_t positiveModulo(int64_t value, int64_t divisor) {
    int64_t result = value % divisor;
    if (result < 0) {
        result += divisor;
    }
    return result;
}

struct CivilDate {
    int64_t year;
    unsigned month;
    unsigned day;
};

struct TimeFields {
    int64_t year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    int millisecond;
};

CivilDate civilFromDays(int64_t z) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t year = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned day = doy - (153 * mp + 2) / 5 + 1;
    const unsigned month = mp + (mp < 10 ? 3 : -9);
    year += month <= 2;
    return {year, month, day};
}

int64_t yearFromUtcMillis(double ms) {
    int64_t wholeMs = static_cast<int64_t>(ms);
    return civilFromDays(floorDiv(wholeMs, 86400000)).year;
}

TimeFields utcFieldsFromTimeValue(double ms) {
    int64_t wholeMs = static_cast<int64_t>(ms);
    int64_t dayIndex = floorDiv(wholeMs, 86400000);
    int64_t timeWithinDay = wholeMs - dayIndex * 86400000;
    CivilDate civil = civilFromDays(dayIndex);
    TimeFields fields;
    fields.year = civil.year;
    fields.month = static_cast<int>(civil.month) - 1;
    fields.day = static_cast<int>(civil.day);
    fields.hour = static_cast<int>(timeWithinDay / 3600000);
    timeWithinDay %= 3600000;
    fields.minute = static_cast<int>(timeWithinDay / 60000);
    timeWithinDay %= 60000;
    fields.second = static_cast<int>(timeWithinDay / 1000);
    fields.millisecond = static_cast<int>(timeWithinDay % 1000);
    return fields;
}

int64_t utcDayIndexFromTimeValue(double ms) {
    int64_t wholeMs = static_cast<int64_t>(ms);
    return floorDiv(wholeMs, 86400000);
}

int weekdayFromDayIndex(int64_t dayIndex) {
    int weekday = static_cast<int>((dayIndex + 4) % 7);
    if (weekday < 0) {
        weekday += 7;
    }
    return weekday;
}

const char* weekdayNameFromIndex(int weekday) {
    static const std::array<const char*, 7> kWeekdays = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    return kWeekdays[weekday];
}

const char* monthNameFromIndex(int month) {
    static const std::array<const char*, 12> kMonths = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    return kMonths[month];
}

double utcTimeValueFromFields(const TimeFields& fields, bool adjustTwoDigitYear = true) {
    return timeClip(makeUtcDateValue(static_cast<double>(fields.year), static_cast<double>(fields.month),
                                     static_cast<double>(fields.day), static_cast<double>(fields.hour),
                                     static_cast<double>(fields.minute), static_cast<double>(fields.second),
                                     static_cast<double>(fields.millisecond), adjustTwoDigitYear));
}

double localTimeValueFromFields(const TimeFields& fields, double offsetMinutes, bool adjustTwoDigitYear = true) {
    double localTimeline = timeClip(makeUtcDateValue(static_cast<double>(fields.year), static_cast<double>(fields.month),
                                                     static_cast<double>(fields.day), static_cast<double>(fields.hour),
                                                     static_cast<double>(fields.minute), static_cast<double>(fields.second),
                                                     static_cast<double>(fields.millisecond), adjustTwoDigitYear));
    if (std::isnan(localTimeline)) {
        return localTimeline;
    }
    return timeClip(localTimeline + offsetMinutes * 60000.0);
}

std::string formatDisplayYear(int year) {
    std::ostringstream oss;
    if (year >= 0) {
        oss << std::setw(4) << std::setfill('0') << year;
    } else {
        oss << "-" << std::setw(4) << std::setfill('0') << std::abs(year);
    }
    return oss.str();
}

std::string formatIsoYear(int year) {
    std::ostringstream oss;
    if (year >= 0 && year <= 9999) {
        oss << std::setw(4) << std::setfill('0') << year;
    } else {
        oss << (year < 0 ? "-" : "+")
            << std::setw(6) << std::setfill('0') << std::abs(year);
    }
    return oss.str();
}

bool parseDecimalDigits(const std::string& input, size_t start, size_t length, int& out) {
    if (start + length > input.size()) return false;
    int value = 0;
    for (size_t i = 0; i < length; ++i) {
        char c = input[start + i];
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

int monthNameToIndex(const std::string& month) {
    static const std::array<const char*, 12> kMonths = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    for (size_t i = 0; i < kMonths.size(); ++i) {
        if (month == kMonths[i]) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::optional<double> parseIsoDateStringToUtcMillis(const std::string& input) {
    size_t pos = 0;
    int sign = 1;
    size_t yearDigits = 4;
    if (!input.empty() && (input[0] == '+' || input[0] == '-')) {
        sign = input[0] == '-' ? -1 : 1;
        ++pos;
        yearDigits = 6;
    }

    int year = 0;
    if (!parseDecimalDigits(input, pos, yearDigits, year)) {
        return std::nullopt;
    }
    if (yearDigits == 6 && sign == -1 && year == 0) {
        return std::nullopt;
    }
    year *= sign;
    pos += yearDigits;

    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int millisecond = 0;

    if (pos == input.size()) {
        return timeClip(makeUtcDateValue(static_cast<double>(year), 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, false));
    }

    if (input[pos] != '-') {
        return std::nullopt;
    }
    ++pos;
    if (!parseDecimalDigits(input, pos, 2, month)) {
        return std::nullopt;
    }
    pos += 2;

    if (pos == input.size()) {
        return timeClip(makeUtcDateValue(static_cast<double>(year), static_cast<double>(month - 1),
                                         1.0, 0.0, 0.0, 0.0, 0.0, false));
    }

    if (input[pos] != '-') {
        return std::nullopt;
    }
    ++pos;
    if (!parseDecimalDigits(input, pos, 2, day)) {
        return std::nullopt;
    }
    pos += 2;

    if (pos == input.size()) {
        return timeClip(makeUtcDateValue(static_cast<double>(year), static_cast<double>(month - 1),
                                         static_cast<double>(day), 0.0, 0.0, 0.0, 0.0, false));
    }

    if (input[pos] != 'T') {
        return std::nullopt;
    }
    ++pos;
    if (!parseDecimalDigits(input, pos, 2, hour) || pos + 2 >= input.size() || input[pos + 2] != ':') {
        return std::nullopt;
    }
    pos += 3;
    if (!parseDecimalDigits(input, pos, 2, minute)) {
        return std::nullopt;
    }
    pos += 2;

    if (pos < input.size() && input[pos] == ':') {
        ++pos;
        if (!parseDecimalDigits(input, pos, 2, second)) {
            return std::nullopt;
        }
        pos += 2;
    }

    if (pos < input.size() && input[pos] == '.') {
        ++pos;
        size_t fracStart = pos;
        while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9') {
            ++pos;
        }
        size_t fracLen = pos - fracStart;
        if (fracLen == 0) return std::nullopt;
        int fracValue = 0;
        for (size_t i = 0; i < std::min<size_t>(3, fracLen); ++i) {
            fracValue = fracValue * 10 + (input[fracStart + i] - '0');
        }
        if (fracLen == 1) fracValue *= 100;
        else if (fracLen == 2) fracValue *= 10;
        millisecond = fracValue;
    }

    if (pos >= input.size()) {
        return std::nullopt;
    }
    if (input[pos] != 'Z' || pos + 1 != input.size()) {
        return std::nullopt;
    }

    return timeClip(makeUtcDateValue(static_cast<double>(year), static_cast<double>(month - 1),
                                     static_cast<double>(day), static_cast<double>(hour),
                                     static_cast<double>(minute), static_cast<double>(second),
                                     static_cast<double>(millisecond), false));
}

void setBuiltinFnProps(const GCPtr<Function>& fn, const std::string& name, int length) {
    fn->properties["name"] = Value(name);
    fn->properties["length"] = Value(static_cast<double>(length));
    fn->properties["__non_writable_name"] = Value(true);
    fn->properties["__non_enum_name"] = Value(true);
    fn->properties["__non_writable_length"] = Value(true);
    fn->properties["__non_enum_length"] = Value(true);
}

} // namespace

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

void setReceiverDateValue(const Value& receiver, double timeValue) {
    auto* props = propertiesForValue(receiver);
    if (!props) {
        throw std::runtime_error("TypeError: Date method called on incompatible receiver");
    }
    (*props)["__date_ms__"] = Value(timeValue);
    if (receiver.isObject()) {
        auto obj = receiver.getGC<Object>();
        if (obj->typeName() == std::string("Date") && std::isfinite(timeValue)) {
            static_cast<Date*>(obj.get())->setTime(static_cast<int64_t>(timeValue));
        }
    }
}

std::tm localTmFromMillis(double ms) {
    Date date(static_cast<int64_t>(ms));
    return date.toTm();
}

std::tm utcTmFromMillis(double ms) {
    Date date(static_cast<int64_t>(ms));
    return date.toUtcTm();
}

double localDateValueFromTm(const std::tm& tm, double millisecond) {
    return makeLocalDateValue(static_cast<double>(tm.tm_year + 1900),
                              static_cast<double>(tm.tm_mon),
                              static_cast<double>(tm.tm_mday),
                              static_cast<double>(tm.tm_hour),
                              static_cast<double>(tm.tm_min),
                              static_cast<double>(tm.tm_sec),
                              millisecond);
}

double utcDateValueFromTm(const std::tm& tm, double millisecond) {
    return timeClip(makeUtcDateValue(static_cast<double>(tm.tm_year + 1900),
                                     static_cast<double>(tm.tm_mon),
                                     static_cast<double>(tm.tm_mday),
                                     static_cast<double>(tm.tm_hour),
                                     static_cast<double>(tm.tm_min),
                                     static_cast<double>(tm.tm_sec),
                                     millisecond));
}

double timezoneOffsetMinutesForTimeValue(double ms) {
    Date date(static_cast<int64_t>(ms));
    std::tm localTm = date.toTm();
    std::tm utcTm = date.toUtcTm();
    std::time_t localSeconds = std::mktime(&localTm);
    std::time_t utcAsLocalSeconds = std::mktime(&utcTm);
    return std::difftime(utcAsLocalSeconds, localSeconds) / 60.0;
}

TimeFields localFieldsFromTimeValue(double ms) {
    double offsetMinutes = timezoneOffsetMinutesForTimeValue(ms);
    double localMs = ms - offsetMinutes * 60000.0;
    return utcFieldsFromTimeValue(localMs);
}

// Date constructor
Value Date_constructor(const std::vector<Value>& args) {
    auto date = GarbageCollector::makeGC<Date>();
    double storedMs = static_cast<double>(date->getTime());

    if (args.size() == 1) {
        if (isDateObject(args[0])) {
            storedMs = dateValueFromReceiver(args[0]);
        } else {
            Value primitive = toPrimitiveForDate(args[0], "default");
            if (primitive.isString()) {
                Value parsed = Date_parse({primitive});
                storedMs = parsed.toNumber();
            } else {
                if (primitive.isSymbol() || primitive.isBigInt()) {
                    throw std::runtime_error("TypeError: Cannot convert to number");
                }
                storedMs = timeClip(primitive.toNumber());
            }
        }
        if (std::isfinite(storedMs)) {
            date->setTime(static_cast<int64_t>(storedMs));
        }
    } else if (args.size() >= 2) {
        // Date(year, month, day?, hour?, minute?, second?, millisecond?)
        double year = toNumberForDateOperation(args[0]);
        double month = toNumberForDateOperation(args[1]);
        double day = args.size() > 2 ? toNumberForDateOperation(args[2]) : 1.0;
        double hour = args.size() > 3 ? toNumberForDateOperation(args[3]) : 0.0;
        double minute = args.size() > 4 ? toNumberForDateOperation(args[4]) : 0.0;
        double second = args.size() > 5 ? toNumberForDateOperation(args[5]) : 0.0;
        double ms = args.size() > 6 ? toNumberForDateOperation(args[6]) : 0.0;

        storedMs = makeLocalDateValue(year, month, day, hour, minute, second, ms);
        if (std::isfinite(storedMs)) {
            date->setTime(static_cast<int64_t>(storedMs));
        }
    }

    // Store time value for prototype/instance methods.
    date->properties["__date_ms__"] = Value(storedMs);
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

    const std::string& input = std::get<std::string>(args[0].data);
    if (input.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    if (input.rfind("-000000", 0) == 0) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    if (auto parsed = parseIsoDateStringToUtcMillis(input)) {
        return Value(*parsed);
    }

    auto tryParseUtcString = [&]() -> std::optional<double> {
        std::tm tm = {};
        std::istringstream iss(input);
        iss >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");
        if (iss.fail()) {
            return std::nullopt;
        }
        return timeClip(makeUtcDateValue(static_cast<double>(tm.tm_year + 1900),
                                         static_cast<double>(tm.tm_mon),
                                         static_cast<double>(tm.tm_mday),
                                         static_cast<double>(tm.tm_hour),
                                         static_cast<double>(tm.tm_min),
                                         static_cast<double>(tm.tm_sec),
                                         0.0));
    };

    auto tryParseLocalDisplayString = [&]() -> std::optional<double> {
        try {
        std::istringstream iss(input);
        std::string dow;
        std::string monthName;
        std::string dayText;
        std::string yearText;
        std::string timeText;
        std::string gmtText;
        if (!(iss >> dow >> monthName >> dayText >> yearText >> timeText >> gmtText)) {
            return std::nullopt;
        }
        if (gmtText.rfind("GMT", 0) != 0 || gmtText.size() != 8) {
            return std::nullopt;
        }
        int month = monthNameToIndex(monthName);
        if (month < 0) {
            return std::nullopt;
        }
        int day = std::stoi(dayText);
        int year = std::stoi(yearText);
        if (timeText.size() != 8 || timeText[2] != ':' || timeText[5] != ':') {
            return std::nullopt;
        }
        int hour = std::stoi(timeText.substr(0, 2));
        int minute = std::stoi(timeText.substr(3, 2));
        int second = std::stoi(timeText.substr(6, 2));
        int sign = gmtText[3] == '-' ? -1 : 1;
        int offsetHour = std::stoi(gmtText.substr(4, 2));
        int offsetMinute = std::stoi(gmtText.substr(6, 2));
        int totalOffsetMinutes = sign * (offsetHour * 60 + offsetMinute);
        return timeClip(makeUtcDateValue(static_cast<double>(year), static_cast<double>(month),
                                         static_cast<double>(day), static_cast<double>(hour),
                                         static_cast<double>(minute), static_cast<double>(second),
                                         0.0) - static_cast<double>(totalOffsetMinutes) * 60000.0);
        } catch (...) {
            return std::nullopt;
        }
    };

    if (auto parsed = tryParseUtcString()) {
        return Value(*parsed);
    }
    if (auto parsed = tryParseLocalDisplayString()) {
        return Value(*parsed);
    }

    // Keep parsing deliberately narrow: accept a small local-time subset and
    // return NaN for everything else instead of falling back to "now".
    auto tryParseLocal = [&](const char* format) -> std::optional<std::tm> {
        std::tm tm = {};
        std::istringstream iss(input);
        iss >> std::get_time(&tm, format);
        if (iss.fail()) {
            return std::nullopt;
        }
        return tm;
    };

    if (auto tm = tryParseLocal("%Y-%m-%dT%H:%M:%S")) {
        return Value(timeClip(static_cast<double>(std::mktime(&*tm)) * 1000.0));
    }

    return Value(std::numeric_limits<double>::quiet_NaN());
}

Value Date_UTC(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double year = toNumberForDateOperation(args[0]);
    double month = args.size() > 1 ? toNumberForDateOperation(args[1]) : 0.0;
    double day = args.size() > 2 ? toNumberForDateOperation(args[2]) : 1.0;
    double hour = args.size() > 3 ? toNumberForDateOperation(args[3]) : 0.0;
    double minute = args.size() > 4 ? toNumberForDateOperation(args[4]) : 0.0;
    double second = args.size() > 5 ? toNumberForDateOperation(args[5]) : 0.0;
    double millisecond = args.size() > 6 ? toNumberForDateOperation(args[6]) : 0.0;

    return Value(timeClip(makeUtcDateValue(year, month, day, hour, minute, second, millisecond)));
}

// Date.prototype.getTime
Value Date_getTime(const std::vector<Value>& args) {
    if (args.empty()) {
        throw std::runtime_error("TypeError: Date method called on incompatible receiver");
    }
    return Value(receiverTimeValue(args));
}

// Date.prototype.getFullYear
Value Date_getFullYear(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    return Value(static_cast<double>(localFieldsFromTimeValue(ms).year));
}

// Date.prototype.getMonth
Value Date_getMonth(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    return Value(static_cast<double>(localFieldsFromTimeValue(ms).month));
}

// Date.prototype.getDate
Value Date_getDate(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    return Value(static_cast<double>(localFieldsFromTimeValue(ms).day));
}

// Date.prototype.getDay
Value Date_getDay(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    double offsetMinutes = timezoneOffsetMinutesForTimeValue(ms);
    double localMs = ms - offsetMinutes * 60000.0;
    int64_t dayIndex = floorDiv(static_cast<int64_t>(localMs), 86400000);
    return Value(static_cast<double>(positiveModulo(dayIndex + 4, 7)));
}

// Date.prototype.getHours
Value Date_getHours(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    return Value(static_cast<double>(localFieldsFromTimeValue(ms).hour));
}

// Date.prototype.getMinutes
Value Date_getMinutes(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    return Value(static_cast<double>(localFieldsFromTimeValue(ms).minute));
}

// Date.prototype.getSeconds
Value Date_getSeconds(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    return Value(static_cast<double>(localFieldsFromTimeValue(ms).second));
}

Value Date_getUTCFullYear(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(static_cast<double>(utcFieldsFromTimeValue(ms).year));
}

Value Date_getUTCMonth(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(static_cast<double>(utcFieldsFromTimeValue(ms).month));
}

Value Date_getUTCDate(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(static_cast<double>(utcFieldsFromTimeValue(ms).day));
}

Value Date_getUTCDay(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) return Value(std::numeric_limits<double>::quiet_NaN());
    int64_t dayIndex = floorDiv(static_cast<int64_t>(ms), 86400000);
    return Value(static_cast<double>(positiveModulo(dayIndex + 4, 7)));
}

Value Date_getUTCHours(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(static_cast<double>(utcFieldsFromTimeValue(ms).hour));
}

Value Date_getUTCMinutes(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(static_cast<double>(utcFieldsFromTimeValue(ms).minute));
}

Value Date_getUTCSeconds(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(static_cast<double>(utcFieldsFromTimeValue(ms).second));
}

Value Date_getMilliseconds(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) return Value(std::numeric_limits<double>::quiet_NaN());
    return Value(static_cast<double>(positiveModulo(static_cast<int64_t>(ms), 1000)));
}

Value Date_getUTCMilliseconds(const std::vector<Value>& args) {
    return Date_getMilliseconds(args);
}

Value Date_getTimezoneOffset(const std::vector<Value>& args) {
    double ms = dateValueFromReceiver(args[0]);
    if (std::isnan(ms)) return Value(std::numeric_limits<double>::quiet_NaN());

    Date date(static_cast<int64_t>(ms));
    std::tm localTm = date.toTm();
    std::tm utcTm = date.toUtcTm();
    std::time_t localSeconds = std::mktime(&localTm);
    std::time_t utcAsLocalSeconds = std::mktime(&utcTm);
    double offsetMinutes = std::difftime(utcAsLocalSeconds, localSeconds) / 60.0;
    return Value(offsetMinutes);
}

Value Date_setTime(const std::vector<Value>& args) {
    if (args.empty()) {
        throw std::runtime_error("TypeError: Date method called on incompatible receiver");
    }
    double current = receiverTimeValue(args);
    (void)current;

    double clipped = timeClip(args.size() > 1 ? toNumberForDateOperation(args[1]) : std::numeric_limits<double>::quiet_NaN());
    auto* props = propertiesForValue(args[0]);
    props->operator[]("__date_ms__") = Value(clipped);
    if (std::isfinite(clipped) && args[0].isObject()) {
        auto obj = args[0].getGC<Object>();
        if (obj->typeName() == std::string("Date")) {
            static_cast<Date*>(obj.get())->setTime(static_cast<int64_t>(clipped));
        }
    }
    return Value(clipped);
}

Value Date_setMilliseconds(const std::vector<Value>& args) {
    double t = receiverTimeValue(args);
    double milli = toNumberForDateOperation(args.size() > 1 ? args[1] : Value(Undefined{}));
    if (std::isnan(t)) return Value(std::numeric_limits<double>::quiet_NaN());
    if (args.size() <= 1 || std::isnan(milli)) {
        double nan = std::numeric_limits<double>::quiet_NaN();
        setReceiverDateValue(args[0], nan);
        return Value(nan);
    }
    double offsetMinutes = timezoneOffsetMinutesForTimeValue(t);
    TimeFields fields = localFieldsFromTimeValue(t);
    fields.millisecond = static_cast<int>(std::trunc(milli));
    double v = localTimeValueFromFields(fields, offsetMinutes);
    setReceiverDateValue(args[0], v);
    return Value(v);
}

Value Date_setUTCMilliseconds(const std::vector<Value>& args) {
    double t = receiverTimeValue(args);
    double milli = toNumberForDateOperation(args.size() > 1 ? args[1] : Value(Undefined{}));
    if (std::isnan(t)) return Value(std::numeric_limits<double>::quiet_NaN());
    if (args.size() <= 1 || std::isnan(milli)) {
        double nan = std::numeric_limits<double>::quiet_NaN();
        setReceiverDateValue(args[0], nan);
        return Value(nan);
    }
    std::tm tm = utcTmFromMillis(t);
    double v = utcDateValueFromTm(tm, milli);
    setReceiverDateValue(args[0], v);
    return Value(v);
}

Value Date_setSeconds(const std::vector<Value>& args) {
    double t = receiverTimeValue(args);
    double sec = toNumberForDateOperation(args.size() > 1 ? args[1] : Value(Undefined{}));
    double milli = 0.0;
    if (args.size() > 2) {
        milli = toNumberForDateOperation(args[2]);
    }
    if (std::isnan(t)) return Value(std::numeric_limits<double>::quiet_NaN());
    if (args.size() <= 1 || std::isnan(sec) || (args.size() > 2 && std::isnan(milli))) {
        double nan = std::numeric_limits<double>::quiet_NaN();
        setReceiverDateValue(args[0], nan);
        return Value(nan);
    }
    double offsetMinutes = timezoneOffsetMinutesForTimeValue(t);
    TimeFields fields = localFieldsFromTimeValue(t);
    fields.second = static_cast<int>(std::trunc(sec));
    fields.millisecond = static_cast<int>(std::trunc(args.size() > 2 ? milli : static_cast<double>(fields.millisecond)));
    double v = localTimeValueFromFields(fields, offsetMinutes);
    setReceiverDateValue(args[0], v);
    return Value(v);
}

Value Date_setUTCSeconds(const std::vector<Value>& args) {
    double t = receiverTimeValue(args);
    double sec = toNumberForDateOperation(args.size() > 1 ? args[1] : Value(Undefined{}));
    double milli = 0.0;
    if (args.size() > 2) {
        milli = toNumberForDateOperation(args[2]);
    }
    if (std::isnan(t)) return Value(std::numeric_limits<double>::quiet_NaN());
    if (args.size() <= 1 || std::isnan(sec) || (args.size() > 2 && std::isnan(milli))) {
        double nan = std::numeric_limits<double>::quiet_NaN();
        setReceiverDateValue(args[0], nan);
        return Value(nan);
    }
    std::tm tm = utcTmFromMillis(t);
    double msPart = args.size() > 2 ? milli : static_cast<double>(positiveModulo(static_cast<int64_t>(t), 1000));
    double v = timeClip(makeUtcDateValue(static_cast<double>(tm.tm_year + 1900), static_cast<double>(tm.tm_mon),
                                         static_cast<double>(tm.tm_mday), static_cast<double>(tm.tm_hour),
                                         static_cast<double>(tm.tm_min), sec, msPart));
    setReceiverDateValue(args[0], v);
    return Value(v);
}

Value Date_setMinutes(const std::vector<Value>& args) {
    double t = receiverTimeValue(args);
    double min = toNumberForDateOperation(args.size() > 1 ? args[1] : Value(Undefined{}));
    double sec = 0.0;
    double milli = 0.0;
    if (args.size() > 2) sec = toNumberForDateOperation(args[2]);
    if (args.size() > 3) milli = toNumberForDateOperation(args[3]);
    if (std::isnan(t)) return Value(std::numeric_limits<double>::quiet_NaN());
    if (args.size() <= 1 || std::isnan(min) ||
        (args.size() > 2 && std::isnan(sec)) ||
        (args.size() > 3 && std::isnan(milli))) {
        double nan = std::numeric_limits<double>::quiet_NaN();
        setReceiverDateValue(args[0], nan);
        return Value(nan);
    }
    double offsetMinutes = timezoneOffsetMinutesForTimeValue(t);
    TimeFields fields = localFieldsFromTimeValue(t);
    fields.minute = static_cast<int>(std::trunc(min));
    if (args.size() > 2) fields.second = static_cast<int>(std::trunc(sec));
    if (args.size() > 3) fields.millisecond = static_cast<int>(std::trunc(milli));
    double v = localTimeValueFromFields(fields, offsetMinutes);
    setReceiverDateValue(args[0], v);
    return Value(v);
}

Value Date_setUTCMinutes(const std::vector<Value>& args) {
    double t = receiverTimeValue(args);
    double min = toNumberForDateOperation(args.size() > 1 ? args[1] : Value(Undefined{}));
    double sec = 0.0;
    double milli = 0.0;
    if (args.size() > 2) sec = toNumberForDateOperation(args[2]);
    if (args.size() > 3) milli = toNumberForDateOperation(args[3]);
    if (std::isnan(t)) return Value(std::numeric_limits<double>::quiet_NaN());
    if (args.size() <= 1 || std::isnan(min) ||
        (args.size() > 2 && std::isnan(sec)) ||
        (args.size() > 3 && std::isnan(milli))) {
        double nan = std::numeric_limits<double>::quiet_NaN();
        setReceiverDateValue(args[0], nan);
        return Value(nan);
    }
    std::tm tm = utcTmFromMillis(t);
    double secPart = args.size() > 2 ? sec : static_cast<double>(tm.tm_sec);
    double msPart = args.size() > 3 ? milli : static_cast<double>(positiveModulo(static_cast<int64_t>(t), 1000));
    double v = timeClip(makeUtcDateValue(static_cast<double>(tm.tm_year + 1900), static_cast<double>(tm.tm_mon),
                                         static_cast<double>(tm.tm_mday), static_cast<double>(tm.tm_hour),
                                         min, secPart, msPart));
    setReceiverDateValue(args[0], v);
    return Value(v);
}

Value Date_setHours(const std::vector<Value>& args) {
    double t = receiverTimeValue(args);
    double hour = toNumberForDateOperation(args.size() > 1 ? args[1] : Value(Undefined{}));
    double min = 0.0;
    double sec = 0.0;
    double milli = 0.0;
    if (args.size() > 2) min = toNumberForDateOperation(args[2]);
    if (args.size() > 3) sec = toNumberForDateOperation(args[3]);
    if (args.size() > 4) milli = toNumberForDateOperation(args[4]);
    if (std::isnan(t)) return Value(std::numeric_limits<double>::quiet_NaN());
    if (args.size() <= 1 || std::isnan(hour) ||
        (args.size() > 2 && std::isnan(min)) ||
        (args.size() > 3 && std::isnan(sec)) ||
        (args.size() > 4 && std::isnan(milli))) {
        double nan = std::numeric_limits<double>::quiet_NaN();
        setReceiverDateValue(args[0], nan);
        return Value(nan);
    }
    double offsetMinutes = timezoneOffsetMinutesForTimeValue(t);
    TimeFields fields = localFieldsFromTimeValue(t);
    fields.hour = static_cast<int>(std::trunc(hour));
    if (args.size() > 2) fields.minute = static_cast<int>(std::trunc(min));
    if (args.size() > 3) fields.second = static_cast<int>(std::trunc(sec));
    if (args.size() > 4) fields.millisecond = static_cast<int>(std::trunc(milli));
    double v = localTimeValueFromFields(fields, offsetMinutes);
    setReceiverDateValue(args[0], v);
    return Value(v);
}

Value Date_setUTCHours(const std::vector<Value>& args) {
    double t = receiverTimeValue(args);
    double hour = toNumberForDateOperation(args.size() > 1 ? args[1] : Value(Undefined{}));
    double min = 0.0;
    double sec = 0.0;
    double milli = 0.0;
    if (args.size() > 2) min = toNumberForDateOperation(args[2]);
    if (args.size() > 3) sec = toNumberForDateOperation(args[3]);
    if (args.size() > 4) milli = toNumberForDateOperation(args[4]);
    if (std::isnan(t)) return Value(std::numeric_limits<double>::quiet_NaN());
    if (args.size() <= 1 || std::isnan(hour) ||
        (args.size() > 2 && std::isnan(min)) ||
        (args.size() > 3 && std::isnan(sec)) ||
        (args.size() > 4 && std::isnan(milli))) {
        double nan = std::numeric_limits<double>::quiet_NaN();
        setReceiverDateValue(args[0], nan);
        return Value(nan);
    }
    std::tm tm = utcTmFromMillis(t);
    double minPart = args.size() > 2 ? min : static_cast<double>(tm.tm_min);
    double secPart = args.size() > 3 ? sec : static_cast<double>(tm.tm_sec);
    double msPart = args.size() > 4 ? milli : static_cast<double>(positiveModulo(static_cast<int64_t>(t), 1000));
    double v = timeClip(makeUtcDateValue(static_cast<double>(tm.tm_year + 1900), static_cast<double>(tm.tm_mon),
                                         static_cast<double>(tm.tm_mday), hour, minPart, secPart, msPart));
    setReceiverDateValue(args[0], v);
    return Value(v);
}

Value Date_setDate(const std::vector<Value>& args) {
    double t = receiverTimeValue(args);
    double day = toNumberForDateOperation(args.size() > 1 ? args[1] : Value(Undefined{}));
    if (std::isnan(t)) return Value(std::numeric_limits<double>::quiet_NaN());
    double offsetMinutes = timezoneOffsetMinutesForTimeValue(t);
    TimeFields fields = localFieldsFromTimeValue(t);
    fields.day = static_cast<int>(std::trunc(day));
    double v = localTimeValueFromFields(fields, offsetMinutes);
    setReceiverDateValue(args[0], v);
    return Value(v);
}

Value Date_setUTCDate(const std::vector<Value>& args) {
    double t = receiverTimeValue(args);
    double day = toNumberForDateOperation(args.size() > 1 ? args[1] : Value(Undefined{}));
    if (std::isnan(t)) return Value(std::numeric_limits<double>::quiet_NaN());
    std::tm tm = utcTmFromMillis(t);
    double v = timeClip(makeUtcDateValue(static_cast<double>(tm.tm_year + 1900), static_cast<double>(tm.tm_mon),
                                         day, static_cast<double>(tm.tm_hour), static_cast<double>(tm.tm_min),
                                         static_cast<double>(tm.tm_sec),
                                         static_cast<double>(positiveModulo(static_cast<int64_t>(t), 1000))));
    setReceiverDateValue(args[0], v);
    return Value(v);
}

Value Date_setMonth(const std::vector<Value>& args) {
    double t = receiverTimeValue(args);
    double month = toNumberForDateOperation(args.size() > 1 ? args[1] : Value(Undefined{}));
    double day = 0.0;
    if (args.size() > 2) day = toNumberForDateOperation(args[2]);
    if (std::isnan(t)) return Value(std::numeric_limits<double>::quiet_NaN());
    double offsetMinutes = timezoneOffsetMinutesForTimeValue(t);
    TimeFields fields = localFieldsFromTimeValue(t);
    fields.month = static_cast<int>(std::trunc(month));
    if (args.size() > 2) fields.day = static_cast<int>(std::trunc(day));
    double v = localTimeValueFromFields(fields, offsetMinutes);
    setReceiverDateValue(args[0], v);
    return Value(v);
}

Value Date_setUTCMonth(const std::vector<Value>& args) {
    double t = receiverTimeValue(args);
    double month = toNumberForDateOperation(args.size() > 1 ? args[1] : Value(Undefined{}));
    double day = 0.0;
    if (args.size() > 2) day = toNumberForDateOperation(args[2]);
    if (std::isnan(t)) return Value(std::numeric_limits<double>::quiet_NaN());
    std::tm tm = utcTmFromMillis(t);
    double dayPart = args.size() > 2 ? day : static_cast<double>(tm.tm_mday);
    double v = timeClip(makeUtcDateValue(static_cast<double>(tm.tm_year + 1900), month, dayPart,
                                         static_cast<double>(tm.tm_hour), static_cast<double>(tm.tm_min),
                                         static_cast<double>(tm.tm_sec),
                                         static_cast<double>(positiveModulo(static_cast<int64_t>(t), 1000))));
    setReceiverDateValue(args[0], v);
    return Value(v);
}

Value Date_setFullYear(const std::vector<Value>& args) {
    double t = receiverTimeValue(args);
    double year = toNumberForDateOperation(args.size() > 1 ? args[1] : Value(Undefined{}));
    double month = 0.0;
    double day = 0.0;
    if (args.size() > 2) month = toNumberForDateOperation(args[2]);
    if (args.size() > 3) day = toNumberForDateOperation(args[3]);
    bool invalidBase = std::isnan(t);
    double baseMs = invalidBase ? 0.0 : t;
    std::tm tm = {};
    if (invalidBase) {
        tm.tm_year = 70;
        tm.tm_mon = 0;
        tm.tm_mday = 1;
        tm.tm_hour = 0;
        tm.tm_min = 0;
        tm.tm_sec = 0;
    } else {
        TimeFields fields = localFieldsFromTimeValue(baseMs);
        tm.tm_year = static_cast<int>(fields.year) - 1900;
        tm.tm_mon = fields.month;
        tm.tm_mday = fields.day;
        tm.tm_hour = fields.hour;
        tm.tm_min = fields.minute;
        tm.tm_sec = fields.second;
    }
    double monthPart = args.size() > 2 ? month : static_cast<double>(tm.tm_mon);
    double dayPart = args.size() > 3 ? day : static_cast<double>(tm.tm_mday);
    double v = makeLocalDateValue(year, monthPart, dayPart,
                                  static_cast<double>(tm.tm_hour), static_cast<double>(tm.tm_min),
                                  static_cast<double>(tm.tm_sec),
                                  invalidBase ? 0.0 : static_cast<double>(positiveModulo(static_cast<int64_t>(baseMs), 1000)),
                                  false);
    setReceiverDateValue(args[0], v);
    return Value(v);
}

Value Date_setUTCFullYear(const std::vector<Value>& args) {
    double t = receiverTimeValue(args);
    double year = toNumberForDateOperation(args.size() > 1 ? args[1] : Value(Undefined{}));
    double month = 0.0;
    double day = 0.0;
    if (args.size() > 2) month = toNumberForDateOperation(args[2]);
    if (args.size() > 3) day = toNumberForDateOperation(args[3]);
    double baseMs = std::isnan(t) ? 0.0 : t;
    TimeFields fields = utcFieldsFromTimeValue(baseMs);
    fields.year = static_cast<int64_t>(std::trunc(year));
    if (args.size() > 2) fields.month = static_cast<int>(std::trunc(month));
    if (args.size() > 3) fields.day = static_cast<int>(std::trunc(day));
    double v = utcTimeValueFromFields(fields, false);
    setReceiverDateValue(args[0], v);
    return Value(v);
}

Value Date_toDateString(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) {
        return Value(std::string("Invalid Date"));
    }

    Date date(static_cast<int64_t>(ms));
    auto tm = date.toTm();
    std::ostringstream oss;
    oss << std::put_time(&tm, "%a %b %d ") << formatDisplayYear(static_cast<int>(yearFromUtcMillis(ms)));
    return Value(oss.str());
}

Value Date_toTimeString(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) {
        return Value(std::string("Invalid Date"));
    }

    Date date(static_cast<int64_t>(ms));
    auto tm = date.toTm();
    char offsetBuffer[8] = {};
    std::strftime(offsetBuffer, sizeof(offsetBuffer), "%z", &tm);
    char zoneBuffer[64] = {};
    std::strftime(zoneBuffer, sizeof(zoneBuffer), "%Z", &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M:%S GMT") << offsetBuffer;
    if (zoneBuffer[0] != '\0') {
        oss << " (" << zoneBuffer << ")";
    }
    return Value(oss.str());
}

Value Date_toUTCString(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) {
        return Value(std::string("Invalid Date"));
    }

    TimeFields fields = utcFieldsFromTimeValue(ms);
    int weekday = weekdayFromDayIndex(utcDayIndexFromTimeValue(ms));
    std::ostringstream oss;
    oss << weekdayNameFromIndex(weekday)
        << ", "
        << std::setw(2) << std::setfill('0') << fields.day
        << " "
        << monthNameFromIndex(fields.month)
        << " "
        << formatDisplayYear(static_cast<int>(fields.year))
        << " "
        << std::setw(2) << std::setfill('0') << fields.hour
        << ":"
        << std::setw(2) << std::setfill('0') << fields.minute
        << ":"
        << std::setw(2) << std::setfill('0') << fields.second
        << " GMT";
    return Value(oss.str());
}

Value Date_toJSON(const std::vector<Value>& args) {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
        throw std::runtime_error("TypeError: Date.prototype.toJSON requires an object");
    }

    Value receiver = args[0];
    Value primitive = toPrimitiveForDate(receiver, "number");
    if (primitive.isNumber() && !std::isfinite(primitive.toNumber())) {
        return Value(Null{});
    }

    auto* interp = getGlobalInterpreter();
    if (!interp) {
        throw std::runtime_error("TypeError: Cannot access toISOString");
    }
    auto [found, toIso] = interp->getPropertyForExternal(receiver, "toISOString");
    if (interp->hasError()) {
        Value err = interp->getError();
        interp->clearError();
        throw JsValueException(err);
    }
    if (!found || !toIso.isFunction()) {
        throw std::runtime_error("TypeError: toISOString is not callable");
    }
    Value result = interp->callForHarness(toIso, {}, receiver);
    if (interp->hasError()) {
        Value err = interp->getError();
        interp->clearError();
        throw JsValueException(err);
    }
    return result;
}

Value Date_toTemporalInstant(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) {
        throw std::runtime_error("RangeError: Invalid time value");
    }

    auto instant = GarbageCollector::makeGC<Object>();
    bigint::BigIntValue epochNanoseconds = bigint::BigIntValue(static_cast<int64_t>(ms));
    epochNanoseconds *= bigint::BigIntValue(1000000);
    instant->properties["epochNanoseconds"] = Value(BigInt(epochNanoseconds));
    return Value(instant);
}

// Date.prototype.toString
Value Date_toString(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) {
        return Value(std::string("Invalid Date"));
    }

    Date date(static_cast<int64_t>(ms));
    auto tm = date.toTm();
    char offsetBuffer[8] = {};
    std::strftime(offsetBuffer, sizeof(offsetBuffer), "%z", &tm);
    char zoneBuffer[64] = {};
    std::strftime(zoneBuffer, sizeof(zoneBuffer), "%Z", &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%a %b %d ") << formatDisplayYear(static_cast<int>(yearFromUtcMillis(ms)))
        << std::put_time(&tm, " %H:%M:%S GMT") << offsetBuffer;
    if (zoneBuffer[0] != '\0') {
        oss << " (" << zoneBuffer << ")";
    }
    return Value(oss.str());
}

// Date.prototype.toISOString
Value Date_toISOString(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) {
        throw std::runtime_error("RangeError: Invalid time value");
    }

    Date date(static_cast<int64_t>(ms));
    int64_t wholeMs = static_cast<int64_t>(ms);
    int64_t dayIndex = floorDiv(wholeMs, 86400000);
    int64_t timeWithinDay = wholeMs - dayIndex * 86400000;
    CivilDate civil = civilFromDays(dayIndex);
    int hour = static_cast<int>(timeWithinDay / 3600000);
    timeWithinDay %= 3600000;
    int minute = static_cast<int>(timeWithinDay / 60000);
    timeWithinDay %= 60000;
    int second = static_cast<int>(timeWithinDay / 1000);
    int millisecond = static_cast<int>(timeWithinDay % 1000);
    std::ostringstream oss;
    oss << formatIsoYear(static_cast<int>(civil.year))
        << "-" << std::setw(2) << std::setfill('0') << civil.month
        << "-" << std::setw(2) << std::setfill('0') << civil.day
        << "T" << std::setw(2) << std::setfill('0') << hour
        << ":" << std::setw(2) << std::setfill('0') << minute
        << ":" << std::setw(2) << std::setfill('0') << second
        << "." << std::setw(3) << std::setfill('0') << millisecond
        << "Z";
    return Value(oss.str());
}

} // namespace lightjs
