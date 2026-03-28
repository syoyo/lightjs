#include "date_object_internal.h"

namespace lightjs {

Value Date_getTime(const std::vector<Value>& args) {
    if (args.empty()) {
        throw std::runtime_error("TypeError: Date method called on incompatible receiver");
    }
    return Value(receiverTimeValue(args));
}

Value Date_getFullYear(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    return Value(static_cast<double>(localFieldsFromTimeValue(ms).year));
}

Value Date_getMonth(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    return Value(static_cast<double>(localFieldsFromTimeValue(ms).month));
}

Value Date_getDate(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    return Value(static_cast<double>(localFieldsFromTimeValue(ms).day));
}

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

Value Date_getHours(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    return Value(static_cast<double>(localFieldsFromTimeValue(ms).hour));
}

Value Date_getMinutes(const std::vector<Value>& args) {
    double ms = receiverTimeValue(args);
    if (std::isnan(ms)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    return Value(static_cast<double>(localFieldsFromTimeValue(ms).minute));
}

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

}  // namespace lightjs
