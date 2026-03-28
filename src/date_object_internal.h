#pragma once

#include "date_object.h"
#include "gc.h"
#include "interpreter.h"
#include "symbols.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace lightjs {

struct Date : public Object {
    std::chrono::system_clock::time_point timePoint;

    Date() : timePoint(std::chrono::system_clock::now()) {}

    explicit Date(int64_t milliseconds) {
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

        auto ms = getTime() % 1000;
        oss << "." << std::setfill('0') << std::setw(3) << ms << "Z";

        return oss.str();
    }

    const char* typeName() const override { return "Date"; }
    void getReferences(std::vector<GCObject*>& refs) const override {}
};

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

std::tm utcTmFromMillis(double ms) {
    Date date(static_cast<int64_t>(ms));
    return date.toUtcTm();
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

}  // namespace

}  // namespace lightjs
