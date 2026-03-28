#include "date_object_internal.h"

namespace lightjs {

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

}  // namespace lightjs
