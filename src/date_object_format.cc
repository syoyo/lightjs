#include "date_object_internal.h"

namespace lightjs {

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

Value Date_toISOString(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) {
        throw std::runtime_error("RangeError: Invalid time value");
    }

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

}  // namespace lightjs
